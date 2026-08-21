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
#include <memory>
#include <unordered_map>

// PROJECT INCLUDES
#include "DegorasASI/ASI/asi_api_lock.h"


// NAMESPACES
namespace dpasi
{
namespace asi
{

namespace
{

/// Shared per-camera mutex registry. One instance per lock scope, selected by the caller.
std::mutex& perCameraMtx(std::unordered_map<int, std::unique_ptr<std::mutex>>& registry,
                         std::mutex& registry_mtx,
                         types::CameraId id)
{
    // The map stores unique_ptr<mutex> so a rehash never relocates a mutex a caller is holding, and entries are
    // never erased, so a returned reference stays valid for the process lifetime.
    const std::lock_guard<std::mutex> lock(registry_mtx);
    std::unique_ptr<std::mutex>& slot = registry[types::toType(id)];
    if (!slot)
        slot = std::make_unique<std::mutex>();
    return *slot;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------

std::mutex& discoveryMtx()
{
    static std::mutex mutex;
    return mutex;
}

std::mutex& cameraMtx(types::CameraId id)
{
    static std::mutex registry_mtx;
    static std::unordered_map<int, std::unique_ptr<std::mutex>> registry;
    return perCameraMtx(registry, registry_mtx, id);
}

std::mutex& acquisitionMtx(types::CameraId id)
{
    static std::mutex registry_mtx;
    static std::unordered_map<int, std::unique_ptr<std::mutex>> registry;
    return perCameraMtx(registry, registry_mtx, id);
}

// ---------------------------------------------------------------------------------------------------------------------

}} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
