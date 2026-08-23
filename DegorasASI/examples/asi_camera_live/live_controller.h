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
#include <string>

// OPENCV INCLUDES
#include <opencv2/core.hpp>

// PROJECT INCLUDES
#include "live_model.h"
#include "live_view.h"


// NAMESPACES
namespace live
{

// ---------------------------------------------------------------------------------------------------------------------
// THE CONTROLLER. It turns input into requests and owns no state worth speaking of.
//
// Its whole reason for existing separately is that neither of the other two should know about the input. The view
// draws what it is given and reports that a slider moved; the model applies what it is asked and reports what took
// effect; this decides that shift+E means "ten percent longer" and that S means "write the file". Change the key map
// and nothing else moves.
//
// Nothing here blocks. Every path is either a request to the model, which returns immediately, or a write to disk of
// a frame the caller already holds.
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Maps keyboard and slider input onto model requests and file writes.
 * @note Holds references to the model and the view; both must outlive it.
 */
class LiveController
{
public:

    /**
     * @brief Binds a controller to a model and a view.
     * @param model The model to send requests to.
     * @param view  The view to read input from and to toggle display options on.
     */
    LiveController(LiveModel& model, LiveView& view);

    /**
     * @brief Forwards any slider movement to the model.
     * @note Call once per iteration, before or after handleKey(); the order does not matter because both only
     *       enqueue requests.
     */
    void pumpSliders();

    /**
     * @brief Handles mouse selection and dragging of reticles.
     * @note Call once per iteration. A press selects the nearest reticle within reach; holding the button then drags
     *       it. Placing a NEW reticle is a key rather than a click, so that a stray click cannot litter the image.
     */
    void pumpMouse();

    /**
     * @brief Sets the file the reticles are read from and written to.
     * @param path Path to use. An empty path disables loading and saving.
     */
    void setReticleFile(const std::string& path);

    /**
     * @brief Loads the reticles from the configured file.
     * @return True when reticles were read.
     */
    bool loadReticles();

    /**
     * @brief Writes the reticles to the configured file.
     * @return True on success; false when no file is configured or the write failed.
     */
    bool saveReticles() const;

    /**
     * @brief A one-line description of the selected reticle, for the HUD.
     * @return The text, or an empty string when nothing is selected.
     */
    std::string selectedReticleText() const;

    /**
     * @brief Acts on one key press.
     * @param key     The key code from the view, or -1 for none.
     * @param frame   The frame currently on screen, for the save commands. May be empty.
     * @param display The 8-bit image currently on screen, for the PNG command. May be empty.
     * @return False when the user asked to quit.
     */
    bool handleKey(int key, dpasi::types::Frame& frame, const cv::Mat& display);

    /**
     * @brief Prints the key map to standard output.
     */
    void printKeys() const;

private:

    LiveModel& model_;   ///< Where requests go.
    LiveView& view_;     ///< Where input comes from, and what display toggles act on.
    int shots_;                  ///< Number of files written so far, used to name the next one.
    bool fine_;                  ///< Whether a reticle nudge uses the fine step instead of the coarse one.
    std::string reticle_file_;   ///< Where the reticles are persisted, or empty for not at all.
};

// ---------------------------------------------------------------------------------------------------------------------

}   // END NAMESPACE

// ---------------------------------------------------------------------------------------------------------------------
