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
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

// OPENCV INCLUDES
#include <opencv2/imgcodecs.hpp>

// PROJECT INCLUDES
#include <DegorasASI/Modules/Helpers>

#include "live_controller.h"


using namespace dpasi;

// NAMESPACES
namespace live
{

namespace
{

// A tenth either way. Multiplicative rather than a fixed step because exposure spans microseconds to seconds: a step
// large enough to matter at one second is meaningless at one millisecond, and vice versa.
constexpr double kExposureUp = 1.1;
constexpr double kExposureDown = 0.9;

constexpr long long kGainStep = 10;
constexpr long long kWhiteBalanceStep = 1;

// Reticle nudging. The coarse step is a whole photosite; the fine one a tenth, because the centroid of a return does
// not land on an integer and a mark that can only sit on integers cannot be aligned with one.
constexpr double kCoarseNudge = 1.0;
constexpr double kFineNudge = 0.1;

// How near a click has to be, in sensor pixels, to grab a reticle rather than miss it.
constexpr double kGrabRadius = 24.0;

// Steps for the shape keys. Whole pixels: a sub-pixel arm length is not a thing anyone needs.
constexpr double kArmStep = 2.0;
constexpr double kGapStep = 1.0;
constexpr double kCircleStep = 2.0;

// Arrow keys as cv::waitKeyEx reports them on Windows. Named rather than inline because they are PLATFORM-SPECIFIC
// and the digit keys below exist precisely so the feature does not depend on them: if a build reports different
// codes, 4/6/8/2 still move the reticle and only this table needs revisiting.
constexpr int kKeyLeft  = 2424832;
constexpr int kKeyUp    = 2490368;
constexpr int kKeyRight = 2555904;
constexpr int kKeyDown  = 2621440;

constexpr int kKeyTab = 9;
constexpr int kKeyBackspace = 8;
constexpr int kKeyDelete = 3014656;

std::string fixed2(double v)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << v;
    return os.str();
}

std::string fixed1(double v)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << v;
    return os.str();
}

}   // namespace

// ---------------------------------------------------------------------------------------------------------------------

LiveController::LiveController(LiveModel& model, LiveView& view) :
    model_(model),
    view_(view),
    shots_(0),
    fine_(false),
    reticle_file_()
{
}

// ---------------------------------------------------------------------------------------------------------------------

void LiveController::pumpSliders()
{
    long long microseconds = 0;
    if (this->view_.takeExposureRequest(microseconds))
        this->model_.requestExposure(microseconds);

    long long gain = 0;
    if (this->view_.takeGainRequest(gain))
        this->model_.requestGain(gain);
}

void LiveController::pumpMouse()
{
    ReticleSet& set = this->view_.reticles();
    const int width = this->view_.frameWidth();
    const int height = this->view_.frameHeight();

    int x = 0;
    int y = 0;
    if (this->view_.takeMousePressInFrame(x, y))
    {
        // A press that lands nowhere near a reticle DESELECTS rather than creating one. Creation is a key, so that
        // clicking on the image to look at something cannot leave a mark behind.
        if (!set.selectNear(x, y, width, height, kGrabRadius))
            set.deselect();
    }

    // Held button plus a selection is a drag. It snaps the reticle centre to the cursor rather than preserving the
    // grab offset, which is the right trade for a viewfinder: coarse placement is the mouse's job and the keyboard
    // does the part that needs precision.
    if (this->view_.mouseHeld() && set.hasSelection())
    {
        int cursor_x = 0;
        int cursor_y = 0;
        if (this->view_.probePointInFrame(cursor_x, cursor_y))
            set.placeSelected(cursor_x, cursor_y, width, height);
    }
}

void LiveController::setReticleFile(const std::string& path)
{
    this->reticle_file_ = path;
}

bool LiveController::loadReticles()
{
    if (this->reticle_file_.empty())
        return false;
    return this->view_.reticles().load(this->reticle_file_);
}

bool LiveController::saveReticles() const
{
    if (this->reticle_file_.empty())
        return false;
    return this->view_.reticles().save(this->reticle_file_);
}

std::string LiveController::selectedReticleText() const
{
    const ReticleSet& set = this->view_.reticles();
    if (!set.hasSelection())
    {
        if (set.size() == 0)
            return std::string();
        return "reticles " + std::to_string(set.size()) + ", none selected";
    }

    const std::size_t index = set.selected();
    const Reticle& item = set.at(index);
    double x = 0.0;
    double y = 0.0;
    set.resolvePosition(index, this->view_.frameWidth(), this->view_.frameHeight(), x, y);

    // The numbers are on screen because that is what makes a fine adjustment an adjustment rather than a guess.
    std::string text = "reticle " + std::to_string(index + 1) + "/" + std::to_string(set.size()) +
                       (item.centred ? " [centred]" : "") +
                       "  x " + fixed2(x) + "  y " + fixed2(y) +
                       "  arm " + fixed1(item.arm) + "  gap " + fixed1(item.gap);
    if (!item.circles.empty())
        text += "  circle " + fixed1(item.circles.back());
    text += this->fine_ ? "  step 0.1" : "  step 1.0";
    return text;
}

bool LiveController::handleKey(int key, types::Frame& frame, const cv::Mat& display)
{
    // Reticles first, so their keys are handled whatever the rest of the map does.
    ReticleSet& set = this->view_.reticles();
    const int width = this->view_.frameWidth();
    const int height = this->view_.frameHeight();
    const double step = this->fine_ ? kFineNudge : kCoarseNudge;

    switch (key)
    {
        case 'v':
        case 'V':
            this->view_.toggleReticles();
            std::cout << "  reticles: " << (this->view_.reticlesVisible() ? "shown" : "hidden") << "\n";
            return true;

        case 'x':
        case 'X':
            if (set.hasCentred())
            {
                set.removeCentred();
                std::cout << "  centred reticle removed\n";
            }
            else
            {
                set.addCentred();
                std::cout << "  centred reticle added\n";
            }
            return true;

        case 'n':
        case 'N':
        {
            int x = 0;
            int y = 0;
            if (this->view_.probePointInFrame(x, y))
            {
                set.addAt(static_cast<double>(x), static_cast<double>(y));
                std::cout << "  reticle " << set.size() << " added at " << x << "," << y << "\n";
            }
            else
            {
                std::cout << "  point at the image first; a new reticle goes where the cursor is\n";
            }
            return true;
        }

        case kKeyTab:
            // Cycles by INDEX rather than by position: two reticles can sit on the same pixel, and selecting by
            // proximity would then never reach the second one.
            if (set.size() > 0)
                set.selectIndex(set.hasSelection() ? (set.selected() + 1) % set.size() : 0);
            return true;

        case kKeyDelete:
        case kKeyBackspace:
            if (set.removeSelected())
                std::cout << "  reticle removed, " << set.size() << " left\n";
            return true;

        case 'f':
        case 'F':
            this->fine_ = !this->fine_;
            std::cout << "  reticle step: " << (this->fine_ ? "0.1 px (fine)" : "1.0 px") << "\n";
            return true;

        case kKeyLeft:  case '4': set.nudgeSelected(-step, 0.0, width, height); return true;
        case kKeyRight: case '6': set.nudgeSelected(step, 0.0, width, height);  return true;
        case kKeyUp:    case '8': set.nudgeSelected(0.0, -step, width, height); return true;
        case kKeyDown:  case '2': set.nudgeSelected(0.0, step, width, height);  return true;

        case '[': set.resizeSelectedArm(-kArmStep); return true;
        case ']': set.resizeSelectedArm(kArmStep);  return true;
        case '-': set.resizeSelectedGap(-kGapStep); return true;
        case '=': set.resizeSelectedGap(kGapStep);  return true;

        case 'o':
        case 'O':
            set.addCircleToSelected();
            return true;

        case 'u':
        case 'U':
            set.removeCircleFromSelected();
            return true;

        case ',': set.resizeSelectedCircle(-kCircleStep); return true;
        case '.': set.resizeSelectedCircle(kCircleStep);  return true;

        case 'k':
        case 'K':
            std::cout << "  " << (this->saveReticles() ? ("reticles saved: " + this->reticle_file_)
                                                       : std::string("reticles NOT saved")) << "\n";
            return true;

        default:
            break;
    }

    switch (key)
    {
        case -1:
            return true;

        case 27:                    // ESC
        case 'q':
        case 'Q':
            return false;

        case 'h':
        case 'H':
            this->printKeys();
            return true;

        case 'a':
        case 'A':
            this->view_.toggleStretch();
            std::cout << "  auto-stretch: " << (this->view_.stretchEnabled() ? "on" : "off") << "\n";
            return true;

        case 'd':
        case 'D':
            this->view_.toggleDemosaic();
            std::cout << "  demosaic: " << (this->view_.demosaicEnabled() ? "on" : "off") << "\n";
            return true;

        case 'i':
        case 'I':
            this->view_.toggleHud();
            return true;

        // Lower case shortens, upper case lengthens. Both only enqueue: during a long exposure the change lands on
        // the next one, and the HUD shows the readback when it does.
        case 'e': this->model_.requestExposureScale(kExposureDown); return true;
        case 'E': this->model_.requestExposureScale(kExposureUp);   return true;

        case 'g': this->model_.requestGainDelta(-kGainStep); return true;
        case 'G': this->model_.requestGainDelta(kGainStep);  return true;

        case 'r': this->model_.requestWbRedDelta(-kWhiteBalanceStep);  return true;
        case 'R': this->model_.requestWbRedDelta(kWhiteBalanceStep);   return true;
        case 'b': this->model_.requestWbBlueDelta(-kWhiteBalanceStep); return true;
        case 'B': this->model_.requestWbBlueDelta(kWhiteBalanceStep);  return true;

        case 's':
        case 'S':
        {
            if (frame.empty())
            {
                std::cout << "  nothing to save yet\n";
                return true;
            }
            // Saved through the library's own writer, not through OpenCV: it is the one that knows the vendor's
            // channel order and the mosaic phase, it keeps full depth, and it keeps these files identical to the
            // other examples'.
            const std::string path = "live_" + std::to_string(++this->shots_) + "." +
                                     imgio::extensionFor(frame.format);
            std::cout << "  saved: " << (imgio::writeFrame(frame, path) ? path : "WRITE FAILED") << "\n";
            return true;
        }

        case 'p':
        case 'P':
        {
            if (display.empty())
            {
                std::cout << "  nothing on screen yet\n";
                return true;
            }
            // The screen image, stretch and all. Deliberately NOT a substitute for S: this is 8-bit and already
            // transformed, so it is a look, not data.
            const std::string path = "live_view_" + std::to_string(++this->shots_) + ".png";
            std::cout << "  " << (cv::imwrite(path, display) ? ("saved: " + path) : "PNG WRITE FAILED") << "\n";
            return true;
        }

        default:
            return true;
    }
}

void LiveController::printKeys() const
{
    std::cout <<
        "Keys, while the window has focus:\n"
        "  Q / ESC        quit\n"
        "  E / shift+E    exposure  -10% / +10%   (multiplicative: a fixed step is useless across us..s)\n"
        "  G / shift+G    gain      -10  / +10\n"
        "  R / shift+R    WB red    -1   / +1     (colour cameras only)\n"
        "  B / shift+B    WB blue   -1   / +1\n"
        "  A              toggle the percentile auto-stretch\n"
        "  D              toggle demosaicing (raw Bayer <-> colour)\n"
        "  I              toggle the HUD\n"
        "\n"
        "Reticles (sensor coordinates, sub-pixel; the numbers are on the HUD):\n"
        "  V              show / hide every reticle\n"
        "  X              add / remove the CENTRED reticle\n"
        "  N              new reticle where the cursor is\n"
        "  click          select the reticle under the cursor; drag to move it\n"
        "  TAB            select the next reticle\n"
        "  DEL / BACKSPC  remove the selected reticle\n"
        "  arrows, 4682   move the selected reticle (the digits always work; arrows are platform-dependent)\n"
        "  F              fine step: 0.1 px instead of 1 px\n"
        "  [ / ]          cross arm shorter / longer\n"
        "  - / =          central gap smaller / larger\n"
        "  O / U          add a circle / remove the last one\n"
        "  , / .          last circle radius smaller / larger\n"
        "  K              save the reticles now (they are also saved on exit)\n"
        "  S              save the frame through the library's own writer (FITS/BMP, full depth)\n"
        "  P              save what is on screen as PNG (8-bit, stretched -- for a quick look)\n"
        "  H              print these keys again\n"
        "Sliders: exposure (logarithmic) and gain. Hover the image to read the pixel under the cursor.\n"
        "A change made during a long exposure applies to the NEXT frame; the window stays responsive throughout.\n";
}

// ---------------------------------------------------------------------------------------------------------------------

}   // END NAMESPACE

// ---------------------------------------------------------------------------------------------------------------------
