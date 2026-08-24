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

#pragma once

// C++ INCLUDES
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// OPENCV INCLUDES
#include <opencv2/core.hpp>

// PROJECT INCLUDES
#include "live_image.h"
#include "live_reticle.h"
#include "live_model.h"


// NAMESPACES
namespace live
{

// ---------------------------------------------------------------------------------------------------------------------
// THE VIEW. It owns the window and it runs ONLY on the main thread, because that is where highgui insists on living.
//
// It knows nothing about the camera. Everything it draws arrives as a ModelState snapshot plus an already-converted
// image, which is what lets it keep painting at a steady rate while the model sits inside a ten-second exposure.
//
// The trackbar plumbing lives here on purpose. highgui hands out C callbacks with a void* payload, so somewhere a
// file-scope pointer has to exist; keeping that inside the view means the rest of the example never sees it. What the
// view exposes instead is a pair of "has the user moved a slider?" queries the controller polls.
//
// THE COORDINATE CHAIN, which is the view's real job:
//
//     sensor --(ROI origin, binning)--> frame --(zoom, pan)--> canvas --(flip, rotation)--> what is on screen
//
// The view owns the last two links and is therefore the only thing that can invert them, which is why every position
// arriving from highgui goes through canvasToFrame() and every mark drawn goes through orientPoint().
//
// One measured fact this depends on: highgui delivers mouse positions in IMAGE coordinates, already divided by
// whatever scale the window was dragged to. It was scaling them a second time here, which put every click -- and so
// every reticle -- at a fraction of the intended distance from the top-left corner. Measured with a probe that moved
// the cursor itself to known client-area points: at a window twice the image, client x=640 arrived as x=320.
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief The words the view draws on top of the image, composed by the caller.
 * @note The view formats and positions them; it does not decide what they say. That keeps the pixel work here and the
 *       interpretation of a frame where the frame is understood.
 */
struct Overlay
{
    /// @brief Establishes an empty overlay.
    Overlay();

    std::string bayer_note;   ///< What is being done about colour, e.g. "demosaic RG" or "bin>1: no mosaic".
    std::string probe_text;    ///< The pixel under the cursor, or empty when the cursor is elsewhere.
    std::string reticle_text;  ///< The selected reticle's numbers, or empty when nothing is selected.
    FrameStats stats;          ///< The histogram and the clipping figures, or empty statistics for no frame.
};

/**
 * @brief Owns the window, the sliders and everything drawn on top of the image.
 * @note Not copyable or movable: it owns a named highgui window and installs callbacks that point at it.
 */
class LiveView
{
public:

    /**
     * @brief Prepares a view for frames of a given size.
     * @param title        Window title.
     * @param frame_width  Width of the frames that will be shown, in pixels.
     * @param frame_height Height of the frames that will be shown, in pixels.
     * @note Does not create the window; call open() for that.
     */
    LiveView(const std::string& title, int frame_width, int frame_height);

    /// @brief Destroys the window if it is still open.
    ~LiveView();

    LiveView(const LiveView&) = delete;
    LiveView& operator=(const LiveView&) = delete;
    LiveView(LiveView&&) = delete;
    LiveView& operator=(LiveView&&) = delete;

    /**
     * @brief Creates the window and installs the mouse callback.
     * @param max_width  Largest window width to open, in pixels; a bigger sensor is scaled down to fit.
     * @param max_height Largest window height to open, in pixels.
     * @return True when the window exists.
     */
    bool open(int max_width, int max_height);

    /**
     * @brief Whether the window is still on screen.
     * @return False once the user has closed it.
     */
    bool isOpen() const;

    /**
     * @brief Adds the exposure slider, mapped logarithmically.
     * @param minimum_us Smallest exposure the camera accepts, in microseconds.
     * @param maximum_us Largest exposure the camera accepts, in microseconds.
     * @param current_us Where to place the handle initially.
     * @note Logarithmic because exposure spans microseconds to seconds: on a linear slider the entire usable range of
     *       a bright target occupies the first pixel.
     */
    void addExposureSlider(long long minimum_us, long long maximum_us, long long current_us);

    /**
     * @brief Adds the gain slider.
     * @param maximum Largest gain the camera accepts.
     * @param current Where to place the handle initially.
     */
    void addGainSlider(long long maximum, long long current);

    /**
     * @brief Moves the sliders to match the camera, without that movement being read back as a user request.
     * @param state The snapshot to follow.
     */
    void syncSliders(const ModelState& state);

    /**
     * @brief Builds the image that would be shown: the frame plus the HUD, the reticles and the progress bar.
     * @param image   The 8-bit BGR image to draw onto a copy of. May be empty before the first frame arrives.
     * @param state   The snapshot the HUD and the progress bar describe.
     * @param overlay The words to draw.
     * @return The composed image, owned by the view and valid until the next call.
     * @note Separate from render() so the overlay can be produced with NO WINDOW at all -- which is how the headless
     *       snapshot mode writes a picture of exactly what the viewfinder would show, and the only way the reticles
     *       and the progress bar can be checked without a human watching a screen.
     */
    const cv::Mat& compose(const cv::Mat& image, const ModelState& state, const Overlay& overlay,
                           const FrameGeometry& geometry);

    /**
     * @brief Composes and shows one frame.
     * @param image   The 8-bit BGR image to show. May be empty before the first frame arrives.
     * @param state   The snapshot the HUD and the progress bar describe.
     * @param overlay The words to draw.
     * @note Throttled: see the implementation. compose() is not, so a caller that wants every frame drawn should use
     *       that one.
     */
    void render(const cv::Mat& image, const ModelState& state, const Overlay& overlay,
                const FrameGeometry& geometry);

    /**
     * @brief Width of the frames being shown, in pixels.
     * @return The width.
     */
    int frameWidth() const;

    /**
     * @brief Height of the frames being shown, in pixels.
     * @return The height.
     */
    int frameHeight() const;

    /**
     * @brief Pumps the GUI and returns any key pressed.
     * @param wait_ms How long to give the event loop, in milliseconds. Must be at least 1.
     * @return The key code, or -1 when no key was pressed.
     * @note This is also what repaints the window, so it must be called every iteration whether or not a frame
     *       arrived. Not calling it is what made the old single-threaded viewer freeze during a long exposure.
     */
    int pollKey(int wait_ms);

    /**
     * @brief Whether the user has moved the exposure slider since the last call.
     * @param microseconds Receives the exposure the slider now represents.
     * @return True when there was a movement to report.
     */
    bool takeExposureRequest(long long& microseconds);

    /**
     * @brief Whether the user has moved the gain slider since the last call.
     * @param value Receives the gain the slider now represents.
     * @return True when there was a movement to report.
     */
    bool takeGainRequest(long long& value);

    /**
     * @brief Multiplies the zoom, keeping one frame position fixed under the cursor.
     * @param factor   Multiplier; above one zooms in.
     * @param anchor_x Frame column to hold still.
     * @param anchor_y Frame row to hold still.
     * @note Zoom is DIGITAL: it crops the frame and enlarges the crop with nearest-neighbour, so a magnified pixel
     *       stays a square of one value rather than being invented by interpolation. That matters when the thing being
     *       looked at is a two-pixel star.
     */
    void zoomBy(double factor, double anchor_x, double anchor_y);

    /**
     * @brief Moves the visible region.
     * @param dx_canvas Horizontal movement in canvas pixels.
     * @param dy_canvas Vertical movement in canvas pixels.
     */
    void panBy(double dx_canvas, double dy_canvas);

    /**
     * @brief Returns to showing the whole frame.
     * @note Zoom and pan only; the orientation is left alone, because a flip set to match the optics is not something
     *       a "fit the frame" key should undo.
     */
    void resetView();

    /// @brief Mirrors the display left to right.
    void toggleFlipHorizontal();

    /// @brief Mirrors the display top to bottom.
    void toggleFlipVertical();

    /**
     * @brief Steps through the four mirror states: none, horizontal, vertical, both.
     * @note One key reaches all four on purpose. The Qt backend of highgui discards shift -- see the note on the key
     *       map -- so a pair of commands that differed only by case would leave half of them unreachable.
     */
    void cycleFlip();

    /**
     * @brief Turns the display in quarter turns.
     * @param quarters Number of 90-degree steps clockwise; negative turns the other way.
     * @note Quarter turns only, and deliberately: they are exact. An arbitrary angle has to resample, which blurs a
     *       two-pixel star and invents structure -- the same reason the zoom is nearest-neighbour.
     */
    void rotateBy(int quarters);

    /**
     * @brief Sets the orientation outright, for a command-line option.
     * @param flip_horizontal Whether to mirror left to right.
     * @param flip_vertical   Whether to mirror top to bottom.
     * @param quarters        Quarter turns clockwise; taken modulo four.
     */
    void setOrientation(bool flip_horizontal, bool flip_vertical, int quarters);

    /**
     * @brief The orientation in words, for the HUD.
     * @return A description, or an empty string when the display is unmodified.
     */
    std::string orientationText() const;

    /**
     * @brief The frame-to-sensor relationship used by the last composition.
     * @return The geometry, so the controller can work in sensor coordinates without being handed it separately.
     */
    const FrameGeometry& geometry() const;

    /**
     * @brief The current zoom factor.
     * @return One when the whole frame is visible, higher when magnified.
     */
    double zoom() const;

    /**
     * @brief Whether a wheel movement happened since the last call, and in which direction.
     * @param steps Receives the notches: positive away from the user.
     * @return True when there was a movement to report.
     */
    bool takeWheel(int& steps);

    /**
     * @brief Whether the right button is held, and how far it has moved since the last call.
     * @param dx Receives the horizontal movement in canvas pixels.
     * @param dy Receives the vertical movement in canvas pixels.
     * @return True when a right-drag is in progress and there was movement.
     */
    bool takeRightDrag(int& dx, int& dy);

    /**
     * @brief Whether the right button was CLICKED rather than dragged, since the last call.
     * @param canvas_x Receives the click column, in canvas pixels.
     * @param canvas_y Receives the click row, in canvas pixels.
     * @return True when there was a click to report.
     * @note Click and drag share the right button because both are natural there: a click opens the menu and a drag
     *       pans. They are told apart by how far the pointer travelled while the button was down, which is what a
     *       toolkit would do for us if highgui had menus.
     */
    bool takeRightClick(int& canvas_x, int& canvas_y);

    /**
     * @brief Opens the context menu.
     * @param canvas_x Column to put its corner at, in canvas pixels.
     * @param canvas_y Row to put its corner at.
     * @param items    The labels, top to bottom. An item reading "-" is drawn as a separator and cannot be chosen.
     * @note The view draws and hit-tests it; it does not know what any item means. highgui has no menus of its own,
     *       so this is a rectangle painted onto the image and a click compared against it.
     */
    void openMenu(int canvas_x, int canvas_y, const std::vector<std::string>& items);

    /**
     * @brief Whether the context menu is open.
     * @return True while it is.
     */
    bool menuOpen() const;

    /// @brief Closes the context menu without choosing anything.
    void closeMenu();

    /**
     * @brief Whether a menu item was chosen since the last call.
     * @param index Receives the item's position in the list passed to openMenu().
     * @return True when there was a choice to report.
     */
    bool takeMenuChoice(int& index);

    /**
     * @brief Where the menu was opened, in FRAME coordinates.
     * @param x Receives the column.
     * @param y Receives the row.
     * @return False when the menu was opened off the image.
     */
    bool menuAnchorInFrame(int& x, int& y) const;

    /**
     * @brief Maps a canvas position to frame coordinates.
     * @param canvas_x Column in canvas pixels, as highgui reports it.
     * @param canvas_y Row in canvas pixels.
     * @param x        Receives the frame column.
     * @param y        Receives the frame row.
     * @return False when the position falls outside the frame.
     * @note Public because the controller has to turn a click into a place on the sensor, and the view is the only
     *       thing that knows the zoom, the pan and the orientation standing between the two.
     */
    bool canvasToFrame(int canvas_x, int canvas_y, int& x, int& y) const;

    /**
     * @brief The cursor position expressed in FRAME coordinates.
     * @param x Receives the column.
     * @param y Receives the row.
     * @return False when the cursor is not over the image, or the window is scaled to nothing.
     * @note The mapping is the view's job because only the view knows how the window is currently scaled; with a
     *       resizable window the raw cursor position is not a frame coordinate.
     */
    bool probePointInFrame(int& x, int& y) const;

    /**
     * @brief The display options the user has toggled, for whoever converts the frames.
     * @return A reference to the live options.
     */
    DisplayOptions& displayOptions();

    /// @brief Turns the percentile stretch on or off.
    void toggleStretch();

    /// @brief Turns demosaicing on or off.
    void toggleDemosaic();

    /// @brief Shows or hides the HUD.
    void toggleHud();

    /// @brief Shows or hides every reticle at once.
    void toggleReticles();

    /**
     * @brief Shows or hides the histogram.
     * @note Off by default: it costs screen space, and it is consulted while setting an exposure rather than watched
     *       continuously.
     */
    void toggleHistogram();

    /**
     * @brief Whether the histogram is being drawn.
     * @return True when it is.
     */
    bool histogramVisible() const;

    /**
     * @brief Whether the reticles are being drawn.
     * @return True when they are.
     */
    bool reticlesVisible() const;

    /**
     * @brief The reticles this view draws, for the controller to edit.
     * @return A reference to the live set.
     * @note They belong to the VIEW, not to the model: a reticle is an overlay on the picture, not a property of the
     *       camera. Nothing about them reaches the acquisition thread.
     */
    ReticleSet& reticles();

    /**
     * @brief Whether a mouse button press has happened since the last call, in FRAME coordinates.
     * @param x Receives the column.
     * @param y Receives the row.
     * @return True when there was a press to report and it fell on the image.
     */
    bool takeMousePressInFrame(int& x, int& y);

    /**
     * @brief Whether the mouse button is currently held down.
     * @return True while it is, which is what makes a drag a drag.
     */
    bool mouseHeld() const;

    /**
     * @brief Whether the percentile stretch is currently on.
     * @return True when it is.
     */
    bool stretchEnabled() const;

    /**
     * @brief Whether demosaicing is currently on.
     * @return True when it is.
     */
    bool demosaicEnabled() const;

private:

    /// @brief Draws the HUD plate and its lines onto the image.
    void drawHud(cv::Mat& image, const ModelState& state, const Overlay& overlay) const;

    /// @brief Draws the wait-for-next-frame bar along the bottom of the image.
    void drawProgress(cv::Mat& image, const ModelState& state) const;

    /// @brief Draws the histogram plate and its curves in the top-right corner.
    void drawHistogram(cv::Mat& image, const FrameStats& stats) const;

    /// @brief Draws every reticle, with the selected one picked out.
    void drawReticles(cv::Mat& image) const;

    /// @brief Draws the context menu, when it is open.
    void drawMenu(cv::Mat& image) const;

    /// @brief Writes the flipped and rotated version of a canvas into another, which may be transposed.
    void applyOrientation(const cv::Mat& source, cv::Mat& target) const;

    /// @brief Whether any flip or rotation is in effect.
    bool isOriented() const;

    /// @brief Maps an unoriented canvas position to where it ends up on screen.
    void orientPoint(double u, double v, double& x, double& y) const;

    /// @brief The inverse of orientPoint().
    void unorientPoint(double x, double y, double& u, double& v) const;

    /// @brief The inverse of orientPoint() for a DIRECTION, which ignores the mirror offsets.
    void unorientDelta(double dx, double dy, double& du, double& dv) const;

    /// @brief Which menu item covers a canvas position, or -1 for none.
    int menuHitTest(int canvas_x, int canvas_y) const;

    /// @brief The menu's rectangle on the canvas, empty when it is closed.
    cv::Rect menuRect() const;

    /// @brief The part of the frame currently visible, in frame pixels.
    cv::Rect visibleRegion() const;

    /// @brief Keeps the visible region inside the frame after a zoom or a pan.
    void clampView();

    std::string title_;          ///< Window name, which is also highgui's handle for it.
    int frame_width_;            ///< Width of the frames being shown.
    int frame_height_;           ///< Height of the frames being shown.
    mutable bool open_;          ///< Whether the window exists. Latched false by isOpen() once it is gone.
    bool has_exposure_slider_;   ///< Whether addExposureSlider() has run.
    bool has_gain_slider_;       ///< Whether addGainSlider() has run.
    long long exposure_min_;     ///< Low end of the logarithmic exposure mapping, in microseconds.
    long long exposure_max_;     ///< High end of the logarithmic exposure mapping, in microseconds.
    long long gain_max_;         ///< Top of the gain slider.
    DisplayOptions options_;     ///< What the user has asked to be done to the frames.
    bool show_hud_;              ///< Whether the HUD is drawn.
    bool show_reticles_;         ///< Whether the reticles are drawn.
    bool show_histogram_;        ///< Whether the histogram is drawn.
    ReticleSet reticles_;        ///< The aiming marks, owned by the view because they are an overlay.
    mutable cv::Mat canvas_;     ///< Scratch the overlay is drawn onto, so the caller's image is left alone.
    mutable cv::Mat raw_;        ///< The unoriented canvas, used only when a flip or a rotation is in effect.
    bool flip_h_;                ///< Whether the display is mirrored left to right.
    bool flip_v_;                ///< Whether the display is mirrored top to bottom.
    int rotation_;               ///< Quarter turns clockwise, 0 to 3.
    bool menu_open_;             ///< Whether the context menu is being drawn.
    int menu_x_;                 ///< Menu corner, canvas column.
    int menu_y_;                 ///< Menu corner, canvas row.
    int menu_frame_x_;           ///< Where the menu was opened, frame column, for "add one here".
    int menu_frame_y_;           ///< Where the menu was opened, frame row.
    bool menu_anchor_valid_;     ///< Whether that frame position is usable.
    std::vector<std::string> menu_items_;   ///< The labels, as the controller supplied them.
    mutable int menu_highlight_;            ///< Item under the pointer, or -1.
    int menu_choice_;                       ///< Item chosen and not yet consumed, or -1.
    double zoom_;                ///< 1.0 shows the whole frame; higher magnifies.
    double centre_x_;            ///< Frame column at the centre of the visible region.
    double centre_y_;            ///< Frame row at the centre of the visible region.
    FrameGeometry geometry_;     ///< The frame's relationship to the sensor, as of the last composition.
    std::chrono::steady_clock::time_point last_render_;   ///< When the last composition happened.
    std::uint64_t last_drawn_sequence_;                   ///< Sequence of the frame last composed.
};

// ---------------------------------------------------------------------------------------------------------------------

}   // END NAMESPACE

// ---------------------------------------------------------------------------------------------------------------------
