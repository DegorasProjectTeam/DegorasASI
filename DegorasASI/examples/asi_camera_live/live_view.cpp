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
#include <cmath>
#include <iomanip>
#include <sstream>
#include <vector>

// OPENCV INCLUDES
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

// PROJECT INCLUDES
#include "live_view.h"


// NAMESPACES
namespace live
{

namespace
{

// ---------------------------------------------------------------------------------------------------------------------
// HIGHGUI'S C CALLBACKS, AND THE ONE PIECE OF FILE-SCOPE STATE THEY FORCE
//
// createTrackbar and setMouseCallback take a plain function pointer. The userdata parameter exists, but the trackbar
// callback in particular is invoked from highgui's own thread and the value-pointer form of the API is deprecated, so
// the least bad arrangement is a small block of state here that the callbacks write and the view reads. There is
// exactly one window in this example, which is what makes that honest rather than a shortcut; a second window would
// need this turned into a per-window structure passed as userdata.
//
// Everything here is written by the GUI thread and read by the main thread, which for these plain scalars is what the
// view's take*Request() methods serialise: they read and clear the flag in one call, and a movement lost to a race
// would only mean the slider is read one frame later.

struct SliderState
{
    int exposure_pos = 0;
    int gain_pos = 0;
    bool exposure_moved = false;
    bool gain_moved = false;
    bool suppress = false;      ///< Set while the VIEW moves a slider, so its own movement is not read back.
    int mouse_x = -1;
    int mouse_y = -1;
    bool button_held = false;    ///< True between press and release, which is what makes a drag a drag.
    bool press_pending = false;  ///< A press not yet consumed by the controller.
    int press_x = -1;
    int press_y = -1;
    long long exposure_min = 1;
    long long exposure_max = 1;
};

SliderState& sliders()
{
    static SliderState state;
    return state;
}

/// Resolution of the logarithmic exposure slider. Fine enough that a step is imperceptible across the whole range.
constexpr int kExposureSteps = 1000;

/// Shortest gap between two compositions. 20 Hz is smooth for a progress bar and cheap; the input rate is separate.
constexpr double kRenderPeriodMs = 50.0;

// Fractional bits for OpenCV fixed-point drawing. cv::line and cv::circle take integer coordinates plus a shift, so a
// reticle placed at 512.30 is drawn there instead of being snapped to 512 -- which is the whole reason the position is
// stored as a double. Four bits is a sixteenth of a pixel, finer than the tenth the keyboard offers.
constexpr int kSubPixelShift = 4;

int toFixed(double value)
{
    return static_cast<int>(std::lround(value * (1 << kSubPixelShift)));
}

double millisSinceRender(const std::chrono::steady_clock::time_point& then)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - then).count();
}

long long sliderToExposure(int pos)
{
    const SliderState& s = sliders();
    const double lo = std::log(static_cast<double>(std::max<long long>(1, s.exposure_min)));
    const double hi = std::log(static_cast<double>(std::max<long long>(2, s.exposure_max)));
    const double t = static_cast<double>(pos) / kExposureSteps;
    return static_cast<long long>(std::exp(lo + t * (hi - lo)));
}

int exposureToSlider(long long microseconds)
{
    const SliderState& s = sliders();
    const double lo = std::log(static_cast<double>(std::max<long long>(1, s.exposure_min)));
    const double hi = std::log(static_cast<double>(std::max<long long>(2, s.exposure_max)));
    const double t = (std::log(static_cast<double>(std::max<long long>(1, microseconds))) - lo) / (hi - lo);
    return std::clamp(static_cast<int>(t * kExposureSteps), 0, kExposureSteps);
}

void onExposureSlider(int pos, void*)
{
    SliderState& s = sliders();
    if (s.suppress)
        return;
    s.exposure_pos = pos;
    s.exposure_moved = true;
}

void onGainSlider(int pos, void*)
{
    SliderState& s = sliders();
    if (s.suppress)
        return;
    s.gain_pos = pos;
    s.gain_moved = true;
}

void onMouse(int event, int x, int y, int, void*)
{
    SliderState& s = sliders();

    // The position is tracked on every event, not only on a move: a press arriving without a preceding move -- which
    // happens when the pointer enters the window already down -- would otherwise be placed at a stale position.
    s.mouse_x = x;
    s.mouse_y = y;

    if (event == cv::EVENT_LBUTTONDOWN)
    {
        s.button_held = true;
        s.press_pending = true;
        s.press_x = x;
        s.press_y = y;
    }
    else if (event == cv::EVENT_LBUTTONUP)
    {
        s.button_held = false;
    }
}

std::string fixed1(double v)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << v;
    return os.str();
}

}   // namespace

// ---------------------------------------------------------------------------------------------------------------------

Overlay::Overlay() :
    bayer_note(),
    probe_text(),
    reticle_text()
{
}

// ---------------------------------------------------------------------------------------------------------------------

LiveView::LiveView(const std::string& title, int frame_width, int frame_height) :
    title_(title),
    frame_width_(frame_width),
    frame_height_(frame_height),
    open_(false),
    has_exposure_slider_(false),
    has_gain_slider_(false),
    exposure_min_(1),
    exposure_max_(1),
    gain_max_(0),
    options_(),
    show_hud_(true),
    show_reticles_(true),
    reticles_(),
    canvas_(),
    last_render_(std::chrono::steady_clock::now()),
    last_drawn_sequence_(0)
{
}

LiveView::~LiveView()
{
    // Guarded for the same reason isOpen() is, and additionally because a destructor must not let an exception out.
    try
    {
        if (this->open_)
        {
            cv::destroyAllWindows();
            // Gives highgui a turn to actually tear the window down before the process exits.
            cv::waitKey(1);
        }
    }
    catch (const cv::Exception&)
    {
    }
}

// ---------------------------------------------------------------------------------------------------------------------

bool LiveView::open(int max_width, int max_height)
{
    // WINDOW_NORMAL so it can be resized and the frame scaled to fit; WINDOW_AUTOSIZE would open a sensor-sized
    // window, which runs off the screen on a large sensor.
    cv::namedWindow(this->title_, cv::WINDOW_NORMAL | cv::WINDOW_KEEPRATIO);
    cv::resizeWindow(this->title_, std::min(this->frame_width_, max_width),
                     std::min(this->frame_height_, max_height));
    cv::setMouseCallback(this->title_, onMouse, nullptr);
    this->open_ = true;
    return true;
}

bool LiveView::isOpen() const
{
    if (!this->open_)
        return false;

    // Every highgui call here is guarded, because the window can vanish between two of them: the user's click on the
    // close button is processed inside waitKey(), so by the time this runs the window may already be gone and
    // getWindowProperty() then THROWS rather than returning false. An uncaught cv::Exception on the way out of the
    // loop was crashing the process with a fail-fast (0xC0000409) instead of exiting cleanly.
    try
    {
        if (cv::getWindowProperty(this->title_, cv::WND_PROP_VISIBLE) >= 1.0)
            return true;
    }
    catch (const cv::Exception&)
    {
    }

    // Latched: once it is gone it stays gone, so nothing tries to draw into it again.
    this->open_ = false;
    return false;
}

void LiveView::addExposureSlider(long long minimum_us, long long maximum_us, long long current_us)
{
    this->exposure_min_ = minimum_us;
    this->exposure_max_ = maximum_us;
    sliders().exposure_min = minimum_us;
    sliders().exposure_max = maximum_us;

    // nullptr for the value pointer, not the address of an int: highgui deprecated that form because it writes to
    // the int from the GUI thread with no synchronisation. The callback already carries the position.
    cv::createTrackbar("exposure (log)", this->title_, nullptr, kExposureSteps, onExposureSlider);
    sliders().suppress = true;
    cv::setTrackbarPos("exposure (log)", this->title_, exposureToSlider(current_us));
    sliders().suppress = false;
    this->has_exposure_slider_ = true;
}

void LiveView::addGainSlider(long long maximum, long long current)
{
    this->gain_max_ = maximum;
    cv::createTrackbar("gain", this->title_, nullptr, static_cast<int>(maximum), onGainSlider);
    sliders().suppress = true;
    cv::setTrackbarPos("gain", this->title_, static_cast<int>(current));
    sliders().suppress = false;
    this->has_gain_slider_ = true;
}

void LiveView::syncSliders(const ModelState& state)
{
    SliderState& s = sliders();

    // Suppressed for the duration: setTrackbarPos fires the callback, and without this the view's own correction
    // would come straight back as a user request and fight whatever the user is doing.
    s.suppress = true;
    if (this->has_exposure_slider_ && !s.exposure_moved)
        cv::setTrackbarPos("exposure (log)", this->title_, exposureToSlider(state.exposure_us));
    if (this->has_gain_slider_ && !s.gain_moved)
        cv::setTrackbarPos("gain", this->title_, static_cast<int>(state.gain));
    s.suppress = false;
}

// ---------------------------------------------------------------------------------------------------------------------

const cv::Mat& LiveView::compose(const cv::Mat& image, const ModelState& state, const Overlay& overlay)
{
    if (image.empty())
    {
        // Before the first frame there is nothing to show, but the window must still exist and the progress bar must
        // still move, so a black canvas of the right size stands in.
        this->canvas_.create(this->frame_height_, this->frame_width_, CV_8UC3);
        this->canvas_.setTo(cv::Scalar(20, 20, 20));
    }
    else
    {
        // Copied, not drawn on: the caller keeps showing the same image while no new frame arrives, and drawing the
        // overlay onto it would accumulate a new HUD on top of the old one every iteration.
        image.copyTo(this->canvas_);
    }

    if (this->show_hud_)
        this->drawHud(this->canvas_, state, overlay);
    if (this->show_reticles_)
        this->drawReticles(this->canvas_);
    if (state.progress_is_useful)
        this->drawProgress(this->canvas_, state);

    return this->canvas_;
}

void LiveView::render(const cv::Mat& image, const ModelState& state, const Overlay& overlay)
{
    if (!this->open_)
        return;

    // THROTTLED, because composing a frame is not free: a full-frame copy, the HUD, and an imshow that converts and
    // scales the whole image. At the loop's input rate that was one core permanently busy redrawing a picture that
    // had not changed. Keys are still polled every iteration -- responsiveness comes from waitKey(), not from this --
    // and the only thing that needs redrawing between frames is the progress bar, which nobody can see move faster
    // than this anyway.
    const bool forced = (state.sequence != this->last_drawn_sequence_);
    if (!forced && millisSinceRender(this->last_render_) < kRenderPeriodMs)
        return;
    this->last_render_ = std::chrono::steady_clock::now();
    this->last_drawn_sequence_ = state.sequence;

    cv::imshow(this->title_, this->compose(image, state, overlay));
}

int LiveView::frameWidth() const
{
    return this->frame_width_;
}

int LiveView::frameHeight() const
{
    return this->frame_height_;
}

int LiveView::pollKey(int wait_ms)
{
    if (!this->open_)
        return -1;
    try
    {
        // waitKeyEx rather than waitKey: the plain one truncates to the low byte, which throws away the arrow keys.
        // Reticle nudging wants them, so the full code is returned and the controller decides what it recognises.
        return cv::waitKeyEx(std::max(1, wait_ms));
    }
    catch (const cv::Exception&)
    {
        this->open_ = false;
        return -1;
    }
}

// ---------------------------------------------------------------------------------------------------------------------

bool LiveView::takeExposureRequest(long long& microseconds)
{
    SliderState& s = sliders();
    if (!s.exposure_moved)
        return false;
    s.exposure_moved = false;
    microseconds = sliderToExposure(s.exposure_pos);
    return true;
}

bool LiveView::takeGainRequest(long long& value)
{
    SliderState& s = sliders();
    if (!s.gain_moved)
        return false;
    s.gain_moved = false;
    value = s.gain_pos;
    return true;
}

bool LiveView::probePointInFrame(int& x, int& y) const
{
    const SliderState& s = sliders();
    if (s.mouse_x < 0 || s.mouse_y < 0)
        return false;
    return this->windowToFrame(s.mouse_x, s.mouse_y, x, y);
}

bool LiveView::windowToFrame(int window_x, int window_y, int& x, int& y) const
{
    if (!this->open_)
        return false;

    // Window pixels are not frame pixels: with WINDOW_NORMAL the image is scaled to whatever size the window was
    // dragged to. Every position arriving from highgui goes through here, so a click and the pixel probe agree, and a
    // reticle lands on the photosite that was actually clicked.
    cv::Rect visible;
    try
    {
        visible = cv::getWindowImageRect(this->title_);
    }
    catch (const cv::Exception&)
    {
        return false;
    }
    if (visible.width <= 0 || visible.height <= 0)
        return false;

    x = static_cast<int>(static_cast<double>(window_x) * this->frame_width_ / visible.width);
    y = static_cast<int>(static_cast<double>(window_y) * this->frame_height_ / visible.height);
    return x >= 0 && y >= 0 && x < this->frame_width_ && y < this->frame_height_;
}

// ---------------------------------------------------------------------------------------------------------------------

DisplayOptions& LiveView::displayOptions()
{
    return this->options_;
}

void LiveView::toggleStretch()
{
    this->options_.stretch = !this->options_.stretch;
}

void LiveView::toggleDemosaic()
{
    this->options_.demosaic = !this->options_.demosaic;
}

void LiveView::toggleHud()
{
    this->show_hud_ = !this->show_hud_;
}

void LiveView::toggleReticles()
{
    this->show_reticles_ = !this->show_reticles_;
}

bool LiveView::reticlesVisible() const
{
    return this->show_reticles_;
}

ReticleSet& LiveView::reticles()
{
    return this->reticles_;
}

bool LiveView::takeMousePressInFrame(int& x, int& y)
{
    SliderState& s = sliders();
    if (!s.press_pending)
        return false;
    s.press_pending = false;
    return this->windowToFrame(s.press_x, s.press_y, x, y);
}

bool LiveView::mouseHeld() const
{
    return sliders().button_held;
}

bool LiveView::stretchEnabled() const
{
    return this->options_.stretch;
}

bool LiveView::demosaicEnabled() const
{
    return this->options_.demosaic;
}

// ---------------------------------------------------------------------------------------------------------------------

void LiveView::drawHud(cv::Mat& image, const ModelState& state, const Overlay& overlay) const
{
    std::vector<std::string> lines;

    std::string first = "exp " + fixed1(state.exposure_us / 1000.0) + " ms    gain " + std::to_string(state.gain);
    if (state.has_wb)
        first += "    WB r" + std::to_string(state.wb_red) + " b" + std::to_string(state.wb_blue);
    lines.push_back(first);

    lines.push_back(fixed1(state.fps) + " fps    dropped " + std::to_string(state.dropped) +
                    "    seq " + std::to_string(state.sequence));
    lines.push_back(std::string("stretch ") + (this->options_.stretch ? "on " : "off") + "    " + overlay.bayer_note);

    if (!overlay.reticle_text.empty())
        lines.push_back(overlay.reticle_text);
    if (!overlay.probe_text.empty())
        lines.push_back(overlay.probe_text);
    if (!state.last_error.empty())
        lines.push_back("last error: " + state.last_error);

    const int    thickness = 1;
    const double scale     = 0.45;
    const int    line_h    = 18;
    const int    pad       = 6;

    int width = 0;
    for (const std::string& line : lines)
    {
        int base = 0;
        const cv::Size size = cv::getTextSize(line, cv::FONT_HERSHEY_SIMPLEX, scale, thickness, &base);
        width = std::max(width, size.width);
    }

    // A dark plate behind the text, dimmed rather than opaque, so the HUD never hides the thing being looked at.
    const cv::Rect plate(4, 4, std::min(width + 2 * pad, image.cols - 8),
                         static_cast<int>(lines.size()) * line_h + pad);
    if (plate.width > 0 && plate.height > 0 && plate.br().x <= image.cols && plate.br().y <= image.rows)
    {
        cv::Mat roi = image(plate);
        roi *= 0.35;
    }

    int y = 4 + line_h - 4;
    for (const std::string& line : lines)
    {
        cv::putText(image, line, cv::Point(4 + pad, y), cv::FONT_HERSHEY_SIMPLEX, scale,
                    cv::Scalar(80, 255, 80), thickness, cv::LINE_AA);
        y += line_h;
    }
}

void LiveView::drawProgress(cv::Mat& image, const ModelState& state) const
{
    // Only drawn when the model says the interval is long enough to be worth watching. At 25 fps a bar would refill
    // forty times a second and mean nothing; at a four-second exposure it is the only way to tell the difference
    // between waiting and hung.
    const int height = 16;
    const int margin = 6;
    const int top = image.rows - height - margin;
    if (top <= 0 || image.cols <= 2 * margin)
        return;

    const cv::Rect track(margin, top, image.cols - 2 * margin, height);
    cv::Mat plate = image(track);
    plate *= 0.30;

    const int filled = static_cast<int>(track.width * std::clamp(state.next_frame_progress, 0.0, 1.0));
    if (filled > 0)
    {
        const cv::Rect done(track.x, track.y, filled, track.height);
        cv::rectangle(image, done, cv::Scalar(70, 190, 70), cv::FILLED);
    }
    cv::rectangle(image, track, cv::Scalar(120, 220, 120), 1);

    const double remaining_s =
        std::max(0.0, state.expected_interval_ms * (1.0 - state.next_frame_progress)) / 1000.0;
    const std::string label = "next frame in " + fixed1(remaining_s) + " s";
    cv::putText(image, label, cv::Point(track.x + 8, track.y + height - 4), cv::FONT_HERSHEY_SIMPLEX, 0.4,
                cv::Scalar(230, 255, 230), 1, cv::LINE_AA);
}

void LiveView::drawReticles(cv::Mat& image) const
{
    // Drawn into a canvas that has the SENSOR resolution, so the coordinates need no scaling and a mark stays on its
    // photosite however the window is sized. highgui does the scaling when it displays.
    for (std::size_t i = 0; i < this->reticles_.size(); ++i)
    {
        const Reticle& item = this->reticles_.at(i);
        double cx = 0.0;
        double cy = 0.0;
        this->reticles_.resolvePosition(i, this->frame_width_, this->frame_height_, cx, cy);

        const bool chosen = this->reticles_.hasSelection() && this->reticles_.selected() == i;
        const cv::Scalar colour = chosen ? cv::Scalar(80, 255, 255) : cv::Scalar(60, 200, 200);
        const int thickness = std::max(1, item.thickness);

        const int fx = toFixed(cx);
        const int fy = toFixed(cy);
        const int arm = toFixed(item.arm);
        const int gap = toFixed(item.gap);

        // Four segments rather than two crossing lines: the central gap is the point of this shape, so that the
        // photosite being marked is never covered by the mark.
        cv::line(image, cv::Point(fx - arm, fy), cv::Point(fx - gap, fy), colour, thickness,
                 cv::LINE_AA, kSubPixelShift);
        cv::line(image, cv::Point(fx + gap, fy), cv::Point(fx + arm, fy), colour, thickness,
                 cv::LINE_AA, kSubPixelShift);
        cv::line(image, cv::Point(fx, fy - arm), cv::Point(fx, fy - gap), colour, thickness,
                 cv::LINE_AA, kSubPixelShift);
        cv::line(image, cv::Point(fx, fy + gap), cv::Point(fx, fy + arm), colour, thickness,
                 cv::LINE_AA, kSubPixelShift);

        for (double radius : item.circles)
            cv::circle(image, cv::Point(fx, fy), toFixed(radius), colour, thickness, cv::LINE_AA, kSubPixelShift);

        if (chosen)
        {
            // A small open square marks the selection, so it is obvious which reticle the keys are about to move.
            const int handle = toFixed(item.gap + 4.0);
            cv::rectangle(image, cv::Point(fx - handle, fy - handle), cv::Point(fx + handle, fy + handle),
                          cv::Scalar(255, 255, 255), 1, cv::LINE_AA, kSubPixelShift);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------

}   // END NAMESPACE

// ---------------------------------------------------------------------------------------------------------------------
