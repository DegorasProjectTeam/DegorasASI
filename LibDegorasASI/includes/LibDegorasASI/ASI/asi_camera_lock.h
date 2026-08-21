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
// HOST-WIDE claim on a camera, so two PROCESSES cannot drive the same camera at once.
//
// asi_camera_registry.h does the same job within one process and explains why it must exist at all. This is the other
// half, and it closes the gap that header names: "a cross-process guarantee would need a named system object and is not
// implemented because no current use case needs it". There is a use case, and the reason is measured rather than
// assumed. With one process already streaming from an ASI224MC, a second process was observed to:
//
//     ASIGetNumOfConnectedCameras() = 1
//     ASIGetCameraProperty(0)       = ASI_SUCCESS
//     ASIOpenCamera(0)              = ASI_SUCCESS      <-- no exclusivity whatsoever
//     ASIInitCamera(0)              = ASI_SUCCESS
//
// Both processes then fight, and the symptoms are non-deterministic rather than an error: enumeration returned ZERO
// cameras on four of six attempts, single-frame captures alternated between success and ASI_EXP_FAILED, and a test
// suite failed at a different assertion on every run. So the SDK cannot be asked whether a camera is busy -- the
// answer has to come from us.
//
// WHAT THIS DOES AND DOES NOT COVER. It is an advisory lock between processes that use THIS LIBRARY. That is the
// realistic case: the station's own viewer, examples and test suite competing with each other. It cannot see a
// third-party application such as ASIStudio, and nothing can: the only observable signal from outside is that the
// program has ASICamera2.dll loaded, and that was measured to be a false positive -- ASIStudio sitting open with its
// capture window closed holds no camera at all.
//
// TWO PROPERTIES WORTH KNOWING, both deliberate.
//
//   * The claim is released by the OPERATING SYSTEM when the holding process dies, however it dies. A stale claim
//     after a crash or a kill is therefore impossible, which matters because killing a viewer with a timeout is an
//     everyday thing and a lock file holding a PID would have needed liveness checks to survive it.
//   * It FAILS OPEN. If the lock cannot be established for any reason other than somebody else holding it -- a
//     read-only temp directory, a filesystem without locking, a permission problem -- the claim is granted and the
//     camera stays usable. An advisory guard that can render hardware unusable is worse than no guard.
//
// Scope is one user session: the lock lives in the per-user temporary directory, so two different logged-in users
// would not see each other's claim. That matches how the station runs and keeps the mechanism free of privileges.
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Claim a camera for this process against every other process on this host.
 * @param id The camera to claim.
 * @return True if the claim succeeded or could not be enforced at all (see the fail-open note above); false only when
 *         another process demonstrably holds the camera.
 * @note A successful claim must be released with unlockCameraOnHost() exactly once; the device layer does so in its
 *       disconnect path and its destructor. Re-claiming from the same process returns true, since the in-process
 *       registry is what refuses that case and it runs first.
 */
LIBDEGORASASI_EXPORT bool tryLockCameraOnHost(types::CameraId id);

/**
 * @brief Release a claim taken by tryLockCameraOnHost().
 * @param id The camera to release.
 * @note Thread-safe, idempotent and never throws, so it is safe to call from a teardown path.
 */
LIBDEGORASASI_EXPORT void unlockCameraOnHost(types::CameraId id) noexcept;

/**
 * @brief Describe the process that currently holds a camera, for a diagnostic message.
 * @param id The camera to ask about.
 * @return Something like "pid 19340 (Example_asi_camera_live.exe)", or an empty string when the camera is free, when
 *         the holder is this process, or when the holder could not be identified.
 * @note Best effort and inherently racy: the holder may exit between this call and the caller reading the result. It
 *       is for telling a human WHY a connection was refused, never for deciding whether to attempt one.
 */
LIBDEGORASASI_EXPORT std::string describeCameraHolder(types::CameraId id);

// ---------------------------------------------------------------------------------------------------------------------

}} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
