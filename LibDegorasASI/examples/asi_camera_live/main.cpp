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

// ---------------------------------------------------------------------------------------------------------------------
// EXAMPLE: live view with interactive control
//
// A viewfinder: the stream in a window, and the controls that matter while pointing a telescope -- exposure, gain,
// white balance -- adjustable from the keyboard with the effect visible immediately.
//
// This is an EXAMPLE, not part of the library, and that is deliberate. LibDegorasASI draws nothing and depends on no
// imaging or GUI toolkit; the point of the Frame value type is that a consumer hands its bytes to whatever it already
// uses. SDL2 appears here and nowhere else, and this example is built only when SDL2 is found -- see the CMakeLists in
// this directory. With SDL2 absent the library still builds and its tests still pass.
//
// WHY SDL2 AND NOT OPENCV
// OpenCV was the obvious first choice and was tried first. The OpenCV in this environment builds highgui against Qt,
// so its config runs find_dependency(Qt6 COMPONENTS ... Core5Compat ...); the qt5compat port was not installed then,
// that dependency failed, and because the failure lands inside OpenCV's own cmake_policy(PUSH) block the error read
// as the misleading "cmake_policy PUSH without matching POP".
//
// That obstacle no longer exists -- qt5compat is installed and find_package(OpenCV) resolves cleanly. SDL2 stays,
// for a better reason than the original one: linking the OpenCV GUI would pull Qt into an example of a library whose
// entire premise is that no GUI toolkit is anywhere near it. A window, a texture and a key queue is the whole
// requirement here, and RGB24 needs no conversion at all -- see below.
// ---------------------------------------------------------------------------------------------------------------------

// C++ INCLUDES
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

// SDL INCLUDES
//
// SDL_MAIN_HANDLED keeps OUR main(). Without it SDL.h does `#define main SDL_main` on Windows and expects to
// supply the entry point itself through SDL2main, which renames main() and leaves the CRT looking for WinMain:
//
//     undefined reference to `WinMain'
//
// That entry point is for a windowed application. This is a console tool that also opens a window -- its stdout is
// half the interface -- so it keeps its own main and tells SDL so, then calls SDL_SetMainReady() before SDL_Init.
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

// PROJECT INCLUDES
#include <LibDegorasASI/Modules/Devices>
#include <LibDegorasASI/Modules/Helpers>

using namespace dpasi;
using dpasi::types::OperationResult;

// ---------------------------------------------------------------------------------------------------------------------

namespace
{

constexpr int kMaxWindowWidth  = 1280;
constexpr int kMaxWindowHeight = 960;

struct Options
{
    int exposure_ms = 30;
    int gain        = 200;
    int bin         = 1;
    int camera      = -1;
    std::string format = "RGB24";
};

void printKeys()
{
    std::cout <<
        "Keys, while the window has focus:\n"
        "  Q / ESC        quit\n"
        "  E / shift+E    exposure  -10% / +10%   (multiplicative: a fixed step is useless across us..s)\n"
        "  G / shift+G    gain      -10  / +10\n"
        "  R / shift+R    WB red    -1   / +1     (colour cameras only)\n"
        "  B / shift+B    WB blue   -1   / +1\n"
        "  A              toggle the display auto-stretch\n"
        "  S              save the current frame through the library's own writer\n"
        "  H              print these keys again\n";
}

void printUsage()
{
    std::cout <<
        "Usage: Example_asi_camera_live [--exposure MS] [--gain N] [--bin N] [--format F] [--camera ID]\n"
        "\n"
        "  --exposure MS   initial exposure in milliseconds (default 30)\n"
        "  --gain N        initial gain (default 200)\n"
        "  --bin N         binning factor (default 1)\n"
        "  --format F      RGB24, RAW8, RAW16 or Y8 (default RGB24)\n"
        "  --camera ID     camera index; default is the first one found\n"
        "\n";
    printKeys();
}

/// Fills a BGR24 scratch buffer from the frame, and returns a pointer to the bytes SDL should upload.
///
/// RGB24 returns the frame's OWN buffer untouched: the SDK delivers B,G,R and SDL_PIXELFORMAT_BGR24 wants exactly
/// that, so the common case costs no conversion and no copy at all. Both vendor facts behind this were measured on
/// hardware and are documented in frame_writer.h: RGB24 is B,G,R, and RAW16 already spans the full 16-bit range
/// (the SDK scales rather than shifts), so it needs no shifting either.
const std::uint8_t* prepareBgr(const types::Frame& frame, bool stretch, std::vector<std::uint8_t>& scratch)
{
    const std::size_t pixels = static_cast<std::size_t>(frame.width) * frame.height;

    switch (frame.format)
    {
        case types::ImageFormat::RGB24:
            return frame.data.data();

        case types::ImageFormat::RAW8:
        case types::ImageFormat::Y8:
        {
            scratch.resize(pixels * 3u);
            std::uint8_t lo = 255, hi = 0;
            if (stretch)
            {
                for (std::size_t i = 0; i < pixels; ++i)
                {
                    lo = std::min<std::uint8_t>(lo, frame.data[i]);
                    hi = std::max<std::uint8_t>(hi, frame.data[i]);
                }
            }
            const int span = (hi > lo) ? (hi - lo) : 1;

            for (std::size_t i = 0; i < pixels; ++i)
            {
                const std::uint8_t v = stretch
                    ? static_cast<std::uint8_t>((frame.data[i] - lo) * 255 / span)
                    : frame.data[i];
                scratch[i * 3u + 0] = v;
                scratch[i * 3u + 1] = v;
                scratch[i * 3u + 2] = v;
            }
            return scratch.data();
        }

        case types::ImageFormat::RAW16:
        {
            scratch.resize(pixels * 3u);
            std::uint16_t lo = 0xFFFF, hi = 0;
            for (std::size_t i = 0; i < pixels; ++i)
            {
                const std::uint16_t v =
                    static_cast<std::uint16_t>(frame.data[i * 2] | (frame.data[i * 2 + 1] << 8));
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            const int span = (hi > lo) ? (hi - lo) : 1;

            for (std::size_t i = 0; i < pixels; ++i)
            {
                const std::uint16_t raw =
                    static_cast<std::uint16_t>(frame.data[i * 2] | (frame.data[i * 2 + 1] << 8));
                // Without the stretch a 12-bit-sensor scene is a nearly black window: the interesting range is a
                // narrow slice of 0..65535 and a plain >>8 throws it away.
                const std::uint8_t v = stretch
                    ? static_cast<std::uint8_t>((raw - lo) * 255 / span)
                    : static_cast<std::uint8_t>(raw >> 8);
                scratch[i * 3u + 0] = v;
                scratch[i * 3u + 1] = v;
                scratch[i * 3u + 2] = v;
            }
            return scratch.data();
        }

        default:
            return nullptr;
    }
}

/// Reads a control, applies a delta, clamps to what the camera accepts, writes it and reports what actually took.
void nudge(AsiCamera& camera, types::ControlType control, const char* label, long long delta)
{
    if (!camera.hasControl(control) || !camera.isControlWritable(control))
    {
        std::cout << "  " << label << ": not adjustable on this camera\n";
        return;
    }

    types::ControlValue value;
    if (camera.getControl(control, value) != OperationResult::OPERATION_OK)
    {
        std::cout << "  " << label << ": could not be read\n";
        return;
    }

    types::ControlCaps caps;
    camera.getControlCaps(control, caps);

    const long long before = value.value;
    value.value  = std::clamp<long long>(before + delta, caps.min_value, caps.max_value);
    value.is_auto = false;

    if (camera.doSetControl(control, value) != OperationResult::OPERATION_OK)
    {
        std::cout << "  " << label << ": write refused\n";
        return;
    }

    // Read back rather than trust the write: the SDK clamps silently, so what was asked for and what took effect are
    // not always the same number.
    types::ControlValue applied;
    camera.getControl(control, applied);
    std::cout << "  " << label << ": " << before << " -> " << applied.value
              << "  [" << caps.min_value << ".." << caps.max_value << "]\n";
}

bool parseArgs(int argc, char** argv, Options& opt)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const auto next = [&](int& out) { if (i + 1 < argc) { out = std::atoi(argv[++i]); return true; } return false; };

        if (arg == "--help" || arg == "-h")         { printUsage(); return false; }
        else if (arg == "--exposure")               { if (!next(opt.exposure_ms)) return false; }
        else if (arg == "--gain")                   { if (!next(opt.gain)) return false; }
        else if (arg == "--bin")                    { if (!next(opt.bin)) return false; }
        else if (arg == "--camera")                 { if (!next(opt.camera)) return false; }
        else if (arg == "--format" && i + 1 < argc) { opt.format = argv[++i]; }
        else
        {
            std::cout << "Unknown argument: " << arg << "\n\n";
            printUsage();
            return false;
        }
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------

int main(int argc, char** argv)
{
    Options opt;
    if (!parseArgs(argc, argv, opt))
        return 1;

    types::ImageFormat format = types::ImageFormat::RGB24;
    if      (opt.format == "RGB24") format = types::ImageFormat::RGB24;
    else if (opt.format == "RAW8")  format = types::ImageFormat::RAW8;
    else if (opt.format == "RAW16") format = types::ImageFormat::RAW16;
    else if (opt.format == "Y8")    format = types::ImageFormat::Y8;
    else
    {
        std::cout << "Unknown format '" << opt.format << "'. Use RGB24, RAW8, RAW16 or Y8.\n";
        return 1;
    }

    // -- Find and open -------------------------------------------------------------------------------------------------
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
              << (desc.is_colour ? ", colour" : ", mono")
              << ", " << types::toString(camera.getUsbLinkSpeed()) << "\n";
    if (!camera.isLinkFullSpeed())
        std::cout << "WARNING: USB3 camera on a USB2 host; expect a lower frame rate.\n";

    if (!camera.supportsFormat(format))
    {
        std::cout << "This camera cannot deliver " << opt.format << ".\n";
        camera.doDisconnect();
        return 1;
    }

    // -- Configure -----------------------------------------------------------------------------------------------------
    if (camera.doSetFullFrameRoi(format, opt.bin) != OperationResult::OPERATION_OK)
    {
        std::cout << "Could not set the ROI.\n";
        camera.doDisconnect();
        return 1;
    }
    camera.doSetExposure(std::chrono::milliseconds(opt.exposure_ms));

    types::ControlValue gain;
    gain.value   = opt.gain;
    gain.is_auto = false;
    camera.doSetControl(types::ControlType::GAIN, gain);

    types::RoiFormat live;
    camera.getRoiFormat(live);
    std::cout << "streaming " << live.width << "x" << live.height << " bin" << live.bin
              << " " << types::toString(live.format) << "\n\n";
    printKeys();
    std::cout << "\n";

    // -- Window --------------------------------------------------------------------------------------------------------
    // Required whenever SDL_MAIN_HANDLED is used: it tells SDL the initialisation its own entry point would have
    // done has been taken care of. Skipping it makes SDL_Init fail.
    SDL_SetMainReady();

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        std::cout << "SDL_Init failed: " << SDL_GetError() << "\n";
        camera.doDisconnect();
        return 1;
    }

    // The window is scaled down if the sensor is larger than the cap; the texture stays at sensor resolution and the
    // renderer does the scaling, so nothing is resampled on the CPU.
    int win_w = std::min(live.width, kMaxWindowWidth);
    int win_h = std::min(live.height, kMaxWindowHeight);

    SDL_Window* window = SDL_CreateWindow("LibDegorasASI - live view",
                                         SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                         win_w, win_h, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED) : nullptr;
    SDL_Texture*  texture  = renderer
        ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_BGR24, SDL_TEXTUREACCESS_STREAMING, live.width, live.height)
        : nullptr;

    if (!window || !renderer || !texture)
    {
        std::cout << "SDL setup failed: " << SDL_GetError() << "\n";
        if (texture)  SDL_DestroyTexture(texture);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window)   SDL_DestroyWindow(window);
        SDL_Quit();
        camera.doDisconnect();
        return 1;
    }

    // -- Stream --------------------------------------------------------------------------------------------------------
    if (camera.doStartVideoCapture() != OperationResult::OPERATION_OK)
    {
        std::cout << "Could not start streaming.\n";
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        camera.doDisconnect();
        return 1;
    }

    // ONE frame and ONE scratch buffer for the whole run. The frame's buffer is reallocated only when the geometry
    // changes, and the scratch buffer settles after the first iteration, so a steady stream costs no allocation.
    types::Frame frame;
    std::vector<std::uint8_t> scratch;

    bool running = true;
    bool stretch = (format == types::ImageFormat::RAW16);   // the format that needs it most, on by default
    int  shots   = 0;

    auto last_report = std::chrono::steady_clock::now();
    int  frames_since_report = 0;

    while (running)
    {
        // -- Input ---------------------------------------------------------------------------------------------------
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_QUIT)
            {
                running = false;
            }
            else if (event.type == SDL_KEYDOWN)
            {
                const bool shift = (event.key.keysym.mod & KMOD_SHIFT) != 0;
                switch (event.key.keysym.sym)
                {
                    case SDLK_ESCAPE:
                    case SDLK_q: running = false; break;
                    case SDLK_h: printKeys(); break;
                    case SDLK_a:
                        stretch = !stretch;
                        std::cout << "  auto-stretch: " << (stretch ? "on" : "off") << "\n";
                        break;

                    case SDLK_e:
                    {
                        types::ControlValue exposure;
                        if (camera.getControl(types::ControlType::EXPOSURE, exposure) == OperationResult::OPERATION_OK)
                        {
                            const long long factor = shift ? 11 : 9;
                            const long long target = std::max<long long>(1, exposure.value * factor / 10);
                            nudge(camera, types::ControlType::EXPOSURE, "exposure (us)", target - exposure.value);
                        }
                        break;
                    }

                    case SDLK_g: nudge(camera, types::ControlType::GAIN,    "gain",    shift ?  10 : -10); break;
                    case SDLK_r: nudge(camera, types::ControlType::WB_RED,  "WB red",  shift ?   1 :  -1); break;
                    case SDLK_b: nudge(camera, types::ControlType::WB_BLUE, "WB blue", shift ?   1 :  -1); break;

                    case SDLK_s:
                    {
                        // Saved through the library's own writer, not through SDL: it is the one that knows the
                        // vendor's channel order, and it keeps these files identical to the other examples'.
                        const std::string path = "live_" + std::to_string(++shots) + "." +
                                                 imgio::extensionFor(frame.format);
                        std::cout << "  saved: " << (imgio::writeFrame(frame, path) ? path : "WRITE FAILED") << "\n";
                        break;
                    }

                    default: break;
                }
            }
        }
        if (!running)
            break;

        // -- Frame ---------------------------------------------------------------------------------------------------
        const OperationResult res = camera.doGetVideoFrame(frame, std::chrono::milliseconds(2000));
        if (res == OperationResult::OPERATION_TIMEOUT)
        {
            std::cout << "  (no frame in 2 s; is the exposure very long?)\n";
            continue;
        }
        if (res != OperationResult::OPERATION_OK)
        {
            std::cout << "  frame retrieval failed: " << types::toString(res) << "\n";
            break;
        }

        const std::uint8_t* bgr = prepareBgr(frame, stretch, scratch);
        if (!bgr)
        {
            std::cout << "  this format cannot be displayed.\n";
            break;
        }

        SDL_UpdateTexture(texture, nullptr, bgr, frame.width * 3);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);

        // -- Rate ----------------------------------------------------------------------------------------------------
        ++frames_since_report;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(2))
        {
            const double secs = std::chrono::duration<double>(now - last_report).count();
            int dropped = 0;
            camera.getDroppedFrames(dropped);
            std::cout << "  " << std::fixed << std::setprecision(1) << (frames_since_report / secs)
                      << " fps, " << dropped << " dropped, sequence " << frame.sequence << "\n";
            frames_since_report = 0;
            last_report = now;
        }
    }

    // -- Teardown ------------------------------------------------------------------------------------------------------
    camera.doStopVideoCapture();
    camera.doDisconnect();

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    std::cout << "\nstopped after sequence " << frame.sequence << "\n";
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
