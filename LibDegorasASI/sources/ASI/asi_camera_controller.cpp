/*
 *  LibDegorasASI - An extensible C++ library for controlling ZWO ASI astronomy cameras.
 *
 *  Developed as free software by and for the Spanish Navy Observatory SLR station (SFEL) in San Fernando.
 *
 *  Copyright (C) 2024-2026 Degoras Project Team
 *                          < Ángel Vera Herrera, avera@roa.es - angelvh.engr@gmail.com >
 *                          < Jesús Relinque Madroñal, jrelinque@roa.es >
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
 *  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along with this program. If not, see
 *  <https://www.gnu.org/licenses/>.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

// C++ INCLUDES
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <sstream>
#include <thread>

// VENDOR SDK INCLUDES
// This translation unit and the discovery one are the ONLY places the vendor header appears. Note that the vendor
// decorates its entry points with __declspec(dllexport) whenever _WINDOWS is defined, which is wrong for a consumer:
// leaving _WINDOWS undefined yields the plain declarations that link against the import library. Do not define it.
#include <ASICamera2.h>

// PROJECT INCLUDES
#include "LibDegorasASI/ASI/asi_camera_controller.h"
#include "LibDegorasASI/ASI/asi_api_lock.h"
#include "LibDegorasASI/ASI/asi_discovery.h"
#include "LibDegorasASI/ASI/asi_error.h"


// NAMESPACES
namespace dpasi
{
namespace asi
{

using namespace dpasi::types;

namespace
{

// ---------------------------------------------------------------------------------------------------------------------
// LOCK ORDERING
// Exactly one nesting is permitted anywhere in this file: acquisitionMtx(id) may be held while cameraMtx(id) is taken
// (a frame retrieval reads back the live geometry before sizing its buffer). The reverse never happens, so no cycle
// exists. discoveryMtx() is only ever taken alone.
// ---------------------------------------------------------------------------------------------------------------------

/// Upper bound on any frame wait. The vendor's "wait forever" value is NEVER passed, and stopping capture does not
/// cancel a retrieval already blocked, so this also bounds how long a concurrent disconnect can be delayed.
constexpr int kMaxFrameWaitMs = 30000;

/// Bound on a best-effort teardown lock acquisition, so shutdown cannot hang behind a wedged worker.
constexpr int kTeardownLockMs = 100;

/// Clamp a caller's timeout into [0, kMaxFrameWaitMs] so the vendor never receives a negative ("forever") value.
int clampWaitMs(std::chrono::milliseconds timeout)
{
    const long long ms = timeout.count();
    if (ms <= 0)
        return 0;
    return (ms > kMaxFrameWaitMs) ? kMaxFrameWaitMs : static_cast<int>(ms);
}

/// Copy a fixed-size vendor char array that is NOT guaranteed to be terminated when full.
std::string boundedString(const char* buf, std::size_t max_len)
{
    return std::string(buf, std::find(buf, buf + max_len, '\0'));
}

/// Acquire a lock for a teardown-only call, giving up after a bounded wait rather than blocking shutdown forever.
bool tryLockForTeardown(std::unique_lock<std::mutex>& lock)
{
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kTeardownLockMs);
    while (!lock.try_lock())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

/// Render the vendor's 8 raw identity bytes as 16 hexadecimal digits. They are NOT a C string: a full-length value
/// leaves no room for a terminator, so the length is fixed and never inferred.
std::string hexIdentity(const unsigned char* bytes, std::size_t len)
{
    static const char* const kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i)
    {
        out += kHex[(bytes[i] >> 4) & 0x0F];
        out += kHex[bytes[i] & 0x0F];
    }
    return out;
}

/// Build a "call(id=N)" context string for an error.
std::string ctx(const char* call, CameraId id)
{
    std::ostringstream ss;
    ss << call << "(id=" << toType(id) << ")";
    return ss.str();
}

/// Build a "call(id=N, extra)" context string for an error.
std::string ctx(const char* call, CameraId id, const std::string& extra)
{
    std::ostringstream ss;
    ss << call << "(id=" << toType(id) << ", " << extra << ")";
    return ss.str();
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------

AsiCameraController::AsiCameraController(CameraId id) :
    id_(id)
{}

CameraId AsiCameraController::cameraId() const
{
    return this->id_;
}

// -- Camera-scoped lifecycle ------------------------------------------------------------------------------------------

DeviceError AsiCameraController::open()
{
    const std::lock_guard<std::mutex> lock(discoveryMtx());

    // The SDK's internal camera list must exist before a camera can be opened, and building it is a side effect of the
    // enumeration entry point. This mirrors the reference library's build-device-list-then-open sequence.
    ASIGetNumOfConnectedCameras();

    const ASI_ERROR_CODE err = ASIOpenCamera(toType(this->id_));
    if (err != ASI_SUCCESS)
        return errorFromAsi(static_cast<int>(err), ctx("ASIOpenCamera", this->id_));
    return successError();
}

DeviceError AsiCameraController::initialise()
{
    // Held for the whole call: this blocks for a noticeable time and no other operation on this camera is meaningful
    // until it completes. It also disturbs a camera that is capturing, which is why the device layer only calls it
    // while idle.
    const std::lock_guard<std::mutex> lock(cameraMtx(this->id_));

    const ASI_ERROR_CODE err = ASIInitCamera(toType(this->id_));
    if (err != ASI_SUCCESS)
        return errorFromAsi(static_cast<int>(err), ctx("ASIInitCamera", this->id_));
    return successError();
}

DeviceError AsiCameraController::close()
{
    // BOTH locks. discoveryMtx because closing mutates the SDK's process-global connection state, and cameraMtx(id)
    // because every short control call is guarded by that one: without it, ASICloseCamera could tear down this
    // camera's internal SDK state while another thread sat inside ASIGetControlValue on the same id. The acquisition
    // lock the device layer holds during teardown does NOT cover that path -- by design it only covers the blocking
    // frame retrievals -- so the control path needs its own exclusion here.
    //
    // Nesting discoveryMtx -> cameraMtx(id) is acyclic: no path anywhere takes cameraMtx and then discoveryMtx.
    const std::lock_guard<std::mutex> disc_lock(discoveryMtx());
    const std::lock_guard<std::mutex> cam_lock(cameraMtx(this->id_));

    const ASI_ERROR_CODE err = ASICloseCamera(toType(this->id_));
    if (err != ASI_SUCCESS)
        return errorFromAsi(static_cast<int>(err), ctx("ASICloseCamera", this->id_));
    return successError();
}

// -- Identity -------------------------------------------------------------------------------------------------------

DeviceError AsiCameraController::readSerialNumber(CameraSN& out_serial) const
{
    const std::lock_guard<std::mutex> lock(cameraMtx(this->id_));

    ASI_SN serial{};
    const ASI_ERROR_CODE err = ASIGetSerialNumber(toType(this->id_), &serial);
    if (err != ASI_SUCCESS)
        return errorFromAsi(static_cast<int>(err), ctx("ASIGetSerialNumber", this->id_));

    out_serial = hexIdentity(serial.id, sizeof(serial.id));
    return successError();
}

// -- Controls -------------------------------------------------------------------------------------------------------

DeviceError AsiCameraController::readControlCaps(ControlCapsList& out_caps) const
{
    const std::lock_guard<std::mutex> lock(cameraMtx(this->id_));

    int count = 0;
    ASI_ERROR_CODE err = ASIGetNumOfControls(toType(this->id_), &count);
    if (err != ASI_SUCCESS)
        return errorFromAsi(static_cast<int>(err), ctx("ASIGetNumOfControls", this->id_));

    out_caps.clear();
    if (count <= 0)
        return successError();

    out_caps.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index)
    {
        // Note: the vendor indexes this by control POSITION, not by control type.
        ASI_CONTROL_CAPS raw{};
        err = ASIGetControlCaps(toType(this->id_), index, &raw);
        if (err != ASI_SUCCESS)
            return errorFromAsi(static_cast<int>(err), ctx("ASIGetControlCaps", this->id_));

        ControlType type = ControlType::GAIN;
        if (!controlTypeFromType(static_cast<int>(raw.ControlType), type))
            continue;   // A control a newer SDK added and this library does not model: skip rather than mis-report it.

        ControlCaps caps;
        caps.type = type;
        caps.name = boundedString(raw.Name, sizeof(raw.Name));
        caps.description = boundedString(raw.Description, sizeof(raw.Description));
        caps.min_value = static_cast<ControlRaw>(raw.MinValue);
        caps.max_value = static_cast<ControlRaw>(raw.MaxValue);
        caps.default_value = static_cast<ControlRaw>(raw.DefaultValue);
        caps.is_auto_supported = (raw.IsAutoSupported == ASI_TRUE);
        caps.is_writable = (raw.IsWritable == ASI_TRUE);
        out_caps.push_back(caps);
    }
    return successError();
}

DeviceError AsiCameraController::readControl(ControlType type, ControlValue& out_value) const
{
    const std::lock_guard<std::mutex> lock(cameraMtx(this->id_));

    long raw_value = 0;         // The vendor's own width here; it never escapes this translation unit.
    ASI_BOOL raw_auto = ASI_FALSE;
    const ASI_ERROR_CODE err = ASIGetControlValue(toType(this->id_),
                                                  static_cast<ASI_CONTROL_TYPE>(toType(type)),
                                                  &raw_value,
                                                  &raw_auto);
    if (err != ASI_SUCCESS)
        return errorFromAsi(static_cast<int>(err), ctx("ASIGetControlValue", this->id_, toString(type)));

    out_value.value = static_cast<ControlRaw>(raw_value);
    out_value.is_auto = (raw_auto == ASI_TRUE);
    return successError();
}

DeviceError AsiCameraController::writeControl(ControlType type, const ControlValue& value)
{
    const std::lock_guard<std::mutex> lock(cameraMtx(this->id_));

    const ASI_ERROR_CODE err = ASISetControlValue(toType(this->id_),
                                                  static_cast<ASI_CONTROL_TYPE>(toType(type)),
                                                  static_cast<long>(value.value),
                                                  value.is_auto ? ASI_TRUE : ASI_FALSE);
    if (err != ASI_SUCCESS)
        return errorFromAsi(static_cast<int>(err), ctx("ASISetControlValue", this->id_, toString(type)));
    return successError();
}

// -- Region of interest and image format ------------------------------------------------------------------------------

DeviceError AsiCameraController::readRoiFormat(RoiFormat& out_roi) const
{
    const std::lock_guard<std::mutex> lock(cameraMtx(this->id_));

    int width = 0;
    int height = 0;
    int bin = 0;
    ASI_IMG_TYPE raw_format = ASI_IMG_END;
    const ASI_ERROR_CODE err = ASIGetROIFormat(toType(this->id_), &width, &height, &bin, &raw_format);
    if (err != ASI_SUCCESS)
        return errorFromAsi(static_cast<int>(err), ctx("ASIGetROIFormat", this->id_));

    ImageFormat format = ImageFormat::RAW8;
    if (!imageFormatFromType(static_cast<int>(raw_format), format))
        return errorFromLibrary(OperationResult::UNSUPPORTED_CAPABILITY, ctx("ASIGetROIFormat", this->id_));

    out_roi.width = width;
    out_roi.height = height;
    out_roi.bin = bin;
    out_roi.format = format;
    return successError();
}

DeviceError AsiCameraController::writeRoiFormat(const RoiFormat& roi)
{
    // Checked before any vendor call: the SDK rejects a misaligned geometry outright, so validating here reports a
    // precise category without a round trip (and without depending on which vendor code that rejection uses).
    if (!isRoiAligned(roi))
        return errorFromLibrary(OperationResult::INVALID_PARAMETER, ctx("writeRoiFormat", this->id_, roi.toJsonStr()));

    const std::lock_guard<std::mutex> lock(cameraMtx(this->id_));

    const ASI_ERROR_CODE err = ASISetROIFormat(toType(this->id_),
                                               roi.width,
                                               roi.height,
                                               roi.bin,
                                               static_cast<ASI_IMG_TYPE>(toType(roi.format)));
    if (err != ASI_SUCCESS)
        return errorFromAsi(static_cast<int>(err), ctx("ASISetROIFormat", this->id_, roi.toJsonStr()));
    return successError();
}

DeviceError AsiCameraController::readRoiPosition(RoiPosition& out_pos) const
{
    const std::lock_guard<std::mutex> lock(cameraMtx(this->id_));

    int start_x = 0;
    int start_y = 0;
    const ASI_ERROR_CODE err = ASIGetStartPos(toType(this->id_), &start_x, &start_y);
    if (err != ASI_SUCCESS)
        return errorFromAsi(static_cast<int>(err), ctx("ASIGetStartPos", this->id_));

    out_pos.start_x = start_x;
    out_pos.start_y = start_y;
    return successError();
}

DeviceError AsiCameraController::writeRoiPosition(const RoiPosition& pos)
{
    if (pos.start_x < 0 || pos.start_y < 0)
        return errorFromLibrary(OperationResult::INVALID_PARAMETER, ctx("writeRoiPosition", this->id_, pos.toJsonStr()));

    const std::lock_guard<std::mutex> lock(cameraMtx(this->id_));

    const ASI_ERROR_CODE err = ASISetStartPos(toType(this->id_), pos.start_x, pos.start_y);
    if (err != ASI_SUCCESS)
        return errorFromAsi(static_cast<int>(err), ctx("ASISetStartPos", this->id_, pos.toJsonStr()));
    return successError();
}

// -- Frame buffer preparation -----------------------------------------------------------------------------------------

DeviceError AsiCameraController::prepareFrame(Frame& out_frame)
{
    // The destination is ALWAYS sized from the camera's live geometry, never from anything a caller supplied. The
    // vendor's retrievals do no bounds check and are documented to crash on a short buffer, so this is the single gate
    // that makes them safe.
    RoiFormat roi;
    const DeviceError roi_err = this->readRoiFormat(roi);
    if (!roi_err.ok())
        return roi_err;

    const std::size_t required = frameBufferSize(roi);
    if (required == 0)
        return errorFromLibrary(OperationResult::BUFFER_TOO_SMALL, ctx("prepareFrame", this->id_, roi.toJsonStr()));

    out_frame.format = roi.format;
    out_frame.width = roi.width;
    out_frame.height = roi.height;
    out_frame.bin = roi.bin;

    // Grow only when the geometry demands it, so a steady stream reuses one allocation and costs no per-frame malloc.
    if (out_frame.data.size() != required)
        out_frame.data.resize(required);

    return successError();
}

// -- Video streaming --------------------------------------------------------------------------------------------------

DeviceError AsiCameraController::startVideoCapture()
{
    const std::lock_guard<std::mutex> lock(cameraMtx(this->id_));

    const ASI_ERROR_CODE err = ASIStartVideoCapture(toType(this->id_));
    if (err != ASI_SUCCESS)
        return errorFromAsi(static_cast<int>(err), ctx("ASIStartVideoCapture", this->id_));
    return successError();
}

DeviceError AsiCameraController::stopVideoCapture() noexcept
{
    // Teardown-safe and best-effort: if the lock cannot be taken within the bound (e.g. a wedged worker holds it),
    // give up rather than block shutdown. The vendor call is idempotent, so a skipped stop is not corrupting.
    std::unique_lock<std::mutex> lock(cameraMtx(this->id_), std::defer_lock);
    if (!tryLockForTeardown(lock))
        return errorFromLibrary(OperationResult::OPERATION_TIMEOUT, "stopVideoCapture(lock)");

    const ASI_ERROR_CODE err = ASIStopVideoCapture(toType(this->id_));
    if (err != ASI_SUCCESS)
        return errorFromAsi(static_cast<int>(err), ctx("ASIStopVideoCapture", this->id_));
    return successError();
}

DeviceError AsiCameraController::readVideoFrame(Frame& out_frame, std::chrono::milliseconds timeout)
{
    // Held across the blocking retrieval by design: that is what lets a concurrent disconnect wait for an in-flight
    // read instead of closing the camera underneath it. Short control calls use cameraMtx(id) and are unaffected.
    const std::lock_guard<std::mutex> lock(acquisitionMtx(this->id_));

    const DeviceError prep = this->prepareFrame(out_frame);
    if (!prep.ok())
        return prep;

    const ASI_ERROR_CODE err = ASIGetVideoData(toType(this->id_),
                                               out_frame.data.data(),
                                               static_cast<long>(out_frame.data.size()),
                                               clampWaitMs(timeout));
    if (err != ASI_SUCCESS)
        return errorFromAsi(static_cast<int>(err), ctx("ASIGetVideoData", this->id_));

    ++out_frame.sequence;
    out_frame.timestamp = std::chrono::system_clock::now();
    return successError();
}

DeviceError AsiCameraController::readDroppedFrames(int& out_dropped) const
{
    const std::lock_guard<std::mutex> lock(cameraMtx(this->id_));

    int dropped = 0;
    const ASI_ERROR_CODE err = ASIGetDroppedFrames(toType(this->id_), &dropped);
    if (err != ASI_SUCCESS)
        return errorFromAsi(static_cast<int>(err), ctx("ASIGetDroppedFrames", this->id_));

    out_dropped = dropped;
    return successError();
}

// -- Single-frame (snapshot) exposure ---------------------------------------------------------------------------------

DeviceError AsiCameraController::startExposure(bool dark)
{
    const std::lock_guard<std::mutex> lock(cameraMtx(this->id_));

    const ASI_ERROR_CODE err = ASIStartExposure(toType(this->id_), dark ? ASI_TRUE : ASI_FALSE);
    if (err != ASI_SUCCESS)
        return errorFromAsi(static_cast<int>(err), ctx("ASIStartExposure", this->id_));
    return successError();
}

DeviceError AsiCameraController::stopExposure() noexcept
{
    // Teardown-safe and best-effort, for the same reason as stopVideoCapture().
    std::unique_lock<std::mutex> lock(cameraMtx(this->id_), std::defer_lock);
    if (!tryLockForTeardown(lock))
        return errorFromLibrary(OperationResult::OPERATION_TIMEOUT, "stopExposure(lock)");

    const ASI_ERROR_CODE err = ASIStopExposure(toType(this->id_));
    if (err != ASI_SUCCESS)
        return errorFromAsi(static_cast<int>(err), ctx("ASIStopExposure", this->id_));
    return successError();
}

DeviceError AsiCameraController::readExposureState(ExposureState& out_state) const
{
    const std::lock_guard<std::mutex> lock(cameraMtx(this->id_));

    ASI_EXPOSURE_STATUS raw_state = ASI_EXP_IDLE;
    // The vendor's return code must be checked: discarding it turns an unplugged camera into an endless poll loop.
    const ASI_ERROR_CODE err = ASIGetExpStatus(toType(this->id_), &raw_state);
    if (err != ASI_SUCCESS)
        return errorFromAsi(static_cast<int>(err), ctx("ASIGetExpStatus", this->id_));

    if (!exposureStateFromType(static_cast<int>(raw_state), out_state))
        return errorFromLibrary(OperationResult::READ_FAILED, ctx("ASIGetExpStatus", this->id_));
    return successError();
}

DeviceError AsiCameraController::readExposureFrame(Frame& out_frame)
{
    // Under the acquisition lock like the streaming retrieval: this call takes no timeout and blocks for an
    // unspecified period, so a disconnect must be able to wait it out rather than close underneath it.
    const std::lock_guard<std::mutex> lock(acquisitionMtx(this->id_));

    const DeviceError prep = this->prepareFrame(out_frame);
    if (!prep.ok())
        return prep;

    const ASI_ERROR_CODE err = ASIGetDataAfterExp(toType(this->id_),
                                                  out_frame.data.data(),
                                                  static_cast<long>(out_frame.data.size()));
    if (err != ASI_SUCCESS)
        return errorFromAsi(static_cast<int>(err), ctx("ASIGetDataAfterExp", this->id_));

    ++out_frame.sequence;
    out_frame.timestamp = std::chrono::system_clock::now();
    return successError();
}

// -- Miscellaneous ----------------------------------------------------------------------------------------------------

DeviceError AsiCameraController::disableDarkSubtract()
{
    const std::lock_guard<std::mutex> lock(cameraMtx(this->id_));

    const ASI_ERROR_CODE err = ASIDisableDarkSubtract(toType(this->id_));
    if (err != ASI_SUCCESS)
        return errorFromAsi(static_cast<int>(err), ctx("ASIDisableDarkSubtract", this->id_));
    return successError();
}

// ---------------------------------------------------------------------------------------------------------------------

}} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
