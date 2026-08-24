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
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

// PROJECT INCLUDES
#include "live_reticle.h"


// NAMESPACES
namespace live
{

namespace
{

/// A ring narrower than this is a dot, so a circle radius is never allowed under it.
constexpr double kMinCircleRadius = 3.0;

/// Where a newly added circle starts, in sensor pixels. Clear of any sensible gap, so it is visible at once.
constexpr double kDefaultCircleRadius = 40.0;

/// The gap is bounded so one mistaken keypress cannot blank the crosshair while it still reads as configured.
constexpr double kMaxGap = 400.0;

/// Thickness bounds, in canvas pixels. One is a hairline; past eight the mark covers what it is meant to point at.
constexpr int kMinThickness = 1;
constexpr int kMaxThickness = 8;

int clampInt(int value, int low, int high)
{
    return std::max(low, std::min(high, value));
}

double clampTo(double value, double low, double high)
{
    return std::max(low, std::min(high, value));
}

/// Upper bound for a sensor coordinate. Falls back generously when the sensor size is unknown, because clamping to
/// zero would collapse every reticle onto the corner.
double sensorLimit(int sensor_extent)
{
    return (sensor_extent > 1) ? static_cast<double>(sensor_extent - 1) : 1.0e9;
}

/// Splits "circles=25,60" style lists.
std::vector<double> parseRadii(const std::string& csv)
{
    std::vector<double> out;
    std::istringstream stream(csv);
    std::string item;
    while (std::getline(stream, item, ','))
    {
        if (item.empty())
            continue;
        try
        {
            out.push_back(std::stod(item));
        }
        catch (const std::exception&)
        {
            // One unreadable radius costs that radius, not the whole reticle.
        }
    }
    return out;
}

}   // namespace

// ---------------------------------------------------------------------------------------------------------------------

FrameGeometry::FrameGeometry() :
    start_x(0),
    start_y(0),
    bin(1),
    width(0),
    height(0),
    sensor_width(0),
    sensor_height(0)
{
}

// ---------------------------------------------------------------------------------------------------------------------

void sensorToFrame(const FrameGeometry& geometry, double sensor_x, double sensor_y, double& x, double& y)
{
    const double bin = (geometry.bin > 0) ? geometry.bin : 1.0;
    x = sensor_x / bin - geometry.start_x;
    y = sensor_y / bin - geometry.start_y;
}

void frameToSensor(const FrameGeometry& geometry, double x, double y, double& sensor_x, double& sensor_y)
{
    const double bin = (geometry.bin > 0) ? geometry.bin : 1.0;
    sensor_x = (x + geometry.start_x) * bin;
    sensor_y = (y + geometry.start_y) * bin;
}

double sensorToFrameScale(const FrameGeometry& geometry)
{
    return (geometry.bin > 0) ? (1.0 / geometry.bin) : 1.0;
}

// ---------------------------------------------------------------------------------------------------------------------

Reticle::Reticle() :
    x(0.0),
    y(0.0),
    gap(8.0),
    thickness(1),
    red(255),
    green(0),
    blue(0),
    centred(false),
    circles()
{
}

// ---------------------------------------------------------------------------------------------------------------------

ReticleSet::ReticleSet() :
    items_(),
    selected_(0)
{
}

// ---------------------------------------------------------------------------------------------------------------------

std::size_t ReticleSet::size() const
{
    return this->items_.size();
}

const Reticle& ReticleSet::at(std::size_t index) const
{
    return this->items_.at(index);
}

bool ReticleSet::hasSelection() const
{
    return this->selected_ < this->items_.size();
}

std::size_t ReticleSet::selected() const
{
    return this->selected_;
}

// ---------------------------------------------------------------------------------------------------------------------

void ReticleSet::addAt(double x, double y)
{
    Reticle item;
    item.x = x;
    item.y = y;
    this->items_.push_back(item);
    this->selected_ = this->items_.size() - 1;
}

void ReticleSet::addCentred()
{
    Reticle item;
    item.centred = true;
    this->items_.push_back(item);
    this->selected_ = this->items_.size() - 1;
}

bool ReticleSet::hasCentred() const
{
    for (const Reticle& item : this->items_)
    {
        if (item.centred)
            return true;
    }
    return false;
}

bool ReticleSet::removeCentred()
{
    for (std::size_t i = 0; i < this->items_.size(); ++i)
    {
        if (this->items_[i].centred)
        {
            this->items_.erase(this->items_.begin() + static_cast<std::ptrdiff_t>(i));
            this->selected_ = this->items_.size();
            return true;
        }
    }
    return false;
}

bool ReticleSet::removeSelected()
{
    if (!this->hasSelection())
        return false;
    this->items_.erase(this->items_.begin() + static_cast<std::ptrdiff_t>(this->selected_));
    this->selected_ = this->items_.size();
    return true;
}

void ReticleSet::clear()
{
    this->items_.clear();
    this->selected_ = 0;
}

// ---------------------------------------------------------------------------------------------------------------------

bool ReticleSet::selectNear(double x, double y, const FrameGeometry& geometry, double radius)
{
    // Everything here is in SENSOR pixels, the radius included, so the reach of a click is a physical distance and
    // does not silently double when the stream is binned.
    std::size_t best = this->items_.size();
    double best_distance = radius;

    for (std::size_t i = 0; i < this->items_.size(); ++i)
    {
        double rx = 0.0;
        double ry = 0.0;
        this->resolvePosition(i, geometry, rx, ry);
        const double distance = std::hypot(rx - x, ry - y);
        if (distance <= best_distance)
        {
            best_distance = distance;
            best = i;
        }
    }

    if (best >= this->items_.size())
        return false;
    this->selected_ = best;
    return true;
}

void ReticleSet::selectIndex(std::size_t index)
{
    this->selected_ = (index < this->items_.size()) ? index : this->items_.size();
}

void ReticleSet::deselect()
{
    this->selected_ = this->items_.size();
}

// ---------------------------------------------------------------------------------------------------------------------

void ReticleSet::nudgeSelected(double dx, double dy, const FrameGeometry& geometry)
{
    if (!this->hasSelection())
        return;
    Reticle& item = this->items_[this->selected_];
    if (item.centred)
        return;
    // Clamped to the SENSOR, not to the frame. A reticle may legitimately sit outside the current ROI -- that is
    // what makes it survive a ROI change -- so clamping to the visible region would drag marks about every time that
    // region moved.
    item.x = clampTo(item.x + dx, 0.0, sensorLimit(geometry.sensor_width));
    item.y = clampTo(item.y + dy, 0.0, sensorLimit(geometry.sensor_height));
}

void ReticleSet::placeSelected(double x, double y, const FrameGeometry& geometry)
{
    if (!this->hasSelection())
        return;
    Reticle& item = this->items_[this->selected_];
    if (item.centred)
        return;
    item.x = clampTo(x, 0.0, sensorLimit(geometry.sensor_width));
    item.y = clampTo(y, 0.0, sensorLimit(geometry.sensor_height));
}

void ReticleSet::resizeSelectedGap(double delta)
{
    if (!this->hasSelection())
        return;
    Reticle& item = this->items_[this->selected_];
    item.gap = clampTo(item.gap + delta, 0.0, kMaxGap);
}

void ReticleSet::resizeSelectedThickness(int delta)
{
    if (!this->hasSelection())
        return;
    Reticle& item = this->items_[this->selected_];
    item.thickness = clampInt(item.thickness + delta, kMinThickness, kMaxThickness);
}

void ReticleSet::setSelectedColour(int red, int green, int blue)
{
    if (!this->hasSelection())
        return;
    Reticle& item = this->items_[this->selected_];
    item.red = clampInt(red, 0, 255);
    item.green = clampInt(green, 0, 255);
    item.blue = clampInt(blue, 0, 255);
}

void ReticleSet::addCircleToSelected()
{
    if (!this->hasSelection())
        return;
    Reticle& item = this->items_[this->selected_];
    // Clear of the gap, so a newly added circle is visible at once rather than hidden inside it.
    item.circles.push_back(std::max(kDefaultCircleRadius, item.gap + kMinCircleRadius));
}

bool ReticleSet::removeCircleFromSelected()
{
    if (!this->hasSelection())
        return false;
    Reticle& item = this->items_[this->selected_];
    if (item.circles.empty())
        return false;
    item.circles.pop_back();
    return true;
}

void ReticleSet::resizeSelectedCircle(double delta)
{
    if (!this->hasSelection())
        return;
    Reticle& item = this->items_[this->selected_];
    if (item.circles.empty())
        return;
    item.circles.back() = std::max(kMinCircleRadius, item.circles.back() + delta);
}

// ---------------------------------------------------------------------------------------------------------------------

void ReticleSet::resolvePosition(std::size_t index, const FrameGeometry& geometry, double& x, double& y) const
{
    const Reticle& item = this->items_.at(index);
    if (item.centred)
    {
        // The centre of the ROI, expressed in sensor coordinates like everything else. Deliberately the ROI and not
        // the sensor: this is the "middle of what I am looking at" mark and it follows a ROI change on purpose. A mark
        // for a fixed optical axis is an ordinary reticle at fixed sensor coordinates, which is the other half of why
        // both kinds exist.
        //
        // Half a pixel less than the half width, so it lands on the centre of a photosite rather than on the boundary
        // between the two middle ones of an even-sized frame.
        frameToSensor(geometry, (geometry.width - 1) / 2.0, (geometry.height - 1) / 2.0, x, y);
    }
    else
    {
        x = item.x;
        y = item.y;
    }
}

// ---------------------------------------------------------------------------------------------------------------------

bool ReticleSet::save(const std::string& path) const
{
    std::ofstream file(path, std::ios::trunc);
    if (!file)
        return false;

    file << "# DegorasASI live-view reticles, version 2.\n"
         << "# One reticle per line. Coordinates are SENSOR pixels and may be fractional; a centred reticle ignores\n"
         << "# x and y and follows the frame centre. circles is a comma-separated list of radii, or absent for none.\n";

    file << std::fixed << std::setprecision(2);
    for (const Reticle& item : this->items_)
    {
        file << "reticle";
        if (item.centred)
            file << " centred=1";
        else
            file << " x=" << item.x << " y=" << item.y;
        file << " gap=" << item.gap << " thickness=" << item.thickness
             << " colour=" << item.red << "," << item.green << "," << item.blue;
        if (!item.circles.empty())
        {
            file << " circles=";
            for (std::size_t i = 0; i < item.circles.size(); ++i)
                file << (i == 0 ? "" : ",") << item.circles[i];
        }
        file << "\n";
    }
    return file.good();
}

bool ReticleSet::load(const std::string& path)
{
    std::ifstream file(path);
    if (!file)
        return false;

    std::vector<Reticle> loaded;
    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#')
            continue;

        std::istringstream stream(line);
        std::string token;
        stream >> token;
        if (token != "reticle")
            continue;

        Reticle item;
        while (stream >> token)
        {
            const std::size_t equals = token.find('=');
            if (equals == std::string::npos)
                continue;
            const std::string key = token.substr(0, equals);
            const std::string value = token.substr(equals + 1);

            try
            {
                if      (key == "x")         item.x = std::stod(value);
                else if (key == "y")         item.y = std::stod(value);
                else if (key == "gap")       item.gap = std::stod(value);
                else if (key == "thickness") item.thickness = clampInt(std::stoi(value), kMinThickness,
                                                                      kMaxThickness);
                else if (key == "centred")   item.centred = (value != "0");
                else if (key == "circles")   item.circles = parseRadii(value);
                else if (key == "colour" || key == "color")
                {
                    // The same parser as the radii. A list that is not three numbers long leaves the default red,
                    // which is better than a half-applied colour.
                    const std::vector<double> parts = parseRadii(value);
                    if (parts.size() == 3)
                    {
                        item.red = clampInt(static_cast<int>(parts[0]), 0, 255);
                        item.green = clampInt(static_cast<int>(parts[1]), 0, 255);
                        item.blue = clampInt(static_cast<int>(parts[2]), 0, 255);
                    }
                }
                // An "arm" key is read and dropped: version 1 files carry one and the shape no longer has arms.
            }
            catch (const std::exception&)
            {
                // Skipped: one unreadable field leaves that field at its constructed value.
            }
        }

        item.gap = clampTo(item.gap, 0.0, kMaxGap);
        loaded.push_back(item);
    }

    if (loaded.empty())
        return false;

    this->items_.swap(loaded);
    this->selected_ = this->items_.size();
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------

}   // END NAMESPACE

// ---------------------------------------------------------------------------------------------------------------------
