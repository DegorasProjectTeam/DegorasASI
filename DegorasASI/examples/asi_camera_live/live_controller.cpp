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
#include <iostream>
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

}   // namespace

// ---------------------------------------------------------------------------------------------------------------------

LiveController::LiveController(LiveModel& model, LiveView& view) :
    model_(model),
    view_(view),
    shots_(0)
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

bool LiveController::handleKey(int key, types::Frame& frame, const cv::Mat& display)
{
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

        case 'x':
        case 'X':
            this->view_.toggleCrosshair();
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
        "  X              toggle the centre crosshair\n"
        "  S              save the frame through the library's own writer (FITS/BMP, full depth)\n"
        "  P              save what is on screen as PNG (8-bit, stretched -- for a quick look)\n"
        "  H              print these keys again\n"
        "Sliders: exposure (logarithmic) and gain. Hover the image to read the pixel under the cursor.\n"
        "A change made during a long exposure applies to the NEXT frame; the window stays responsive throughout.\n";
}

// ---------------------------------------------------------------------------------------------------------------------

}   // END NAMESPACE

// ---------------------------------------------------------------------------------------------------------------------
