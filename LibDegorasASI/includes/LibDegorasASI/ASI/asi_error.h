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

// PROJECT INCLUDES
#include "LibDegorasASI/Global/libdegorasasi_export.h"
#include "LibDegorasASI/Common/common_types.h"


// NAMESPACES
namespace dpasi
{
namespace asi
{

// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Raw ZWO/ASI return codes, mirroring the vendor's ASI_ERROR_CODE ordinals.
 * @note Declared here rather than taken from the vendor header so that this mapping — and its unit tests — need no
 *       access to the SDK. The Layer-2 adapter passes the vendor's code through as a plain int.
 * @warning These ordinals are an ABI detail of the installed SDK: never persist or wire-serialise them. The vendor's
 *          own header and its PDF disagree on the terminating value, so never bounds-check against a documented count.
 */
enum class AsiError : int
{
    SUCCESS                 = 0,   ///< Operation succeeded.
    INVALID_INDEX           = 1,   ///< No camera connected, or the index is out of range.
    INVALID_ID              = 2,   ///< No camera with this identifier.
    INVALID_CONTROL_TYPE    = 3,   ///< The camera does not have this control.
    CAMERA_CLOSED           = 4,   ///< The camera was not opened.
    CAMERA_REMOVED          = 5,   ///< The camera could not be found; it may have been unplugged.
    INVALID_PATH            = 6,   ///< The given file path does not exist.
    INVALID_FILEFORMAT      = 7,   ///< The given file is not in the expected format.
    INVALID_SIZE            = 8,   ///< Invalid ROI geometry (violates the width/height alignment rules).
    INVALID_IMGTYPE         = 9,   ///< Unsupported image format -- or, per the vendor, bad geometry or binning.
    OUTOF_BOUNDARY          = 10,  ///< The ROI start position puts the image outside the sensor.
    TIMEOUT                 = 11,  ///< The call timed out without producing data.
    INVALID_SEQUENCE        = 12,  ///< Illegal call order; capture must be stopped first.
    BUFFER_TOO_SMALL        = 13,  ///< The supplied buffer cannot hold one image.
    VIDEO_MODE_ACTIVE       = 14,  ///< Video capture is running; stop it first.
    EXPOSURE_IN_PROGRESS    = 15,  ///< A snapshot exposure is running; stop it first.
    GENERAL_ERROR           = 16,  ///< General failure, e.g. a value outside its valid range.
    INVALID_MODE            = 17,  ///< The current camera mode is wrong for this call.
    GPS_NOT_SUPPORTED       = 18,  ///< This camera has no GPS module.
    GPS_VER_ERR             = 19,  ///< The FPGA GPS version is too old.
    GPS_FPGA_ERR            = 20,  ///< Reading from or writing to the FPGA failed.
    GPS_PARAM_OUT_OF_RANGE  = 21,  ///< The GPS start or end line is outside [0, MaxHeight - 1].
    GPS_DATA_INVALID        = 22   ///< No satellite fix yet, or GPS data could not be read.
};

/**
 * @brief Map a raw ZWO/ASI return code onto the library's high-level result category.
 * @param asi_code The vendor code, as returned by an ASI_* entry point.
 * @return The category to report. OperationResult::ZWO_INTERNAL_ERROR is the fallback for any code without a more
 *         specific meaning, and for any code this library does not recognise (e.g. one added by a newer SDK).
 * @note Pure and hardware-free, so it is unit-testable without a camera or the vendor SDK. The raw code is always
 *       preserved alongside the category in DeviceError::asi_code, so mapping to a general category never loses
 *       information.
 */
LIBDEGORASASI_EXPORT types::OperationResult categoryFromAsi(int asi_code);

/// @brief Human-readable name for a raw ZWO/ASI return code, or "ASI_UNKNOWN_ERROR" if unrecognised.
LIBDEGORASASI_EXPORT std::string asiErrorToString(int asi_code);

/**
 * @brief Build a self-contained DeviceError from a raw ZWO/ASI return code.
 * @param asi_code The vendor code, as returned by an ASI_* entry point.
 * @param context Human context naming the call and its arguments, e.g. "ASIOpenCamera(id=0)".
 * @return An error whose category is categoryFromAsi(@p asi_code) and which carries the raw code and context.
 */
LIBDEGORASASI_EXPORT types::DeviceError errorFromAsi(int asi_code, const std::string& context);

/**
 * @brief Build a self-contained DeviceError for a failure detected by the library itself, before or instead of an
 *        SDK call (e.g. a misaligned ROI, a buffer too small, an illegal acquisition transition).
 * @param category The high-level category to report; must not be OPERATION_OK.
 * @param context Human context naming the check that failed.
 * @return An error carrying @p category, a zero raw code (no vendor call was made) and @p context.
 */
LIBDEGORASASI_EXPORT types::DeviceError errorFromLibrary(types::OperationResult category, const std::string& context);

/// @brief A success DeviceError (category OPERATION_OK, no raw code, empty context).
LIBDEGORASASI_EXPORT types::DeviceError successError();

// ---------------------------------------------------------------------------------------------------------------------

}} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
