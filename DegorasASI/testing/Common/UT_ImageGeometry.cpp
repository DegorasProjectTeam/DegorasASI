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
#include <iostream>

// PROJECT INCLUDES (module aggregators)
#include <DegorasASI/Modules/Common>


using namespace dpasi;
using namespace dpasi::types;

// ---------------------------------------------------------------------------------------------------------------------
// No-hardware self-check for the image-geometry arithmetic. This is the library's most safety-critical pure code: the
// vendor's frame-retrieval calls perform NO bounds check and are documented to crash on a short buffer, so every
// buffer in the library is sized from frameBufferSize() and every ROI is pre-validated by isRoiAligned().
// ---------------------------------------------------------------------------------------------------------------------

namespace
{

RoiFormat makeRoi(int w, int h, int bin, ImageFormat format)
{
    RoiFormat roi;
    roi.width = w; roi.height = h; roi.bin = bin; roi.format = format;
    return roi;
}

void testBytesPerPixel()
{
    assert(bytesPerPixel(ImageFormat::RAW8) == 1);
    assert(bytesPerPixel(ImageFormat::Y8) == 1);
    assert(bytesPerPixel(ImageFormat::RAW16) == 2);
    assert(bytesPerPixel(ImageFormat::RGB24) == 3);
}

void testFrameBufferSize()
{
    // The vendor's documented sizes: 8-bit mono w*h, 16-bit mono w*h*2, RGB24 w*h*3.
    assert(frameBufferSize(makeRoi(640, 480, 1, ImageFormat::RAW8))  == 640u * 480u);
    assert(frameBufferSize(makeRoi(640, 480, 1, ImageFormat::Y8))    == 640u * 480u);
    assert(frameBufferSize(makeRoi(640, 480, 1, ImageFormat::RAW16)) == 640u * 480u * 2u);
    assert(frameBufferSize(makeRoi(640, 480, 1, ImageFormat::RGB24)) == 640u * 480u * 3u);

    // Full-sensor AsiCamera geometry.
    assert(frameBufferSize(makeRoi(1304, 976, 1, ImageFormat::RAW16)) == 1304u * 976u * 2u);

    // Binning does not enter the size: the ROI dimensions are already post-binning, as the SDK defines them.
    assert(frameBufferSize(makeRoi(640, 480, 2, ImageFormat::RAW8)) ==
           frameBufferSize(makeRoi(640, 480, 1, ImageFormat::RAW8)));

    // A non-positive geometry yields zero rather than a wrapped-around size.
    assert(frameBufferSize(makeRoi(0, 480, 1, ImageFormat::RAW8)) == 0);
    assert(frameBufferSize(makeRoi(640, 0, 1, ImageFormat::RAW8)) == 0);
    assert(frameBufferSize(makeRoi(-8, -2, 1, ImageFormat::RAW8)) == 0);

    // The product must be computed in std::size_t, not int. A 32-bit multiply would wrap and UNDER-size the buffer,
    // which is precisely the condition the vendor documents as crashing its frame-retrieval calls. The geometry below
    // is synthetic (larger than any real ASI sensor) purely to push the product past both 2^31 and 2^32.
    const std::size_t big = frameBufferSize(makeRoi(40000, 40000, 1, ImageFormat::RGB24));
    assert(big == static_cast<std::size_t>(40000) * 40000u * 3u);
    assert(big > 0xFFFFFFFFull);   // 4.8e9: wraps in both signed and unsigned 32-bit arithmetic
}

void testRoiAlignment()
{
    // Verified against hardware: the SDK requires width % 8 == 0 and height % 2 == 0, and rejects anything else with
    // ASI_ERROR_INVALID_SIZE while leaving the previous ROI intact.
    assert(isRoiAligned(makeRoi(640, 480, 1, ImageFormat::RAW8)));
    assert(isRoiAligned(makeRoi(8, 2, 1, ImageFormat::RAW8)));
    assert(isRoiAligned(makeRoi(1304, 976, 1, ImageFormat::RAW8)));   // full AsiCamera sensor: 1304%8==0, 976%2==0

    assert(!isRoiAligned(makeRoi(641, 480, 1, ImageFormat::RAW8)));   // width not a multiple of 8
    assert(!isRoiAligned(makeRoi(644, 480, 1, ImageFormat::RAW8)));   // width a multiple of 4 but not 8
    assert(!isRoiAligned(makeRoi(100, 100, 1, ImageFormat::RAW8)));   // 100 % 8 == 4
    assert(!isRoiAligned(makeRoi(640, 481, 1, ImageFormat::RAW8)));   // height not a multiple of 2
    assert(!isRoiAligned(makeRoi(0, 480, 1, ImageFormat::RAW8)));     // non-positive width
    assert(!isRoiAligned(makeRoi(640, 480, 0, ImageFormat::RAW8)));   // bin must be at least 1
}

void testFrameExpectedBytes()
{
    Frame frame;
    frame.format = ImageFormat::RAW16;
    frame.width = 320; frame.height = 240; frame.bin = 2;
    assert(frame.expectedBytes() == 320u * 240u * 2u);
    assert(frame.empty());

    frame.data.assign(frame.expectedBytes(), 0);
    assert(!frame.empty());
    assert(frame.data.size() == frame.expectedBytes());

    const Frame fresh;
    assert(fresh.empty() && fresh.expectedBytes() == 0 && fresh.sequence == 0);
}

void testNumericAdapters()
{
    // Forward: library enum -> raw SDK ordinal.
    assert(toType(ImageFormat::RAW8) == 0);
    assert(toType(ImageFormat::RGB24) == 1);
    assert(toType(ImageFormat::RAW16) == 2);
    assert(toType(ImageFormat::Y8) == 3);
    assert(toType(ControlType::GAIN) == 0);
    assert(toType(ControlType::EXPOSURE) == 1);
    assert(toType(ControlType::TEMPERATURE) == 8);
    assert(toType(ControlType::MONO_BIN) == 18);
    assert(toType(ControlType::ROLLING_INTERVAL) == 28);
    assert(toType(FlipMode::BOTH) == 3);
    assert(toType(CameraId{7}) == 7);

    // Reverse: raw ordinal -> library enum, with validation. An unknown ordinal must be REJECTED, not cast blindly.
    ImageFormat format = ImageFormat::RGB24;
    assert(imageFormatFromType(0, format) && format == ImageFormat::RAW8);
    assert(imageFormatFromType(2, format) && format == ImageFormat::RAW16);
    assert(!imageFormatFromType(-1, format));    // the vendor's ASI_IMG_END sentinel is not a format
    assert(!imageFormatFromType(4, format));
    assert(format == ImageFormat::RAW16);        // out-parameter untouched on failure

    BayerPattern pattern = BayerPattern::GB;
    assert(bayerPatternFromType(0, pattern) && pattern == BayerPattern::RG);
    assert(!bayerPatternFromType(4, pattern) && pattern == BayerPattern::RG);

    ControlType type = ControlType::GAMMA;
    assert(controlTypeFromType(8, type) && type == ControlType::TEMPERATURE);
    assert(controlTypeFromType(28, type) && type == ControlType::ROLLING_INTERVAL);
    assert(!controlTypeFromType(29, type));      // beyond the range this library models
    assert(!controlTypeFromType(-1, type));
    assert(type == ControlType::ROLLING_INTERVAL);

    ExposureState state = ExposureState::IDLE;
    assert(exposureStateFromType(2, state) && state == ExposureState::SUCCESS);
    assert(exposureStateFromType(3, state) && state == ExposureState::FAILED);
    assert(!exposureStateFromType(4, state) && state == ExposureState::FAILED);
}

void testUsbLinkSpeed()
{
    // SuperSpeed needs BOTH ends. A USB3 camera in a USB2 port is the case that silently costs most of the frame rate,
    // so it must read as USB2 and as a degraded link, not as the camera's rated capability.
    CameraDescriptor d;

    d.is_usb3_camera = true;  d.is_usb3_host = true;
    assert(d.linkSpeed() == UsbLinkSpeed::USB3);
    assert(d.isLinkFullSpeed());

    d.is_usb3_camera = true;  d.is_usb3_host = false;
    assert(d.linkSpeed() == UsbLinkSpeed::USB2);
    assert(!d.isLinkFullSpeed());     // the degraded case

    d.is_usb3_camera = false; d.is_usb3_host = true;
    assert(d.linkSpeed() == UsbLinkSpeed::USB2);
    assert(d.isLinkFullSpeed());      // a USB2 camera is at full speed in any port

    d.is_usb3_camera = false; d.is_usb3_host = false;
    assert(d.linkSpeed() == UsbLinkSpeed::USB2);
    assert(d.isLinkFullSpeed());

    assert(toString(UsbLinkSpeed::USB2) == "USB2.0");
    assert(toString(UsbLinkSpeed::USB3) == "USB3.0");
    assert(toString(static_cast<UsbLinkSpeed>(9)) == "UNKNOWN_USB_LINK_SPEED");
}

void testStringification()
{
    assert(toString(OperationResult::OPERATION_OK) == "OPERATION_OK");
    assert(toString(OperationResult::BUFFER_TOO_SMALL) == "BUFFER_TOO_SMALL");
    assert(toString(OperationResult::INVALID_SEQUENCE) == "INVALID_SEQUENCE");
    assert(toString(static_cast<OperationResult>(200)) == "UNKNOWN_OPERATION_RESULT");

    assert(toString(ImageFormat::RAW16) == "RAW16");
    assert(toString(static_cast<ImageFormat>(9)) == "UNKNOWN_IMAGE_FORMAT");
    assert(toString(BayerPattern::GB) == "GB");
    assert(toString(ControlType::AUTO_MAX_EXPOSURE) == "AUTO_MAX_EXPOSURE");
    assert(toString(ExposureState::WORKING) == "WORKING");
    assert(toString(AcquisitionMode::SNAPSHOT) == "SNAPSHOT");

    // Every modelled control must stringify to something other than the fallback.
    for (int raw = 0; raw <= toType(ControlType::ROLLING_INTERVAL); ++raw)
    {
        ControlType type = ControlType::GAIN;
        assert(controlTypeFromType(raw, type));
        assert(toString(type) != "UNKNOWN_CONTROL_TYPE");
    }
}

void testDeviceError()
{
    // A DeviceError is self-contained: category + raw vendor code + context, with no stored last-error state.
    DeviceError ok;
    assert(ok.ok() && ok.category == OperationResult::OPERATION_OK && ok.asi_code == 0);

    DeviceError err;
    err.category = OperationResult::ZWO_INTERNAL_ERROR;
    err.asi_code = 4;   // ASI_ERROR_CAMERA_CLOSED
    err.context = "ASIGetControlValue(id=0, GAIN)";
    assert(!err.ok());

    const std::string text = err.toString();
    assert(text.find("ZWO_INTERNAL_ERROR") != std::string::npos);
    assert(text.find("asi=4") != std::string::npos);
    assert(text.find("ASIGetControlValue(id=0, GAIN)") != std::string::npos);
}

} // namespace

int main()
{
    testBytesPerPixel();
    testFrameBufferSize();
    testRoiAlignment();
    testFrameExpectedBytes();
    testNumericAdapters();
    testUsbLinkSpeed();
    testStringification();
    testDeviceError();

    std::cout << "UT_ImageGeometry: ALL CHECKS PASSED" << std::endl;
    return 0;
}
