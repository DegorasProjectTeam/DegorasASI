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
// THREE DECISIONS WORTH KNOWING.
//
//   * COORDINATES ARE ABSOLUTE SENSOR PIXELS, unbinned, and that is the whole point. A reticle marks a physical
//     photosite -- where a return is expected to land -- so it has to survive everything that changes what is on
//     screen without changing the sensor: a resized window, a digital zoom, a pan, a different binning factor, a
//     different ROI. Store a frame coordinate and every one of those strands the mark somewhere else. Store the
//     sensor coordinate and the mark stays on the sky.
//   * SIZES ARE SENSOR PIXELS TOO. A gap of 8 covers eight photosites whether the stream is binned or not, so a
//     circle drawn around a target keeps meaning the same angle on the sky when the binning changes. The line
//     THICKNESS is the exception and is deliberately in canvas pixels: it is a legibility setting, not a measurement,
//     and a one-pixel line has to stay one pixel wide at every magnification.
//   * POSITIONS ARE SUB-PIXEL. They are doubles because the point of a fine adjustment is to land between photosites:
//     the centroid of a return is not an integer, so a reticle that can only sit on integers cannot be aligned with
//     one. The coarse step is a whole pixel and the fine step a tenth.
//
// A reticle is a FULL-SPAN crosshair: the two lines run the whole width and the whole height of the image, with a
// central GAP so the marked photosite is never covered, plus any number of concentric circles for judging angular
// size. Full span rather than short arms because a line that reaches the edges can be aligned against the edges --
// which is how a crosshair is actually used -- and because a mark whose centre has been panned out of view still
// shows the row and the column it sits on instead of vanishing.
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief What is needed to relate a frame to the sensor it came from.
 * @note The one place the mapping is written down. The vendor reports the ROI origin in POST-BINNING coordinates, so
 *       the conversion is (frame + origin) * bin and not the other way round -- getting that backwards puts a reticle
 *       in the right place at bin 1 and the wrong place everywhere else, which is the kind of bug that survives a
 *       casual test.
 */
struct FrameGeometry
{
    /// @brief Establishes an identity geometry: no ROI offset, no binning, no size.
    FrameGeometry();

    int start_x;         ///< ROI origin column, in post-binning sensor coordinates.
    int start_y;         ///< ROI origin row, in post-binning sensor coordinates.
    int bin;             ///< Binning factor; 1 means unbinned.
    int width;           ///< Frame width, in frame pixels.
    int height;          ///< Frame height, in frame pixels.
    int sensor_width;    ///< Full sensor width, unbinned, for clamping.
    int sensor_height;   ///< Full sensor height, unbinned, for clamping.
};

/**
 * @brief Converts an absolute sensor position to a position within the current frame.
 * @param geometry The frame's relationship to the sensor.
 * @param sensor_x Sensor column, unbinned.
 * @param sensor_y Sensor row, unbinned.
 * @param x        Receives the frame column, which may fall outside the frame.
 * @param y        Receives the frame row, which may fall outside the frame.
 */
void sensorToFrame(const FrameGeometry& geometry, double sensor_x, double sensor_y, double& x, double& y);

/**
 * @brief Converts a position within the current frame to an absolute sensor position.
 * @param geometry The frame's relationship to the sensor.
 * @param x        Frame column.
 * @param y        Frame row.
 * @param sensor_x Receives the sensor column, unbinned.
 * @param sensor_y Receives the sensor row, unbinned.
 */
void frameToSensor(const FrameGeometry& geometry, double x, double y, double& sensor_x, double& sensor_y);

/**
 * @brief How many frame pixels one sensor pixel spans.
 * @param geometry The frame's relationship to the sensor.
 * @return The factor to multiply a sensor-pixel length by to get a frame-pixel length.
 */
double sensorToFrameScale(const FrameGeometry& geometry);

/**
 * @brief The arms of an aiming mark: an upright cross, or the same mark turned through 45 degrees.
 * @note Both span the whole canvas and keep the central gap. Which one reads better depends on the scene: the
 *       upright cross shares its direction with the sensor rows and columns, so it hides whatever sits on the
 *       row and the column of the marked point -- exactly the pixels a drift measurement often cares about. The
 *       diagonal leaves those clear and is easier to tell apart from a star's diffraction spikes, which are
 *       themselves usually upright.
 */
enum class ReticleShape
{
    CROSS = 0,   ///< Horizontal and vertical arms. The default.
    X     = 1,   ///< Two diagonal arms at 45 degrees.
};

/// @brief The shape's name for the interface: "cross" or "X".
const char* toString(ReticleShape shape);

/**
 * @brief One aiming mark: a gapped cross or X, plus concentric circles.
 * @note Defaults live in the constructor, so this header stays a description of the type.
 */
struct Reticle
{
    /// @brief Establishes a red, one-pixel, unlocked cross at the origin, with a legible gap and no circles.
    Reticle();

    double x;                       ///< Absolute sensor column, unbinned, sub-pixel. Ignored while centred.
    double y;                       ///< Absolute sensor row, unbinned, sub-pixel. Ignored while centred.
    double gap;                     ///< Radius left blank at the centre, so the marked point stays visible.
    int thickness;                  ///< Line and circle thickness, in canvas pixels.
    int red;                        ///< Colour, red component, 0 to 255.
    int green;                      ///< Colour, green component, 0 to 255.
    int blue;                       ///< Colour, blue component, 0 to 255.
    bool centred;                   ///< Follows the frame centre instead of x,y, so a geometry change cannot strand it.
    ReticleShape shape;             ///< Upright cross or diagonal X. Purely how it is drawn; the centre is the same.
    bool locked;                    ///< Refuses to move or be deleted. See ReticleSet::toggleSelectedLock.
    std::vector<double> circles;    ///< Radii of the concentric circles, in unbinned sensor pixels. May be empty.
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
     * @brief Removes the selected reticle, unless it is locked.
     * @return True when one was removed; false when there is no selection OR the selection is locked.
     * @note A lock refuses deletion as well as movement, and deliberately so: a mark is locked because it was
     *       aligned once and must not change, and losing it outright is a worse accident than nudging it. Call
     *       isSelectedLocked() to tell the two false results apart and say which happened.
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
     * @param geometry     The frame's relationship to the sensor.
     * @param radius       How near the point must be, in sensor pixels.
     * @return True when the selection changed to a reticle; false when nothing was near enough.
     */
    bool selectNear(double x, double y, const FrameGeometry& geometry, double radius);

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
     * @param geometry     The frame's relationship to the sensor, for clamping to the sensor.
     * @note A centred reticle is not moved: it is defined by the frame, so nudging it would be a contradiction.
     */
    void nudgeSelected(double dx, double dy, const FrameGeometry& geometry);

    /**
     * @brief Places the selected reticle at a sensor position, clamped to the frame.
     * @param x            Sensor column.
     * @param y            Sensor row.
     * @param geometry     The frame's relationship to the sensor, for clamping to the sensor.
     * @note A centred reticle is not moved, for the same reason as nudgeSelected().
     */
    void placeSelected(double x, double y, const FrameGeometry& geometry);

    /**
     * @brief Locks or unlocks the selected reticle.
     * @return The state it ends in: true when locked. False when there is no selection.
     * @note A LOCK IS ABOUT POSITION, not appearance. It refuses nudgeSelected(), placeSelected() -- which is
     *       what the mouse drag goes through, so one guard covers both -- and removeSelected(). Colour, gap,
     *       thickness, shape and circles stay editable, because changing how a mark looks does not move it and
     *       having to unlock to recolour would make the lock a nuisance rather than a safeguard.
     */
    bool toggleSelectedLock();

    /**
     * @brief Whether the selected reticle is locked.
     * @return False when nothing is selected.
     */
    bool isSelectedLocked() const;

    /**
     * @brief Advances the selected reticle to the next shape, wrapping round.
     * @return The shape it ends on; ReticleShape::CROSS when there is no selection.
     * @note A cycle rather than a pair of keys, for the reason given at the top of live_controller.cpp: the Qt
     *       backend of highgui discards the case of a letter, so nothing here may depend on shift.
     */
    ReticleShape cycleSelectedShape();

    /**
     * @brief Changes the selected reticle's central gap.
     * @param delta Pixels to add; the result is kept at or above zero and below the arm.
     */
    void resizeSelectedGap(double delta);

    /**
     * @brief Thickens or thins the selected reticle's lines and circles.
     * @param delta Change in canvas pixels; negative thins. Clamped to a legible range.
     */
    void resizeSelectedThickness(int delta);

    /**
     * @brief Recolours the selected reticle.
     * @param red   Red component; values outside 0 to 255 are clamped.
     * @param green Green component.
     * @param blue  Blue component.
     */
    void setSelectedColour(int red, int green, int blue);

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
     * @param geometry     The frame's relationship to the sensor.
     * @param x            Receives the absolute sensor column.
     * @param y            Receives the absolute sensor row.
     */
    void resolvePosition(std::size_t index, const FrameGeometry& geometry, double& x, double& y) const;

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
