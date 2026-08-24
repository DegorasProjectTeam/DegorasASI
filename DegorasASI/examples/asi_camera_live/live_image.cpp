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
#include <cstdint>
#include <sstream>
#include <vector>

// OPENCV INCLUDES
#include <opencv2/imgproc.hpp>

// PROJECT INCLUDES
#include "live_image.h"


using namespace dpasi;

// NAMESPACES
namespace live
{

namespace
{

// ---------------------------------------------------------------------------------------------------------------------
// BAYER: FROM THE SDK'S NAMING TO OPENCV'S, WITH THE ROI PHASE APPLIED
//
// This is the one place in the example where a plausible-looking line is wrong, so it is done the long way.
//
// The SDK names a pattern by its FIRST ROW: types::BayerPattern::RG means the sensor's top-left 2x2 cell reads
// R G / G B, i.e. what a datasheet calls RGGB. OpenCV names its conversion codes by the SECOND row instead --
// COLOR_BayerC1C2BGR takes C1 from row 1 column 1 and C2 from row 1 column 2 -- so the letters do NOT correspond:
//
//     SDK RG (RGGB)  ->  cv::COLOR_BayerBG2BGR      SDK GR (GRBG)  ->  cv::COLOR_BayerGB2BGR
//     SDK BG (BGGR)  ->  cv::COLOR_BayerRG2BGR      SDK GB (GBRG)  ->  cv::COLOR_BayerGR2BGR
//
// Writing COLOR_BayerRG2BGR for a pattern the SDK calls RG swaps red and blue, which on a star field looks merely
// "a bit off" rather than obviously broken -- the failure mode that cost a day when the FITS writer had it. The
// mapping below was verified against the vendor's own demosaicer on the same scene: an RGB24 capture and a RAW16
// capture demosaiced here agreed to within 0.1 on all three channel means.
//
// Rather than hard-code that table, the 2x2 cell is spelled out as letters, the ROI phase is applied to it, and the
// code is derived from the result. The phase matters: the pattern is a fixed property of the SENSOR's top-left
// photosite and does not change with the ROI, but an ODD ROI origin shifts which photosite the frame starts on, and
// the vendor accepts an odd origin silently. frame_writer.cpp solves the same problem for FITS with
// mosaicOffset() = ((start % 2) + 2) % 2 -- a POSITIVE modulo -- and this must agree with it, frame for frame.

struct BayerCell
{
    char c[2][2];
};

BayerCell bayerCell(types::BayerPattern pattern)
{
    // Indexed [row][column] from the sensor's top-left photosite, using the four-letter datasheet spelling.
    switch (pattern)
    {
        case types::BayerPattern::RG: return {{{'R', 'G'}, {'G', 'B'}}};   // RGGB
        case types::BayerPattern::BG: return {{{'B', 'G'}, {'G', 'R'}}};   // BGGR
        case types::BayerPattern::GR: return {{{'G', 'R'}, {'B', 'G'}}};   // GRBG
        case types::BayerPattern::GB: return {{{'G', 'B'}, {'R', 'G'}}};   // GBRG
    }
    return {{{'R', 'G'}, {'G', 'B'}}};
}

/// Positive modulo 2 of a ROI origin: the phase the frame starts on. Mirrors mosaicOffset() in frame_writer.cpp.
int mosaicOffset(int start)
{
    return ((start % 2) + 2) % 2;
}

}   // namespace

// ---------------------------------------------------------------------------------------------------------------------

DisplayOptions::DisplayOptions() :
    stretch(false),
    demosaic(true)
{
}

// ---------------------------------------------------------------------------------------------------------------------

int bayerCodeFor(types::BayerPattern pattern, const types::Frame& frame)
{
    // No mosaic to undo. Binning sums neighbouring photosites, which destroys the pattern outright, and RGB24 has
    // already been demosaiced by the SDK. Same rule as the FITS writer's mosaic_gone, and the same reason.
    if (frame.bin > 1 || frame.format == types::ImageFormat::RGB24)
        return -1;

    const BayerCell cell = bayerCell(pattern);
    const int dx = mosaicOffset(frame.start_x);
    const int dy = mosaicOffset(frame.start_y);

    // The cell as the FRAME sees it, shifted by the ROI phase.
    const char c11 = cell.c[(1 + dy) % 2][(1 + dx) % 2];
    const char c12 = cell.c[(1 + dy) % 2][(2 + dx) % 2];

    // OpenCV's C1C2 are row 1, columns 1 and 2 of the shifted cell.
    if (c11 == 'B' && c12 == 'G') return cv::COLOR_BayerBG2BGR;
    if (c11 == 'R' && c12 == 'G') return cv::COLOR_BayerRG2BGR;
    if (c11 == 'G' && c12 == 'B') return cv::COLOR_BayerGB2BGR;
    if (c11 == 'G' && c12 == 'R') return cv::COLOR_BayerGR2BGR;
    return -1;
}

cv::Mat wrapFrame(types::Frame& frame)
{
    const int rows = frame.height;
    const int cols = frame.width;

    switch (frame.format)
    {
        // Already B,G,R, which is cv::Mat 8UC3 order. Nothing to convert.
        case types::ImageFormat::RGB24:
            return cv::Mat(rows, cols, CV_8UC3, frame.data.data(), static_cast<std::size_t>(cols) * 3u);

        case types::ImageFormat::RAW8:
        case types::ImageFormat::Y8:
            return cv::Mat(rows, cols, CV_8UC1, frame.data.data(), static_cast<std::size_t>(cols));

        // Little-endian 16-bit, which is what CV_16U expects on this platform, so the bytes need no swapping.
        case types::ImageFormat::RAW16:
            return cv::Mat(rows, cols, CV_16UC1, frame.data.data(), static_cast<std::size_t>(cols) * 2u);

        default:
            return cv::Mat();
    }
}

void stretchToEightBits(const cv::Mat& src, cv::Mat& dst, double low_pct, double high_pct)
{
    const int    bins    = (src.depth() == CV_16U) ? 4096 : 256;
    const double max_val = (src.depth() == CV_16U) ? 65536.0 : 256.0;

    // The histogram is taken on a single channel: a colour image is stretched by one common transform so the white
    // balance is not silently altered by stretching each channel to its own limits.
    cv::Mat grey;
    if (src.channels() == 3)
        cv::cvtColor(src, grey, cv::COLOR_BGR2GRAY);
    else
        grey = src;

    cv::Mat hist;
    cv::calcHist(std::vector<cv::Mat>{grey}, {0}, cv::Mat(), hist, {bins},
                 std::vector<float>{0.0f, static_cast<float>(max_val)});

    const double total   = static_cast<double>(grey.total());
    const double want_lo = total * low_pct;
    const double want_hi = total * high_pct;

    double acc = 0.0;
    int bin_lo = 0;
    int bin_hi = bins - 1;
    for (int i = 0; i < bins; ++i)
    {
        acc += hist.at<float>(i);
        if (acc <= want_lo)
            bin_lo = i;
        if (acc <= want_hi)
            bin_hi = i;
    }

    const double scale = max_val / bins;
    const double lo = bin_lo * scale;
    double hi = (bin_hi + 1) * scale;
    if (hi <= lo)
        hi = lo + 1.0;

    const double alpha = 255.0 / (hi - lo);
    src.convertTo(dst, CV_8U, alpha, -lo * alpha);
}

FrameStats::FrameStats() :
    blue(),
    green(),
    red(),
    has_colour(false),
    tallest_bin(0),
    clipped_high_pct(0.0),
    clipped_low_pct(0.0),
    mean_pct(0.0)
{
}

// ---------------------------------------------------------------------------------------------------------------------

void computeStats(types::Frame& frame, const cv::Mat& display, FrameStats& stats)
{
    stats = FrameStats();

    // -- The histogram: what is on screen -----------------------------------------------------------------------------
    if (!display.empty() && display.type() == CV_8UC3)
    {
        const int bins = 256;
        const float range[] = {0.0f, 256.0f};
        const float* ranges[] = {range};

        std::vector<int>* targets[3] = {&stats.blue, &stats.green, &stats.red};
        for (int channel = 0; channel < 3; ++channel)
        {
            cv::Mat hist;
            cv::calcHist(&display, 1, &channel, cv::Mat(), hist, 1, &bins, ranges);

            targets[channel]->resize(static_cast<std::size_t>(bins));
            for (int i = 0; i < bins; ++i)
            {
                const int count = static_cast<int>(hist.at<float>(i));
                (*targets[channel])[static_cast<std::size_t>(i)] = count;
                stats.tallest_bin = std::max(stats.tallest_bin, count);
            }
        }

        // Compared rather than assumed: a mono frame, and a colour frame with demosaicing off, both arrive here as
        // three identical channels, and drawing three curves on top of each other would suggest a measurement that
        // was never made.
        stats.has_colour = (stats.blue != stats.green) || (stats.green != stats.red);
    }

    // -- The clipping: what the sensor did -----------------------------------------------------------------------------
    cv::Mat raw = wrapFrame(frame);
    if (raw.empty())
        return;

    // Full scale of the FORMAT, not of the sensor. A 12-bit sensor delivered as RAW16 is left-aligned by the SDK, so
    // its saturated samples do reach the top of the 16-bit range.
    const double full_scale = (raw.depth() == CV_16U) ? 65535.0 : 255.0;

    const cv::Mat flat = raw.isContinuous() ? raw.reshape(1) : raw.clone().reshape(1);
    const double samples = static_cast<double>(flat.total());
    if (samples <= 0.0)
        return;

    cv::Mat mask;
    cv::compare(flat, full_scale, mask, cv::CMP_GE);
    stats.clipped_high_pct = 100.0 * cv::countNonZero(mask) / samples;

    cv::compare(flat, 0.0, mask, cv::CMP_LE);
    stats.clipped_low_pct = 100.0 * cv::countNonZero(mask) / samples;

    stats.mean_pct = 100.0 * cv::mean(flat)[0] / full_scale;
}

// ---------------------------------------------------------------------------------------------------------------------

void buildDisplay(types::Frame& frame, const DisplayOptions& opts, int bayer, cv::Mat& out)
{
    cv::Mat view = wrapFrame(frame);
    if (view.empty())
    {
        out = cv::Mat();
        return;
    }

    cv::Mat colour;
    if (bayer >= 0 && opts.demosaic)
    {
        // Demosaic at native depth, so a 16-bit frame keeps its dynamic range through the interpolation and only
        // loses it in the stretch below, where the loss is deliberate.
        cv::cvtColor(view, colour, bayer);
    }
    else
    {
        colour = view;
    }

    cv::Mat eight;
    if (opts.stretch)
    {
        stretchToEightBits(colour, eight, 0.005, 0.995);
    }
    else if (colour.depth() == CV_16U)
    {
        // No stretch on a 16-bit frame means the top 8 bits, which on a 12-bit sensor is a nearly black window.
        // It is offered anyway because it is the only view that shows the raw scale honestly.
        colour.convertTo(eight, CV_8U, 1.0 / 257.0);
    }
    else
    {
        eight = colour;
    }

    if (eight.channels() == 1)
        cv::cvtColor(eight, out, cv::COLOR_GRAY2BGR);
    else
        out = eight;
}

std::string describePixel(types::Frame& frame, int x, int y)
{
    if (x < 0 || y < 0 || x >= frame.width || y >= frame.height)
        return std::string();

    cv::Mat view = wrapFrame(frame);
    if (view.empty())
        return std::string();

    std::ostringstream os;
    os << "(" << x << "," << y << ") ";
    if (view.type() == CV_16UC1)
        os << "raw " << view.at<std::uint16_t>(y, x) << " / 65535";
    else if (view.type() == CV_8UC1)
        os << "raw " << static_cast<int>(view.at<std::uint8_t>(y, x)) << " / 255";
    else if (view.type() == CV_8UC3)
    {
        const cv::Vec3b p = view.at<cv::Vec3b>(y, x);
        os << "B" << int(p[0]) << " G" << int(p[1]) << " R" << int(p[2]);
    }
    return os.str();
}

// ---------------------------------------------------------------------------------------------------------------------

}   // END NAMESPACE

// ---------------------------------------------------------------------------------------------------------------------
