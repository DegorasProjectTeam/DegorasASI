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
// ASSERTIONS MUST BE LIVE IN EVERY BUILD TYPE.
//
// This suite is assert-based and has no framework, so assert() IS the test. Release defines NDEBUG, which makes
// assert() expand to nothing and discards the WHOLE expression -- including any call inside it. Measured on this
// suite before the fix: objdump found zero references to _assert in all eleven Release objects, so the entire
// assertion layer was dead, and 610 of the 716 assert sites wrap a call whose side effect went with it. The visible
// symptoms were a suite that passed while doing nothing (Test_AsiCameraCapture reported "serial: (empty), 0
// controls, sensor 0x0" and still declared success, because assert(camera.doConnect() == ...) never opened the
// camera) and two tests that hung outright (UT_FramePump, Test_FrameCallback: assert(pump.start(...)) never started
// the pump, so a later loop waited on a counter that could not advance).
//
// BOTH LINES ARE REQUIRED, in this order, above the first #include. The bare #undef is NOT enough: if anything has
// already pulled in <cassert> while NDEBUG was defined, assert is already expanded away and stays dead. Re-including
// the header re-arms it, because assert.h does #undef assert and redefines the macro on every inclusion and
// <cassert> deliberately has no include guard. Verified by compiling both forms with -O3 -DNDEBUG: the two-line
// form fires, the bare #undef placed after an earlier <cassert> produces zero _assert references.
#undef NDEBUG
#include <cassert>


// C++ INCLUDES
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>

// PROJECT INCLUDES (module aggregators)
#include <LibDegorasASI/Modules/Common>
#include <LibDegorasASI/Modules/ASI>
#include <LibDegorasASI/Modules/Devices>


using namespace dpasi;
using namespace dpasi::types;

/// The camera model these checks are written against. One class drives every ASI model, so this is only the model
/// the run is VALIDATED on; point it at another model and the same code exercises that camera unchanged.
constexpr const char* kModel = "ASI224MC";

// ---------------------------------------------------------------------------------------------------------------------
// No-hardware self-check for the device layer's guards. Because a constructor performs NO device I/O, a device object
// can be built and exercised with no camera attached: every operation must refuse with NOT_CONNECTED rather than reach
// for hardware that is not there. This also covers the status type's JSON round-trip and the destructor's safety on a
// never-connected object.
// ---------------------------------------------------------------------------------------------------------------------

namespace
{

void testConstructionDoesNoDeviceIo()
{
    // Constructing with an identifier that certainly does not exist must not fail, block or touch hardware.
    AsiCamera camera(CameraId{9999});

    assert(camera.getCameraId() == CameraId{9999});
    assert(camera.getExpectedModel().empty());   // no required model -> any ASI camera is accepted
    assert(!camera.isConnected());
    assert(camera.getAcquisitionMode() == AcquisitionMode::IDLE);
    assert(!camera.isStatusPollingRunning());
}

void testEveryOperationRefusesWhenNotConnected()
{
    AsiCamera camera(CameraId{9999});

    // Identity and capability reads.
    CameraDescriptor descriptor;
    assert(camera.getDescriptor(descriptor) == OperationResult::NOT_CONNECTED);
    CameraSN serial;
    assert(camera.getSerialNumber(serial) == OperationResult::NOT_CONNECTED);
    ControlCapsList caps;
    assert(camera.getControlCaps(caps) == OperationResult::NOT_CONNECTED);
    ControlCaps one_cap;
    assert(camera.getControlCaps(ControlType::GAIN, one_cap) == OperationResult::NOT_CONNECTED);

    // Controls.
    ControlValue value;
    assert(camera.getControl(ControlType::GAIN, value) == OperationResult::NOT_CONNECTED);
    assert(camera.doSetControl(ControlType::GAIN, value) == OperationResult::NOT_CONNECTED);
    assert(camera.doSetExposure(std::chrono::milliseconds(10)) == OperationResult::NOT_CONNECTED);
    std::chrono::microseconds exposure{0};
    assert(camera.getExposure(exposure) == OperationResult::NOT_CONNECTED);
    double celsius = 0;
    assert(camera.getSensorTemperature(celsius) == OperationResult::NOT_CONNECTED);
    assert(camera.doSetFlip(FlipMode::HORIZONTAL) == OperationResult::NOT_CONNECTED);
    FlipMode flip = FlipMode::NONE;
    assert(camera.getFlip(flip) == OperationResult::NOT_CONNECTED);

    // Region of interest.
    RoiFormat roi;
    roi.width = 640; roi.height = 480; roi.bin = 1; roi.format = ImageFormat::RAW8;
    assert(camera.getRoiFormat(roi) == OperationResult::NOT_CONNECTED);
    assert(camera.doSetRoi(roi, RoiPosition()) == OperationResult::NOT_CONNECTED);
    RoiPosition pos;
    assert(camera.getRoiPosition(pos) == OperationResult::NOT_CONNECTED);
    assert(camera.doSetRoiPosition(pos) == OperationResult::NOT_CONNECTED);
    assert(camera.doSetFullFrameRoi(ImageFormat::RAW8, 1) == OperationResult::NOT_CONNECTED);

    // Acquisition, both workflows.
    assert(camera.doStartVideoCapture() == OperationResult::NOT_CONNECTED);
    assert(camera.doStopVideoCapture() == OperationResult::NOT_CONNECTED);
    Frame frame;
    assert(camera.doGetVideoFrame(frame, std::chrono::milliseconds(10)) == OperationResult::NOT_CONNECTED);
    int dropped = 0;
    assert(camera.getDroppedFrames(dropped) == OperationResult::NOT_CONNECTED);
    assert(camera.doStartExposure() == OperationResult::NOT_CONNECTED);
    assert(camera.doStopExposure() == OperationResult::NOT_CONNECTED);
    ExposureState state = ExposureState::IDLE;
    assert(camera.getExposureState(state) == OperationResult::NOT_CONNECTED);
    assert(camera.doGetExposureFrame(frame) == OperationResult::NOT_CONNECTED);
    assert(camera.doCaptureSingleFrame(frame, std::chrono::milliseconds(50)) == OperationResult::NOT_CONNECTED);

    // Nothing above may have produced image data.
    assert(frame.empty());

    // Telemetry, and disconnecting something that was never connected.
    CameraStatus status;
    assert(camera.getDeviceStatus(status) == OperationResult::NOT_CONNECTED);
    assert(camera.startStatusPolling() == OperationResult::NOT_CONNECTED);
    assert(camera.stopStatusPolling() == OperationResult::WORKER_NOT_RUNNING);
    assert(camera.doDisconnect() == OperationResult::NOT_CONNECTED);
}

void testConnectFailsCleanlyForAnAbsentCamera()
{
    AsiCamera camera(CameraId{9999});

    // The camera is validated as present AND of the right model before anything is opened, so an absent identifier is
    // reported rather than acted on.
    assert(camera.doConnect() == OperationResult::DEVICE_NOT_FOUND);
    assert(!camera.isConnected());

    // A failed connect must leave no ownership claim behind, or the camera would be permanently unusable.
    assert(!asi::isCameraClaimed(CameraId{9999}));

    // And it must be retryable.
    assert(camera.doConnect() == OperationResult::DEVICE_NOT_FOUND);
    assert(!asi::isCameraClaimed(CameraId{9999}));
}

void testModelRequirementIsOptional()
{
    // One class drives every model, so requiring one is a CHOICE the caller makes, not something baked into a type.
    const AsiCamera any(CameraId{9999});
    assert(any.getExpectedModel().empty());

    const AsiCamera specific(CameraId{9999}, kModel);
    assert(specific.getExpectedModel() == std::string(kModel));

    // The matching itself is a pure name check that contacts no camera, so a discovery result can be screened before
    // any connection is attempted.
    assert(asi::nameMatchesModel("ZWO ASI224MC", kModel));
    assert(!asi::nameMatchesModel("ZWO ASI183MM Pro", kModel));
    assert(!asi::nameMatchesModel("", kModel));

    // A required model that the camera at that identifier does not report is refused at connect, not at construction.
    AsiCamera mismatched(CameraId{9999}, "ASI2600MM");
    assert(mismatched.doConnect() == OperationResult::DEVICE_NOT_FOUND);
    assert(!mismatched.isConnected());
}

void testStatusJsonRoundTrip()
{
    CameraStatus status;
    status.id = CameraId{0};
    status.connected = true;
    status.acq_mode = AcquisitionMode::VIDEO;
    status.exposure_state = ExposureState::SUCCESS;
    status.temperature_valid = true;
    status.temperature_c = 23.5;
    status.dropped_frames = 7;
    status.roi.width = 640;
    status.roi.height = 480;
    status.roi.bin = 2;
    status.roi.format = ImageFormat::RAW16;

    const CameraStatus rt = CameraStatus::fromJsonStr(status.toJsonStr());
    assert(rt.id == status.id && rt.connected == status.connected && rt.acq_mode == status.acq_mode);
    assert(rt.exposure_state == status.exposure_state);
    assert(rt.temperature_valid && rt.temperature_c > 23.4 && rt.temperature_c < 23.6);
    assert(rt.dropped_frames == status.dropped_frames);
    assert(rt.roi.width == 640 && rt.roi.height == 480 && rt.roi.bin == 2 && rt.roi.format == ImageFormat::RAW16);

    // Via the pretty form too, and the nested ROI object must survive the re-indentation.
    const CameraStatus rtp = CameraStatus::fromJsonStr(status.toJsonStr(true));
    assert(rtp.acq_mode == AcquisitionMode::VIDEO && rtp.roi.width == 640 && rtp.roi.format == ImageFormat::RAW16);

    // A default status must not claim a valid temperature: an unread field is flagged, never defaulted to a
    // plausible-looking zero.
    const CameraStatus fresh;
    assert(!fresh.connected && !fresh.temperature_valid && fresh.acq_mode == AcquisitionMode::IDLE);
}

void testDestructionIsSafeWithoutConnecting()
{
    // A never-connected camera must tear down cleanly, both by value and when held by pointer.
    {
        AsiCamera camera(CameraId{9999});
        (void)camera;
    }
    {
        const AsiCameraPtr device = std::make_shared<AsiCamera>(CameraId{9999});
        assert(!device->isConnected());
        assert(device->doDisconnect() == OperationResult::NOT_CONNECTED);
    }
    assert(!asi::isCameraClaimed(CameraId{9999}));
}

} // namespace

int main()
{
    testConstructionDoesNoDeviceIo();
    testEveryOperationRefusesWhenNotConnected();
    testConnectFailsCleanlyForAnAbsentCamera();
    testModelRequirementIsOptional();
    testStatusJsonRoundTrip();
    testDestructionIsSafeWithoutConnecting();

    std::cout << "UT_DeviceGuards: ALL CHECKS PASSED" << std::endl;
    return 0;
}
