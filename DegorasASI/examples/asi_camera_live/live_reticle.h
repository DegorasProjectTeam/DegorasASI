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
#include <cstddef>
#include <string>
#include <vector>


// NAMESPACES
namespace live
{

// ---------------------------------------------------------------------------------------------------------------------
// RETICLES: the data and the geometry, and nothing else.
//
// Deliberately free of OpenCV and of highgui. This example is a demo, but the multi-camera control software that comes
// later will want exactly this -- a set of aiming marks, placed, nudged and persisted -- and a model that knows how to
// draw itself in one particular toolkit is a model that has to be rewritten. So positions and sizes are plain doubles,
// hit-testing is arithmetic, and the drawing lives in the view.
//
// TWO DECISIONS WORTH KNOWING.
//
//   * COORDINATES ARE SENSOR PIXELS, not window pixels. A reticle marks a physical place on the sensor -- where a
//     return is expected to land -- so it must stay on that photosite when the window is resized or scaled. The view
//     maps to window coordinates at draw time and back at click time; nothing here knows the window exists.
//   * POSITIONS ARE SUB-PIXEL. They are doubles because the point of a fine adjustment is to land between photosites:
//     the centroid of a return is not an integer, so a reticle that can only sit on integers cannot be aligned with
//     one. The coarse step is a whole pixel and the fine step a tenth.
//
// A reticle is a cross with a central GAP -- so it marks a point without covering it -- plus any number of concentric
// circles, which is the classic viewfinder arrangement for judging both position and angular size.
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief One aiming mark: a gapped cross plus concentric circles.
 * @note Defaults live in the constructor, so this header stays a description of the type.
 */
struct Reticle
{
    /// @brief Establishes a reticle at the origin with a legible default cross and no circles.
    Reticle();

    double x;                       ///< Sensor column, sub-pixel. Ignored while centred is true.
    double y;                       ///< Sensor row, sub-pixel. Ignored while centred is true.
    double arm;                     ///< Half-length of each cross arm, in sensor pixels.
    double gap;                     ///< Radius left blank at the centre, so the marked point stays visible.
    int thickness;                  ///< Line thickness, in window pixels.
    bool centred;                   ///< Follows the frame centre instead of x,y, so a geometry change cannot strand it.
    std::vector<double> circles;    ///< Radii of the concentric circles, in sensor pixels. May be empty.
};

/**
 * @brief A set of reticles with one of them selected, and the operations the interface needs.
 * @note Indices are stable only until a removal; hold the selection, not an index.
 */
class ReticleSet
{
public:

    /// @brief Establishes an empty set with nothing selected.
    ReticleSet();

    /**
     * @brief How many reticles the set holds.
     * @return The count.
     */
    std::size_t size() const;

    /**
     * @brief Read-only access to one reticle.
     * @param index Position in the set; must be below size().
     * @return The reticle.
     */
    const Reticle& at(std::size_t index) const;

    /**
     * @brief Whether a reticle is currently selected.
     * @return True when there is a selection.
     */
    bool hasSelection() const;

    /**
     * @brief Index of the selected reticle.
     * @return The index, or size() when nothing is selected.
     */
    std::size_t selected() const;

    /**
     * @brief Adds a reticle at a sensor position and selects it.
     * @param x Sensor column.
     * @param y Sensor row.
     */
    void addAt(double x, double y);

    /**
     * @brief Adds a reticle that follows the frame centre, and selects it.
     * @note At most one centred reticle exists; a second call replaces nothing and simply adds another, which is
     *       harmless but pointless, so callers should use hasCentred() first.
     */
    void addCentred();

    /**
     * @brief Whether the set already holds a centred reticle.
     * @return True when one exists.
     */
    bool hasCentred() const;

    /**
     * @brief Removes the centred reticle, if there is one.
     * @return True when one was removed.
     */
    bool removeCentred();

    /**
     * @brief Removes the selected reticle.
     * @return True when one was removed.
     */
    bool removeSelected();

    /**
     * @brief Removes every reticle.
     */
    void clear();

    /**
     * @brief Selects the reticle nearest to a sensor position, if one is close enough.
     * @param x            Sensor column.
     * @param y            Sensor row.
     * @param frame_width  Frame width, needed to resolve a centred reticle's position.
     * @param frame_height Frame height, needed for the same reason.
     * @param radius       How near the point must be, in sensor pixels.
     * @return True when the selection changed to a reticle; false when nothing was near enough.
     */
    bool selectNear(double x, double y, int frame_width, int frame_height, double radius);

    /**
     * @brief Selects one reticle by index.
     * @param index Position in the set. A value at or above size() clears the selection.
     */
    void selectIndex(std::size_t index);

    /**
     * @brief Clears the selection.
     */
    void deselect();

    /**
     * @brief Moves the selected reticle by an offset, clamped to the frame.
     * @param dx           Columns to add.
     * @param dy           Rows to add.
     * @param frame_width  Frame width, for clamping.
     * @param frame_height Frame height, for clamping.
     * @note A centred reticle is not moved: it is defined by the frame, so nudging it would be a contradiction.
     */
    void nudgeSelected(double dx, double dy, int frame_width, int frame_height);

    /**
     * @brief Places the selected reticle at a sensor position, clamped to the frame.
     * @param x            Sensor column.
     * @param y            Sensor row.
     * @param frame_width  Frame width, for clamping.
     * @param frame_height Frame height, for clamping.
     * @note A centred reticle is not moved, for the same reason as nudgeSelected().
     */
    void placeSelected(double x, double y, int frame_width, int frame_height);

    /**
     * @brief Changes the selected reticle's cross arm length.
     * @param delta Pixels to add; the result is kept above a visible minimum.
     */
    void resizeSelectedArm(double delta);

    /**
     * @brief Changes the selected reticle's central gap.
     * @param delta Pixels to add; the result is kept at or above zero and below the arm.
     */
    void resizeSelectedGap(double delta);

    /**
     * @brief Appends a circle to the selected reticle, at its current arm radius.
     */
    void addCircleToSelected();

    /**
     * @brief Removes the last circle from the selected reticle.
     * @return True when there was one to remove.
     */
    bool removeCircleFromSelected();

    /**
     * @brief Changes the radius of the selected reticle's last circle.
     * @param delta Pixels to add; the result is kept above a visible minimum.
     */
    void resizeSelectedCircle(double delta);

    /**
     * @brief The position a reticle is drawn at, resolving the centred case.
     * @param index        Which reticle.
     * @param frame_width  Frame width.
     * @param frame_height Frame height.
     * @param x            Receives the sensor column.
     * @param y            Receives the sensor row.
     */
    void resolvePosition(std::size_t index, int frame_width, int frame_height, double& x, double& y) const;

    /**
     * @brief Writes the set to a text file.
     * @param path Where to write.
     * @return True on success.
     * @note The format is one reticle per line in key=value form, deliberately human-editable: careful geometry is
     *       easier to set in an editor than through a viewfinder's keyboard, and a calibration worth keeping is worth
     *       being able to read.
     */
    bool save(const std::string& path) const;

    /**
     * @brief Replaces the set with the contents of a text file.
     * @param path Where to read from.
     * @return True when the file was read; false when it does not exist or held nothing usable.
     * @note A malformed line is skipped rather than aborting the load: a viewfinder that refuses to start because one
     *       line of its configuration is wrong is worse than one that starts with the reticles it could understand.
     */
    bool load(const std::string& path);

private:

    std::vector<Reticle> items_;   ///< The set, in creation order.
    std::size_t selected_;         ///< Index of the selection, or items_.size() for none.
};

// ---------------------------------------------------------------------------------------------------------------------

}   // END NAMESPACE

// ---------------------------------------------------------------------------------------------------------------------
