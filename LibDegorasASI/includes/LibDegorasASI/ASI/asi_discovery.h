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
// Process-global ASI camera discovery. Every function here takes discoveryMtx(), because the SDK's camera list is
// process-global state that must not be rebuilt concurrently with itself or with an open/close.
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Enumerate every attached ASI camera.
 * @param[out] list Filled with one descriptor per attached camera (cleared first).
 * @return OPERATION_OK on success, ZWO_INTERNAL_ERROR if a camera's properties could not be read.
 * @note Rebuilds the camera list first. Requires no camera to be open, and does not open any.
 */
LIBDEGORASASI_EXPORT types::OperationResult enumerateCameras(types::CameraDescriptorList& list);

/**
 * @brief Enumerate the attached ASI cameras of a given model.
 * @param model The model to match, e.g. "ASI224MC".
 * @param[out] list Filled with the descriptors of the matching cameras (cleared first).
 * @return OPERATION_OK on success (an empty list is success), ZWO_INTERNAL_ERROR on a discovery failure.
 * @note The device-model analogue of enumerating by device type in the reference library: a concrete camera class uses
 *       this to offer its own getDeviceList().
 */
LIBDEGORASASI_EXPORT types::OperationResult enumerateByModel(const std::string& model,
                                                             types::CameraDescriptorList& list);

/**
 * @brief Look up one attached camera by identifier.
 * @param id The camera identifier to find.
 * @param[out] out_desc Filled with that camera's descriptor when found.
 * @return OPERATION_OK when found, DEVICE_NOT_FOUND if no attached camera has that identifier.
 * @note Rebuilds the camera list first, so this also serves as a liveness check before connecting: a camera identifier
 *       obtained from an earlier enumeration may no longer exist.
 */
LIBDEGORASASI_EXPORT types::OperationResult findCameraById(types::CameraId id, types::CameraDescriptor& out_desc);

/**
 * @brief Whether a camera's model name identifies a given model.
 * @param name  The camera's vendor name string, e.g. "ZWO ASI224MC".
 * @param model The model to test for, e.g. "ASI224MC".
 * @return True if @p name contains @p model, compared case-insensitively.
 * @details The vendor prefixes its model names with the brand ("ZWO ASI224MC"), and the exact prefix is not guaranteed,
 *          so this matches on containment rather than equality. Pure string check: it contacts no camera and is safe to
 *          call before connecting, which is what lets a concrete camera class validate a user-supplied camera before
 *          attempting to connect to it.
 */
LIBDEGORASASI_EXPORT bool nameMatchesModel(const std::string& name, const std::string& model);

/**
 * @brief The vendor SDK's version string.
 * @return The version reported by the SDK, e.g. "1, 41, 0, 0", or an empty string if it could not be read.
 * @note The value is copied out of the SDK's own storage immediately, so the returned string is safe to keep.
 */
LIBDEGORASASI_EXPORT std::string getSdkVersion();

// ---------------------------------------------------------------------------------------------------------------------

}} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
