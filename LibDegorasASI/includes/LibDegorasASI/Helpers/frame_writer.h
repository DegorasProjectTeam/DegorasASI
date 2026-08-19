/*
 *  LibDegorasASI - An extensible C++ library for controlling ZWO ASI astronomy cameras.
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

// PROJECT INCLUDES
#include "LibDegorasASI/Global/libdegorasasi_export.h"
#include "LibDegorasASI/Common/common_types.h"


// NAMESPACES
namespace dpasi
{
namespace imgio
{

// ---------------------------------------------------------------------------------------------------------------------
// Writing a captured frame to a file, and glancing at one without a viewer.
//
// This lives in the library rather than in an example for one reason: it encodes what the VENDOR DOES NOT DOCUMENT
// about its own pixel data. Both facts below were measured on hardware, and a consumer that had to rediscover them
// would get silently wrong images until it did:
//
//   * RGB24 arrives as B,G,R -- not R,G,B. Get it wrong and reds and blues swap, which is easy to miss on a dim scene.
//   * RAW16 already spans the full 16-bit range on a 12-bit sensor (the SDK scales rather than shifts), so it needs no
//     shifting. Shifting it "to correct for 12 bits" would darken the image by a factor of 16.
//
// Nothing here depends on a GUI toolkit, an imaging library or the vendor SDK: only <fstream> and the Frame type. The
// library still draws nothing and owns no image pipeline -- it just refuses to make every consumer relearn the format.
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Write an RGB24 frame as a 24-bit BMP.
 * @param frame The frame to write; must be ImageFormat::RGB24.
 * @param path Destination path.
 * @return False if the frame is not RGB24, is empty or inconsistent, or the file could not be written.
 * @note Chosen for colour because BMP stores pixels B,G,R exactly as the SDK delivers them, so a row is a straight
 *       copy, and because a desktop opens one on a double click. Rows are written BOTTOM-UP and padded to a multiple
 *       of four bytes, as the format requires.
 */
LIBDEGORASASI_EXPORT bool writeBmp(const types::Frame& frame, const std::string& path);

/**
 * @brief Write a single-channel frame (RAW8, Y8 or RAW16) as a binary PGM.
 * @param frame The frame to write; must be ImageFormat::RAW8, Y8 or RAW16.
 * @param path Destination path.
 * @return False if the frame is not single-channel, is empty, or the file could not be written.
 * @note PGM carries 16 bits, which BMP cannot, and every astronomy tool and image editor reads it. Its 16-bit samples
 *       are BIG-endian while the SDK delivers little-endian, so RAW16 is byte-swapped on the way out; the values
 *       themselves are written unchanged, because they already span the full 16-bit range.
 * @warning RAW8 and RAW16 from a COLOUR camera are Bayer-mosaiced, not grey pictures: written as-is they look like a
 *          fine checkerboard until demosaiced. Capture ImageFormat::RGB24 when a viewable colour image is wanted --
 *          the SDK demosaics that one for you. The raw formats are for processing, not for looking at.
 */
LIBDEGORASASI_EXPORT bool writePgm(const types::Frame& frame, const std::string& path);

// -- FITS ------------------------------------------------------------------------------------------------------------
//
// The archival format for astronomy, and the reason this module carries a third writer: BMP and PGM are for looking at
// a frame, FITS is for keeping it. Everything a pipeline needs later -- when it was taken, how long for, at what gain,
// which Bayer mosaic to demosaic with -- travels inside the file rather than in a filename convention.

/// One FITS header card. Build these with @ref fitsInt, @ref fitsReal or @ref fitsText rather than by hand, so the
/// value is formatted the way the standard requires.
struct LIBDEGORASASI_EXPORT FitsCard
{
    std::string key;       ///< Keyword, up to 8 characters. Lower case is upper-cased on the way out.
    std::string value;     ///< Value, already formatted for FITS (quoted for text, bare for numbers).
    std::string comment;   ///< Optional comment; truncated if the card would exceed its 80 columns.
};

using FitsCards = std::vector<FitsCard>;   ///< Extra cards to record alongside the mandatory ones.

/// @brief A card holding an integer, e.g. fitsInt("GAIN", 450, "sensor gain").
LIBDEGORASASI_EXPORT FitsCard fitsInt(const std::string& key, long long value, const std::string& comment = {});

/// @brief A card holding a floating-point value, e.g. fitsReal("EXPTIME", 0.2, "exposure time in seconds").
LIBDEGORASASI_EXPORT FitsCard fitsReal(const std::string& key, double value, const std::string& comment = {});

/// @brief A card holding text, e.g. fitsText("OBJECT", "M31", "target"). Quoting is handled here.
LIBDEGORASASI_EXPORT FitsCard fitsText(const std::string& key, const std::string& value,
                                       const std::string& comment = {});

/**
 * @brief The BAYERPAT card for a colour sensor's mosaic, e.g. BayerPattern::RG becomes "RGGB".
 * @note Use this rather than building the string from types::toString(). The SDK names a pattern by its first ROW
 *       ("RG"), FITS by the whole 2x2 CELL ("RGGB"), and only RG is completed by "GB" -- appending it to the others
 *       produces BGGB, GRGB and GBGB, which are not Bayer patterns.
 * @note The name is the mosaic at the SENSOR'S TOP-LEFT, which is what the convention means and what a datasheet
 *       quotes. @ref writeFits stores rows top-down and says so, so this needs no adjustment for orientation.
 * @warning Meaningful only on a raw frame from a colour camera at bin 1. @ref writeFits drops the card otherwise.
 */
LIBDEGORASASI_EXPORT FitsCard fitsBayerPattern(types::BayerPattern pattern);

/**
 * @brief Write a frame as a FITS image.
 * @param frame The frame to write. Any of RAW8, Y8, RAW16 or RGB24.
 * @param path Destination path.
 * @param extra Extra header cards, written after the mandatory ones. Exposure, gain, sensor temperature and the Bayer
 *        pattern belong here: the frame itself does not carry them, and a FITS without them is far less useful to
 *        whatever reads it years later.
 * @return False if the frame is empty or inconsistent, or the file could not be written.
 *
 * @note Layout follows the standard: 2880-byte blocks, big-endian samples, and the first axis varying fastest. Rows go
 *       out TOP-DOWN -- as the sensor delivers them -- and a ROWORDER card says so. FITS is often said to put the
 *       first pixel at the lower left, but the standard does not require it: that is a recommendation from WCS Paper
 *       I, and practice went the other way. Siril, PixInsight, DeepSkyStacker, ASTAP and KStars all assume TOP-DOWN
 *       when ROWORDER is absent, and ZWO's ASIStudio does not read the card at all. Storing rows bottom-up would
 *       therefore be wrong for every reader, and on a colour camera it would also shift the Bayer mosaic by one row.
 * @note Siril renders every image bottom-up, so it displays these frames inverted -- as it does for INDI, N.I.N.A.
 *       and SharpCap files, for the same reason. The colour is right, which is what the mosaic phase governs.
 * @note Samples are ALWAYS written as BITPIX 16, including for an 8-bit frame, with BZERO 32768 because that BITPIX is
 *       SIGNED in FITS while sensor data is unsigned; readers apply BZERO automatically, so the values come back
 *       unchanged. BITPIX 8 is perfectly legal and would be the obvious choice for an 8-bit frame, but 8-bit FITS is
 *       rare in astronomy and widely unimplemented -- ZWO's own ASIStudio refuses to open one at all. An 8-bit value
 *       fits a 16-bit sample exactly, so nothing is lost by promoting it. Values keep the sensor's own ADU rather than
 *       being stretched to fill the range, because photometry needs the real numbers; DATAMIN and DATAMAX record the
 *       range actually present so a viewer can scale its display.
 * @note RGB24 becomes a three-plane cube (NAXIS3 = 3) de-interleaved into R, G, B plane order -- FITS stores colour
 *       plane by plane, and the SDK delivers it interleaved as B,G,R.
 * @warning A RAW8 or RAW16 frame from a colour camera is Bayer-mosaiced. Pass the pattern in @p extra by calling
 *          @ref fitsBayerPattern; without it the frame will be shown as grey.
 * @note A BAYERPAT card in @p extra is DROPPED when the frame is binned or is RGB24, because neither carries a mosaic
 *       any more: binning sums neighbouring photosites and the SDK has already demosaiced RGB24. Labelling either
 *       would make a reader invent colour from data that has none.
 * @note XBAYROFF and YBAYROFF are always DERIVED from the frame's own @ref types::Frame::start_x and start_y and are
 *       never taken from @p extra, so a windowed capture at an odd origin -- which the vendor accepts silently, and
 *       which shifts the mosaic by one photosite -- stays correctly labelled without the caller having to know. They
 *       are written only alongside a surviving BAYERPAT, since with no mosaic they mean nothing.
 */
LIBDEGORASASI_EXPORT bool writeFits(const types::Frame& frame, const std::string& path,
                                    const FitsCards& extra = FitsCards());

// -- Dispatch ----------------------------------------------------------------------------------------------------------

/**
 * @brief Write a frame with whichever writer fits its format: BMP for RGB24, PGM otherwise.
 * @return False if the format is not one this module writes, or the file could not be written.
 * @note Chooses the format meant for LOOKING at a frame. Call @ref writeFits directly when the frame is being kept.
 */
LIBDEGORASASI_EXPORT bool writeFrame(const types::Frame& frame, const std::string& path);

/// @brief The conventional file extension for the format, without a dot: "bmp" for RGB24, "pgm" otherwise.
LIBDEGORASASI_EXPORT std::string extensionFor(types::ImageFormat format);

/**
 * @brief Render a frame as a coarse ASCII brightness map.
 * @param frame The frame to render.
 * @param columns Width of the map in characters. The aspect ratio is preserved and halved, because a character cell
 *        is about twice as tall as it is wide and the picture would otherwise come out stretched.
 * @return The map as newline-separated rows, or an empty string if the frame is empty or @p columns is not positive.
 * @note Answers "is the camera seeing anything at all?" with no file, no viewer and no toolkit, which is the first
 *       question when a camera is plugged in for the first time. Returned rather than printed so it can go to a log,
 *       a status pane or a test assertion just as easily as to a terminal.
 */
LIBDEGORASASI_EXPORT std::string framePreview(const types::Frame& frame, int columns = 64);

// ---------------------------------------------------------------------------------------------------------------------

}} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
