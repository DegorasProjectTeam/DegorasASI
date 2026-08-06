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

// C++ INCLUDES
#include <mutex>
#include <unordered_set>

// PROJECT INCLUDES
#include "LibDegorasASI/ASI/asi_camera_registry.h"


// NAMESPACES
namespace dpasi
{
namespace asi
{

namespace
{

std::mutex& claimsMtx()
{
    static std::mutex mutex;
    return mutex;
}

std::unordered_set<int>& claims()
{
    static std::unordered_set<int> owned;
    return owned;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------

bool tryClaimCamera(types::CameraId id)
{
    const std::lock_guard<std::mutex> lock(claimsMtx());
    return claims().insert(types::toType(id)).second;
}

void releaseCamera(types::CameraId id) noexcept
{
    // Teardown-safe: the container operations used here do not throw, and erasing an absent key is a no-op.
    const std::lock_guard<std::mutex> lock(claimsMtx());
    claims().erase(types::toType(id));
}

bool isCameraClaimed(types::CameraId id)
{
    const std::lock_guard<std::mutex> lock(claimsMtx());
    return claims().count(types::toType(id)) != 0;
}

// ---------------------------------------------------------------------------------------------------------------------

}} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
