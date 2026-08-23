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
    const cv::Mat& compose(const cv::Mat& image, const ModelState& state, const Overlay& overlay);

    /**
     * @brief Composes and shows one frame.
     * @param image   The 8-bit BGR image to show. May be empty before the first frame arrives.
     * @param state   The snapshot the HUD and the progress bar describe.
     * @param overlay The words to draw.
     * @note Throttled: see the implementation. compose() is not, so a caller that wants every frame drawn should use
     *       that one.
     */
    void render(const cv::Mat& image, const ModelState& state, const Overlay& overlay);

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

    /// @brief Draws every reticle, with the selected one picked out.
    void drawReticles(cv::Mat& image) const;

    /**
     * @brief Maps a window position to frame coordinates.
     * @param window_x Column in window pixels.
     * @param window_y Row in window pixels.
     * @param x        Receives the frame column.
     * @param y        Receives the frame row.
     * @return False when the window has no usable size.
     */
    bool windowToFrame(int window_x, int window_y, int& x, int& y) const;

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
    ReticleSet reticles_;        ///< The aiming marks, owned by the view because they are an overlay.
    mutable cv::Mat canvas_;     ///< Scratch the overlay is drawn onto, so the caller's image is left alone.
    std::chrono::steady_clock::time_point last_render_;   ///< When the last composition happened.
    std::uint64_t last_drawn_sequence_;                   ///< Sequence of the frame last composed.
};

// ---------------------------------------------------------------------------------------------------------------------

}   // END NAMESPACE

// ---------------------------------------------------------------------------------------------------------------------
