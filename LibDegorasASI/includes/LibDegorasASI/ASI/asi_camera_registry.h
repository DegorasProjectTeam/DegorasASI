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

// PROJECT INCLUDES
#include "LibDegorasASI/Global/libdegorasasi_export.h"
#include "LibDegorasASI/Common/common_types.h"


// NAMESPACES
namespace dpasi
{
namespace asi
{

// ---------------------------------------------------------------------------------------------------------------------
// Process-wide record of which cameras this process has open, so a second device object addressing an already-owned
// camera is refused with OperationResult::CAMERA_IN_USE instead of silently sharing it.
//
// The SDK offers NO signal of its own here: verified on hardware, ASIOpenCamera() on an already-open camera returns
// ASI_SUCCESS, and the camera keeps appearing in the enumeration. Two objects would each believe they owned it and
// would fight over one frame stream and one control set. This registry is therefore the ZWO equivalent of the aliasing
// guarantee the reference library gets for free (an open Kinesis device disappears from its device list, so no registry
// is needed there).
//
// Scope is THIS PROCESS. Another process opening the same camera cannot be detected through the SDK, so it is not
// claimed to be; a cross-process guarantee would need a named system object and is not implemented because no current
// use case needs it.
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Claim exclusive ownership of a camera for this process.
 * @param id The camera to claim.
 * @return True if the claim succeeded, false if this process already holds a claim on @p id.
 * @note Thread-safe. A successful claim must be released with releaseCamera() exactly once; the device layer does so in
 *       its disconnect path and its destructor.
 */
LIBDEGORASASI_EXPORT bool tryClaimCamera(types::CameraId id);

/**
 * @brief Release a claim taken by tryClaimCamera().
 * @param id The camera to release.
 * @note Thread-safe, idempotent and never throws, so it is safe to call from a teardown path.
 */
LIBDEGORASASI_EXPORT void releaseCamera(types::CameraId id) noexcept;

/**
 * @brief Whether this process currently holds a claim on a camera.
 * @param id The camera to test.
 * @return True while a claim taken by tryClaimCamera() has not yet been released.
 */
LIBDEGORASASI_EXPORT bool isCameraClaimed(types::CameraId id);

// ---------------------------------------------------------------------------------------------------------------------

}} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
