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
#include <vector>

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
     * @brief Handles mouse selection, dragging, zooming, panning and the context menu.
     * @note Call once per iteration. A left press selects the nearest reticle within reach and holding the button
     *       drags it; a right CLICK opens the menu and a right DRAG pans. Placing a new reticle with the left button
     *       alone is deliberately not possible, so a stray click cannot litter the image: it is either the menu or a
     *       key.
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

    /// @brief What the context menu can be asked to do. The order of the enumerators is not the order on screen.
    enum class MenuAction
    {
        ADD_HERE,          ///< Put a new reticle where the menu was opened.
        ADD_CENTRED,       ///< Put a reticle on the frame centre.
        REMOVE_CENTRED,    ///< Take the centred reticle away.
        DELETE_SELECTED,   ///< Delete the reticle the menu was opened on.
        DELETE_ALL,        ///< Delete every reticle.
        SET_COLOUR,        ///< Recolour the selected reticle; the palette index says which colour.
        THICKER,           ///< Thicken the selected reticle's lines.
        THINNER,           ///< Thin them.
        ADD_CIRCLE,        ///< Add a circle to the selected reticle.
        REMOVE_CIRCLE,     ///< Take its outermost circle away.
        TOGGLE_LOCK,       ///< Lock or unlock the selected reticle against movement and deletion.
        CYCLE_SHAPE,       ///< Switch the selected reticle between an upright cross and a diagonal X.
    };

    /// @brief One entry of the menu the view is currently showing.
    struct MenuEntry
    {
        /// @brief Establishes an entry that does nothing, for a separator.
        MenuEntry();

        MenuAction action;   ///< What choosing it does.
        int palette;         ///< Palette index for SET_COLOUR; ignored otherwise.
    };

    /// @brief Builds the menu for a right-click and hands it to the view.
    void openMenuAt(int canvas_x, int canvas_y);

    /// @brief Carries out one menu choice.
    void applyMenuChoice(int index);

    LiveModel& model_;   ///< Where requests go.
    LiveView& view_;     ///< Where input comes from, and what display toggles act on.
    int shots_;                  ///< Number of files written so far, used to name the next one.
    bool fine_;                  ///< Whether a reticle nudge uses the fine step instead of the coarse one.

    // THE GRAB OFFSET, which is why clicking a reticle no longer moves it. The drag used to snap the centre to
    // the cursor, so merely selecting a mark -- which is what you do before locking it -- shifted it by however
    // far the click landed from its centre, up to the grab radius. Holding the offset means a press with no
    // motion moves nothing at all, and a drag moves the mark exactly as far as the pointer travels.
    /// @brief What the typed box is currently editing, so its value goes to the right control.
    enum class EntryTarget
    {
        NONE,
        EXPOSURE,   ///< Value read as milliseconds, because that is the unit the HUD and the slider both use.
        GAIN,       ///< Value read as a plain number, clamped to what the camera reports.
    };

    /// @brief Sends a finished typed value to the camera, if one is ready. Cheap and safe to call every key.
    void applyEntry();

    EntryTarget entry_target_;   ///< Which control the open box belongs to.

    bool grabbed_;               ///< Whether the current press began on the selected reticle.
    double grab_dx_;             ///< Sensor-x distance from the cursor to the reticle centre at the press.
    double grab_dy_;             ///< Sensor-y distance from the cursor to the reticle centre at the press.
    std::string reticle_file_;   ///< Where the reticles are persisted, or empty for not at all.

    /// The actions behind the labels the view is showing, in the same order. Kept here rather than in the view
    /// because the view deliberately does not know what any item means.
    std::vector<MenuEntry> menu_entries_;
};

// ---------------------------------------------------------------------------------------------------------------------

}   // END NAMESPACE

// ---------------------------------------------------------------------------------------------------------------------
