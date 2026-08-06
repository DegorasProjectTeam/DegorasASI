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

// PROJECT INCLUDES
#include "LibDegorasASI/ASI/asi_error.h"


// NAMESPACES
namespace dpasi
{
namespace asi
{

// ---------------------------------------------------------------------------------------------------------------------
// RAW ERROR -> CATEGORY MAPPING

types::OperationResult categoryFromAsi(int asi_code)
{
    switch (static_cast<AsiError>(asi_code))
    {
        case AsiError::SUCCESS:
            return types::OperationResult::OPERATION_OK;

        // The camera is gone or was never opened.
        case AsiError::CAMERA_CLOSED:
            return types::OperationResult::NOT_CONNECTED;
        case AsiError::INVALID_INDEX:
        case AsiError::INVALID_ID:
        case AsiError::CAMERA_REMOVED:
            return types::OperationResult::DEVICE_NOT_FOUND;

        // The camera does not have the requested control or cannot deliver the requested format.
        case AsiError::INVALID_CONTROL_TYPE:
            return types::OperationResult::UNSUPPORTED_CAPABILITY;

        // Parameters the library or the caller got wrong. INVALID_IMGTYPE is deliberately grouped here rather than with
        // UNSUPPORTED_CAPABILITY: the vendor documents it as also covering bad geometry or binning, so reporting it as
        // purely a format problem would mislead. The raw code is preserved either way.
        case AsiError::INVALID_SIZE:
        case AsiError::INVALID_IMGTYPE:
        case AsiError::OUTOF_BOUNDARY:
        case AsiError::INVALID_PATH:
        case AsiError::INVALID_FILEFORMAT:
        case AsiError::GPS_PARAM_OUT_OF_RANGE:
            return types::OperationResult::INVALID_PARAMETER;

        // The call was legal but not in the camera's current state.
        case AsiError::INVALID_SEQUENCE:
        case AsiError::VIDEO_MODE_ACTIVE:
        case AsiError::EXPOSURE_IN_PROGRESS:
        case AsiError::INVALID_MODE:
            return types::OperationResult::INVALID_SEQUENCE;

        case AsiError::BUFFER_TOO_SMALL:
            return types::OperationResult::BUFFER_TOO_SMALL;

        case AsiError::TIMEOUT:
            return types::OperationResult::OPERATION_TIMEOUT;

        // GPS is absent or unusable on this camera.
        case AsiError::GPS_NOT_SUPPORTED:
        case AsiError::GPS_VER_ERR:
            return types::OperationResult::UNSUPPORTED_CAPABILITY;
        case AsiError::GPS_FPGA_ERR:
        case AsiError::GPS_DATA_INVALID:
            return types::OperationResult::READ_FAILED;

        case AsiError::GENERAL_ERROR:
            return types::OperationResult::ZWO_INTERNAL_ERROR;
    }

    // Any code this library does not model (e.g. one introduced by a newer SDK) is reported generically; the raw value
    // is still carried in DeviceError::asi_code, so nothing is lost.
    return types::OperationResult::ZWO_INTERNAL_ERROR;
}

std::string asiErrorToString(int asi_code)
{
    switch (static_cast<AsiError>(asi_code))
    {
        case AsiError::SUCCESS:                return "ASI_SUCCESS";
        case AsiError::INVALID_INDEX:          return "ASI_ERROR_INVALID_INDEX";
        case AsiError::INVALID_ID:             return "ASI_ERROR_INVALID_ID";
        case AsiError::INVALID_CONTROL_TYPE:   return "ASI_ERROR_INVALID_CONTROL_TYPE";
        case AsiError::CAMERA_CLOSED:          return "ASI_ERROR_CAMERA_CLOSED";
        case AsiError::CAMERA_REMOVED:         return "ASI_ERROR_CAMERA_REMOVED";
        case AsiError::INVALID_PATH:           return "ASI_ERROR_INVALID_PATH";
        case AsiError::INVALID_FILEFORMAT:     return "ASI_ERROR_INVALID_FILEFORMAT";
        case AsiError::INVALID_SIZE:           return "ASI_ERROR_INVALID_SIZE";
        case AsiError::INVALID_IMGTYPE:        return "ASI_ERROR_INVALID_IMGTYPE";
        case AsiError::OUTOF_BOUNDARY:         return "ASI_ERROR_OUTOF_BOUNDARY";
        case AsiError::TIMEOUT:                return "ASI_ERROR_TIMEOUT";
        case AsiError::INVALID_SEQUENCE:       return "ASI_ERROR_INVALID_SEQUENCE";
        case AsiError::BUFFER_TOO_SMALL:       return "ASI_ERROR_BUFFER_TOO_SMALL";
        case AsiError::VIDEO_MODE_ACTIVE:      return "ASI_ERROR_VIDEO_MODE_ACTIVE";
        case AsiError::EXPOSURE_IN_PROGRESS:   return "ASI_ERROR_EXPOSURE_IN_PROGRESS";
        case AsiError::GENERAL_ERROR:          return "ASI_ERROR_GENERAL_ERROR";
        case AsiError::INVALID_MODE:           return "ASI_ERROR_INVALID_MODE";
        case AsiError::GPS_NOT_SUPPORTED:      return "ASI_ERROR_GPS_NOT_SUPPORTED";
        case AsiError::GPS_VER_ERR:            return "ASI_ERROR_GPS_VER_ERR";
        case AsiError::GPS_FPGA_ERR:           return "ASI_ERROR_GPS_FPGA_ERR";
        case AsiError::GPS_PARAM_OUT_OF_RANGE: return "ASI_ERROR_GPS_PARAM_OUT_OF_RANGE";
        case AsiError::GPS_DATA_INVALID:       return "ASI_ERROR_GPS_DATA_INVALID";
    }
    return "ASI_UNKNOWN_ERROR";
}

// ---------------------------------------------------------------------------------------------------------------------
// ERROR CONSTRUCTION

types::DeviceError errorFromAsi(int asi_code, const std::string& context)
{
    types::DeviceError error;
    error.category = categoryFromAsi(asi_code);
    error.asi_code = asi_code;
    error.context = context;
    return error;
}

types::DeviceError errorFromLibrary(types::OperationResult category, const std::string& context)
{
    types::DeviceError error;
    error.category = category;
    error.asi_code = 0;   // No vendor call was made, so there is no raw code to report.
    error.context = context;
    return error;
}

types::DeviceError successError()
{
    return types::DeviceError();
}

// ---------------------------------------------------------------------------------------------------------------------

}} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
