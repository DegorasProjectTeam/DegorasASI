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

// ---------------------------------------------------------------------------------------------------------------------
// EXAMPLE: live view with interactive control
//
// A viewfinder: the stream in a window, and the controls that matter while pointing a telescope -- exposure, gain,
// white balance -- adjustable while watching the effect.
//
// This is an EXAMPLE, not part of the library, and that is deliberate. DegorasASI draws nothing and depends on no
// imaging or GUI toolkit; the point of the Frame value type is that a consumer hands its bytes to whatever it already
// uses. OpenCV appears here and nowhere else, and this example is built only when OpenCV is found -- see the
// CMakeLists in this directory. With OpenCV absent the library still builds and its tests still pass.
//
// STRUCTURE: model, view, controller, in five pairs of files.
//
//   live_model      owns the camera and the ONLY thread that talks to it. Publishes frames and a state snapshot.
//   live_view       owns the window. Runs on the main thread, because highgui insists. Draws what it is given.
//   live_controller turns keys, clicks and slider movements into requests. Knows nothing about pixels or the SDK.
//   live_image      Frame -> cv::Mat, demosaicing and the stretch. Pure; no camera, no window, no thread.
//   live_reticle    the aiming marks: positions, geometry, hit-testing and their file. Free of OpenCV entirely, so
//                   the multi-camera control software can take it as it stands.
//   main            this file: arguments, wiring, and the loop that drives the view.
//
// WHY IT IS SPLIT THAT WAY, and it is not for tidiness. The previous version was one loop on the main thread: grab,
// show, waitKey, repeat. doGetVideoFrame() blocks for at least the exposure and waitKey() is what pumps the GUI, so
// the window went dead for the length of every exposure -- at two seconds it stuttered, at ten it looked broken, and
// dragging the exposure slider was impossible because the slider could not be serviced while the grab was waiting.
//
// With the grab on its own thread the main loop runs at a fixed rate whatever the camera is doing: the window
// repaints, the sliders move, the HUD updates, and a progress bar along the bottom says how long the current wait has
// left. Control changes become requests that the acquisition thread applies between grabs, so a change made during a
// long exposure takes effect on the next one -- which is what the hardware does, said out loud rather than hidden
// behind a frozen interface.
//
// KEEP IN MIND
//   * The Frame buffer is never copied: the model and this loop swap buffers, so a steady stream allocates nothing.
//   * Two vendor facts, both measured on hardware and documented in frame_writer.h, make the display cheap: RGB24
//     arrives as B,G,R -- already cv::Mat 8UC3 order, no conversion at all -- and RAW16 already spans the full
//     16-bit range on a 12-bit sensor, so it must NOT be shifted down.
//   * A HARD KILL CAN LEAVE THE CAMERA UNUSABLE. See the signal handler below.
// ---------------------------------------------------------------------------------------------------------------------

// C++ INCLUDES
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

// OPENCV INCLUDES
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

// PROJECT INCLUDES
#include <DegorasASI/Modules/ASI>
#include <DegorasASI/Modules/Devices>
#include <DegorasASI/Modules/Helpers>

#include "live_controller.h"
#include "live_image.h"
#include "live_model.h"
#include "live_view.h"


using namespace dpasi;
using dpasi::types::OperationResult;

// ---------------------------------------------------------------------------------------------------------------------

namespace
{

constexpr int kMaxWindowWidth  = 1280;
constexpr int kMaxWindowHeight = 960;
const char*   kWindowTitle     = "DegorasASI - live view";

// How long each turn of the main loop gives the GUI. Short enough to feel immediate, long enough not to spin: it is
// the frame rate of the INTERFACE, which is now independent of the frame rate of the camera.
constexpr int kUiPeriodMs = 15;

// A HARD KILL CAN LEAVE THE CAMERA UNUSABLE, so this example goes out of its way to avoid one.
//
// Measured the hard way: after repeatedly killing processes that held the camera open, the ASI SDK began reporting
// zero connected cameras while Windows still reported the device present and healthy (Status OK, CM_PROB_NONE), and
// eventually the enumeration call itself blocked -- a sampling process sat ten minutes on 0.1 s of CPU. Nothing held
// the camera and no lock file remained: the wedge is inside the vendor driver, invisible to PnP, and it took a
// physical re-plug to clear. So Ctrl-C and a terminating signal are caught and turned into a normal exit, which stops
// the stream and disconnects properly.
//
// The handler does nothing but set a flag, because that is all a signal handler may safely do. It cannot help with
// SIGKILL, a taskkill /f or a debugger detaching -- nothing can.
std::atomic<bool> g_stop{false};

extern "C" void onSignal(int)
{
    g_stop.store(true);
}

// ---------------------------------------------------------------------------------------------------------------------
// COMMAND LINE

struct Options
{
    Options();

    int exposure_ms;
    int gain;
    int bin;
    int camera;
    bool demosaic;
    bool stretch_set;
    bool stretch;
    int snap;                  ///< >0: grab this many frames, write the last one, exit. No window.
    std::string snap_name;
    std::string format;
    std::string reticles;
    double zoom;
    std::string flip;
    int rotate;
};

Options::Options() :
    exposure_ms(30),
    gain(200),
    bin(1),
    camera(-1),
    demosaic(true),
    stretch_set(false),
    stretch(false),
    snap(0),
    snap_name("live_snap"),
    format("RGB24"),
    reticles("live_reticles.txt"),
    zoom(1.0),
    flip(),
    rotate(0)
{
}

void printUsage()
{
    std::cout <<
        "Usage: Example_asi_camera_live [--exposure MS] [--gain N] [--bin N] [--format F] [--camera ID]\n"
        "                              [--no-demosaic] [--stretch] [--no-stretch] [--snap N]\n"
        "\n"
        "  --exposure MS   initial exposure in milliseconds (default 30)\n"
        "  --gain N        initial gain (default 200)\n"
        "  --bin N         binning factor (default 1). Binning destroys the Bayer mosaic, so bin > 1 is always mono\n"
        "  --format F      RGB24, RAW8, RAW16 or Y8 (default RGB24)\n"
        "  --camera ID     camera index; default is the first one found\n"
        "  --no-demosaic   show raw Bayer frames as they are, without interpolating colour\n"
        "  --stretch       start with the percentile auto-stretch on  (default: on for RAW16, off otherwise)\n"
        "  --no-stretch    start with it off\n"
        "  --snap N        headless: grab N frames (so exposure settles), write the last one, exit\n"
        "  --snap-name S   base name for the --snap output (default live_snap)\n"
        "  --reticles P    reticle file, read at start and written at exit (default live_reticles.txt).\n"
        "                  Pass \"none\" to keep the reticles in memory only\n"
        "  --zoom F        start magnified F times on the frame centre (default 1, the whole frame)\n"
        "  --flip WHICH    flip the display: h, v or hv (default none)\n"
        "  --rotate DEG    rotate the display: 0, 90, 180 or 270 clockwise (default 0)\n"
        "\n";
}

bool parseArgs(int argc, char** argv, Options& opt)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const auto next = [&](int& out) { if (i + 1 < argc) { out = std::atoi(argv[++i]); return true; } return false; };

        if (arg == "--help" || arg == "-h")            { printUsage(); return false; }
        else if (arg == "--exposure")                  { if (!next(opt.exposure_ms)) return false; }
        else if (arg == "--gain")                      { if (!next(opt.gain)) return false; }
        else if (arg == "--bin")                       { if (!next(opt.bin)) return false; }
        else if (arg == "--camera")                    { if (!next(opt.camera)) return false; }
        else if (arg == "--snap")                      { if (!next(opt.snap)) return false; }
        else if (arg == "--no-demosaic")               { opt.demosaic = false; }
        else if (arg == "--stretch")                   { opt.stretch_set = true; opt.stretch = true; }
        else if (arg == "--no-stretch")                { opt.stretch_set = true; opt.stretch = false; }
        else if (arg == "--format" && i + 1 < argc)    { opt.format = argv[++i]; }
        else if (arg == "--snap-name" && i + 1 < argc) { opt.snap_name = argv[++i]; }
        else if (arg == "--reticles" && i + 1 < argc)  { opt.reticles = argv[++i]; }
        else if (arg == "--zoom" && i + 1 < argc)      { opt.zoom = std::atof(argv[++i]); }
        else if (arg == "--flip" && i + 1 < argc)      { opt.flip = argv[++i]; }
        else if (arg == "--rotate" && i + 1 < argc)    { if (!next(opt.rotate)) return false; }
        else
        {
            std::cout << "Unknown argument: " << arg << "\n\n";
            printUsage();
            return false;
        }
    }
    return true;
}

bool formatFromName(const std::string& name, types::ImageFormat& format)
{
    if      (name == "RGB24") format = types::ImageFormat::RGB24;
    else if (name == "RAW8")  format = types::ImageFormat::RAW8;
    else if (name == "RAW16") format = types::ImageFormat::RAW16;
    else if (name == "Y8")    format = types::ImageFormat::Y8;
    else return false;
    return true;
}

std::string fixed1(double v)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << v;
    return os.str();
}

/// What the HUD says about colour for this frame.
std::string bayerNote(const types::CameraDescriptor& desc, const types::Frame& frame, int bayer, bool demosaic)
{
    if (bayer >= 0)
        return demosaic ? ("demosaic " + types::toString(desc.bayer_pattern)) : std::string("raw mosaic");
    return (frame.bin > 1) ? std::string("bin>1: no mosaic") : std::string("no mosaic");
}

/// Headless capture: no window, no keyboard. Exists so the example can be exercised without a human, and it is also
/// how the demosaic mapping is verified -- capture the same scene as RGB24 and as demosaiced RAW and compare means.
int runSnapshot(live::LiveModel& model, live::LiveView& view, live::LiveController& controller,
                const types::CameraDescriptor& desc, const Options& opt)
{
    types::Frame frame;
    cv::Mat display;
    int seen = 0;

    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(120);

    while (seen < opt.snap && !g_stop.load())
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            std::cout << "snap: gave up waiting for " << opt.snap << " frames\n";
            return 1;
        }
        if (model.takeFrame(frame))
            ++seen;
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (frame.empty())
        return 1;

    const int bayer = desc.is_colour ? live::bayerCodeFor(desc.bayer_pattern, frame) : -1;
    live::buildDisplay(frame, view.displayOptions(), bayer, display);

    const std::string png = opt.snap_name + ".png";
    const std::string raw = opt.snap_name + "." + imgio::extensionFor(frame.format);
    const bool png_ok = cv::imwrite(png, display);
    const bool raw_ok = imgio::writeFrame(frame, raw);

    // A third file: the frame with everything the viewfinder would draw on it. It costs one composition and it is the
    // only way to check the HUD, the reticles and the progress bar without a person watching a screen.
    live::FrameGeometry geometry;
    geometry.start_x = frame.start_x;
    geometry.start_y = frame.start_y;
    geometry.bin = frame.bin;
    geometry.width = frame.width;
    geometry.height = frame.height;
    geometry.sensor_width = desc.max_width;
    geometry.sensor_height = desc.max_height;

    live::Overlay overlay;
    overlay.bayer_note = bayerNote(desc, frame, bayer, view.displayOptions().demosaic);
    const std::string view_png = opt.snap_name + "_view.png";
    const bool view_ok = cv::imwrite(view_png, view.compose(display, model.state(), overlay, geometry));

    // The means are the point of this mode: an RGB24 capture and a demosaiced RAW capture of the same scene must
    // agree on which channel is which. If the Bayer code were wrong, B and R would swap.
    const cv::Scalar mean = cv::mean(display);
    std::cout << "snap: " << frame.width << "x" << frame.height << " " << types::toString(frame.format)
              << "  mean B=" << fixed1(mean[0]) << " G=" << fixed1(mean[1]) << " R=" << fixed1(mean[2])
              << "  " << bayerNote(desc, frame, bayer, view.displayOptions().demosaic) << "\n";
    std::cout << "  " << (png_ok ? png : std::string("PNG WRITE FAILED"))
              << "  " << (raw_ok ? raw : std::string("RAW WRITE FAILED"))
              << "  " << (view_ok ? view_png : std::string("VIEW WRITE FAILED")) << "\n";
    return (png_ok && raw_ok && view_ok) ? 0 : 1;
}

}   // namespace

// ---------------------------------------------------------------------------------------------------------------------

int main(int argc, char** argv)
{
    // Installed before anything opens the camera, so an interrupt during startup is handled too.
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    Options opt;
    if (!parseArgs(argc, argv, opt))
        return 1;

    types::ImageFormat format = types::ImageFormat::RGB24;
    if (!formatFromName(opt.format, format))
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
    const OperationResult conn = camera.doConnect();
    if (conn != OperationResult::OPERATION_OK)
    {
        // CAMERA_IN_USE is by far the most common failure here and a generic message hides it. The library takes a
        // host-wide claim at connect precisely so this can be answered, and answered with a name.
        if (conn == OperationResult::CAMERA_IN_USE)
        {
            const std::string holder = asi::describeCameraHolder(id);
            std::cout << "Camera " << types::toType(id) << " is already in use"
                      << (holder.empty() ? std::string() : (" by " + holder)) << ".\n"
                      << "Close that program first; the ASI SDK cannot share a camera between processes.\n";
        }
        else
        {
            std::cout << "Could not connect to camera id " << types::toType(id)
                      << ": " << types::toString(conn) << "\n";
        }
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

    types::ControlValue gain_value;
    gain_value.value   = opt.gain;
    gain_value.is_auto = false;
    camera.doSetControl(types::ControlType::GAIN, gain_value);

    types::RoiFormat live_format;
    camera.getRoiFormat(live_format);
    std::cout << "streaming " << live_format.width << "x" << live_format.height << " bin" << live_format.bin
              << " " << types::toString(live_format.format) << "\n";

    const bool has_wb = desc.is_colour && camera.hasControl(types::ControlType::WB_RED);

    // -- Model ---------------------------------------------------------------------------------------------------------
    live::LiveModel model(camera, has_wb);
    if (!model.start())
    {
        std::cout << "Could not start streaming.\n";
        camera.doDisconnect();
        return 1;
    }

    // -- View and controller -------------------------------------------------------------------------------------------
    // Both exist before the headless branch, because a snapshot composes the same overlay the window would show. The
    // WINDOW, on the other hand, is only opened for the interactive path.
    live::LiveView view(kWindowTitle, live_format.width, live_format.height);
    view.displayOptions().demosaic = opt.demosaic;
    view.displayOptions().stretch  = opt.stretch_set ? opt.stretch : (format == types::ImageFormat::RAW16);

    // Applied before the headless branch, so a snapshot can be taken magnified, flipped or turned -- which is how the
    // reticles were checked against all of it without a person driving the wheel.
    if (opt.zoom > 1.0)
        view.zoomBy(opt.zoom, live_format.width / 2.0, live_format.height / 2.0);

    if (!opt.flip.empty() || opt.rotate != 0)
    {
        const bool flip_h = (opt.flip.find('h') != std::string::npos);
        const bool flip_v = (opt.flip.find('v') != std::string::npos);
        if (!opt.flip.empty() && !flip_h && !flip_v)
            std::cout << "  --flip not understood, expected h, v or hv: " << opt.flip << "\n";
        // Degrees in, quarter turns stored: anything that is not a multiple of ninety would have to resample, and a
        // magnified star that has been resampled is no longer evidence about focus.
        if (opt.rotate % 90 != 0)
            std::cout << "  --rotate is not a multiple of 90, rounding down: " << opt.rotate << "\n";
        view.setOrientation(flip_h, flip_v, opt.rotate / 90);
    }

    live::LiveController controller(model, view);

    // Loaded before the first frame, so a saved calibration is on screen from the outset rather than appearing a
    // moment later. "none" is spelled out because an empty argument is awkward to pass through a shell.
    if (opt.reticles != "none")
    {
        controller.setReticleFile(opt.reticles);
        if (controller.loadReticles())
            std::cout << "reticles loaded from " << opt.reticles << "\n";
    }

    // -- Headless ------------------------------------------------------------------------------------------------------
    if (opt.snap > 0)
    {
        const int result = runSnapshot(model, view, controller, desc, opt);
        model.stop();
        camera.doDisconnect();
        return result;
    }

    view.open(kMaxWindowWidth, kMaxWindowHeight);

    long long exposure_min = 0;
    long long exposure_max = 0;
    if (model.exposureRange(exposure_min, exposure_max))
        view.addExposureSlider(exposure_min, exposure_max, model.state().exposure_us);

    long long gain_max = 0;
    if (model.gainMaximum(gain_max))
        view.addGainSlider(gain_max, model.state().gain);

    std::cout << "\n";
    controller.printKeys();
    std::cout << "\n";

    // -- Loop ----------------------------------------------------------------------------------------------------------
    // Runs at the interface's own rate, not the camera's. Nothing in here blocks on the camera: takeFrame() returns
    // immediately whether or not a frame is ready, and every control change is a request.
    types::Frame frame;
    cv::Mat display;
    live::Overlay overlay;

    // The frame-to-sensor relationship. Seeded from the stream format so the first composition has something sane,
    // then taken from each frame: the ROI origin and the binning are properties of the FRAME, and reading them from
    // there is what lets a reticle survive a change of either.
    live::FrameGeometry geometry;
    geometry.bin = live_format.bin;
    geometry.width = live_format.width;
    geometry.height = live_format.height;
    geometry.sensor_width = desc.max_width;
    geometry.sensor_height = desc.max_height;
    live::DisplayOptions shown = view.displayOptions();
    bool rebuild = false;

    while (!g_stop.load() && view.isOpen())
    {
        const bool arrived = model.takeFrame(frame);

        // The display is also rebuilt when a DISPLAY OPTION changed, not only when a frame did. Otherwise toggling the
        // stretch during a ten-second exposure would appear to do nothing until the next frame came in.
        const live::DisplayOptions& current = view.displayOptions();
        if (current.stretch != shown.stretch || current.demosaic != shown.demosaic)
        {
            shown = current;
            rebuild = true;
        }

        if (arrived && !frame.empty())
        {
            geometry.start_x = frame.start_x;
            geometry.start_y = frame.start_y;
            geometry.bin = frame.bin;
            geometry.width = frame.width;
            geometry.height = frame.height;
        }

        if ((arrived || rebuild) && !frame.empty())
        {
            const int bayer = desc.is_colour ? live::bayerCodeFor(desc.bayer_pattern, frame) : -1;
            live::buildDisplay(frame, current, bayer, display);
            overlay.bayer_note = bayerNote(desc, frame, bayer, current.demosaic);
            rebuild = false;
        }

        const live::ModelState state = model.state();
        view.syncSliders(state);

        int probe_x = 0;
        int probe_y = 0;
        overlay.probe_text = (!frame.empty() && view.probePointInFrame(probe_x, probe_y))
                                 ? live::describePixel(frame, probe_x, probe_y)
                                 : std::string();

        overlay.reticle_text = controller.selectedReticleText();

        view.render(display, state, overlay, geometry);

        controller.pumpSliders();
        controller.pumpMouse();
        if (!controller.handleKey(view.pollKey(kUiPeriodMs), frame, display))
            break;
    }

    // -- Teardown ------------------------------------------------------------------------------------------------------
    // Saved unconditionally rather than on a key: a fine adjustment nobody remembered to save is an adjustment that
    // has to be done again, and rewriting the file with what is on screen is what the user was looking at.
    if (opt.reticles != "none" && view.reticles().size() > 0)
        std::cout << (controller.saveReticles() ? ("reticles saved to " + opt.reticles)
                                               : std::string("reticles could NOT be saved")) << "\n";

    model.stop();
    camera.doDisconnect();

    if (g_stop.load())
        std::cout << "\n(interrupted; the stream was stopped and the camera released cleanly)\n";
    std::cout << "stopped after sequence " << model.state().sequence << "\n";
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
