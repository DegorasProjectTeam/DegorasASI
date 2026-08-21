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
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

// PROJECT INCLUDES (module aggregators)
#include <LibDegorasASI/Modules/ASI>
#include <LibDegorasASI/Modules/Devices>


using namespace dpasi;
using namespace dpasi::types;

/// The camera model these checks are written against. One class drives every ASI model, so this is only the model
/// the run is VALIDATED on; point it at another model and the same code exercises that camera unchanged.
constexpr const char* kModel = "ASI224MC";

// ---------------------------------------------------------------------------------------------------------------------
// Integration test against a real camera: the full connection lifecycle, both acquisition workflows, and every guard
// the library adds on top of the SDK. Takes an optional camera id and otherwise uses the first discovered camera.
//
// SELF-SKIPS with success when no camera is attached, so it is safe to register with CTest on a machine without one.
//
// There is no ZWO camera simulator, so unlike the reference library's simulator-backed tests this one needs real
// hardware. That is why the hardware-free guards are covered separately by UT_DeviceGuards, and the pure arithmetic by
// UT_ImageGeometry: those two carry the automated coverage when no camera is present.
// ---------------------------------------------------------------------------------------------------------------------

namespace
{

void testLifecycleAndOwnership(AsiCamera& camera, CameraId id)
{
    std::cout << "  lifecycle & ownership\n";

    assert(!camera.isConnected());
    assert(camera.doConnect() == OperationResult::OPERATION_OK);
    assert(camera.isConnected());
    assert(camera.getAcquisitionMode() == AcquisitionMode::IDLE);

    // The owning object's reconnect is idempotent.
    assert(camera.doConnect() == OperationResult::ALREADY_CONNECTED);
    assert(camera.isConnected());

    // A second object is refused. The SDK does NOT protect against this -- opening an already-open camera returns
    // success -- so this asserts the library's own ownership guarantee.
    {
        AsiCamera rival(id);
        assert(rival.doConnect() == OperationResult::CAMERA_IN_USE);
        assert(!rival.isConnected());
        // The refused object must not have taken the claim away on destruction.
    }
    assert(camera.isConnected());

    CameraSN serial;
    assert(camera.getSerialNumber(serial) == OperationResult::OPERATION_OK);
    assert(serial.size() == 16);   // 8 raw bytes rendered as hex; NOT a C string.
    std::cout << "    serial: " << serial << "\n";
}

void testCapabilitiesAreDiscoveredNotAssumed(AsiCamera& camera)
{
    std::cout << "  capability discovery\n";

    CameraDescriptor desc;
    assert(camera.getDescriptor(desc) == OperationResult::OPERATION_OK);
    assert(desc.max_width > 0 && desc.max_height > 0);
    assert(!desc.supported_formats.empty() && !desc.supported_bins.empty());
    assert(desc.supportsBin(1));
    assert(desc.supportsFormat(ImageFormat::RAW8));

    ControlCapsList caps;
    assert(camera.getControlCaps(caps) == OperationResult::OPERATION_OK);
    assert(!caps.empty());
    std::cout << "    " << caps.size() << " controls, sensor " << desc.max_width << "x" << desc.max_height << "\n";

    // Exposure and gain must exist on any ASI camera.
    ControlCaps exposure_caps;
    assert(camera.getControlCaps(ControlType::EXPOSURE, exposure_caps) == OperationResult::OPERATION_OK);
    assert(exposure_caps.is_writable && exposure_caps.max_value > exposure_caps.min_value);
    ControlCaps gain_caps;
    assert(camera.getControlCaps(ControlType::GAIN, gain_caps) == OperationResult::OPERATION_OK);

    // The capability predicates must agree with the capabilities they summarise -- they are what a user interface
    // binds to when deciding what to offer, so a disagreement would show the user a control the camera lacks.
    assert(camera.hasControl(ControlType::EXPOSURE));
    assert(camera.hasControl(ControlType::GAIN));
    assert(!camera.hasControl(ControlType::GAMMA));
    assert(camera.isControlWritable(ControlType::GAIN) == gain_caps.is_writable);
    assert(camera.isControlAutoCapable(ControlType::EXPOSURE) == exposure_caps.is_auto_supported);
    assert(camera.isColour() == desc.is_colour);
    assert(camera.hasCooler() == desc.is_cooled);
    assert(camera.supportsFormat(ImageFormat::RAW8) == desc.supportsFormat(ImageFormat::RAW8));
    assert(camera.supportsBin(1) == desc.supportsBin(1));
    assert(!camera.hasControl(ControlType::GAMMA));

    // On a camera WITHOUT a cooler the whole cooler surface must refuse rather than answer.
    if (!camera.hasCooler())
    {
        int target = 0;
        bool on = true;
        assert(camera.getCoolerTarget(target) == OperationResult::UNSUPPORTED_CAPABILITY);
        assert(camera.isCoolerEnabled(on) == OperationResult::UNSUPPORTED_CAPABILITY);
    }

    // A control this camera does NOT have must be refused, not fabricated. The ASI224MC has no gamma control, which is
    // precisely why the control set is discovered rather than assumed.
    ControlCaps gamma_caps;
    const OperationResult gamma_res = camera.getControlCaps(ControlType::GAMMA, gamma_caps);
    assert(gamma_res == OperationResult::UNSUPPORTED_CAPABILITY);
    ControlValue gamma_value;
    assert(camera.getControl(ControlType::GAMMA, gamma_value) == OperationResult::UNSUPPORTED_CAPABILITY);
    assert(camera.doSetControl(ControlType::GAMMA, gamma_value) == OperationResult::UNSUPPORTED_CAPABILITY);

    // Temperature is present but read-only: writing it must be refused rather than silently dropped.
    ControlCaps temp_caps;
    if (camera.getControlCaps(ControlType::TEMPERATURE, temp_caps) == OperationResult::OPERATION_OK)
    {
        assert(!temp_caps.is_writable);
        ControlValue temp_write;
        temp_write.value = 0;
        assert(camera.doSetControl(ControlType::TEMPERATURE, temp_write) == OperationResult::UNSUPPORTED_CAPABILITY);

        double celsius = 0;
        assert(camera.getSensorTemperature(celsius) == OperationResult::OPERATION_OK);
        assert(celsius > -50.0 && celsius < 100.0);   // Unscaled from the SDK's tenths of a degree.
        std::cout << "    sensor temperature: " << celsius << " C\n";
    }
}

void testControlRangeIsEnforced(AsiCamera& camera)
{
    std::cout << "  control range enforcement\n";

    ControlCaps gain_caps;
    assert(camera.getControlCaps(ControlType::GAIN, gain_caps) == OperationResult::OPERATION_OK);

    // In range: accepted and actually applied.
    ControlValue gain;
    gain.value = (gain_caps.min_value + gain_caps.max_value) / 2;
    gain.is_auto = false;
    assert(camera.doSetControl(ControlType::GAIN, gain) == OperationResult::OPERATION_OK);

    ControlValue read_back;
    assert(camera.getControl(ControlType::GAIN, read_back) == OperationResult::OPERATION_OK);
    assert(read_back.value == gain.value);

    // Out of range: refused BEFORE reaching the SDK. This matters because the SDK silently clamps an out-of-range value
    // and reports success, which would leave a caller believing it applied a setting the camera never took.
    ControlValue too_big;
    too_big.value = gain_caps.max_value + 1000;
    assert(camera.doSetControl(ControlType::GAIN, too_big) == OperationResult::INVALID_PARAMETER);
    ControlValue too_small;
    too_small.value = gain_caps.min_value - 1000;
    assert(camera.doSetControl(ControlType::GAIN, too_small) == OperationResult::INVALID_PARAMETER);

    // The refused writes must not have changed anything.
    assert(camera.getControl(ControlType::GAIN, read_back) == OperationResult::OPERATION_OK);
    assert(read_back.value == gain.value);

    // Typed exposure convenience: the microsecond conversion happens in one place.
    assert(camera.doSetExposure(std::chrono::milliseconds(20)) == OperationResult::OPERATION_OK);
    std::chrono::microseconds exposure{0};
    assert(camera.getExposure(exposure) == OperationResult::OPERATION_OK);
    assert(exposure == std::chrono::microseconds(20000));
}

void testUsbTrafficSurface(AsiCamera& camera)
{
    std::cout << "  USB link and traffic\n";

    // The link speed must agree with the descriptor: it is derived from it, and a GUI shows both.
    CameraDescriptor desc;
    assert(camera.getDescriptor(desc) == OperationResult::OPERATION_OK);
    assert(camera.getUsbLinkSpeed() == desc.linkSpeed());
    assert(camera.isLinkFullSpeed() == desc.isLinkFullSpeed());
    std::cout << "    link: " << toString(camera.getUsbLinkSpeed())
              << " (camera usb3=" << desc.is_usb3_camera << ", host usb3=" << desc.is_usb3_host
              << ", full speed=" << camera.isLinkFullSpeed() << ")\n";

    // Bandwidth is the lever over dropped frames. Its bounds come from the control capabilities, exactly as a slider
    // in a capture application would read them.
    ControlCaps caps;
    if (camera.getControlCaps(ControlType::BANDWIDTH_OVERLOAD, caps) != OperationResult::OPERATION_OK)
    {
        std::cout << "    camera has no bandwidth control; skipping\n";
        return;
    }
    std::cout << "    bandwidth range [" << caps.min_value << ", " << caps.max_value
              << "], default " << caps.default_value << ", auto-capable " << caps.is_auto_supported << "\n";

    const int mid = static_cast<int>((caps.min_value + caps.max_value) / 2);
    assert(camera.doSetUsbBandwidth(mid) == OperationResult::OPERATION_OK);

    int percent = 0;
    bool automatic = true;
    assert(camera.getUsbBandwidth(percent, automatic) == OperationResult::OPERATION_OK);
    assert(percent == mid);
    assert(!automatic);

    // The Auto box the vendor software offers, when the camera supports it.
    if (caps.is_auto_supported)
    {
        assert(camera.doSetUsbBandwidth(mid, true) == OperationResult::OPERATION_OK);
        assert(camera.getUsbBandwidth(percent, automatic) == OperationResult::OPERATION_OK);
        assert(automatic);
        assert(camera.doSetUsbBandwidth(mid, false) == OperationResult::OPERATION_OK);
    }

    // Out of range is refused before the SDK can silently clamp it.
    assert(camera.doSetUsbBandwidth(static_cast<int>(caps.max_value) + 50) == OperationResult::INVALID_PARAMETER);
    assert(camera.doSetUsbBandwidth(static_cast<int>(caps.min_value) - 50) == OperationResult::INVALID_PARAMETER);

    // High-speed readout: a different lever (bit depth for frame rate), present on this camera.
    if (camera.hasControl(ControlType::HIGH_SPEED_MODE))
    {
        bool high = true;
        assert(camera.doSetHighSpeedMode(false) == OperationResult::OPERATION_OK);
        assert(camera.isHighSpeedMode(high) == OperationResult::OPERATION_OK);
        assert(!high);
        assert(camera.doSetHighSpeedMode(true) == OperationResult::OPERATION_OK);
        assert(camera.isHighSpeedMode(high) == OperationResult::OPERATION_OK);
        assert(high);
        assert(camera.doSetHighSpeedMode(false) == OperationResult::OPERATION_OK);
    }

    // An uncooled camera must refuse the cooler surface rather than pretend.
    if (!camera.hasCooler())
    {
        assert(camera.doSetCoolerTarget(-10) == OperationResult::UNSUPPORTED_CAPABILITY);
        assert(camera.doEnableCooler(true) == OperationResult::UNSUPPORTED_CAPABILITY);
        int power = 0;
        assert(camera.getCoolerPower(power) == OperationResult::UNSUPPORTED_CAPABILITY);
    }
}

void testRoiConfiguration(AsiCamera& camera)
{
    std::cout << "  ROI configuration\n";

    RoiFormat roi;
    roi.width = 640;
    roi.height = 480;
    roi.bin = 1;
    roi.format = ImageFormat::RAW8;
    assert(camera.doSetRoi(roi, RoiPosition()) == OperationResult::OPERATION_OK);

    RoiFormat live;
    assert(camera.getRoiFormat(live) == OperationResult::OPERATION_OK);
    assert(live.width == 640 && live.height == 480 && live.bin == 1 && live.format == ImageFormat::RAW8);

    // Setting the format RECENTRES the origin on the sensor, so the library re-applies the requested origin afterwards.
    // Without that, the origin would silently be wherever the SDK moved it.
    RoiPosition pos;
    assert(camera.getRoiPosition(pos) == OperationResult::OPERATION_OK);
    assert(pos.start_x == 0 && pos.start_y == 0);

    // Misaligned geometry is refused by the library before the SDK sees it, and the live ROI is left untouched.
    RoiFormat bad_width = roi;
    bad_width.width = 641;
    assert(camera.doSetRoi(bad_width, RoiPosition()) == OperationResult::INVALID_PARAMETER);
    RoiFormat bad_height = roi;
    bad_height.height = 481;
    assert(camera.doSetRoi(bad_height, RoiPosition()) == OperationResult::INVALID_PARAMETER);
    RoiFormat bad_multiple = roi;
    bad_multiple.width = 644;   // a multiple of 4 but not of 8
    assert(camera.doSetRoi(bad_multiple, RoiPosition()) == OperationResult::INVALID_PARAMETER);

    assert(camera.getRoiFormat(live) == OperationResult::OPERATION_OK);
    assert(live.width == 640 && live.height == 480);

    // A binning factor or format this camera does not offer is refused as an unsupported capability.
    RoiFormat bad_bin = roi;
    bad_bin.bin = 7;
    assert(camera.doSetRoi(bad_bin, RoiPosition()) == OperationResult::UNSUPPORTED_CAPABILITY);

    // Geometry larger than the binned sensor is refused as a bad parameter.
    CameraDescriptor desc;
    assert(camera.getDescriptor(desc) == OperationResult::OPERATION_OK);
    RoiFormat too_big = roi;
    too_big.width = (desc.max_width / 8 * 8) + 8;
    assert(camera.doSetRoi(too_big, RoiPosition()) == OperationResult::INVALID_PARAMETER);

    // Full-frame convenience, then back to a small ROI for the acquisition tests.
    assert(camera.doSetFullFrameRoi(ImageFormat::RAW8, 1) == OperationResult::OPERATION_OK);
    assert(camera.getRoiFormat(live) == OperationResult::OPERATION_OK);
    assert(live.width == desc.max_width / 8 * 8 && live.height == desc.max_height / 2 * 2);
    std::cout << "    full frame: " << live.width << "x" << live.height
              << " (" << frameBufferSize(live) << " bytes)\n";

    if (desc.supportsBin(2))
    {
        assert(camera.doSetFullFrameRoi(ImageFormat::RAW8, 2) == OperationResult::OPERATION_OK);
        assert(camera.getRoiFormat(live) == OperationResult::OPERATION_OK);
        assert(live.bin == 2 && live.width <= desc.max_width / 2);
    }

    assert(camera.doSetRoi(roi, RoiPosition()) == OperationResult::OPERATION_OK);
}

void testSnapshotWorkflow(AsiCamera& camera)
{
    std::cout << "  snapshot workflow\n";

    assert(camera.doSetExposure(std::chrono::milliseconds(20)) == OperationResult::OPERATION_OK);

    // The one-call convenience, which always returns the camera to idle.
    Frame frame;
    assert(camera.doCaptureSingleFrame(frame, std::chrono::seconds(5)) == OperationResult::OPERATION_OK);
    assert(!frame.empty());
    assert(frame.data.size() == frame.expectedBytes());
    assert(frame.width == 640 && frame.height == 480 && frame.format == ImageFormat::RAW8);
    assert(frame.sequence > 0);
    assert(frame.timestamp.time_since_epoch().count() > 0);
    assert(camera.getAcquisitionMode() == AcquisitionMode::IDLE);
    std::cout << "    captured " << frame.data.size() << " bytes\n";

    // The explicit four-step sequence, and the state tracking around it.
    assert(camera.doStartExposure() == OperationResult::OPERATION_OK);
    assert(camera.getAcquisitionMode() == AcquisitionMode::SNAPSHOT);

    // Configuration and the other workflow are both illegal now.
    RoiFormat roi;
    roi.width = 320; roi.height = 240; roi.bin = 1; roi.format = ImageFormat::RAW8;
    assert(camera.doSetRoi(roi, RoiPosition()) == OperationResult::INVALID_SEQUENCE);
    assert(camera.doStartVideoCapture() == OperationResult::INVALID_SEQUENCE);
    assert(camera.doStartExposure() == OperationResult::INVALID_SEQUENCE);

    ExposureState state = ExposureState::IDLE;
    assert(camera.getExposureState(state) == OperationResult::OPERATION_OK);

    assert(camera.doStopExposure() == OperationResult::OPERATION_OK);
    assert(camera.getAcquisitionMode() == AcquisitionMode::IDLE);
    assert(camera.doStopExposure() == OperationResult::INVALID_SEQUENCE);

    // Downloading with no exposure in progress is refused rather than yielding a stale or empty frame.
    Frame stale;
    assert(camera.doGetExposureFrame(stale) == OperationResult::INVALID_SEQUENCE);
    assert(stale.empty());
}

void testVideoWorkflow(AsiCamera& camera)
{
    std::cout << "  video workflow\n";

    assert(camera.doSetExposure(std::chrono::milliseconds(20)) == OperationResult::OPERATION_OK);

    // Retrieving before starting is refused.
    Frame frame;
    assert(camera.doGetVideoFrame(frame, std::chrono::milliseconds(100)) == OperationResult::INVALID_SEQUENCE);
    assert(camera.doStopVideoCapture() == OperationResult::INVALID_SEQUENCE);

    assert(camera.doStartVideoCapture() == OperationResult::OPERATION_OK);
    assert(camera.getAcquisitionMode() == AcquisitionMode::VIDEO);
    assert(camera.doStartVideoCapture() == OperationResult::OPERATION_OK);   // idempotent while already streaming

    // THE critical guard. The SDK ACCEPTS a ROI/format change while streaming and silently changes the frame geometry a
    // caller has already sized a buffer for; the library refuses it instead.
    RoiFormat smaller;
    smaller.width = 320; smaller.height = 240; smaller.bin = 1; smaller.format = ImageFormat::RAW8;
    assert(camera.doSetRoi(smaller, RoiPosition()) == OperationResult::INVALID_SEQUENCE);
    assert(camera.doSetFullFrameRoi(ImageFormat::RAW8, 1) == OperationResult::INVALID_SEQUENCE);
    assert(camera.doStartExposure() == OperationResult::INVALID_SEQUENCE);

    // The geometry must be exactly what it was before those refusals.
    RoiFormat live;
    assert(camera.getRoiFormat(live) == OperationResult::OPERATION_OK);
    assert(live.width == 640 && live.height == 480);

    // Moving the ROI origin IS legal while streaming: it does not change the frame geometry.
    assert(camera.doSetRoiPosition(RoiPosition()) == OperationResult::OPERATION_OK);

    // Stream a burst, reusing one frame object. Its buffer must be reallocated only once.
    int received = 0;
    const void* first_buffer = nullptr;
    std::uint64_t last_sequence = 0;
    constexpr int kWanted = 20;
    for (int i = 0; i < kWanted; ++i)
    {
        const OperationResult res = camera.doGetVideoFrame(frame, std::chrono::milliseconds(2000));
        if (res != OperationResult::OPERATION_OK)
            continue;

        ++received;
        assert(frame.data.size() == frame.expectedBytes());
        assert(frame.width == 640 && frame.height == 480 && frame.format == ImageFormat::RAW8);
        assert(frame.sequence > last_sequence);   // monotonic
        last_sequence = frame.sequence;

        if (first_buffer == nullptr)
            first_buffer = frame.data.data();
        else
            assert(frame.data.data() == first_buffer);   // no per-frame reallocation in a steady stream
    }
    assert(received > 0);

    // Sampled BEFORE stopping: the counter's reset point is documented inconsistently, so it is a per-session figure.
    int dropped = 0;
    assert(camera.getDroppedFrames(dropped) == OperationResult::OPERATION_OK);
    std::cout << "    received " << received << "/" << kWanted << " frames, dropped " << dropped << "\n";

    assert(camera.doStopVideoCapture() == OperationResult::OPERATION_OK);
    assert(camera.getAcquisitionMode() == AcquisitionMode::IDLE);

    // Configuration is allowed again now that the camera is idle.
    assert(camera.doSetRoi(smaller, RoiPosition()) == OperationResult::OPERATION_OK);
    assert(camera.getRoiFormat(live) == OperationResult::OPERATION_OK);
    assert(live.width == 320 && live.height == 240);
}

void testGeometryChangeResizesTheFrame(AsiCamera& camera)
{
    std::cout << "  frame resizes with the geometry\n";

    RoiFormat small_roi;
    small_roi.width = 320; small_roi.height = 240; small_roi.bin = 1; small_roi.format = ImageFormat::RAW8;
    assert(camera.doSetRoi(small_roi, RoiPosition()) == OperationResult::OPERATION_OK);
    assert(camera.doSetExposure(std::chrono::milliseconds(10)) == OperationResult::OPERATION_OK);

    Frame frame;
    assert(camera.doCaptureSingleFrame(frame, std::chrono::seconds(5)) == OperationResult::OPERATION_OK);
    assert(frame.data.size() == 320u * 240u);

    // A 16-bit format doubles the required size; the same frame object must grow to match rather than under-run.
    CameraDescriptor desc;
    assert(camera.getDescriptor(desc) == OperationResult::OPERATION_OK);
    if (desc.supportsFormat(ImageFormat::RAW16))
    {
        RoiFormat deep = small_roi;
        deep.format = ImageFormat::RAW16;
        assert(camera.doSetRoi(deep, RoiPosition()) == OperationResult::OPERATION_OK);
        assert(camera.doCaptureSingleFrame(frame, std::chrono::seconds(5)) == OperationResult::OPERATION_OK);
        assert(frame.data.size() == 320u * 240u * 2u);
        assert(frame.format == ImageFormat::RAW16);
    }

    if (desc.supportsFormat(ImageFormat::RGB24))
    {
        RoiFormat colour = small_roi;
        colour.format = ImageFormat::RGB24;
        assert(camera.doSetRoi(colour, RoiPosition()) == OperationResult::OPERATION_OK);
        assert(camera.doCaptureSingleFrame(frame, std::chrono::seconds(5)) == OperationResult::OPERATION_OK);
        assert(frame.data.size() == 320u * 240u * 3u);
        assert(frame.format == ImageFormat::RGB24);
    }
    std::cout << "    geometry changes tracked across RAW8 / RAW16 / RGB24\n";
}

void testTelemetryPolling(AsiCamera& camera)
{
    std::cout << "  telemetry polling\n";

    int polls = 0;
    assert(camera.setNewStatusCb([&polls](OperationResult res, const CameraStatus& status)
    {
        if (res == OperationResult::OPERATION_OK && status.connected)
            ++polls;
    }) == OperationResult::OPERATION_OK);

    assert(!camera.isStatusPollingRunning());
    assert(camera.startStatusPolling() == OperationResult::OPERATION_OK);
    assert(camera.isStatusPollingRunning());
    assert(camera.startStatusPolling() == OperationResult::WORKER_ALREADY_RUNNING);

    std::this_thread::sleep_for(std::chrono::milliseconds(1600));

    assert(camera.stopStatusPolling() == OperationResult::OPERATION_OK);
    assert(!camera.isStatusPollingRunning());
    assert(camera.stopStatusPolling() == OperationResult::WORKER_NOT_RUNNING);
    assert(polls > 0);
    std::cout << "    " << polls << " telemetry callbacks delivered\n";

    CameraStatus status;
    assert(camera.getDeviceStatus(status) == OperationResult::OPERATION_OK);
    assert(status.connected && status.acq_mode == AcquisitionMode::IDLE);
    assert(status.roi.width > 0 && status.roi.height > 0);
}

void testTelemetryRateComesFromTheConfig(CameraId id)
{
    std::cout << "  telemetry rate honours DeviceConfig\n";

    // The poll interval must come from the DeviceConfig given to doConnect, not from the default. A 10x-faster rate is
    // asserted with a wide margin (expect ~10 polls in 1 s, assert more than 4) so the check is not timing-fragile,
    // while still failing outright if the default 500 ms interval were used instead (which would yield ~2).
    AsiCamera camera(id);
    DeviceConfig cfg;
    cfg.telemetry_rate_ms = 100;
    assert(camera.doConnect(cfg) == OperationResult::OPERATION_OK);

    std::atomic<int> polls{0};
    assert(camera.setNewStatusCb([&polls](OperationResult res, const CameraStatus&)
    {
        if (res == OperationResult::OPERATION_OK)
            ++polls;
    }) == OperationResult::OPERATION_OK);

    assert(camera.startStatusPolling() == OperationResult::OPERATION_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    assert(camera.stopStatusPolling() == OperationResult::OPERATION_OK);

    std::cout << "    " << polls.load() << " polls in 1 s at a 100 ms interval\n";
    assert(polls.load() > 4);

    assert(camera.doDisconnect() == OperationResult::OPERATION_OK);
}

void testDisconnectAndReconnect(AsiCamera& camera, CameraId id)
{
    std::cout << "  disconnect & reconnect\n";

    assert(camera.doDisconnect() == OperationResult::OPERATION_OK);
    assert(!camera.isConnected());
    assert(camera.doDisconnect() == OperationResult::NOT_CONNECTED);

    // The claim must be released, so another object can now take the camera.
    assert(!asi::isCameraClaimed(id));
    {
        AsiCamera successor(id);
        assert(successor.doConnect() == OperationResult::OPERATION_OK);
        assert(successor.doDisconnect() == OperationResult::OPERATION_OK);
    }

    // And the original object can reconnect.
    assert(camera.doConnect() == OperationResult::OPERATION_OK);
    assert(camera.isConnected());
}

void testDestructorDisconnectsWhileStreaming(CameraId id)
{
    std::cout << "  destructor tears down a streaming camera\n";

    // The camera is deliberately left STREAMING with telemetry running. The destructor must stop the worker, wait out
    // any retrieval in flight, close the camera and release the claim -- without hanging.
    {
        AsiCamera camera(id);
        assert(camera.doConnect() == OperationResult::OPERATION_OK);
        assert(camera.doSetExposure(std::chrono::milliseconds(10)) == OperationResult::OPERATION_OK);
        assert(camera.startStatusPolling() == OperationResult::OPERATION_OK);
        assert(camera.doStartVideoCapture() == OperationResult::OPERATION_OK);

        Frame frame;
        camera.doGetVideoFrame(frame, std::chrono::milliseconds(1000));
        assert(camera.getAcquisitionMode() == AcquisitionMode::VIDEO);
        // No explicit disconnect: the destructor must do all of it.
    }

    assert(!asi::isCameraClaimed(id));

    // The camera must be fully usable afterwards, which proves the teardown really completed.
    AsiCamera after(id);
    assert(after.doConnect() == OperationResult::OPERATION_OK);
    assert(after.doDisconnect() == OperationResult::OPERATION_OK);
    std::cout << "    camera reusable after destructor teardown\n";
}

} // namespace

int main(int argc, char* argv[])
{
    std::cout << "Test_AsiCameraCapture (ZWO ASI SDK " << asi::getSdkVersion() << ")\n";

    CameraDescriptorList cameras;
    if (AsiCamera::getDeviceList(kModel, cameras) != OperationResult::OPERATION_OK || cameras.empty())
    {
        std::cout << "SKIPPED: no " << kModel << " attached." << std::endl;
        return 0;
    }

    CameraId id = cameras.front().id;
    if (argc > 1)
        id = static_cast<CameraId>(std::atoi(argv[1]));
    std::cout << "Using camera id " << toType(id) << "\n";

    AsiCamera camera(id);

    testLifecycleAndOwnership(camera, id);
    testCapabilitiesAreDiscoveredNotAssumed(camera);
    testControlRangeIsEnforced(camera);
    testUsbTrafficSurface(camera);
    testRoiConfiguration(camera);
    testSnapshotWorkflow(camera);
    testVideoWorkflow(camera);
    testGeometryChangeResizesTheFrame(camera);
    testTelemetryPolling(camera);
    testDisconnectAndReconnect(camera, id);

    assert(camera.doDisconnect() == OperationResult::OPERATION_OK);
    testTelemetryRateComesFromTheConfig(id);
    testDestructorDisconnectsWhileStreaming(id);

    std::cout << "Test_AsiCameraCapture: ALL CHECKS PASSED" << std::endl;
    return 0;
}
