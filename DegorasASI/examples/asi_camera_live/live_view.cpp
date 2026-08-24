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
    int wheel_steps = 0;         ///< Accumulated wheel notches not yet consumed.
    bool right_held = false;     ///< True while the right button is down, which is how panning is driven.
    int right_last_x = -1;       ///< Where the right-drag was last read, for the delta.
    int right_last_y = -1;
    int right_dx = 0;            ///< Accumulated right-drag movement not yet consumed.
    int right_dy = 0;
    int right_travel = 0;        ///< How far the pointer moved while the right button was down, to tell click
                                 ///< from drag: a click opens the menu, a drag pans.
    bool right_click_pending = false;   ///< A right CLICK not yet consumed.
    int right_click_x = -1;
    int right_click_y = -1;
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

/// How far the pointer may travel with the right button down and still count as a click rather than a pan.
constexpr int kRightClickSlop = 4;

// The context menu, drawn by hand because highgui has none: it offers a window, trackbars and a mouse callback, and
// that is the whole toolkit. So the menu is a rectangle painted onto the image and a click compared against it.
constexpr int kMenuItemHeight = 20;
constexpr int kMenuSeparatorHeight = 9;
constexpr int kMenuPadX = 12;
constexpr int kMenuPadY = 5;
constexpr double kMenuFontScale = 0.44;

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

/// How far the digital zoom is allowed to go. Past about this a photosite is a large flat square and there is nothing
/// more to see; below one the frame would be smaller than the canvas, which is what the window scaling already does.
constexpr double kMinZoom = 1.0;
constexpr double kMaxZoom = 32.0;

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

void onMouse(int event, int x, int y, int flags, void*)
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
    else if (event == cv::EVENT_RBUTTONDOWN)
    {
        s.right_held = true;
        s.right_last_x = x;
        s.right_last_y = y;
        s.right_travel = 0;
    }
    else if (event == cv::EVENT_RBUTTONUP)
    {
        s.right_held = false;
        if (s.right_travel <= kRightClickSlop)
        {
            s.right_click_pending = true;
            s.right_click_x = x;
            s.right_click_y = y;
        }
    }
    else if (event == cv::EVENT_MOUSEWHEEL)
    {
        // The delta is packed into the flags rather than passed as a parameter, and it arrives in notches of 120.
        // Whether it arrives at all depends on the highgui backend, which is why the keyboard also zooms.
        const int delta = cv::getMouseWheelDelta(flags);
        s.wheel_steps += (delta > 0) ? 1 : ((delta < 0) ? -1 : 0);
    }

    if (s.right_held && event == cv::EVENT_MOUSEMOVE)
    {
        const int step_x = x - s.right_last_x;
        const int step_y = y - s.right_last_y;
        s.right_dx += step_x;
        s.right_dy += step_y;
        // Total distance travelled, not net displacement: a pan that returns to where it started is still a pan.
        s.right_travel += std::abs(step_x) + std::abs(step_y);
        s.right_last_x = x;
        s.right_last_y = y;
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
    raw_(),
    flip_h_(false),
    flip_v_(false),
    rotation_(0),
    menu_open_(false),
    menu_x_(0),
    menu_y_(0),
    menu_frame_x_(0),
    menu_frame_y_(0),
    menu_anchor_valid_(false),
    menu_items_(),
    menu_highlight_(-1),
    menu_choice_(-1),
    zoom_(1.0),
    centre_x_(frame_width / 2.0),
    centre_y_(frame_height / 2.0),
    geometry_(),
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
    //
    // WINDOW_GUI_NORMAL matters more than it looks. With the default expanded GUI, the Qt backend builds its own
    // toolbar AND its own right-click menu, and that menu SWALLOWS THE RIGHT BUTTON: our callback never sees it, so
    // neither the context menu nor the right-drag pan could work. In OpenCV 4.12's window_QT.cpp the actions behind
    // that menu are created only for CV_GUI_EXPANDED (window_QT.cpp:1717) and the menu is shown only when that action
    // list is non-empty (window_QT.cpp:2801), so asking for the plain GUI hands the button back to us. What is given
    // up is Qt's own toolbar and status bar, neither of which this example uses -- it has its own HUD -- and Qt's own
    // zoom and pan, which fought with ours anyway.
    cv::namedWindow(this->title_, cv::WINDOW_NORMAL | cv::WINDOW_KEEPRATIO | cv::WINDOW_GUI_NORMAL);
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

const cv::Mat& LiveView::compose(const cv::Mat& image, const ModelState& state, const Overlay& overlay,
                                const FrameGeometry& geometry)
{
    this->geometry_ = geometry;
    this->clampView();

    // The picture is built in three steps and the ORDER is the whole design. First the visible crop is enlarged into
    // a frame-sized canvas, so a reticle stays one pixel wide at any magnification -- draw first and enlarge
    // afterwards and a 32x zoom would give it a 32-pixel line. Then the flip and the rotation are applied to that
    // canvas, which is exact because they are quarter turns and mirrors. Only then is the overlay drawn, on top of
    // the oriented picture, so the HUD is never upside down and a click can be mapped straight back.
    const bool oriented = this->isOriented();
    cv::Mat& picture = oriented ? this->raw_ : this->canvas_;
    picture.create(this->frame_height_, this->frame_width_, CV_8UC3);

    if (image.empty())
    {
        // Before the first frame there is nothing to show, but the window must still exist and the progress bar must
        // still move, so a flat canvas stands in.
        picture.setTo(cv::Scalar(20, 20, 20));
    }
    else
    {
        const cv::Rect visible = this->visibleRegion();
        if (visible.width == image.cols && visible.height == image.rows)
        {
            // Copied, not drawn on: the caller keeps showing the same image while no new frame arrives, and drawing
            // the overlay onto it would accumulate a new HUD on top of the old one every iteration.
            image.copyTo(picture);
        }
        else
        {
            // INTER_NEAREST on purpose. Magnifying a star field with a smooth filter invents structure that is not in
            // the data; a hard square per photosite is what somebody judging focus needs to see.
            cv::resize(image(visible), picture, picture.size(), 0.0, 0.0, cv::INTER_NEAREST);
        }
    }

    if (oriented)
        this->applyOrientation(this->raw_, this->canvas_);

    // The highlight follows the pointer, so the menu behaves like a menu.
    if (this->menu_open_)
        this->menu_highlight_ = this->menuHitTest(sliders().mouse_x, sliders().mouse_y);

    if (this->show_hud_)
        this->drawHud(this->canvas_, state, overlay);
    if (this->show_reticles_)
        this->drawReticles(this->canvas_);
    if (state.progress_is_useful)
        this->drawProgress(this->canvas_, state);
    if (this->menu_open_)
        this->drawMenu(this->canvas_);

    return this->canvas_;
}

void LiveView::render(const cv::Mat& image, const ModelState& state, const Overlay& overlay,
                     const FrameGeometry& geometry)
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

    cv::imshow(this->title_, this->compose(image, state, overlay, geometry));
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
    return this->canvasToFrame(s.mouse_x, s.mouse_y, x, y);
}

cv::Rect LiveView::visibleRegion() const
{
    const int w = std::max(1, static_cast<int>(std::lround(this->frame_width_ / this->zoom_)));
    const int h = std::max(1, static_cast<int>(std::lround(this->frame_height_ / this->zoom_)));
    int x = static_cast<int>(std::lround(this->centre_x_ - w / 2.0));
    int y = static_cast<int>(std::lround(this->centre_y_ - h / 2.0));
    x = std::clamp(x, 0, std::max(0, this->frame_width_ - w));
    y = std::clamp(y, 0, std::max(0, this->frame_height_ - h));
    return cv::Rect(x, y, std::min(w, this->frame_width_), std::min(h, this->frame_height_));
}

void LiveView::clampView()
{
    this->zoom_ = std::clamp(this->zoom_, kMinZoom, kMaxZoom);
    const double half_w = this->frame_width_ / this->zoom_ / 2.0;
    const double half_h = this->frame_height_ / this->zoom_ / 2.0;
    this->centre_x_ = std::clamp(this->centre_x_, half_w, this->frame_width_ - half_w);
    this->centre_y_ = std::clamp(this->centre_y_, half_h, this->frame_height_ - half_h);
}

void LiveView::zoomBy(double factor, double anchor_x, double anchor_y)
{
    const double before = this->zoom_;
    this->zoom_ = std::clamp(this->zoom_ * factor, kMinZoom, kMaxZoom);
    if (this->zoom_ == before)
        return;

    // Hold the anchor still: the point under the cursor should not slide away while zooming, which is the difference
    // between a usable magnifier and a frustrating one.
    const double ratio = before / this->zoom_;
    this->centre_x_ = anchor_x + (this->centre_x_ - anchor_x) * ratio;
    this->centre_y_ = anchor_y + (this->centre_y_ - anchor_y) * ratio;
    this->clampView();
}

void LiveView::panBy(double dx_canvas, double dy_canvas)
{
    // The drag is measured on the SCREEN, and the pan happens in frame coordinates, so a flip or a rotation between
    // the two has to be undone or the image would run away from the pointer instead of following it.
    double dx = 0.0;
    double dy = 0.0;
    this->unorientDelta(dx_canvas, dy_canvas, dx, dy);

    // Canvas pixels are not frame pixels once magnified, so the movement is divided by the zoom: dragging by a
    // centimetre moves the image by a centimetre whatever the magnification.
    this->centre_x_ -= dx / this->zoom_;
    this->centre_y_ -= dy / this->zoom_;
    this->clampView();
}

void LiveView::resetView()
{
    this->zoom_ = 1.0;
    this->centre_x_ = this->frame_width_ / 2.0;
    this->centre_y_ = this->frame_height_ / 2.0;
}

void LiveView::toggleFlipHorizontal()
{
    this->flip_h_ = !this->flip_h_;
}

void LiveView::toggleFlipVertical()
{
    this->flip_v_ = !this->flip_v_;
}

void LiveView::cycleFlip()
{
    if (!this->flip_h_ && !this->flip_v_)
    {
        this->flip_h_ = true;
    }
    else if (this->flip_h_ && !this->flip_v_)
    {
        this->flip_h_ = false;
        this->flip_v_ = true;
    }
    else if (!this->flip_h_ && this->flip_v_)
    {
        this->flip_h_ = true;
    }
    else
    {
        this->flip_h_ = false;
        this->flip_v_ = false;
    }
}

void LiveView::rotateBy(int quarters)
{
    // Positive modulo, so turning anticlockwise from zero lands on three rather than on minus one.
    this->rotation_ = ((this->rotation_ + quarters) % 4 + 4) % 4;
}

void LiveView::setOrientation(bool flip_horizontal, bool flip_vertical, int quarters)
{
    this->flip_h_ = flip_horizontal;
    this->flip_v_ = flip_vertical;
    this->rotation_ = ((quarters % 4) + 4) % 4;
}

std::string LiveView::orientationText() const
{
    std::string text;
    if (this->flip_h_)
        text += "flipH";
    if (this->flip_v_)
        text += text.empty() ? "flipV" : "+flipV";
    if (this->rotation_ != 0)
    {
        const std::string turn = "rot" + std::to_string(this->rotation_ * 90);
        text += text.empty() ? turn : ("+" + turn);
    }
    return text;
}

bool LiveView::isOriented() const
{
    return this->flip_h_ || this->flip_v_ || this->rotation_ != 0;
}

void LiveView::applyOrientation(const cv::Mat& source, cv::Mat& target) const
{
    // Flip first, then rotate. orientPoint() composes them in the same order, and these are the only two places
    // either operation appears, which is what keeps the picture and the marks drawn on it from drifting apart.
    const int flip_code = (this->flip_h_ && this->flip_v_) ? -1 : (this->flip_h_ ? 1 : 0);

    if (this->rotation_ == 0)
    {
        if (this->flip_h_ || this->flip_v_)
            cv::flip(source, target, flip_code);
        else
            source.copyTo(target);
        return;
    }

    cv::Mat flipped;
    if (this->flip_h_ || this->flip_v_)
        cv::flip(source, flipped, flip_code);
    else
        flipped = source;

    // Quarter turns, so cv::rotate does it by transposing and flipping: no interpolation, no invented pixels.
    const int code = (this->rotation_ == 1) ? cv::ROTATE_90_CLOCKWISE
                                            : ((this->rotation_ == 2) ? cv::ROTATE_180
                                                                      : cv::ROTATE_90_COUNTERCLOCKWISE);
    cv::rotate(flipped, target, code);
}

void LiveView::orientPoint(double u, double v, double& x, double& y) const
{
    // Mirrors are about the far EDGE of the last pixel, matching cv::flip: a point at column 0 lands on the last
    // column, so a mark drawn at a photosite stays on that photosite after the flip.
    const double w = this->frame_width_ - 1.0;
    const double h = this->frame_height_ - 1.0;
    const double fu = this->flip_h_ ? (w - u) : u;
    const double fv = this->flip_v_ ? (h - v) : v;

    switch (this->rotation_)
    {
        case 1:  x = h - fv;  y = fu;      break;
        case 2:  x = w - fu;  y = h - fv;  break;
        case 3:  x = fv;      y = w - fu;  break;
        default: x = fu;      y = fv;      break;
    }
}

void LiveView::unorientPoint(double x, double y, double& u, double& v) const
{
    const double w = this->frame_width_ - 1.0;
    const double h = this->frame_height_ - 1.0;

    double fu = 0.0;
    double fv = 0.0;
    switch (this->rotation_)
    {
        case 1:  fu = y;      fv = h - x;  break;
        case 2:  fu = w - x;  fv = h - y;  break;
        case 3:  fu = w - y;  fv = x;      break;
        default: fu = x;      fv = y;      break;
    }

    u = this->flip_h_ ? (w - fu) : fu;
    v = this->flip_v_ ? (h - fv) : fv;
}

void LiveView::unorientDelta(double dx, double dy, double& du, double& dv) const
{
    // A direction, not a position, so the mirror offsets drop out and only the signs and the axis swap remain.
    double fu = 0.0;
    double fv = 0.0;
    switch (this->rotation_)
    {
        case 1:  fu = dy;   fv = -dx;  break;
        case 2:  fu = -dx;  fv = -dy;  break;
        case 3:  fu = -dy;  fv = dx;   break;
        default: fu = dx;   fv = dy;   break;
    }

    du = this->flip_h_ ? -fu : fu;
    dv = this->flip_v_ ? -fv : fv;
}

const FrameGeometry& LiveView::geometry() const
{
    return this->geometry_;
}

double LiveView::zoom() const
{
    return this->zoom_;
}

bool LiveView::takeWheel(int& steps)
{
    SliderState& s = sliders();
    if (s.wheel_steps == 0)
        return false;
    steps = s.wheel_steps;
    s.wheel_steps = 0;
    return true;
}

bool LiveView::takeRightDrag(int& dx, int& dy)
{
    SliderState& s = sliders();
    if (!s.right_held || (s.right_dx == 0 && s.right_dy == 0))
        return false;
    dx = s.right_dx;
    dy = s.right_dy;
    s.right_dx = 0;
    s.right_dy = 0;
    return true;
}

bool LiveView::takeRightClick(int& canvas_x, int& canvas_y)
{
    SliderState& s = sliders();
    if (!s.right_click_pending)
        return false;
    s.right_click_pending = false;
    canvas_x = s.right_click_x;
    canvas_y = s.right_click_y;
    return true;
}

void LiveView::openMenu(int canvas_x, int canvas_y, const std::vector<std::string>& items)
{
    this->menu_items_ = items;
    this->menu_choice_ = -1;
    this->menu_highlight_ = -1;
    this->menu_anchor_valid_ = this->canvasToFrame(canvas_x, canvas_y, this->menu_frame_x_, this->menu_frame_y_);
    this->menu_open_ = !this->menu_items_.empty();

    // Kept inside the picture, so a click near the right or the bottom edge does not open a menu half off-screen
    // where its own items cannot be reached.
    this->menu_x_ = canvas_x;
    this->menu_y_ = canvas_y;
    const cv::Rect box = this->menuRect();
    const int width = (this->canvas_.cols > 0) ? this->canvas_.cols : this->frame_width_;
    const int height = (this->canvas_.rows > 0) ? this->canvas_.rows : this->frame_height_;
    this->menu_x_ = std::clamp(this->menu_x_, 0, std::max(0, width - box.width));
    this->menu_y_ = std::clamp(this->menu_y_, 0, std::max(0, height - box.height));
}

bool LiveView::menuOpen() const
{
    return this->menu_open_;
}

void LiveView::closeMenu()
{
    this->menu_open_ = false;
    this->menu_highlight_ = -1;
}

bool LiveView::takeMenuChoice(int& index)
{
    if (this->menu_choice_ < 0)
        return false;
    index = this->menu_choice_;
    this->menu_choice_ = -1;
    return true;
}

bool LiveView::menuAnchorInFrame(int& x, int& y) const
{
    if (!this->menu_anchor_valid_)
        return false;
    x = this->menu_frame_x_;
    y = this->menu_frame_y_;
    return true;
}

cv::Rect LiveView::menuRect() const
{
    if (this->menu_items_.empty())
        return cv::Rect();

    int width = 0;
    int height = 2 * kMenuPadY;
    for (const std::string& item : this->menu_items_)
    {
        if (item == "-")
        {
            height += kMenuSeparatorHeight;
            continue;
        }
        int base = 0;
        const cv::Size size = cv::getTextSize(item, cv::FONT_HERSHEY_SIMPLEX, kMenuFontScale, 1, &base);
        width = std::max(width, size.width);
        height += kMenuItemHeight;
    }
    return cv::Rect(this->menu_x_, this->menu_y_, width + 2 * kMenuPadX, height);
}

int LiveView::menuHitTest(int canvas_x, int canvas_y) const
{
    if (!this->menu_open_)
        return -1;

    const cv::Rect box = this->menuRect();
    if (!box.contains(cv::Point(canvas_x, canvas_y)))
        return -1;

    int y = box.y + kMenuPadY;
    for (std::size_t i = 0; i < this->menu_items_.size(); ++i)
    {
        const bool separator = (this->menu_items_[i] == "-");
        const int item_height = separator ? kMenuSeparatorHeight : kMenuItemHeight;
        if (canvas_y >= y && canvas_y < y + item_height)
            return separator ? -1 : static_cast<int>(i);
        y += item_height;
    }
    return -1;
}

bool LiveView::canvasToFrame(int canvas_x, int canvas_y, int& x, int& y) const
{
    // highgui hands back positions in IMAGE coordinates -- the pixel of the Mat that was passed to imshow, not the
    // pixel of the window -- and it has already divided out whatever scale the window was dragged to. This used to
    // divide by that scale a SECOND time, using getWindowImageRect(), which put every click at a fraction of its
    // real distance from the top-left corner: with the window at twice the frame, a click landed at half the
    // intended position, and a reticle placed by clicking appeared visibly away from the pointer.
    //
    // Measured rather than assumed. A probe that moved the cursor itself to known client-area points, with the window
    // opened at twice the image size, reported client x=320 as 160 and client x=640 as 320: image coordinates, exact.
    //
    // So there are two links left, and both belong to this view: undo the flip and the rotation, then undo the zoom
    // and the pan. No highgui call is involved any more, which is also what lets the headless snapshot mode use it.
    double u = 0.0;
    double v = 0.0;
    this->unorientPoint(canvas_x, canvas_y, u, v);

    const cv::Rect region = this->visibleRegion();
    if (region.width <= 0 || region.height <= 0)
        return false;

    x = static_cast<int>(std::floor(region.x + u * region.width / this->frame_width_));
    y = static_cast<int>(std::floor(region.y + v * region.height / this->frame_height_));
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

    // A press on the open menu is a choice, not a click on the picture, and a press anywhere else dismisses the menu
    // without also doing whatever that place would normally do. Both are handled here because this is the one place
    // a press is consumed, so there is no way for a second reader to see it as well.
    if (this->menu_open_)
    {
        const int hit = this->menuHitTest(s.press_x, s.press_y);
        if (hit >= 0)
            this->menu_choice_ = hit;
        this->closeMenu();
        return false;
    }

    return this->canvasToFrame(s.press_x, s.press_y, x, y);
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
    std::string third = std::string("stretch ") + (this->options_.stretch ? "on " : "off") + "    " +
                        overlay.bayer_note;
    if (this->zoom_ > 1.0)
    {
        const cv::Rect region = this->visibleRegion();
        third += "    zoom " + fixed1(this->zoom_) + "x @ " + std::to_string(region.x) + "," +
                 std::to_string(region.y);
    }
    if (this->geometry_.bin > 1)
        third += "    bin" + std::to_string(this->geometry_.bin);
    const std::string orientation = this->orientationText();
    if (!orientation.empty())
        third += "    " + orientation;
    lines.push_back(third);

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
    // THE COORDINATE CHAIN, and the reason a reticle survives everything: it is stored against the SENSOR, converted
    // to the current FRAME through the ROI and the binning, then to the CANVAS through the zoom and the pan, and
    // finally through the flip and the rotation to where it ends up on screen. Any of those can change without the
    // mark moving on the sky.
    const cv::Rect region = this->visibleRegion();
    if (region.width <= 0 || region.height <= 0)
        return;

    const double canvas_per_frame_x = static_cast<double>(this->frame_width_) / region.width;
    const double canvas_per_frame_y = static_cast<double>(this->frame_height_) / region.height;
    const double size_scale = sensorToFrameScale(this->geometry_) * canvas_per_frame_x;

    const int last_column = toFixed(image.cols - 1.0);
    const int last_row = toFixed(image.rows - 1.0);

    for (std::size_t i = 0; i < this->reticles_.size(); ++i)
    {
        const Reticle& item = this->reticles_.at(i);
        double sensor_x = 0.0;
        double sensor_y = 0.0;
        this->reticles_.resolvePosition(i, this->geometry_, sensor_x, sensor_y);

        double frame_x = 0.0;
        double frame_y = 0.0;
        sensorToFrame(this->geometry_, sensor_x, sensor_y, frame_x, frame_y);

        double cx = 0.0;
        double cy = 0.0;
        this->orientPoint((frame_x - region.x) * canvas_per_frame_x, (frame_y - region.y) * canvas_per_frame_y,
                          cx, cy);

        // The crosshair is drawn in the ORIENTED canvas and spans it edge to edge, which is why the rotation needs no
        // special case here: a full-width line and a full-height line are the same pair whichever way round the
        // picture is. Each half is drawn only when its line crosses the picture at all, so a mark panned off to one
        // side still shows the row it sits on -- and that is deliberate, because that stripe is the information that
        // the mark is out there at that height.
        const bool row_crosses = (cy >= 0.0) && (cy <= image.rows - 1.0);
        const bool column_crosses = (cx >= 0.0) && (cx <= image.cols - 1.0);
        if (!row_crosses && !column_crosses && item.circles.empty())
            continue;

        const bool chosen = this->reticles_.hasSelection() && this->reticles_.selected() == i;
        // The reticle's OWN colour, always: it is a setting, so the selection must not override it. Which one is
        // selected is shown by the handle below instead.
        const cv::Scalar colour(item.blue, item.green, item.red);
        const int thickness = std::max(1, item.thickness);

        const int fx = toFixed(cx);
        const int fy = toFixed(cy);
        const int gap = toFixed(item.gap * size_scale);

        // Four segments rather than two crossing lines: the central gap is the point of this shape, so the photosite
        // being marked is never covered by the mark. Where the centre lies outside the picture the near segment
        // simply collapses and the far one spans the whole edge, which is what clipping gives for free.
        if (row_crosses)
        {
            cv::line(image, cv::Point(0, fy), cv::Point(fx - gap, fy), colour, thickness,
                     cv::LINE_AA, kSubPixelShift);
            cv::line(image, cv::Point(fx + gap, fy), cv::Point(last_column, fy), colour, thickness,
                     cv::LINE_AA, kSubPixelShift);
        }
        if (column_crosses)
        {
            cv::line(image, cv::Point(fx, 0), cv::Point(fx, fy - gap), colour, thickness,
                     cv::LINE_AA, kSubPixelShift);
            cv::line(image, cv::Point(fx, fy + gap), cv::Point(fx, last_row), colour, thickness,
                     cv::LINE_AA, kSubPixelShift);
        }

        // Not culled: a circle whose centre is off the picture can still have an arc on it, and clipping is cheaper
        // than deciding.
        for (double radius : item.circles)
            cv::circle(image, cv::Point(fx, fy), toFixed(radius * size_scale), colour, thickness, cv::LINE_AA,
                       kSubPixelShift);

        if (chosen && row_crosses && column_crosses)
        {
            // A small open square marks the selection, so it is obvious which reticle the keys are about to move.
            const int handle = toFixed((item.gap + 4.0) * size_scale);
            cv::rectangle(image, cv::Point(fx - handle, fy - handle), cv::Point(fx + handle, fy + handle),
                          cv::Scalar(255, 255, 255), 1, cv::LINE_AA, kSubPixelShift);
        }
    }
}

void LiveView::drawMenu(cv::Mat& image) const
{
    const cv::Rect box = this->menuRect();
    const cv::Rect clipped = box & cv::Rect(0, 0, image.cols, image.rows);
    if (clipped.width <= 0 || clipped.height <= 0)
        return;

    // Dimmed rather than opaque, like the HUD plate, so the menu never completely hides what is underneath it.
    cv::Mat area = image(clipped);
    cv::Mat plate(area.size(), area.type(), cv::Scalar(26, 26, 26));
    cv::addWeighted(plate, 0.86, area, 0.14, 0.0, area);
    cv::rectangle(image, clipped, cv::Scalar(180, 180, 180), 1, cv::LINE_AA);

    int y = box.y + kMenuPadY;
    for (std::size_t i = 0; i < this->menu_items_.size(); ++i)
    {
        const std::string& label = this->menu_items_[i];
        if (label == "-")
        {
            const int mid = y + kMenuSeparatorHeight / 2;
            cv::line(image, cv::Point(box.x + kMenuPadX / 2, mid),
                     cv::Point(box.x + box.width - kMenuPadX / 2, mid), cv::Scalar(110, 110, 110), 1);
            y += kMenuSeparatorHeight;
            continue;
        }

        if (static_cast<int>(i) == this->menu_highlight_)
            cv::rectangle(image, cv::Rect(box.x + 1, y, box.width - 2, kMenuItemHeight),
                          cv::Scalar(90, 90, 90), cv::FILLED);

        cv::putText(image, label, cv::Point(box.x + kMenuPadX, y + kMenuItemHeight - 6),
                    cv::FONT_HERSHEY_SIMPLEX, kMenuFontScale, cv::Scalar(240, 240, 240), 1, cv::LINE_AA);
        y += kMenuItemHeight;
    }
}

// ---------------------------------------------------------------------------------------------------------------------

}   // END NAMESPACE

// ---------------------------------------------------------------------------------------------------------------------
