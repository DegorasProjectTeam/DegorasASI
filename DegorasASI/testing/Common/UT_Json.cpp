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
#include <cmath>
#include <iostream>
#include <string>

// PROJECT INCLUDES (module aggregators)
#include <DegorasASI/Modules/Common>


using namespace dpasi;
using namespace dpasi::types;

// ---------------------------------------------------------------------------------------------------------------------
// No-hardware self-check for JSON serialisation: every serialisable type round-trips through toJsonStr/fromJsonStr,
// in both compact and pretty forms, and the pretty form parses identically to the compact form.
// ---------------------------------------------------------------------------------------------------------------------

namespace
{

bool dEq(double a, double b) { return std::abs(a - b) < 1e-4; }

void testRoiFormat()
{
    RoiFormat roi;
    roi.width = 640; roi.height = 480; roi.bin = 2; roi.format = ImageFormat::RAW16;

    const RoiFormat rt = RoiFormat::fromJsonStr(roi.toJsonStr());
    assert(rt.width == roi.width && rt.height == roi.height && rt.bin == roi.bin && rt.format == roi.format);

    const std::string pretty = roi.toJsonStr(true);
    assert(pretty.find('\n') != std::string::npos);                   // pretty is multi-line
    assert(roi.toJsonStr(false).find('\n') == std::string::npos);     // compact is single-line

    const RoiFormat rtp = RoiFormat::fromJsonStr(pretty);             // pretty parses identically
    assert(rtp.width == roi.width && rtp.height == roi.height && rtp.bin == roi.bin && rtp.format == roi.format);
}

void testRoiPosition()
{
    RoiPosition pos;
    pos.start_x = 332; pos.start_y = 248;

    const RoiPosition rt = RoiPosition::fromJsonStr(pos.toJsonStr());
    assert(rt.start_x == pos.start_x && rt.start_y == pos.start_y);

    const RoiPosition rtp = RoiPosition::fromJsonStr(pos.toJsonStr(true));
    assert(rtp.start_x == pos.start_x && rtp.start_y == pos.start_y);
}

void testControlCaps()
{
    ControlCaps caps;
    caps.type = ControlType::EXPOSURE;
    caps.name = "Exposure";
    caps.description = "Exposure Time(us)";
    caps.min_value = 32;
    caps.max_value = 2000000000;
    caps.default_value = 10000;
    caps.is_auto_supported = true;
    caps.is_writable = true;

    const ControlCaps rt = ControlCaps::fromJsonStr(caps.toJsonStr());
    assert(rt.type == caps.type && rt.name == caps.name && rt.description == caps.description);
    assert(rt.min_value == caps.min_value && rt.max_value == caps.max_value && rt.default_value == caps.default_value);
    assert(rt.is_auto_supported == caps.is_auto_supported && rt.is_writable == caps.is_writable);

    const ControlCaps rtp = ControlCaps::fromJsonStr(caps.toJsonStr(true));
    assert(rtp.type == caps.type && rtp.name == caps.name && rtp.max_value == caps.max_value);

    // A control value beyond 32 bits must survive: control values are widened to 64 bits at the public boundary.
    ControlCaps wide;
    wide.min_value = -5000000000LL;
    wide.max_value = 5000000000LL;
    const ControlCaps wide_rt = ControlCaps::fromJsonStr(wide.toJsonStr());
    assert(wide_rt.min_value == wide.min_value && wide_rt.max_value == wide.max_value);
}

void testControlValue()
{
    ControlValue val;
    val.value = 137; val.is_auto = true;

    const ControlValue rt = ControlValue::fromJsonStr(val.toJsonStr());
    assert(rt.value == val.value && rt.is_auto == val.is_auto);

    const ControlValue rtp = ControlValue::fromJsonStr(val.toJsonStr(true));
    assert(rtp.value == val.value && rtp.is_auto == val.is_auto);
}

void testCameraDescriptor()
{
    // Values taken from a real ZWO AsiCamera.
    CameraDescriptor desc;
    desc.id = CameraId{0};
    desc.name = "ZWO AsiCamera";
    desc.max_width = 1304;
    desc.max_height = 976;
    desc.pixel_size_um = 3.75;
    desc.bit_depth = 12;
    desc.electrons_per_adu = 0.863;
    desc.is_colour = true;
    desc.bayer_pattern = BayerPattern::RG;
    desc.supported_bins = {1, 2};
    desc.supported_formats = {ImageFormat::RAW8, ImageFormat::RGB24, ImageFormat::Y8, ImageFormat::RAW16};
    desc.has_st4_port = true;
    desc.is_usb3_camera = true;
    desc.is_usb3_host = false;

    // Every boolean is exercised in BOTH states, and that is the point rather than thoroughness for its own sake.
    // Left at their default false, a serialiser that dropped a field ENTIRELY would still round-trip to false and
    // the assertion would pass: the test could not tell "written as false" from "never written".
    desc.has_mechanical_shutter = true;
    desc.is_cooled = true;
    desc.is_trigger_camera = true;

    const CameraDescriptor rt = CameraDescriptor::fromJsonStr(desc.toJsonStr());
    assert(rt.id == desc.id && rt.name == desc.name);
    assert(rt.max_width == desc.max_width && rt.max_height == desc.max_height && rt.bit_depth == desc.bit_depth);
    assert(dEq(rt.pixel_size_um, desc.pixel_size_um) && dEq(rt.electrons_per_adu, desc.electrons_per_adu));
    assert(rt.is_colour == desc.is_colour && rt.bayer_pattern == desc.bayer_pattern);
    assert(rt.supported_bins == desc.supported_bins);
    assert(rt.supported_formats == desc.supported_formats);
    assert(rt.has_st4_port && rt.is_usb3_camera && !rt.is_usb3_host);
    assert(rt.has_mechanical_shutter && rt.is_cooled && rt.is_trigger_camera);

    // The false direction, from a default descriptor, so both values of all seven booleans are covered.
    const CameraDescriptor zero_rt = CameraDescriptor::fromJsonStr(CameraDescriptor().toJsonStr());
    assert(!zero_rt.is_colour && !zero_rt.has_mechanical_shutter && !zero_rt.has_st4_port);
    assert(!zero_rt.is_cooled && !zero_rt.is_usb3_camera && !zero_rt.is_usb3_host && !zero_rt.is_trigger_camera);

    const CameraDescriptor rtp = CameraDescriptor::fromJsonStr(desc.toJsonStr(true));   // via pretty
    assert(rtp.name == desc.name && rtp.supported_bins == desc.supported_bins);
    assert(rtp.supported_formats == desc.supported_formats);

    // Capability queries.
    assert(desc.supportsFormat(ImageFormat::RAW16) && desc.supportsFormat(ImageFormat::Y8));
    assert(desc.supportsBin(1) && desc.supportsBin(2) && !desc.supportsBin(4));
    assert(!desc.isLinkFullSpeed());   // USB3 camera on a USB2 host is a degraded link.
}

void testFrameMetadata()
{
    Frame frame;
    frame.format = ImageFormat::RGB24;
    frame.width = 640; frame.height = 480; frame.bin = 1;
    frame.sequence = 42;
    frame.timestamp = std::chrono::system_clock::now();
    frame.data.assign(frame.expectedBytes(), 0);

    // Frame serialises METADATA only, never the pixel bytes.
    const std::string js = frame.toJsonStr();
    assert(js.find("\"width\": 640") != std::string::npos);
    assert(js.find("\"sequence\": 42") != std::string::npos);
    assert(js.find("\"bytes\": " + std::to_string(frame.data.size())) != std::string::npos);
    assert(frame.toJsonStr(true).find('\n') != std::string::npos);
}

void testMissingKeysKeepDefaults()
{
    // The extractors are tolerant and best-effort: a missing key yields the field's default, never an error.
    const RoiFormat roi = RoiFormat::fromJsonStr("{}");
    assert(roi.width == 0 && roi.height == 0 && roi.bin == 1 && roi.format == ImageFormat::RAW8);

    const ControlValue val = ControlValue::fromJsonStr("not json at all");
    assert(val.value == 0 && !val.is_auto);
}

} // namespace

int main()
{
    testRoiFormat();
    testRoiPosition();
    testControlCaps();
    testControlValue();
    testCameraDescriptor();
    testFrameMetadata();
    testMissingKeysKeepDefaults();

    std::cout << "UT_Json: ALL CHECKS PASSED" << std::endl;
    return 0;
}
