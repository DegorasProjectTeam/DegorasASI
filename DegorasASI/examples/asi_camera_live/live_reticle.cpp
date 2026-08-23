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

/// Below this a cross is not a cross, so the arm is never allowed under it.
constexpr double kMinArm = 4.0;

/// Same for a circle: a two-pixel ring is a dot.
constexpr double kMinCircleRadius = 3.0;

double clampTo(double value, double low, double high)
{
    return std::max(low, std::min(high, value));
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

Reticle::Reticle() :
    x(0.0),
    y(0.0),
    arm(40.0),
    gap(8.0),
    thickness(1),
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

bool ReticleSet::selectNear(double x, double y, int frame_width, int frame_height, double radius)
{
    std::size_t best = this->items_.size();
    double best_distance = radius;

    for (std::size_t i = 0; i < this->items_.size(); ++i)
    {
        double rx = 0.0;
        double ry = 0.0;
        this->resolvePosition(i, frame_width, frame_height, rx, ry);
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

void ReticleSet::nudgeSelected(double dx, double dy, int frame_width, int frame_height)
{
    if (!this->hasSelection())
        return;
    Reticle& item = this->items_[this->selected_];
    if (item.centred)
        return;
    item.x = clampTo(item.x + dx, 0.0, static_cast<double>(frame_width - 1));
    item.y = clampTo(item.y + dy, 0.0, static_cast<double>(frame_height - 1));
}

void ReticleSet::placeSelected(double x, double y, int frame_width, int frame_height)
{
    if (!this->hasSelection())
        return;
    Reticle& item = this->items_[this->selected_];
    if (item.centred)
        return;
    item.x = clampTo(x, 0.0, static_cast<double>(frame_width - 1));
    item.y = clampTo(y, 0.0, static_cast<double>(frame_height - 1));
}

void ReticleSet::resizeSelectedArm(double delta)
{
    if (!this->hasSelection())
        return;
    Reticle& item = this->items_[this->selected_];
    item.arm = std::max(kMinArm, item.arm + delta);
    // The gap can never swallow the arm, or the cross disappears while looking like it is still configured.
    item.gap = std::min(item.gap, item.arm - 1.0);
}

void ReticleSet::resizeSelectedGap(double delta)
{
    if (!this->hasSelection())
        return;
    Reticle& item = this->items_[this->selected_];
    item.gap = clampTo(item.gap + delta, 0.0, item.arm - 1.0);
}

void ReticleSet::addCircleToSelected()
{
    if (!this->hasSelection())
        return;
    Reticle& item = this->items_[this->selected_];
    // At the arm radius, so a newly added circle is immediately visible rather than hidden inside the gap.
    item.circles.push_back(std::max(kMinCircleRadius, item.arm));
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

void ReticleSet::resolvePosition(std::size_t index, int frame_width, int frame_height, double& x, double& y) const
{
    const Reticle& item = this->items_.at(index);
    if (item.centred)
    {
        // Half a pixel less than the half width, so the mark sits on the centre of the sensor rather than on the
        // boundary between the two middle photosites of an even-sized frame.
        x = (frame_width - 1) / 2.0;
        y = (frame_height - 1) / 2.0;
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

    file << "# DegorasASI live-view reticles, version 1.\n"
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
        file << " arm=" << item.arm << " gap=" << item.gap << " thickness=" << item.thickness;
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
                else if (key == "arm")       item.arm = std::stod(value);
                else if (key == "gap")       item.gap = std::stod(value);
                else if (key == "thickness") item.thickness = std::max(1, std::stoi(value));
                else if (key == "centred")   item.centred = (value != "0");
                else if (key == "circles")   item.circles = parseRadii(value);
            }
            catch (const std::exception&)
            {
                // Skipped: one unreadable field leaves that field at its constructed value.
            }
        }

        item.arm = std::max(kMinArm, item.arm);
        item.gap = clampTo(item.gap, 0.0, item.arm - 1.0);
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
