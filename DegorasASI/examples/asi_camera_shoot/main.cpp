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
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

// PROJECT INCLUDES (module aggregators)
#include <DegorasASI/Modules/Helpers>
#include <DegorasASI/Modules/Devices>


// Example: take a shot with the exposure and gain YOU choose, then look at it.
//
// This is the smallest useful capture tool, and the seed of a capture button in a real application. Everything it does
// is capability-driven: the limits it validates against are the ones this camera reports, never constants baked into
// the program, so it behaves correctly on a model it has never met.
//
//   Example_asi_camera_shoot [options]
//     --exposure <ms>     exposure time in milliseconds       (default 50)
//     --gain <value>      sensor gain, in the camera's units  (default: the camera's own default)
//     --format <fmt>      RAW8 | RGB24 | RAW16 | Y8           (default RGB24, the only viewable one on a colour camera)
//     --bin <n>           binning factor                      (default 1)
//     --output <stem>     output file stem                    (default "shot")
//     --camera <id>       camera identifier                   (default: the first discovered)
//     --no-preview        skip the terminal brightness map
//     --fits              also write a FITS alongside, with exposure, gain, temperature and Bayer pattern

using namespace dpasi;
using dpasi::types::OperationResult;

namespace
{

struct Options
{
    int exposure_ms = 50;
    long long gain = -1;          // negative means "leave the camera's default"
    std::string format = "RGB24";
    int bin = 1;
    std::string output = "shot";
    int camera = -1;              // negative means "the first discovered"
    bool preview = true;
    bool fits = false;
};

bool parseFormat(const std::string& text, types::ImageFormat& out)
{
    if (text == "RAW8")  { out = types::ImageFormat::RAW8;  return true; }
    if (text == "RGB24") { out = types::ImageFormat::RGB24; return true; }
    if (text == "RAW16") { out = types::ImageFormat::RAW16; return true; }
    if (text == "Y8")    { out = types::ImageFormat::Y8;    return true; }
    return false;
}

/// @return False if an argument was malformed; the caller then exits rather than shooting with a misread setting.
bool parseArgs(int argc, char* argv[], Options& opt)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const bool has_value = (i + 1 < argc);

        if (arg == "--no-preview")            { opt.preview = false; }
        else if (arg == "--fits")             { opt.fits = true; }
        else if (arg == "--exposure" && has_value) { opt.exposure_ms = std::atoi(argv[++i]); }
        else if (arg == "--gain" && has_value)     { opt.gain = std::atoll(argv[++i]); }
        else if (arg == "--format" && has_value)   { opt.format = argv[++i]; }
        else if (arg == "--bin" && has_value)      { opt.bin = std::atoi(argv[++i]); }
        else if (arg == "--output" && has_value)   { opt.output = argv[++i]; }
        else if (arg == "--camera" && has_value)   { opt.camera = std::atoi(argv[++i]); }
        else
        {
            std::cout << "Unrecognised or incomplete argument: " << arg << "\n";
            return false;
        }
    }
    return opt.exposure_ms > 0 && opt.bin > 0;
}

/**
 * @brief Apply one control after checking it against the camera's OWN limits, and report what happened.
 * @note The library would refuse an out-of-range value anyway -- it range-checks against the discovered capabilities
 *       precisely because the SDK silently clamps instead. Reading the limits first simply lets this tool say what the
 *       acceptable range actually is rather than just reporting a rejection.
 */
bool applyControl(AsiCamera& camera, types::ControlType type, long long value, const std::string& label)
{
    types::ControlCaps caps;
    if (camera.getControlCaps(type, caps) != OperationResult::OPERATION_OK)
    {
        std::cout << "  " << label << ": this camera has no such control, skipping\n";
        return true;                       // not fatal: the camera simply does not have it
    }
    if (value < caps.min_value || value > caps.max_value)
    {
        std::cout << "  " << label << ": " << value << " is outside this camera's range ["
                  << caps.min_value << ", " << caps.max_value << "]\n";
        return false;
    }

    types::ControlValue control;
    control.value = static_cast<types::ControlRaw>(value);
    control.is_auto = false;
    const OperationResult res = camera.doSetControl(type, control);
    std::cout << "  " << label << ": " << value << " -> " << types::toString(res)
              << "  (range [" << caps.min_value << ", " << caps.max_value << "], default "
              << caps.default_value << ")\n";
    return res == OperationResult::OPERATION_OK;
}

} // namespace

int main(int argc, char* argv[])
{
    Options opt;
    if (!parseArgs(argc, argv, opt))
    {
        std::cout << "Usage: Example_asi_camera_shoot [--exposure <ms>] [--gain <v>] [--format RAW8|RGB24|RAW16|Y8]\n"
                     "                                [--bin <n>] [--output <stem>] [--camera <id>] [--no-preview]\n";
        return 1;
    }

    types::ImageFormat format = types::ImageFormat::RGB24;
    if (!parseFormat(opt.format, format))
    {
        std::cout << "Unknown format '" << opt.format << "'. Use RAW8, RGB24, RAW16 or Y8.\n";
        return 1;
    }

    types::CameraDescriptorList cameras;
    if (AsiCamera::getDeviceList(cameras) != OperationResult::OPERATION_OK || cameras.empty())
    {
        std::cout << "No ASI camera found. Connect one and retry.\n";
        return 1;
    }

    const types::CameraId id = (opt.camera >= 0) ? static_cast<types::CameraId>(opt.camera) : cameras.front().id;

    AsiCamera camera(id);
    if (camera.doConnect() != OperationResult::OPERATION_OK)
    {
        std::cout << "Could not connect to camera id " << types::toType(id) << "\n";
        return 1;
    }

    types::CameraDescriptor desc;
    camera.getDescriptor(desc);
    std::cout << desc.name << " -- " << desc.max_width << "x" << desc.max_height
              << (desc.is_colour ? ", colour" : ", mono") << ", " << types::toString(camera.getUsbLinkSpeed()) << "\n";
    if (!camera.isLinkFullSpeed())
        std::cout << "WARNING: USB3 camera on a USB2 host; throughput is degraded.\n";

    // -- What the camera can actually do -------------------------------------------------------------------------------
    // Asked, not assumed. A mono camera has no white balance, an uncooled one has no cooler, and this camera has no
    // gamma control at all -- so a tool that assumed any of them would misbehave on the next model.
    if (!camera.supportsFormat(format))
    {
        std::cout << "This camera cannot deliver " << opt.format << ". It supports:";
        for (const types::ImageFormat f : desc.supported_formats)
            std::cout << " " << types::toString(f);
        std::cout << "\n";
        camera.doDisconnect();
        return 1;
    }
    if (!camera.supportsBin(opt.bin))
    {
        std::cout << "This camera does not support bin" << opt.bin << ". It supports:";
        for (const int b : desc.supported_bins)
            std::cout << " bin" << b;
        std::cout << "\n";
        camera.doDisconnect();
        return 1;
    }

    // -- Settings ------------------------------------------------------------------------------------------------------
    std::cout << "\n-- settings --\n";

    const OperationResult roi_res = camera.doSetFullFrameRoi(format, opt.bin);
    if (roi_res != OperationResult::OPERATION_OK)
    {
        std::cout << "  doSetFullFrameRoi -> " << types::toString(roi_res) << "\n";
        camera.doDisconnect();
        return 1;
    }

    // Exposure goes through the typed wrapper so the microsecond conversion happens in exactly one place.
    const OperationResult exp_res = camera.doSetExposure(std::chrono::milliseconds(opt.exposure_ms));
    std::cout << "  exposure: " << opt.exposure_ms << " ms -> " << types::toString(exp_res) << "\n";
    if (exp_res != OperationResult::OPERATION_OK)
    {
        types::ControlCaps exp_caps;
        if (camera.getControlCaps(types::ControlType::EXPOSURE, exp_caps) == OperationResult::OPERATION_OK)
            std::cout << "            this camera accepts " << (exp_caps.min_value / 1000.0) << " to "
                      << (exp_caps.max_value / 1000.0) << " ms\n";
        camera.doDisconnect();
        return 1;
    }

    if (opt.gain >= 0 && !applyControl(camera, types::ControlType::GAIN, opt.gain, "gain"))
    {
        camera.doDisconnect();
        return 1;
    }

    types::RoiFormat live;
    camera.getRoiFormat(live);
    std::cout << "  geometry: " << live.width << "x" << live.height << " bin" << live.bin
              << " " << types::toString(live.format)
              << " (" << types::frameBufferSize(live) << " bytes per frame)\n";

    // -- Shoot ---------------------------------------------------------------------------------------------------------
    std::cout << "\n-- capture --\n";

    // The timeout must outlast the exposure itself, or a long shot would report a timeout it never had.
    const auto timeout = std::chrono::milliseconds(opt.exposure_ms * 2 + 5000);

    types::Frame frame;
    const OperationResult res = camera.doCaptureSingleFrame(frame, timeout);
    if (res != OperationResult::OPERATION_OK)
    {
        std::cout << "  doCaptureSingleFrame -> " << types::toString(res) << "\n";
        camera.doDisconnect();
        return 1;
    }

    std::cout << "  captured " << frame.width << "x" << frame.height << " " << types::toString(frame.format)
              << ", " << frame.data.size() << " bytes\n";

    double celsius = 0;
    const bool has_temp = (camera.getSensorTemperature(celsius) == OperationResult::OPERATION_OK);
    if (has_temp)
        std::cout << "  sensor temperature: " << celsius << " C\n";

    // The viewable file. Absolute, because a bare relative name leaves the reader hunting through a build directory.
    const std::string path = opt.output + "." + imgio::extensionFor(frame.format);
    const bool written = imgio::writeFrame(frame, path);
    std::cout << "  saved: " << (written ? std::filesystem::absolute(path).string()
                                         : std::string("WRITE FAILED (" + path + ")")) << "\n";

    // The archival file. Everything a pipeline needs later goes INSIDE it rather than into a filename convention:
    // when it was taken, how long for, at what gain, and -- for a raw colour frame -- which mosaic to demosaic with.
    if (opt.fits)
    {
        imgio::FitsCards cards;
        cards.push_back(imgio::fitsReal("EXPTIME", opt.exposure_ms / 1000.0, "exposure time in seconds"));
        cards.push_back(imgio::fitsText("INSTRUME", desc.name, "camera model"));
        if (opt.gain >= 0)
            cards.push_back(imgio::fitsInt("GAIN", opt.gain, "sensor gain"));
        if (has_temp)
            cards.push_back(imgio::fitsReal("CCD-TEMP", celsius, "sensor temperature in C"));

        // Only meaningful on a raw frame from a colour sensor: RGB24 is already demosaiced and binning destroys the
        // mosaic outright. writeFits drops the card in both cases anyway, so this guard is belt to its braces.
        if (desc.is_colour && frame.format != types::ImageFormat::RGB24 && frame.bin == 1)
            cards.push_back(imgio::fitsBayerPattern(desc.bayer_pattern));

        const std::string fits_path = opt.output + ".fits";
        const bool fits_written = imgio::writeFits(frame, fits_path, cards);
        std::cout << "  saved: " << (fits_written ? std::filesystem::absolute(fits_path).string()
                                                  : std::string("WRITE FAILED (" + fits_path + ")")) << "\n";
    }

    if (opt.preview)
    {
        std::cout << "\n" << imgio::framePreview(frame, 72) << "\n";
        if (frame.format != types::ImageFormat::RGB24 && desc.is_colour)
            std::cout << "(this is a Bayer mosaic, not a grey picture: use --format RGB24 for a viewable image)\n\n";
    }

    camera.doDisconnect();
    return 0;
}
