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
#include <chrono>
#include <functional>

// PROJECT INCLUDES
#include "DegorasASI/Global/degorasasi_export.h"
#include "DegorasASI/Common/common_types.h"


// NAMESPACES
namespace dpasi
{

// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Poll a predicate until it becomes true or a timeout elapses.
 * @param pred The condition to test. Evaluated immediately, then once per @p interval. True means the condition is met.
 * @param timeout Maximum time to wait for @p pred to become true.
 * @param interval Delay between successive evaluations of @p pred.
 * @return OperationResult::OPERATION_OK if @p pred became true within @p timeout, otherwise
 *         OperationResult::OPERATION_TIMEOUT.
 * @note Generic deadline-poll helper. It uses a steady clock, so it is unaffected by wall-clock adjustments. It throws
 *       nothing of its own; whatever @p pred throws propagates to the caller.
 * @note The predicate is taken as a std::function, not as a deduced callable, so that the body lives in the library
 *       instead of in this header. Any lambda converts implicitly, and since the predicate runs once per @p interval
 *       the indirect call is not measurable against the wait itself.
 * @note @p interval is a required argument rather than a defaulted one: a default argument in a shared library is
 *       compiled into the CALLER, so correcting it later would need every client recompiled.
 * @note Generic, project-agnostic: candidate to migrate into LibDegorasBase if shared across the Degoras libraries.
 */
DEGORASASI_EXPORT types::OperationResult waitForCondition(std::function<bool()> pred,
                                                          std::chrono::milliseconds timeout,
                                                          std::chrono::milliseconds interval);

// ---------------------------------------------------------------------------------------------------------------------

} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
