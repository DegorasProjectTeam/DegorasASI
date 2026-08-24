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
#include <DegorasASI/Modules/Common>


// NAMESPACES
namespace live
{

// ---------------------------------------------------------------------------------------------------------------------
// PIXELS ONLY. Nothing here knows about a camera, a window or a thread: it turns a Frame into something showable, and
// that is the whole of it. Keeping it separate is what lets the model run on its own thread and the view stay on the
// main one without either of them owning the conversion.
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief What the viewer should do to a frame before showing it.
 * @note Defaults live in the constructor rather than in the member declarations, so this header stays a description
 *       of the type and nothing else.
 */
struct DisplayOptions
{
    /// @brief Establishes the defaults: no stretch, demosaic on.
    DisplayOptions();

    bool stretch;    ///< Map the frame onto 8 bits through a percentile window instead of a plain shift.
    bool demosaic;   ///< Interpolate colour from a raw Bayer frame. Ignored when the frame carries no mosaic.
};

/**
 * @brief The OpenCV Bayer conversion code for a frame, accounting for its ROI phase.
 * @param pattern The sensor's colour-filter order, as the SDK names it (by the FIRST row of the 2x2 cell).
 * @param frame   The frame to be converted; its format, binning and ROI origin all take part.
 * @return A cv::COLOR_Bayer*2BGR value, or -1 when the frame carries no mosaic to undo.
 * @note The SDK and OpenCV name Bayer patterns by DIFFERENT rows of the cell, so the letters do not correspond and a
 *       direct mapping swaps red and blue. See the implementation for the derivation.
 */
int bayerCodeFor(dpasi::types::BayerPattern pattern, const dpasi::types::Frame& frame);

/**
 * @brief Wraps a frame's bytes in a cv::Mat without copying them.
 * @param frame The frame to view. Must outlive the returned Mat.
 * @return A Mat sharing the frame's buffer, or an empty Mat for a format that cannot be shown.
 */
cv::Mat wrapFrame(dpasi::types::Frame& frame);

/**
 * @brief Maps an image onto 8 bits using a percentile window.
 * @param src      Source image, CV_8U or CV_16U, one or three channels.
 * @param dst      Destination, 8-bit, same geometry and channel count as @p src.
 * @param low_pct  Fraction of pixels to clip at the bottom, as 0..1.
 * @param high_pct Cumulative fraction at which to clip the top, as 0..1.
 * @note A min/max stretch is defeated by a single hot pixel; a percentile window is not, which is why the limits are
 *       parameters here rather than being derived from the extremes.
 */
void stretchToEightBits(const cv::Mat& src, cv::Mat& dst, double low_pct, double high_pct);

/**
 * @brief Turns a frame into the 8-bit BGR image that goes on screen.
 * @param frame The frame to render. Not modified, but taken by reference because wrapping it is non-const.
 * @param opts  What to do to it on the way.
 * @param bayer The conversion code from bayerCodeFor(), or -1 for no demosaicing.
 * @param out   Destination image, CV_8UC3.
 */
void buildDisplay(dpasi::types::Frame& frame, const DisplayOptions& opts, int bayer, cv::Mat& out);

/**
 * @brief Reads one pixel at NATIVE depth and describes it, for the on-screen probe.
 * @param frame The frame to read.
 * @param x     Column in frame coordinates.
 * @param y     Row in frame coordinates.
 * @return Something like "(310,244) raw 4821 / 65535", or an empty string when the point is outside the frame.
 * @note Native depth, not the displayed value: the displayed one has already been through the stretch, and the raw
 *       number is what tells you whether the exposure is saturating.
 */
std::string describePixel(dpasi::types::Frame& frame, int x, int y);

/**
 * @brief What a frame's samples look like: a histogram of what is on screen, plus what the SENSOR actually did.
 * @note TWO DIFFERENT MEASUREMENTS ON PURPOSE, and the distinction is the whole point of the thing. The histogram is
 *       taken from the DISPLAY image, so it describes what is being looked at. The clipping figures are taken from the
 *       RAW frame against its format's full scale, because saturation is only real there: with the auto-stretch on the
 *       display is pushed to 255 by construction, so a display histogram alone would always look full and would hide
 *       exactly the condition anybody consults a histogram to find.
 */
struct FrameStats
{
    /// @brief Establishes empty statistics, describing no frame.
    FrameStats();

    std::vector<int> blue;      ///< 256 bins of the display image, dark to bright. Empty when there was no frame.
    std::vector<int> green;     ///< As above, green channel.
    std::vector<int> red;       ///< As above, red channel.
    bool has_colour;            ///< Whether the three channels are different; false for a mono or unmosaiced frame.
    int tallest_bin;            ///< Largest count in any channel, for scaling a plot.
    double clipped_high_pct;    ///< Percentage of RAW samples at the top of the format's range.
    double clipped_low_pct;     ///< Percentage of RAW samples at zero.
    double mean_pct;            ///< Mean RAW sample, as a percentage of the format's full scale.
};

/**
 * @brief Measures a frame and the image built from it.
 * @param frame   The frame, for the raw-sample figures. Non-const because wrapping it needs its buffer.
 * @param display The 8-bit BGR image built from that frame, for the histogram. May be empty.
 * @param stats   Receives the measurements; left empty when there is nothing to measure.
 */
void computeStats(dpasi::types::Frame& frame, const cv::Mat& display, FrameStats& stats);

// ---------------------------------------------------------------------------------------------------------------------

}   // END NAMESPACE

// ---------------------------------------------------------------------------------------------------------------------
