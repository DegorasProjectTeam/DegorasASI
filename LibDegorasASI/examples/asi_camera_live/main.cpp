/***********************************************************************************************************************
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
 **********************************************************************************************************************/

// ---------------------------------------------------------------------------------------------------------------------
// EXAMPLE: live view with interactive control
//
// A viewfinder: the stream in a window, and the controls that matter while pointing a telescope -- exposure, gain,
// white balance -- adjustable while watching the effect.
//
// This is an EXAMPLE, not part of the library, and that is deliberate. LibDegorasASI draws nothing and depends on no
// imaging or GUI toolkit; the point of the Frame value type is that a consumer hands its bytes to whatever it already
// uses. OpenCV appears here and nowhere else, and this example is built only when OpenCV is found -- see the
// CMakeLists in this directory. With OpenCV absent the library still builds and its tests still pass.
//
// WHAT OPENCV BUYS OVER THE SDL2 VERSION THIS REPLACES
//   * DEMOSAICING. The old viewer could only show a raw Bayer frame as grey, so RAW8/RAW16 -- the formats you
//     actually want for photometry -- looked monochrome. cvtColor does it properly, including the ROI phase
//     correction described below, which is the part that is easy to get wrong.
//   * A PERCENTILE STRETCH instead of min/max. One hot pixel at 65535 destroys a min/max stretch; a 0.5%..99.5%
//     window is what astronomy viewers use and what makes a 12-bit sky visible.
//   * A HUD and a pixel probe. putText puts the live control values on the image, and the pixel under the cursor
//     with its raw ADU is exactly what you need for focusing and for checking you are not saturating.
//   * Sliders for exposure and gain, which beats tapping a key twenty times.
//   * imwrite, so a frame can be dropped straight to PNG next to the library's own writer.
//
// KEEP IN MIND
//   * The Frame buffer is wrapped, never copied: cv::Mat views frame.data directly, so the steady state costs no
//     allocation beyond the display image.
//   * Two vendor facts, both measured on hardware and documented in frame_writer.h, make this cheap: RGB24 arrives
//     as B,G,R -- already cv::Mat 8UC3 order, no conversion at all -- and RAW16 already spans the full 16-bit range
//     on a 12-bit sensor (the SDK scales rather than shifts), so it must NOT be shifted down.
// ---------------------------------------------------------------------------------------------------------------------

// C++ INCLUDES
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// OPENCV INCLUDES
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

// PROJECT INCLUDES
#include <LibDegorasASI/Modules/Devices>
#include <LibDegorasASI/Modules/Helpers>

using namespace dpasi;
using dpasi::types::OperationResult;

// ---------------------------------------------------------------------------------------------------------------------

namespace
{

constexpr int  kMaxWindowWidth  = 1280;
constexpr int  kMaxWindowHeight = 960;
const char*    kWindow          = "LibDegorasASI - live view";
constexpr int  kExposureSlider  = 1000;      // resolution of the logarithmic exposure slider

// ---------------------------------------------------------------------------------------------------------------------
// BAYER: FROM THE SDK'S NAMING TO OPENCV'S, WITH THE ROI PHASE APPLIED
//
// This is the one place in the file where a plausible-looking line is wrong, so it is done the long way.
//
// The SDK names a pattern by its FIRST ROW: types::BayerPattern::RG means the sensor's top-left 2x2 cell reads
// R G / G B, i.e. what a datasheet calls RGGB. OpenCV names its conversion codes by the SECOND row instead --
// COLOR_BayerC1C2BGR takes C1 from row 1 column 1 and C2 from row 1 column 2 -- so the letters do NOT correspond:
//
//     SDK RG (RGGB)  ->  cv::COLOR_BayerBG2BGR      SDK GR (GRBG)  ->  cv::COLOR_BayerGB2BGR
//     SDK BG (BGGR)  ->  cv::COLOR_BayerRG2BGR      SDK GB (GBRG)  ->  cv::COLOR_BayerGR2BGR
//
// Writing COLOR_BayerRG2BGR for a pattern the SDK calls RG swaps red and blue, which on a star field looks merely
// "a bit off" rather than obviously broken -- the failure mode that cost a day when the FITS writer had it.
//
// Rather than hard-code that table, the 2x2 cell is spelled out as letters, the ROI phase is applied to it, and the
// code is derived from the result. The phase matters: the pattern is a fixed property of the SENSOR's top-left
// photosite and does not change with the ROI, but an ODD ROI origin shifts which photosite the frame starts on, and
// the vendor accepts an odd origin silently. frame_writer.cpp solves the same problem for FITS with
// mosaicOffset() = ((start % 2) + 2) % 2 -- a POSITIVE modulo -- and this must agree with it, frame for frame.

struct BayerCell
{
    char c[2][2];
};

BayerCell bayerCell(types::BayerPattern pattern)
{
    // Indexed [row][column] from the sensor's top-left photosite, using the four-letter datasheet spelling.
    switch (pattern)
    {
        case types::BayerPattern::RG: return {{{'R', 'G'}, {'G', 'B'}}};   // RGGB
        case types::BayerPattern::BG: return {{{'B', 'G'}, {'G', 'R'}}};   // BGGR
        case types::BayerPattern::GR: return {{{'G', 'R'}, {'B', 'G'}}};   // GRBG
        case types::BayerPattern::GB: return {{{'G', 'B'}, {'R', 'G'}}};   // GBRG
    }
    return {{{'R', 'G'}, {'G', 'B'}}};
}

/// Positive modulo 2 of a ROI origin: the phase the frame starts on. Mirrors mosaicOffset() in frame_writer.cpp.
int mosaicOffset(int start)
{
    return ((start % 2) + 2) % 2;
}

/// The OpenCV Bayer conversion code for this sensor pattern as seen through this frame's ROI origin, or -1 if the
/// frame carries no mosaic to undo.
int bayerCode(types::BayerPattern pattern, const types::Frame& frame)
{
    // No mosaic to undo. Binning sums neighbouring photosites, which destroys the pattern outright, and RGB24 has
    // already been demosaiced by the SDK. Same rule as the FITS writer's mosaic_gone, and the same reason.
    if (frame.bin > 1 || frame.format == types::ImageFormat::RGB24)
        return -1;

    const BayerCell cell = bayerCell(pattern);
    const int dx = mosaicOffset(frame.start_x);
    const int dy = mosaicOffset(frame.start_y);

    // The cell as the FRAME sees it, shifted by the ROI phase.
    const char c11 = cell.c[(1 + dy) % 2][(1 + dx) % 2];
    const char c12 = cell.c[(1 + dy) % 2][(2 + dx) % 2];

    // OpenCV's C1C2 are row 1, columns 1 and 2 of the shifted cell.
    if (c11 == 'B' && c12 == 'G') return cv::COLOR_BayerBG2BGR;
    if (c11 == 'R' && c12 == 'G') return cv::COLOR_BayerRG2BGR;
    if (c11 == 'G' && c12 == 'B') return cv::COLOR_BayerGB2BGR;
    if (c11 == 'G' && c12 == 'R') return cv::COLOR_BayerGR2BGR;
    return -1;
}

// ---------------------------------------------------------------------------------------------------------------------
// DISPLAY

/// Wraps the frame's bytes in a cv::Mat WITHOUT copying them. The Mat is only valid while the frame is.
cv::Mat viewFrame(types::Frame& frame)
{
    const int rows = frame.height;
    const int cols = frame.width;

    switch (frame.format)
    {
        // Already B,G,R, which is cv::Mat 8UC3 order. Nothing to convert.
        case types::ImageFormat::RGB24:
            return cv::Mat(rows, cols, CV_8UC3, frame.data.data(), static_cast<std::size_t>(cols) * 3u);

        case types::ImageFormat::RAW8:
        case types::ImageFormat::Y8:
            return cv::Mat(rows, cols, CV_8UC1, frame.data.data(), static_cast<std::size_t>(cols));

        // Little-endian 16-bit, which is what CV_16U expects on this platform, so the bytes need no swapping.
        case types::ImageFormat::RAW16:
            return cv::Mat(rows, cols, CV_16UC1, frame.data.data(), static_cast<std::size_t>(cols) * 2u);

        default:
            return cv::Mat();
    }
}

/// Maps src onto 8 bits using a percentile window, so that a few hot pixels cannot flatten the whole image.
///
/// A min/max stretch is what the SDL version did and it is fragile for exactly this reason: one stuck pixel at
/// 65535 and the sky goes black. Works on CV_8U and CV_16U, one or three channels.
void percentileStretch(const cv::Mat& src, cv::Mat& dst, double low_pct, double high_pct)
{
    const int    bins     = (src.depth() == CV_16U) ? 4096 : 256;
    const double max_val  = (src.depth() == CV_16U) ? 65536.0 : 256.0;
    const float  range[]  = {0.0f, static_cast<float>(max_val)};
    const float* ranges[] = {range};

    // The histogram is taken on a single channel: a colour image is stretched by one common transform so the white
    // balance is not silently altered by stretching each channel to its own limits.
    cv::Mat grey;
    if (src.channels() == 3)
        cv::cvtColor(src, grey, cv::COLOR_BGR2GRAY);
    else
        grey = src;

    cv::Mat hist;
    cv::calcHist(std::vector<cv::Mat>{grey}, {0}, cv::Mat(), hist, {bins}, std::vector<float>{range[0], range[1]});

    const double total = static_cast<double>(grey.total());
    const double want_lo = total * low_pct;
    const double want_hi = total * high_pct;

    double acc = 0.0;
    int bin_lo = 0;
    int bin_hi = bins - 1;
    for (int i = 0; i < bins; ++i)
    {
        acc += hist.at<float>(i);
        if (acc <= want_lo)
            bin_lo = i;
        if (acc <= want_hi)
            bin_hi = i;
    }

    const double scale = max_val / bins;
    double lo = bin_lo * scale;
    double hi = (bin_hi + 1) * scale;
    if (hi <= lo)
        hi = lo + 1.0;

    const double alpha = 255.0 / (hi - lo);
    src.convertTo(dst, CV_8U, alpha, -lo * alpha);
    (void)ranges;
}

struct ViewState
{
    bool stretch   = false;
    bool demosaic  = true;
    bool hud       = true;
    bool crosshair = false;
    int  probe_x   = -1;
    int  probe_y   = -1;
};

/// Turns a frame into the 8-bit BGR image that goes on screen.
void buildDisplay(types::Frame& frame, const ViewState& state, int bayer, cv::Mat& out)
{
    cv::Mat view = viewFrame(frame);
    if (view.empty())
    {
        out = cv::Mat();
        return;
    }

    cv::Mat colour;
    if (bayer >= 0 && state.demosaic)
    {
        // Demosaic at native depth, so a 16-bit frame keeps its dynamic range through the interpolation and only
        // loses it in the stretch below, where the loss is deliberate.
        cv::cvtColor(view, colour, bayer);
    }
    else
    {
        colour = view;
    }

    cv::Mat eight;
    if (state.stretch)
    {
        percentileStretch(colour, eight, 0.005, 0.995);
    }
    else if (colour.depth() == CV_16U)
    {
        // No stretch on a 16-bit frame means the top 8 bits, which on a 12-bit sensor is a nearly black window.
        // It is offered anyway because it is the only view that shows the raw scale honestly.
        colour.convertTo(eight, CV_8U, 1.0 / 257.0);
    }
    else
    {
        eight = colour;
    }

    if (eight.channels() == 1)
        cv::cvtColor(eight, out, cv::COLOR_GRAY2BGR);
    else
        out = eight;
}

// ---------------------------------------------------------------------------------------------------------------------
// HUD

std::string fixed1(double v)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << v;
    return os.str();
}

struct Hud
{
    long long exposure_us = 0;
    long long gain        = 0;
    long long wb_red      = 0;
    long long wb_blue     = 0;
    bool      has_wb      = false;
    double    fps         = 0.0;
    int       dropped     = 0;
    std::uint64_t sequence = 0;
    std::string   bayer_note;
    std::string   probe;
};

void drawHud(cv::Mat& img, const Hud& hud, const ViewState& state)
{
    if (img.empty())
        return;

    std::vector<std::string> lines;
    lines.push_back("exp " + fixed1(hud.exposure_us / 1000.0) + " ms    gain " + std::to_string(hud.gain) +
                    (hud.has_wb ? ("    WB r" + std::to_string(hud.wb_red) + " b" + std::to_string(hud.wb_blue))
                                : std::string()));
    lines.push_back(fixed1(hud.fps) + " fps    dropped " + std::to_string(hud.dropped) +
                    "    seq " + std::to_string(hud.sequence));
    lines.push_back(std::string("stretch ") + (state.stretch ? "on " : "off") + "    " + hud.bayer_note);
    if (!hud.probe.empty())
        lines.push_back(hud.probe);

    const int    thickness = 1;
    const double scale     = 0.45;
    const int    line_h    = 18;
    const int    pad       = 6;

    int width = 0;
    for (const std::string& l : lines)
    {
        int base = 0;
        const cv::Size s = cv::getTextSize(l, cv::FONT_HERSHEY_SIMPLEX, scale, thickness, &base);
        width = std::max(width, s.width);
    }

    // A dark plate behind the text, blended rather than opaque, so the HUD never hides the thing being looked at.
    const cv::Rect plate(4, 4, std::min(width + 2 * pad, img.cols - 8),
                         static_cast<int>(lines.size()) * line_h + pad);
    if (plate.width > 0 && plate.height > 0 && plate.br().x <= img.cols && plate.br().y <= img.rows)
    {
        cv::Mat roi = img(plate);
        roi *= 0.35;
    }

    int y = 4 + line_h - 4;
    for (const std::string& l : lines)
    {
        cv::putText(img, l, cv::Point(4 + pad, y), cv::FONT_HERSHEY_SIMPLEX, scale,
                    cv::Scalar(80, 255, 80), thickness, cv::LINE_AA);
        y += line_h;
    }

    if (state.crosshair)
    {
        const cv::Point c(img.cols / 2, img.rows / 2);
        const cv::Scalar col(80, 255, 255);
        cv::line(img, cv::Point(c.x - 25, c.y), cv::Point(c.x + 25, c.y), col, 1);
        cv::line(img, cv::Point(c.x, c.y - 25), cv::Point(c.x, c.y + 25), col, 1);
        cv::circle(img, c, 40, col, 1);
    }
}

/// Reads the pixel under the cursor at NATIVE depth, which is the number that matters for exposure and focus:
/// the displayed 8-bit value has already been through the stretch.
std::string probeText(types::Frame& frame, int x, int y)
{
    if (x < 0 || y < 0 || x >= frame.width || y >= frame.height)
        return std::string();

    cv::Mat view = viewFrame(frame);
    if (view.empty())
        return std::string();

    std::ostringstream os;
    os << "(" << x << "," << y << ") ";
    if (view.type() == CV_16UC1)
        os << "raw " << view.at<std::uint16_t>(y, x) << " / 65535";
    else if (view.type() == CV_8UC1)
        os << "raw " << static_cast<int>(view.at<std::uint8_t>(y, x)) << " / 255";
    else if (view.type() == CV_8UC3)
    {
        const cv::Vec3b p = view.at<cv::Vec3b>(y, x);
        os << "B" << int(p[0]) << " G" << int(p[1]) << " R" << int(p[2]);
    }
    return os.str();
}

// ---------------------------------------------------------------------------------------------------------------------
// CAMERA CONTROL

/// Reads a control, applies a delta, clamps to what the camera accepts, writes it and reports what actually took.
long long nudge(AsiCamera& camera, types::ControlType control, const char* label, long long delta, bool absolute)
{
    if (!camera.hasControl(control) || !camera.isControlWritable(control))
    {
        std::cout << "  " << label << ": not adjustable on this camera\n";
        return -1;
    }

    types::ControlValue value;
    if (camera.getControl(control, value) != OperationResult::OPERATION_OK)
    {
        std::cout << "  " << label << ": could not be read\n";
        return -1;
    }

    types::ControlCaps caps;
    camera.getControlCaps(control, caps);

    const long long before = value.value;
    value.value   = std::clamp<long long>(absolute ? delta : before + delta, caps.min_value, caps.max_value);
    value.is_auto = false;

    if (camera.doSetControl(control, value) != OperationResult::OPERATION_OK)
    {
        std::cout << "  " << label << ": write refused\n";
        return -1;
    }

    // Read back rather than trust the write: the SDK clamps silently, so what was asked for and what took effect
    // are not always the same number.
    types::ControlValue applied;
    camera.getControl(control, applied);
    if (!absolute)
    {
        std::cout << "  " << label << ": " << before << " -> " << applied.value
                  << "  [" << caps.min_value << ".." << caps.max_value << "]\n";
    }
    return applied.value;
}

/// State the trackbar callbacks need. A plain struct behind a file-scope pointer, because highgui callbacks are C
/// function pointers with a void* payload and there is exactly one window.
struct Ui
{
    AsiCamera* camera = nullptr;
    ViewState* state  = nullptr;
    long long  exp_min = 1;
    long long  exp_max = 1;
    bool       suppress = false;    // set while WE move a slider, so the callback does not fight the user
    long long  pending_exposure = -1;
    long long  pending_gain     = -1;
};

Ui g_ui;

/// Exposure spans microseconds to seconds, so a linear slider is useless: the whole usable range of a bright target
/// sits in the first pixel. Mapped logarithmically instead.
long long sliderToExposure(int pos)
{
    const double t = static_cast<double>(pos) / kExposureSlider;
    const double lo = std::log(static_cast<double>(std::max<long long>(1, g_ui.exp_min)));
    const double hi = std::log(static_cast<double>(std::max<long long>(2, g_ui.exp_max)));
    return static_cast<long long>(std::exp(lo + t * (hi - lo)));
}

int exposureToSlider(long long us)
{
    const double lo = std::log(static_cast<double>(std::max<long long>(1, g_ui.exp_min)));
    const double hi = std::log(static_cast<double>(std::max<long long>(2, g_ui.exp_max)));
    const double t  = (std::log(static_cast<double>(std::max<long long>(1, us))) - lo) / (hi - lo);
    return std::clamp(static_cast<int>(t * kExposureSlider), 0, kExposureSlider);
}

void onExposureSlider(int pos, void*)
{
    if (g_ui.suppress)
        return;
    g_ui.pending_exposure = sliderToExposure(pos);
}

void onGainSlider(int pos, void*)
{
    if (g_ui.suppress)
        return;
    g_ui.pending_gain = pos;
}

void onMouse(int event, int x, int y, int, void*)
{
    if (!g_ui.state)
        return;
    if (event == cv::EVENT_MOUSEMOVE)
    {
        g_ui.state->probe_x = x;
        g_ui.state->probe_y = y;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// COMMAND LINE

struct Options
{
    int exposure_ms = 30;
    int gain        = 200;
    int bin         = 1;
    int camera      = -1;
    bool demosaic   = true;
    bool stretch_set = false;
    bool stretch    = false;
    int  snap       = 0;               // >0: grab this many frames, write the last one, exit. No window.
    std::string snap_name = "live_snap";
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
        "  A              toggle the percentile auto-stretch\n"
        "  D              toggle demosaicing (raw Bayer <-> colour)\n"
        "  I              toggle the HUD\n"
        "  X              toggle the centre crosshair\n"
        "  S              save the frame through the library's own writer (FITS/BMP, full depth)\n"
        "  P              save what is on screen as PNG (8-bit, stretched -- for a quick look)\n"
        "  H              print these keys again\n"
        "Sliders: exposure (logarithmic) and gain. Hover the image to read the pixel under the cursor.\n";
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
        "\n";
    printKeys();
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
        else if (arg == "--no-demosaic")            { opt.demosaic = false; }
        else if (arg == "--stretch")                { opt.stretch_set = true; opt.stretch = true; }
        else if (arg == "--no-stretch")             { opt.stretch_set = true; opt.stretch = false; }
        else if (arg == "--snap")                   { if (!next(opt.snap)) return false; }
        else if (arg == "--snap-name" && i + 1 < argc) { opt.snap_name = argv[++i]; }
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

    types::ControlValue gain_value;
    gain_value.value   = opt.gain;
    gain_value.is_auto = false;
    camera.doSetControl(types::ControlType::GAIN, gain_value);

    types::RoiFormat live;
    camera.getRoiFormat(live);
    std::cout << "streaming " << live.width << "x" << live.height << " bin" << live.bin
              << " " << types::toString(live.format) << "\n";

    // -- Stream --------------------------------------------------------------------------------------------------------
    if (camera.doStartVideoCapture() != OperationResult::OPERATION_OK)
    {
        std::cout << "Could not start streaming.\n";
        camera.doDisconnect();
        return 1;
    }

    // ONE frame and ONE display image for the whole run. The frame's buffer is reallocated only when the geometry
    // changes, and the display image settles after the first iteration, so a steady stream costs no allocation.
    types::Frame frame;
    cv::Mat display;

    ViewState state;
    state.demosaic = opt.demosaic;
    state.stretch  = opt.stretch_set ? opt.stretch : (format == types::ImageFormat::RAW16);

    g_ui.camera = &camera;
    g_ui.state  = &state;

    // HEADLESS. --snap exists so the example can be exercised without a human: it grabs a few frames, so that
    // auto-anything has time to settle, writes the result and exits. It is also the only way to VERIFY the
    // demosaic mapping, by capturing the same scene as RGB24 and as demosaiced RAW and comparing the channels.
    const bool headless = (opt.snap > 0);

    const bool has_wb = desc.is_colour && camera.hasControl(types::ControlType::WB_RED);

    if (!headless)
    {
        // -- Window --------------------------------------------------------------------------------------------------------
        // WINDOW_NORMAL so the window can be resized and the frame scaled to it; WINDOW_AUTOSIZE would open a
        // sensor-sized window, which on a 1304x976 camera is fine but on a larger one runs off the screen.
        cv::namedWindow(kWindow, cv::WINDOW_NORMAL | cv::WINDOW_KEEPRATIO);
        cv::resizeWindow(kWindow, std::min(live.width, kMaxWindowWidth), std::min(live.height, kMaxWindowHeight));
        cv::setMouseCallback(kWindow, onMouse, nullptr);

        types::ControlCaps exp_caps;
        types::ControlCaps gain_caps;
        if (camera.hasControl(types::ControlType::EXPOSURE) &&
            camera.getControlCaps(types::ControlType::EXPOSURE, exp_caps) == OperationResult::OPERATION_OK)
        {
            g_ui.exp_min = exp_caps.min_value;
            g_ui.exp_max = exp_caps.max_value;
            types::ControlValue now;
            camera.getControl(types::ControlType::EXPOSURE, now);
            // nullptr for the value pointer, not &int: highgui deprecated that form because it writes to the int
            // from the GUI thread with no synchronisation. The callback already carries the position.
            cv::createTrackbar("exposure (log)", kWindow, nullptr, kExposureSlider, onExposureSlider);
            g_ui.suppress = true;
            cv::setTrackbarPos("exposure (log)", kWindow, exposureToSlider(now.value));
            g_ui.suppress = false;
        }
        if (camera.hasControl(types::ControlType::GAIN) &&
            camera.getControlCaps(types::ControlType::GAIN, gain_caps) == OperationResult::OPERATION_OK)
        {
            types::ControlValue now;
            camera.getControl(types::ControlType::GAIN, now);
            cv::createTrackbar("gain", kWindow, nullptr, static_cast<int>(gain_caps.max_value), onGainSlider);
            g_ui.suppress = true;
            cv::setTrackbarPos("gain", kWindow, static_cast<int>(now.value));
            g_ui.suppress = false;
        }


        std::cout << "\n";
        printKeys();
        std::cout << "\n";
    }

    bool running = true;
    int  shots   = 0;
    Hud  hud;
    hud.has_wb = has_wb;

    auto last_report = std::chrono::steady_clock::now();
    auto last_poll   = last_report;
    int  frames_since_report = 0;

    while (running)
    {
        // -- Frame ---------------------------------------------------------------------------------------------------
        const OperationResult res = camera.doGetVideoFrame(frame, std::chrono::milliseconds(2000));
        if (res == OperationResult::OPERATION_TIMEOUT)
        {
            std::cout << "  (no frame in 2 s; is the exposure very long?)\n";
            if (cv::waitKey(1) == 'q')
                break;
            continue;
        }
        if (res != OperationResult::OPERATION_OK)
        {
            std::cout << "  frame retrieval failed: " << types::toString(res) << "\n";
            break;
        }

        // The pattern is a sensor property; the CODE depends on this frame's ROI phase, so it is derived per frame.
        const int bayer = desc.is_colour ? bayerCode(desc.bayer_pattern, frame) : -1;

        buildDisplay(frame, state, bayer, display);
        if (display.empty())
        {
            std::cout << "  this format cannot be displayed.\n";
            break;
        }

        // -- Pending slider writes -----------------------------------------------------------------------------------
        // Applied here rather than inside the callback: a callback runs on the GUI thread, and writing a control
        // from there while the capture thread is mid-transfer is exactly the kind of race the library's lock
        // ordering exists to avoid. Deferring keeps every camera call on this thread.
        if (g_ui.pending_exposure >= 0)
        {
            nudge(camera, types::ControlType::EXPOSURE, "exposure (us)", g_ui.pending_exposure, true);
            g_ui.pending_exposure = -1;
        }
        if (g_ui.pending_gain >= 0)
        {
            nudge(camera, types::ControlType::GAIN, "gain", g_ui.pending_gain, true);
            g_ui.pending_gain = -1;
        }

        // -- HUD -----------------------------------------------------------------------------------------------------
        const auto now = std::chrono::steady_clock::now();
        ++frames_since_report;

        // Control reads are USB round trips, so they are polled at 4 Hz rather than per frame.
        if (now - last_poll >= std::chrono::milliseconds(250))
        {
            types::ControlValue v;
            if (camera.getControl(types::ControlType::EXPOSURE, v) == OperationResult::OPERATION_OK)
                hud.exposure_us = v.value;
            if (camera.getControl(types::ControlType::GAIN, v) == OperationResult::OPERATION_OK)
                hud.gain = v.value;
            if (has_wb)
            {
                if (camera.getControl(types::ControlType::WB_RED, v) == OperationResult::OPERATION_OK)
                    hud.wb_red = v.value;
                if (camera.getControl(types::ControlType::WB_BLUE, v) == OperationResult::OPERATION_OK)
                    hud.wb_blue = v.value;
            }
            camera.getDroppedFrames(hud.dropped);
            last_poll = now;
        }

        hud.sequence = frame.sequence;
        hud.bayer_note = (bayer >= 0)
            ? (state.demosaic ? ("demosaic " + types::toString(desc.bayer_pattern)) : "raw mosaic")
            : (frame.bin > 1 ? "bin>1: no mosaic" : "no mosaic");

        // The probe coordinates arrive in WINDOW pixels; with WINDOW_NORMAL the window is scaled, so they are mapped
        // back to frame coordinates. Without this the reported pixel is wrong whenever the window is not 1:1.
        hud.probe.clear();
        if (state.probe_x >= 0)
        {
            const cv::Rect vis = cv::getWindowImageRect(kWindow);
            if (vis.width > 0 && vis.height > 0)
            {
                const int fx = static_cast<int>(static_cast<double>(state.probe_x) * frame.width  / vis.width);
                const int fy = static_cast<int>(static_cast<double>(state.probe_y) * frame.height / vis.height);
                hud.probe = probeText(frame, fx, fy);
            }
        }

        if (now - last_report >= std::chrono::seconds(2))
        {
            const double secs = std::chrono::duration<double>(now - last_report).count();
            hud.fps = frames_since_report / secs;
            std::cout << "  " << fixed1(hud.fps) << " fps, " << hud.dropped << " dropped, sequence "
                      << frame.sequence << "\n";
            frames_since_report = 0;
            last_report = now;
        }

        // Not in headless mode: the HUD is green text, and it would contaminate the channel means that are
        // the whole point of a snapshot.
        if (state.hud && !headless)
            drawHud(display, hud, state);

        // -- Headless snapshot ---------------------------------------------------------------------------------------
        if (headless)
        {
            if (static_cast<int>(frame.sequence) < opt.snap)
                continue;

            const std::string png = opt.snap_name + ".png";
            const std::string raw = opt.snap_name + "." + imgio::extensionFor(frame.format);
            const bool png_ok = cv::imwrite(png, display);
            const bool raw_ok = imgio::writeFrame(frame, raw);

            // The means are the point of this mode: an RGB24 capture and a demosaiced RAW capture of the same
            // scene must agree on which channel is which. If the Bayer code were wrong, B and R would swap.
            const cv::Scalar mean = cv::mean(display);
            std::cout << "snap: " << frame.width << "x" << frame.height << " " << types::toString(frame.format)
                      << "  mean B=" << std::fixed << std::setprecision(1) << mean[0]
                      << " G=" << mean[1] << " R=" << mean[2]
                      << "  " << hud.bayer_note << "\n";
            std::cout << "  " << (png_ok ? png : std::string("PNG WRITE FAILED"))
                      << "  " << (raw_ok ? raw : std::string("RAW WRITE FAILED")) << "\n";
            return (png_ok && raw_ok) ? 0 : 1;
        }

        cv::imshow(kWindow, display);

        // -- Input ---------------------------------------------------------------------------------------------------
        // waitKey(1) is also what pumps the GUI event loop, so it must be called every iteration even when no key
        // is pressed -- without it the window never repaints and the sliders never move.
        const int key = cv::waitKey(1);
        switch (key)
        {
            case -1: break;
            case 27:
            case 'q':
            case 'Q': running = false; break;
            case 'h':
            case 'H': printKeys(); break;

            case 'a':
            case 'A':
                state.stretch = !state.stretch;
                std::cout << "  auto-stretch: " << (state.stretch ? "on" : "off") << "\n";
                break;

            case 'd':
            case 'D':
                state.demosaic = !state.demosaic;
                std::cout << "  demosaic: " << (state.demosaic ? "on" : "off")
                          << (bayer < 0 ? "  (this frame carries no mosaic anyway)" : "") << "\n";
                break;

            case 'i':
            case 'I': state.hud = !state.hud; break;

            case 'x':
            case 'X': state.crosshair = !state.crosshair; break;

            case 'e':
            case 'E':
            {
                types::ControlValue exposure;
                if (camera.getControl(types::ControlType::EXPOSURE, exposure) == OperationResult::OPERATION_OK)
                {
                    const long long factor = (key == 'E') ? 11 : 9;
                    const long long target = std::max<long long>(1, exposure.value * factor / 10);
                    const long long applied = nudge(camera, types::ControlType::EXPOSURE, "exposure (us)",
                                                    target, true);
                    if (applied > 0)
                    {
                        // Move the slider to match, with the callback suppressed so it does not write it back.
                        g_ui.suppress = true;
                        cv::setTrackbarPos("exposure (log)", kWindow, exposureToSlider(applied));
                        g_ui.suppress = false;
                    }
                }
                break;
            }

            case 'g':
            case 'G':
            {
                const long long applied = nudge(camera, types::ControlType::GAIN, "gain",
                                                (key == 'G') ? 10 : -10, false);
                if (applied >= 0)
                {
                    g_ui.suppress = true;
                    cv::setTrackbarPos("gain", kWindow, static_cast<int>(applied));
                    g_ui.suppress = false;
                }
                break;
            }

            case 'r': nudge(camera, types::ControlType::WB_RED,  "WB red",   -1, false); break;
            case 'R': nudge(camera, types::ControlType::WB_RED,  "WB red",    1, false); break;
            case 'b': nudge(camera, types::ControlType::WB_BLUE, "WB blue",  -1, false); break;
            case 'B': nudge(camera, types::ControlType::WB_BLUE, "WB blue",   1, false); break;

            case 's':
            case 'S':
            {
                // Saved through the library's own writer, not through OpenCV: it is the one that knows the vendor's
                // channel order and the mosaic phase, it keeps full depth, and it keeps these files identical to
                // the other examples'.
                const std::string path = "live_" + std::to_string(++shots) + "." + imgio::extensionFor(frame.format);
                std::cout << "  saved: " << (imgio::writeFrame(frame, path) ? path : "WRITE FAILED") << "\n";
                break;
            }

            case 'p':
            case 'P':
            {
                // The screen image, stretch and all. Deliberately NOT a substitute for 'S': this is 8-bit and
                // already transformed, so it is a look, not data.
                const std::string path = "live_view_" + std::to_string(++shots) + ".png";
                std::cout << "  " << (cv::imwrite(path, display) ? ("saved: " + path) : "PNG WRITE FAILED") << "\n";
                break;
            }

            default: break;
        }

        // Closing the window with its own button is the other way out, and it must be honoured or the loop keeps
        // running with nowhere to draw.
        if (running && cv::getWindowProperty(kWindow, cv::WND_PROP_VISIBLE) < 1.0)
            running = false;
    }

    // -- Teardown ------------------------------------------------------------------------------------------------------
    camera.doStopVideoCapture();
    camera.doDisconnect();
    if (!headless)
    {
        cv::destroyAllWindows();
        cv::waitKey(1);   // lets highgui actually tear the window down before the process exits
    }

    std::cout << "\nstopped after sequence " << frame.sequence << "\n";
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
