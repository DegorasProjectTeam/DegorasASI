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
#include <mutex>

// PROJECT INCLUDES
#include "LibDegorasASI/Global/libdegorasasi_export.h"
#include "LibDegorasASI/Common/common_types.h"


// NAMESPACES
namespace dpasi
{
namespace asi
{

// ---------------------------------------------------------------------------------------------------------------------
// The ASI SDK keeps process-global state (its internal camera list) alongside per-camera state, and its frame-retrieval
// call BLOCKS for as long as the caller's timeout allows. Three lock scopes therefore exist, and the split between the
// last two is what keeps a control call from being stuck behind a frame wait. All three are reached through a function
// rather than held as a member, so two adapter objects addressing the same camera share the same mutex and a call site
// never has to know which lock it needs beyond naming it.
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Process-wide mutex serialising the global ASI discovery and connection calls.
 * @return Reference to the single discovery mutex.
 * @note ASIGetNumOfConnectedCameras() does not merely count cameras: it (re)builds the SDK's internal device list, and
 *       ASIGetCameraProperty() fails until it has been called at least once. That list is process-global, so
 *       enumeration must never run concurrently with itself or with an open/close. Per-camera operations use
 *       cameraMtx() instead, so steady-state I/O on distinct cameras is NOT serialised against each other.
 */
LIBDEGORASASI_EXPORT std::mutex& discoveryMtx();

/**
 * @brief Per-camera mutex serialising the short, non-blocking calls that target a single camera.
 * @param id The camera the caller is about to act on.
 * @return Reference to the mutex dedicated to @p id (stable for the process lifetime).
 * @note Two different cameras get two different mutexes, so independent cameras can be driven concurrently. The
 *       returned reference is stable: registry entries are never moved or erased while the process runs.
 * @warning This lock is NEVER held across the blocking frame-retrieval call -- that is what acquisitionMtx() is for.
 *          Holding it there would make every control read wait out a whole frame timeout.
 */
LIBDEGORASASI_EXPORT std::mutex& cameraMtx(types::CameraId id);

/**
 * @brief Per-camera mutex serialising the acquisition path, held across the blocking frame retrieval.
 * @param id The camera the caller is about to acquire from.
 * @return Reference to the acquisition mutex dedicated to @p id (stable for the process lifetime).
 *
 * @details Deliberately distinct from cameraMtx(). Two measured properties of the SDK force the split:
 *          - A short control call issued while another thread is blocked inside the frame-retrieval call is safe: both
 *            threads make progress and neither reports an error. So the control lock must NOT cover the blocking read,
 *            or control calls would serialise behind frame waits for no reason.
 *          - Stopping video capture does NOT cancel a frame retrieval that is already blocked: the pending call runs to
 *            its full timeout and then reports a timeout. So closing the camera while a retrieval is in flight cannot
 *            be avoided by stopping first -- it has to be waited out.
 *
 *          This mutex is therefore what makes teardown safe: a disconnect acquires it, which waits for any in-flight
 *          retrieval to return before the camera is closed. It also serialises concurrent retrievals against each
 *          other, which would otherwise both consume from one frame stream.
 *
 * @warning Blocking while holding this lock is its PURPOSE, not a violation of the no-lock-across-a-wait rule. Worst-
 *          case wait is the frame timeout in force, which is why the library never passes the SDK's "wait forever"
 *          value and always clamps the timeout to a bounded maximum.
 */
LIBDEGORASASI_EXPORT std::mutex& acquisitionMtx(types::CameraId id);

// ---------------------------------------------------------------------------------------------------------------------

}} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
