/*
 *  DegorasASI - An extensible C++ library for controlling ZWO ASI astronomy cameras.
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
#include <thread>
#include <utility>

// PROJECT INCLUDES
#include "DegorasASI/Devices/asi_camera.h"
#include "DegorasASI/ASI/asi_api_lock.h"
#include "DegorasASI/ASI/asi_camera_registry.h"
#include "DegorasASI/ASI/asi_camera_lock.h"
#include "DegorasASI/ASI/asi_discovery.h"
#include "DegorasASI/Helpers/wait_for.h"


// NAMESPACES
namespace dpasi
{

using namespace dpasi::types;

namespace
{

/// Interval between exposure-state polls while waiting for a single frame.
constexpr int kExposurePollMs = 2;

/// Scale factor between the SDK's raw temperature control and degrees Celsius.
constexpr double kTemperatureScale = 10.0;

/// Ceiling applied to a caller-supplied capture timeout, matching the bound the adapter enforces on the vendor wait.
constexpr int kMaxCaptureWaitMs = 30000;

} // namespace

// ---------------------------------------------------------------------------------------------------------------------

AsiCamera::AsiCamera(CameraId id) :
    AsiCamera(id, std::string())
{
    // Delegating: no required model means any ASI camera at this identifier is accepted.
}

AsiCamera::AsiCamera(CameraId id, std::string expected_model) :
    id_(id),
    expected_model_(std::move(expected_model)),
    connected_(false),
    i_own_claim_(false),
    telemetry_rate_ms_(DeviceConfig().telemetry_rate_ms),
    acq_mode_(AcquisitionMode::IDLE),
    descriptor_(CameraDescriptor()),
    control_caps_(),
    ctrl_(id),
    cb_(),
    frame_cb_(),
    poller_(),
    pump_()
{
    // No device I/O here on purpose: constructing a device object must never touch hardware.
}

AsiCamera::~AsiCamera()
{
    // Deterministic and bounded. doDisconnect() stops the telemetry worker first, then waits out any frame retrieval
    // already in flight before closing, because closing underneath a pending retrieval is unsafe and stopping capture
    // does not cancel one.
    this->doDisconnect();
}

// -- Identity ---------------------------------------------------------------------------------------------------------

CameraId AsiCamera::getCameraId() const
{
    return this->id_;
}

std::string AsiCamera::getExpectedModel() const
{
    return this->expected_model_;
}

bool AsiCamera::isConnected() const
{
    const std::lock_guard<std::mutex> lock(this->state_mtx_);
    return this->connected_;
}

// -- Connection -------------------------------------------------------------------------------------------------------

OperationResult AsiCamera::doConnect(const DeviceConfig& cfg)
{
    {
        // Same object reconnecting is idempotent; a different object on a claimed camera gets CAMERA_IN_USE. Checked
        // before enumeration so the answer does not depend on hardware state.
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        if (this->connected_)
            return OperationResult::ALREADY_CONNECTED;
    }

    // Validate that the camera is present before opening anything, and -- only if a model was required -- that it is
    // the right one. With no required model any ASI camera is accepted, which is the point: every model speaks the
    // same API, so the differences are discovered below rather than decided here.
    CameraDescriptor descriptor;
    const OperationResult found = asi::findCameraById(this->id_, descriptor);
    if (found != OperationResult::OPERATION_OK)
        return found;
    if (!this->expected_model_.empty() && !asi::nameMatchesModel(descriptor.name, this->expected_model_))
        return OperationResult::DEVICE_NOT_FOUND;

    // The SDK gives no aliasing protection of its own: opening an already-open camera returns success, so two objects
    // would each believe they owned it. The claim is what makes CAMERA_IN_USE meaningful.
    //
    // TWO SCOPES, cheapest first. tryClaimCamera() covers this process and is a set lookup; tryLockCameraOnHost()
    // covers every other process on the machine and touches the filesystem, so it only runs once the local answer is
    // yes. The second was added because the first is not enough and the SDK will not help: measured with one process
    // streaming, a second process still enumerated the camera, opened it and initialised it, all ASI_SUCCESS, and the
    // two then fought -- enumeration returning empty four times in six, exposures failing at random. See
    // asi_camera_lock.h for the measurements and for why the host-wide lock fails open.
    if (!asi::tryClaimCamera(this->id_))
        return OperationResult::CAMERA_IN_USE;

    if (!asi::tryLockCameraOnHost(this->id_))
    {
        asi::releaseCamera(this->id_);
        return OperationResult::CAMERA_IN_USE;
    }

    DeviceError err = this->ctrl_.open();
    if (!err.ok())
    {
        asi::releaseCamera(this->id_);
        asi::unlockCameraOnHost(this->id_);
        return err.category;
    }

    err = this->ctrl_.initialise();
    if (!err.ok())
    {
        this->ctrl_.close();
        asi::releaseCamera(this->id_);
        asi::unlockCameraOnHost(this->id_);
        return err.category;
    }

    // Discovered once: measured on hardware, the control set and its writability are identical while streaming, so
    // there is nothing to gain from re-reading them on every state change.
    ControlCapsList caps;
    err = this->ctrl_.readControlCaps(caps);
    if (!err.ok())
    {
        this->ctrl_.close();
        asi::releaseCamera(this->id_);
        asi::unlockCameraOnHost(this->id_);
        return err.category;
    }

    // Non-fatal: a camera that cannot report this setting is still perfectly usable. It is attempted because the
    // setting PERSISTS in the registry across process lifetimes, so a previous run could otherwise silently alter every
    // frame this connection delivers.
    if (cfg.disable_dark_subtract)
        this->ctrl_.disableDarkSubtract();

    // Same reasoning, and the same non-fatal treatment: FLIP persists too, ASIStudio sets it from a menu, and a
    // mirrored buffer shifts the Bayer mosaic as surely as reversing the rows does. Written through the control API
    // rather than doSetFlip() because the connection is not published yet, so the public method would refuse.
    if (cfg.reset_flip)
    {
        ControlValue none;
        none.value = static_cast<ControlRaw>(toType(FlipMode::NONE));
        none.is_auto = false;
        this->ctrl_.writeControl(ControlType::FLIP, none);
    }

    {
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        this->descriptor_ = descriptor;
        this->control_caps_ = std::move(caps);
        this->telemetry_rate_ms_ = (cfg.telemetry_rate_ms > 0) ? cfg.telemetry_rate_ms
                                                              : DeviceConfig().telemetry_rate_ms;
        this->connected_ = true;
        this->i_own_claim_ = true;
        this->acq_mode_ = AcquisitionMode::IDLE;
    }
    return OperationResult::OPERATION_OK;
}

OperationResult AsiCamera::doDisconnect()
{
    {
        // The transition is CLAIMED here, atomically with the test, by clearing connected_ before anything else runs.
        // Testing it and clearing it later (inside the teardown) would let two concurrent disconnects both pass and
        // both close the camera -- and the loser's close could land on a camera that a reconnecting owner had already
        // re-opened in between. Clearing it up front also shuts the door on a worker being started mid-teardown, since
        // every start path gates on checkConnectedLocked().
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        if (!this->connected_)
            return OperationResult::NOT_CONNECTED;
        this->connected_ = false;
    }

    // Stop BOTH workers first, so nothing is in flight while the camera is torn down, and stop them with no lock held:
    // each worker's loop needs state_mtx_, so holding it here would deadlock the join. Both stops are bounded -- a
    // wedged worker is detached rather than joined -- so this can never hang.
    this->pump_.stop();
    this->poller_.stop();

    this->releaseConnection();
    return OperationResult::OPERATION_OK;
}

void AsiCamera::releaseConnection() noexcept
{
    // Taken BEFORE anything is torn down, and held across the whole teardown. This is what makes closing the camera
    // safe: a frame retrieval already inside the vendor SDK cannot be cancelled (stopping capture does not do it), so
    // the only correct move is to WAIT for it, which acquiring the acquisition lock does. Closing underneath a pending
    // vendor call would otherwise be a use-after-free inside the SDK.
    //
    // Lock order is the one this library permits: acquisitionMtx(id) first, then state_mtx_ / cameraMtx(id) inside.
    // No path holds state_mtx_ while waiting for acquisitionMtx, so there is no cycle.
    const std::lock_guard<std::mutex> acq_lock(asi::acquisitionMtx(this->id_));

    AcquisitionMode mode = AcquisitionMode::IDLE;
    {
        // connected_ was already cleared by doDisconnect(), which is what claims the teardown; only the remaining
        // state is reset here.
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        mode = this->acq_mode_;
        this->acq_mode_ = AcquisitionMode::IDLE;
        this->control_caps_.clear();
    }

    // Return the camera to idle before closing. Both stops are teardown-safe and idempotent.
    if (mode == AcquisitionMode::VIDEO)
        this->ctrl_.stopVideoCapture();
    else if (mode == AcquisitionMode::SNAPSHOT)
        this->ctrl_.stopExposure();

    this->ctrl_.close();

    const std::lock_guard<std::mutex> lock(this->state_mtx_);
    if (this->i_own_claim_)
    {
        asi::releaseCamera(this->id_);
        // Released here rather than at process exit, so a long-lived program can disconnect and let the viewer -- or
        // the test suite -- have the camera without being restarted.
        asi::unlockCameraOnHost(this->id_);
        this->i_own_claim_ = false;
    }
}

// -- Capabilities -----------------------------------------------------------------------------------------------------

OperationResult AsiCamera::getDescriptor(CameraDescriptor& descriptor) const
{
    const std::lock_guard<std::mutex> lock(this->state_mtx_);
    const OperationResult res = this->checkConnectedLocked();
    if (res != OperationResult::OPERATION_OK)
        return res;
    descriptor = this->descriptor_;
    return OperationResult::OPERATION_OK;
}

OperationResult AsiCamera::getSerialNumber(CameraSN& serial) const
{
    {
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        const OperationResult res = this->checkConnectedLocked();
        if (res != OperationResult::OPERATION_OK)
            return res;
    }
    // The adapter is stateless beyond its identifier, so calling it on a const device is sound; the SDK only reveals a
    // serial number for an open camera, which the check above has established.
    CameraSN read;
    const DeviceError err = this->ctrl_.readSerialNumber(read);
    if (!err.ok())
        return err.category;
    serial = read;
    return OperationResult::OPERATION_OK;
}

OperationResult AsiCamera::getControlCaps(ControlCapsList& caps) const
{
    const std::lock_guard<std::mutex> lock(this->state_mtx_);
    const OperationResult res = this->checkConnectedLocked();
    if (res != OperationResult::OPERATION_OK)
        return res;
    caps = this->control_caps_;
    return OperationResult::OPERATION_OK;
}

OperationResult AsiCamera::getControlCaps(ControlType type, ControlCaps& caps) const
{
    const std::lock_guard<std::mutex> lock(this->state_mtx_);
    const OperationResult res = this->checkConnectedLocked();
    if (res != OperationResult::OPERATION_OK)
        return res;
    if (!this->findCapsLocked(type, caps))
        return OperationResult::UNSUPPORTED_CAPABILITY;
    return OperationResult::OPERATION_OK;
}

bool AsiCamera::findCapsLocked(ControlType type, ControlCaps& out_caps) const
{
    const auto it = std::find_if(this->control_caps_.cbegin(), this->control_caps_.cend(),
                                 [type](const ControlCaps& c){ return c.type == type; });
    if (it == this->control_caps_.cend())
        return false;
    out_caps = *it;
    return true;
}

bool AsiCamera::queryCaps(ControlType type, ControlCaps& out_caps) const
{
    const std::lock_guard<std::mutex> lock(this->state_mtx_);
    if (!this->connected_)
        return false;   // Nothing is known before connecting, so every predicate answers "cannot".
    return this->findCapsLocked(type, out_caps);
}

// The predicates below let a caller ask "can this camera do X?" without an error round-trip -- which is how one class
// serves every model: a user interface queries the capability and only then offers the control.

bool AsiCamera::hasControl(ControlType type) const
{
    ControlCaps caps;
    return this->queryCaps(type, caps);
}

bool AsiCamera::isControlWritable(ControlType type) const
{
    ControlCaps caps;
    return this->queryCaps(type, caps) && caps.is_writable;
}

bool AsiCamera::isControlAutoCapable(ControlType type) const
{
    ControlCaps caps;
    return this->queryCaps(type, caps) && caps.is_auto_supported;
}

bool AsiCamera::hasCooler() const
{
    const std::lock_guard<std::mutex> lock(this->state_mtx_);
    return this->connected_ && this->descriptor_.is_cooled;
}

bool AsiCamera::isColour() const
{
    const std::lock_guard<std::mutex> lock(this->state_mtx_);
    return this->connected_ && this->descriptor_.is_colour;
}

UsbLinkSpeed AsiCamera::getUsbLinkSpeed() const
{
    const std::lock_guard<std::mutex> lock(this->state_mtx_);
    // Nothing is known before connecting; USB2 is the conservative answer, since it is the slower of the two.
    return this->connected_ ? this->descriptor_.linkSpeed() : UsbLinkSpeed::USB2;
}

bool AsiCamera::isLinkFullSpeed() const
{
    const std::lock_guard<std::mutex> lock(this->state_mtx_);
    return this->connected_ && this->descriptor_.isLinkFullSpeed();
}

bool AsiCamera::supportsFormat(ImageFormat format) const
{
    const std::lock_guard<std::mutex> lock(this->state_mtx_);
    return this->connected_ && this->descriptor_.supportsFormat(format);
}

bool AsiCamera::supportsBin(int bin) const
{
    const std::lock_guard<std::mutex> lock(this->state_mtx_);
    return this->connected_ && this->descriptor_.supportsBin(bin);
}

// -- Controls ---------------------------------------------------------------------------------------------------------

OperationResult AsiCamera::getControl(ControlType type, ControlValue& value)
{
    {
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        const OperationResult res = this->checkConnectedLocked();
        if (res != OperationResult::OPERATION_OK)
            return res;

        // Refuse a control this camera does not have, rather than letting the SDK answer with a fabricated value.
        ControlCaps caps;
        if (!this->findCapsLocked(type, caps))
            return OperationResult::UNSUPPORTED_CAPABILITY;
    }

    const DeviceError err = this->ctrl_.readControl(type, value);
    return err.category;
}

OperationResult AsiCamera::doSetControl(ControlType type, const ControlValue& value)
{
    {
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        const OperationResult res = this->checkConnectedLocked();
        if (res != OperationResult::OPERATION_OK)
            return res;

        ControlCaps caps;
        if (!this->findCapsLocked(type, caps) || !caps.is_writable)
            return OperationResult::UNSUPPORTED_CAPABILITY;
        if (value.is_auto && !caps.is_auto_supported)
            return OperationResult::UNSUPPORTED_CAPABILITY;

        // Range-checked here because the SDK silently CLAMPS an out-of-range value and reports success, which would
        // leave a caller believing it applied a setting the camera never took.
        if (value.value < caps.min_value || value.value > caps.max_value)
            return OperationResult::INVALID_PARAMETER;
    }

    const DeviceError err = this->ctrl_.writeControl(type, value);
    return err.category;
}

// -- Typed control conveniences ---------------------------------------------------------------------------------------

OperationResult AsiCamera::doSetExposure(std::chrono::microseconds exposure)
{
    ControlValue value;
    value.value = static_cast<ControlRaw>(exposure.count());   // The SDK's unit for this control IS microseconds.
    value.is_auto = false;
    return this->doSetControl(ControlType::EXPOSURE, value);
}

OperationResult AsiCamera::getExposure(std::chrono::microseconds& exposure)
{
    ControlValue value;
    const OperationResult res = this->getControl(ControlType::EXPOSURE, value);
    if (res != OperationResult::OPERATION_OK)
        return res;
    exposure = std::chrono::microseconds(value.value);
    return OperationResult::OPERATION_OK;
}

OperationResult AsiCamera::getSensorTemperature(double& celsius)
{
    ControlValue value;
    const OperationResult res = this->getControl(ControlType::TEMPERATURE, value);
    if (res != OperationResult::OPERATION_OK)
        return res;
    // The SDK reports this control in tenths of a degree. The cooler set point is NOT scaled this way, which is why
    // there is no single generic conversion for temperature-like controls.
    celsius = static_cast<double>(value.value) / kTemperatureScale;
    return OperationResult::OPERATION_OK;
}

OperationResult AsiCamera::getIntControl(ControlType type, int& out_value)
{
    ControlValue value;
    const OperationResult res = this->getControl(type, value);
    if (res != OperationResult::OPERATION_OK)
        return res;
    out_value = static_cast<int>(value.value);
    return OperationResult::OPERATION_OK;
}

OperationResult AsiCamera::setIntControl(ControlType type, int value)
{
    ControlValue control;
    control.value = static_cast<ControlRaw>(value);
    control.is_auto = false;
    return this->doSetControl(type, control);
}

OperationResult AsiCamera::doSetUsbBandwidth(int percent, bool automatic)
{
    ControlValue value;
    value.value = static_cast<ControlRaw>(percent);
    value.is_auto = automatic;
    return this->doSetControl(ControlType::BANDWIDTH_OVERLOAD, value);
}

OperationResult AsiCamera::getUsbBandwidth(int& percent, bool& automatic)
{
    ControlValue value;
    const OperationResult res = this->getControl(ControlType::BANDWIDTH_OVERLOAD, value);
    if (res != OperationResult::OPERATION_OK)
        return res;
    percent = static_cast<int>(value.value);
    automatic = value.is_auto;
    return OperationResult::OPERATION_OK;
}

OperationResult AsiCamera::doSetHighSpeedMode(bool enable)
{
    return this->setIntControl(ControlType::HIGH_SPEED_MODE, enable ? 1 : 0);
}

OperationResult AsiCamera::isHighSpeedMode(bool& enabled)
{
    int raw = 0;
    const OperationResult res = this->getIntControl(ControlType::HIGH_SPEED_MODE, raw);
    if (res != OperationResult::OPERATION_OK)
        return res;
    enabled = (raw != 0);
    return OperationResult::OPERATION_OK;
}

OperationResult AsiCamera::doEnableCooler(bool enable)
{
    return this->setIntControl(ControlType::COOLER_ON, enable ? 1 : 0);
}

OperationResult AsiCamera::isCoolerEnabled(bool& enabled)
{
    int raw = 0;
    const OperationResult res = this->getIntControl(ControlType::COOLER_ON, raw);
    if (res != OperationResult::OPERATION_OK)
        return res;
    enabled = (raw != 0);
    return OperationResult::OPERATION_OK;
}

OperationResult AsiCamera::doSetCoolerTarget(int celsius)
{
    // Whole degrees, NOT the tenths getSensorTemperature() unscales. The vendor is inconsistent between the two
    // controls, which is exactly why each gets its own wrapper instead of one generic temperature conversion.
    return this->setIntControl(ControlType::TARGET_TEMPERATURE, celsius);
}

OperationResult AsiCamera::getCoolerTarget(int& celsius)
{
    return this->getIntControl(ControlType::TARGET_TEMPERATURE, celsius);
}

OperationResult AsiCamera::getCoolerPower(int& percent)
{
    return this->getIntControl(ControlType::COOLER_POWER_PERC, percent);
}

OperationResult AsiCamera::doSetFlip(FlipMode mode)
{
    ControlValue value;
    value.value = static_cast<ControlRaw>(toType(mode));
    value.is_auto = false;
    return this->doSetControl(ControlType::FLIP, value);
}

OperationResult AsiCamera::getFlip(FlipMode& mode)
{
    ControlValue value;
    const OperationResult res = this->getControl(ControlType::FLIP, value);
    if (res != OperationResult::OPERATION_OK)
        return res;
    if (value.value < static_cast<ControlRaw>(FlipMode::NONE) || value.value > static_cast<ControlRaw>(FlipMode::BOTH))
        return OperationResult::READ_FAILED;
    mode = static_cast<FlipMode>(value.value);
    return OperationResult::OPERATION_OK;
}

// -- Region of interest and image format ------------------------------------------------------------------------------

OperationResult AsiCamera::getRoiFormat(RoiFormat& roi) const
{
    {
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        const OperationResult res = this->checkConnectedLocked();
        if (res != OperationResult::OPERATION_OK)
            return res;
    }
    const DeviceError err = this->ctrl_.readRoiFormat(roi);
    return err.category;
}

OperationResult AsiCamera::doSetRoi(const RoiFormat& roi, const RoiPosition& pos)
{
    {
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        const OperationResult res = this->checkConnectedLocked();
        if (res != OperationResult::OPERATION_OK)
            return res;

        // The guard that matters: the SDK ACCEPTS a format change while streaming and silently changes the frame
        // geometry a caller has already sized a buffer for. Refusing it here is what keeps that from happening.
        const OperationResult idle = this->checkIdleLocked();
        if (idle != OperationResult::OPERATION_OK)
            return idle;

        if (!this->descriptor_.supportsFormat(roi.format) || !this->descriptor_.supportsBin(roi.bin))
            return OperationResult::UNSUPPORTED_CAPABILITY;

        // Post-binning geometry must fit the binned sensor.
        if (roi.bin < 1 || roi.width > this->descriptor_.max_width / roi.bin ||
            roi.height > this->descriptor_.max_height / roi.bin)
            return OperationResult::INVALID_PARAMETER;
    }

    if (!isRoiAligned(roi))
        return OperationResult::INVALID_PARAMETER;

    // Serialised against the acquisition path, not merely against the tracked mode. The mode check above closes the
    // common case, but on its own it leaves a window: another thread could stop capture (which does NOT cancel a
    // retrieval already blocked in the vendor call) and the mode would then read IDLE while that retrieval is still in
    // flight holding a buffer sized for the OLD geometry. Growing the geometry underneath it is exactly the short-buffer
    // condition the vendor documents as crashing. Taking the acquisition lock makes a geometry change wait the pending
    // retrieval out. It is a configuration call, never a hot path, so the wait costs nothing that matters.
    //
    // Lock order is the one this library permits: acquisitionMtx(id) first, then cameraMtx(id) inside the adapter. No
    // path takes them the other way round, and state_mtx_ is released above before either is acquired.
    const std::lock_guard<std::mutex> acq_lock(asi::acquisitionMtx(this->id_));

    DeviceError err = this->ctrl_.writeRoiFormat(roi);
    if (!err.ok())
        return err.category;

    // Setting the format RECENTRES the origin on the sensor, so the intended origin is applied afterwards, never
    // before, and only once the format actually took.
    err = this->ctrl_.writeRoiPosition(pos);
    if (!err.ok())
        return err.category;

    // Read both back: the SDK clamps and aligns what it was given, so the live geometry is the authority, not the
    // request. Anything that reads a frame later sizes its buffer from this same live geometry.
    RoiFormat applied;
    err = this->ctrl_.readRoiFormat(applied);
    if (!err.ok())
        return err.category;

    RoiPosition applied_pos;
    err = this->ctrl_.readRoiPosition(applied_pos);
    return err.category;
}

OperationResult AsiCamera::getRoiPosition(RoiPosition& pos) const
{
    {
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        const OperationResult res = this->checkConnectedLocked();
        if (res != OperationResult::OPERATION_OK)
            return res;
    }
    const DeviceError err = this->ctrl_.readRoiPosition(pos);
    return err.category;
}

OperationResult AsiCamera::doSetRoiPosition(const RoiPosition& pos)
{
    {
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        const OperationResult res = this->checkConnectedLocked();
        if (res != OperationResult::OPERATION_OK)
            return res;
    }
    // Deliberately allowed while streaming: moving the origin does not change the frame geometry, so a buffer already
    // sized for the current ROI stays correct.
    const DeviceError err = this->ctrl_.writeRoiPosition(pos);
    return err.category;
}

OperationResult AsiCamera::doSetFullFrameRoi(ImageFormat format, int bin)
{
    RoiFormat roi;
    {
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        const OperationResult res = this->checkConnectedLocked();
        if (res != OperationResult::OPERATION_OK)
            return res;
        if (bin < 1 || !this->descriptor_.supportsBin(bin) || !this->descriptor_.supportsFormat(format))
            return OperationResult::UNSUPPORTED_CAPABILITY;

        // Full sensor at this binning, aligned DOWN to the vendor's rules so the result is always acceptable.
        roi.width = (this->descriptor_.max_width / bin) / 8 * 8;
        roi.height = (this->descriptor_.max_height / bin) / 2 * 2;
        roi.bin = bin;
        roi.format = format;
    }
    return this->doSetRoi(roi, RoiPosition());
}

// -- Acquisition, common ----------------------------------------------------------------------------------------------

AcquisitionMode AsiCamera::getAcquisitionMode() const
{
    const std::lock_guard<std::mutex> lock(this->state_mtx_);
    return this->acq_mode_;
}

// -- Acquisition: video streaming -------------------------------------------------------------------------------------

OperationResult AsiCamera::doStartVideoCapture()
{
    // Held across the WHOLE start sequence -- the mode check, arming the hardware, and publishing the new mode.
    //
    // Publishing the mode only after arming would leave a window in which the stream is live while acq_mode_ still
    // reads IDLE. doSetRoi gates on that flag and then serialises on this same lock, so in that window it would pass
    // both gates and hand ASISetROIFormat to a running stream. The SDK accepts that silently, and the next retrieval
    // would size its buffer from the NEW geometry while the pipeline still delivers frames at the OLD one -- a heap
    // overflow inside the vendor DLL, which performs no bounds check. Taking the lock here makes the start sequence
    // and a geometry change mutually exclusive. Arming is a short non-blocking call, so holding it costs nothing.
    const std::lock_guard<std::mutex> acq_lock(asi::acquisitionMtx(this->id_));

    {
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        const OperationResult res = this->checkConnectedLocked();
        if (res != OperationResult::OPERATION_OK)
            return res;
        // Idempotent for the streaming case; refused outright while a snapshot exposure is in progress.
        if (this->acq_mode_ == AcquisitionMode::VIDEO)
            return OperationResult::OPERATION_OK;
        if (this->acq_mode_ != AcquisitionMode::IDLE)
            return OperationResult::INVALID_SEQUENCE;
    }

    const DeviceError err = this->ctrl_.startVideoCapture();
    if (!err.ok())
        return err.category;

    const std::lock_guard<std::mutex> lock(this->state_mtx_);
    this->acq_mode_ = AcquisitionMode::VIDEO;
    return OperationResult::OPERATION_OK;
}

OperationResult AsiCamera::doStopVideoCapture()
{
    // Stop the frame worker first, with NO lock held: it loops on doGetVideoFrame(), which needs state_mtx_, so holding
    // that here would deadlock the join. Leaving it running would be worse -- it would keep pulling from a stream that
    // is about to stop and report a flood of timeouts to the callback.
    this->pump_.stop();

    {
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        const OperationResult res = this->checkConnectedLocked();
        if (res != OperationResult::OPERATION_OK)
            return res;
        if (this->acq_mode_ != AcquisitionMode::VIDEO)
            return OperationResult::INVALID_SEQUENCE;
    }

    const DeviceError err = this->ctrl_.stopVideoCapture();

    // The mode is cleared even if the vendor call reported a problem: leaving the object believing it is still
    // streaming would block every subsequent configuration change.
    const std::lock_guard<std::mutex> lock(this->state_mtx_);
    this->acq_mode_ = AcquisitionMode::IDLE;
    return err.category;
}

OperationResult AsiCamera::doGetVideoFrame(Frame& frame, std::chrono::milliseconds timeout)
{
    {
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        const OperationResult res = this->checkConnectedLocked();
        if (res != OperationResult::OPERATION_OK)
            return res;
        if (this->acq_mode_ != AcquisitionMode::VIDEO)
            return OperationResult::INVALID_SEQUENCE;
    }

    // The state lock is released before the blocking retrieval: it must not be held across a wait. The adapter takes the
    // per-camera acquisition lock instead, which is what a concurrent disconnect waits on.
    const DeviceError err = this->ctrl_.readVideoFrame(frame, timeout);
    return err.category;
}

OperationResult AsiCamera::getDroppedFrames(int& dropped)
{
    {
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        const OperationResult res = this->checkConnectedLocked();
        if (res != OperationResult::OPERATION_OK)
            return res;
    }
    const DeviceError err = this->ctrl_.readDroppedFrames(dropped);
    return err.category;
}

// -- Acquisition: single frame (snapshot) -----------------------------------------------------------------------------

OperationResult AsiCamera::doStartExposure(bool dark)
{
    // Held across the whole arm sequence, for exactly the reason given in doStartVideoCapture: otherwise the sensor is
    // exposing while acq_mode_ still reads IDLE, and a concurrent doSetRoi would shrink the geometry mid-exposure. The
    // download would then size its buffer from the new ROI while the sensor produced a frame at the old one, which the
    // vendor's unchecked ASIGetDataAfterExp turns into a heap overflow.
    const std::lock_guard<std::mutex> acq_lock(asi::acquisitionMtx(this->id_));

    {
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        const OperationResult res = this->checkConnectedLocked();
        if (res != OperationResult::OPERATION_OK)
            return res;
        // Refused while streaming (the SDK rejects it too) and while another exposure is already running.
        const OperationResult idle = this->checkIdleLocked();
        if (idle != OperationResult::OPERATION_OK)
            return idle;
    }

    const DeviceError err = this->ctrl_.startExposure(dark);
    if (!err.ok())
        return err.category;

    const std::lock_guard<std::mutex> lock(this->state_mtx_);
    this->acq_mode_ = AcquisitionMode::SNAPSHOT;
    return OperationResult::OPERATION_OK;
}

OperationResult AsiCamera::doStopExposure()
{
    {
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        const OperationResult res = this->checkConnectedLocked();
        if (res != OperationResult::OPERATION_OK)
            return res;
        if (this->acq_mode_ != AcquisitionMode::SNAPSHOT)
            return OperationResult::INVALID_SEQUENCE;
    }

    const DeviceError err = this->ctrl_.stopExposure();

    const std::lock_guard<std::mutex> lock(this->state_mtx_);
    this->acq_mode_ = AcquisitionMode::IDLE;
    return err.category;
}

OperationResult AsiCamera::getExposureState(ExposureState& state)
{
    {
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        const OperationResult res = this->checkConnectedLocked();
        if (res != OperationResult::OPERATION_OK)
            return res;
    }
    const DeviceError err = this->ctrl_.readExposureState(state);
    return err.category;
}

OperationResult AsiCamera::doGetExposureFrame(Frame& frame)
{
    {
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        const OperationResult res = this->checkConnectedLocked();
        if (res != OperationResult::OPERATION_OK)
            return res;
        if (this->acq_mode_ != AcquisitionMode::SNAPSHOT)
            return OperationResult::INVALID_SEQUENCE;
    }

    // Distinguish the three ways an exposure can not be downloadable, instead of letting the vendor call fail opaquely.
    ExposureState state = ExposureState::IDLE;
    const DeviceError state_err = this->ctrl_.readExposureState(state);
    if (!state_err.ok())
        return state_err.category;

    if (state == ExposureState::WORKING)
        return OperationResult::OPERATION_TIMEOUT;
    if (state == ExposureState::FAILED)
        return OperationResult::ACQUISITION_FAILED;
    if (state != ExposureState::SUCCESS)
    {
        // The sensor reports idle: the exposure never ran or was already collected. Neither a success nor a failure, so
        // it is reported explicitly rather than silently yielding no frame.
        return OperationResult::READ_FAILED;
    }

    const DeviceError err = this->ctrl_.readExposureFrame(frame);
    return err.category;
}

OperationResult AsiCamera::doCaptureSingleFrame(Frame& frame, std::chrono::milliseconds timeout, bool dark)
{
    const OperationResult started = this->doStartExposure(dark);
    if (started != OperationResult::OPERATION_OK)
        return started;

    // Poll until the sensor leaves the working state, or the deadline passes. The vendor's read code is checked on every
    // poll, so an unplugged camera ends the wait instead of spinning forever.
    ExposureState state = ExposureState::WORKING;
    OperationResult read_res = OperationResult::OPERATION_OK;
    const OperationResult waited = waitForCondition(
        [this, &state, &read_res]()
        {
            read_res = this->getExposureState(state);
            return read_res != OperationResult::OPERATION_OK || state != ExposureState::WORKING;
        },
        timeout,
        std::chrono::milliseconds(kExposurePollMs));

    // Every exit path below returns the camera to idle, so a failed capture never leaves it mid-exposure.
    if (read_res != OperationResult::OPERATION_OK)
    {
        this->doStopExposure();
        return read_res;
    }
    if (waited != OperationResult::OPERATION_OK)
    {
        this->doStopExposure();
        return OperationResult::OPERATION_TIMEOUT;
    }
    if (state == ExposureState::FAILED)
    {
        this->doStopExposure();
        return OperationResult::ACQUISITION_FAILED;
    }

    const OperationResult got = this->doGetExposureFrame(frame);
    this->doStopExposure();
    return got;
}

// -- Telemetry --------------------------------------------------------------------------------------------------------

OperationResult AsiCamera::fillStatus(CameraStatus& status)
{
    status = CameraStatus();
    status.id = this->id_;

    {
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        status.connected = this->connected_;
        status.acq_mode = this->acq_mode_;
        if (!this->connected_)
            return OperationResult::NOT_CONNECTED;
    }

    // A field that could not be read is left flagged invalid rather than defaulted, so a plausible-looking zero is
    // never mistaken for a real reading.
    double celsius = 0;
    if (this->getSensorTemperature(celsius) == OperationResult::OPERATION_OK)
    {
        status.temperature_c = celsius;
        status.temperature_valid = true;
    }

    this->ctrl_.readDroppedFrames(status.dropped_frames);
    this->ctrl_.readExposureState(status.exposure_state);

    // The live geometry is the one field a consumer must be able to trust, since it determines every frame's size.
    const DeviceError roi_err = this->ctrl_.readRoiFormat(status.roi);
    if (!roi_err.ok())
        return roi_err.category;

    return OperationResult::OPERATION_OK;
}

OperationResult AsiCamera::getDeviceStatus(CameraStatus& status)
{
    return this->fillStatus(status);
}

// -- Callback-driven frame acquisition --------------------------------------------------------------------------------

OperationResult AsiCamera::setNewFrameCb(NewFrameCb cb)
{
    const std::lock_guard<std::mutex> lock(this->cb_mtx_);
    this->frame_cb_ = std::move(cb);
    return OperationResult::OPERATION_OK;
}

OperationResult AsiCamera::startFrameAcquisition(std::chrono::milliseconds timeout)
{
    {
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        const OperationResult res = this->checkConnectedLocked();
        if (res != OperationResult::OPERATION_OK)
            return res;

        // Requires the stream to be running already. Starting it here instead would give the acquisition state machine
        // a second owner and let a worker silently reconfigure the hardware.
        if (this->acq_mode_ != AcquisitionMode::VIDEO)
            return OperationResult::INVALID_SEQUENCE;
    }

    // Clamped to the same ceiling the adapter already enforces on the vendor wait, and floored so a zero or negative
    // timeout cannot turn every capture into an instant failure. Clamping first also keeps the join bound below from
    // overflowing on an absurd input such as milliseconds::max(), which would make the very first stop() give up and
    // detach a perfectly healthy worker.
    const std::chrono::milliseconds capped =
        std::min(std::max(timeout, std::chrono::milliseconds(1)), std::chrono::milliseconds(kMaxCaptureWaitMs));

    // The join bound is deliberately longer than the capture timeout, so a HEALTHY worker is always joined and
    // detaching stays reserved for genuinely stuck hardware.
    const std::chrono::milliseconds join_timeout = capped + std::chrono::milliseconds(1000);

    const OperationResult started = this->pump_.start(
        [this, capped](Frame& frame){ return this->doGetVideoFrame(frame, capped); },
        [this](OperationResult res, const Frame& frame)
        {
            // Copy the callback out under the lock, then invoke it OUTSIDE: a callback must never run holding a lock,
            // and it is free to destroy this object. The frame passes through BY REFERENCE, so nothing is copied.
            NewFrameCb cb;
            {
                const std::lock_guard<std::mutex> lock(this->cb_mtx_);
                cb = this->frame_cb_;
            }
            if (cb)
                cb(res, frame);
        },
        join_timeout);

    if (started != OperationResult::OPERATION_OK)
        return started;

    // The precondition is re-checked now that the worker EXISTS. Between the check above and pump_.start() the lock was
    // released, so a concurrent doDisconnect could have run to completion: its pump_.stop() would have found nothing to
    // stop, and the worker created afterwards would survive it, spinning against a closed camera. State_mtx_ cannot
    // simply be held across pump_.start(), because stop() waits on a worker that itself needs state_mtx_ -- that would
    // be a real lock cycle. Re-checking and unwinding is the safe shape.
    bool still_valid = false;
    {
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        still_valid = this->connected_ && this->acq_mode_ == AcquisitionMode::VIDEO;
    }
    if (!still_valid)
    {
        this->pump_.stop();
        return OperationResult::NOT_CONNECTED;
    }
    return OperationResult::OPERATION_OK;
}

OperationResult AsiCamera::stopFrameAcquisition()
{
    return this->pump_.stop();
}

bool AsiCamera::isFrameAcquisitionRunning() const
{
    return this->pump_.isRunning();
}

std::uint64_t AsiCamera::getAcquiredFrameCount() const
{
    return this->pump_.deliveredCount();
}

std::uint64_t AsiCamera::getFailedFrameCount() const
{
    return this->pump_.failedCount();
}

OperationResult AsiCamera::setNewStatusCb(NewStatusCb cb)
{
    const std::lock_guard<std::mutex> lock(this->cb_mtx_);
    this->cb_ = std::move(cb);
    return OperationResult::OPERATION_OK;
}

OperationResult AsiCamera::startStatusPolling()
{
    {
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        const OperationResult res = this->checkConnectedLocked();
        if (res != OperationResult::OPERATION_OK)
            return res;
    }

    int rate_ms = 0;
    {
        const std::lock_guard<std::mutex> lock(this->state_mtx_);
        rate_ms = this->telemetry_rate_ms_;   // As supplied to doConnect().
    }

    return this->poller_.start(
        [this](CameraStatus& status){ return this->fillStatus(status); },
        [this](OperationResult res, const CameraStatus& status)
        {
            // Copy the callback out under the lock, then invoke it OUTSIDE: a callback must never run holding a lock,
            // and it is free to destroy this object.
            NewStatusCb cb;
            {
                const std::lock_guard<std::mutex> lock(this->cb_mtx_);
                cb = this->cb_;
            }
            if (cb)
                cb(res, status);
        },
        std::chrono::milliseconds(rate_ms));
}

OperationResult AsiCamera::stopStatusPolling()
{
    return this->poller_.stop();
}

bool AsiCamera::isStatusPollingRunning() const
{
    return this->poller_.isRunning();
}

// -- Discovery --------------------------------------------------------------------------------------------------------

OperationResult AsiCamera::getDeviceList(CameraDescriptorList& list)
{
    return asi::enumerateCameras(list);
}

OperationResult AsiCamera::getDeviceList(const std::string& model, CameraDescriptorList& list)
{
    return asi::enumerateByModel(model, list);
}

// -- Internal guards --------------------------------------------------------------------------------------------------

OperationResult AsiCamera::checkConnectedLocked() const
{
    return this->connected_ ? OperationResult::OPERATION_OK : OperationResult::NOT_CONNECTED;
}

OperationResult AsiCamera::checkIdleLocked() const
{
    return (this->acq_mode_ == AcquisitionMode::IDLE) ? OperationResult::OPERATION_OK
                                                      : OperationResult::INVALID_SEQUENCE;
}

// ---------------------------------------------------------------------------------------------------------------------

} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
