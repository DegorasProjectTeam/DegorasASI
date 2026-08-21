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
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <mutex>

// VENDOR SDK INCLUDES
// This translation unit and the Layer-2 adapter are the ONLY places the vendor header appears. Note that the vendor
// decorates its entry points with __declspec(dllexport) whenever _WINDOWS is defined, which is wrong for a consumer:
// leaving _WINDOWS undefined yields the plain declarations that link against the import library. Do not define it.
#include <ASICamera2.h>

// PROJECT INCLUDES
#include "DegorasASI/ASI/asi_discovery.h"
#include "DegorasASI/ASI/asi_api_lock.h"
#include "DegorasASI/ASI/asi_error.h"


// NAMESPACES
namespace dpasi
{
namespace asi
{

namespace
{

/// Copy a fixed-size vendor char array that is NOT guaranteed to be terminated when full.
std::string boundedString(const char* buf, std::size_t max_len)
{
    return std::string(buf, std::find(buf, buf + max_len, '\0'));
}

/// Translate the vendor's camera-info struct into the library's descriptor. Never fails: it only copies and maps.
types::CameraDescriptor descriptorFromInfo(const ASI_CAMERA_INFO& info)
{
    types::CameraDescriptor desc;
    desc.id = static_cast<types::CameraId>(info.CameraID);
    desc.name = boundedString(info.Name, sizeof(info.Name));
    desc.max_width = static_cast<int>(info.MaxWidth);
    desc.max_height = static_cast<int>(info.MaxHeight);
    desc.pixel_size_um = info.PixelSize;
    desc.bit_depth = info.BitDepth;
    desc.electrons_per_adu = static_cast<double>(info.ElecPerADU);
    desc.is_colour = (info.IsColorCam == ASI_TRUE);
    desc.has_mechanical_shutter = (info.MechanicalShutter == ASI_TRUE);
    desc.has_st4_port = (info.ST4Port == ASI_TRUE);
    desc.is_cooled = (info.IsCoolerCam == ASI_TRUE);
    desc.is_usb3_camera = (info.IsUSB3Camera == ASI_TRUE);
    desc.is_usb3_host = (info.IsUSB3Host == ASI_TRUE);
    desc.is_trigger_camera = (info.IsTriggerCam == ASI_TRUE);

    if (!types::bayerPatternFromType(static_cast<int>(info.BayerPattern), desc.bayer_pattern))
        desc.bayer_pattern = types::BayerPattern::RG;

    // The two capability arrays use DIFFERENT terminators, and one of them collides with a valid value: SupportedBins
    // ends at 0, while SupportedVideoFormat ends at ASI_IMG_END (-1) and its first valid entry, ASI_IMG_RAW8, IS 0.
    // Each scan is bounded by the declared array length as well as its own sentinel.
    const std::size_t bins_len = sizeof(info.SupportedBins) / sizeof(info.SupportedBins[0]);
    for (std::size_t i = 0; i < bins_len && info.SupportedBins[i] != 0; ++i)
        desc.supported_bins.push_back(info.SupportedBins[i]);

    const std::size_t fmts_len = sizeof(info.SupportedVideoFormat) / sizeof(info.SupportedVideoFormat[0]);
    for (std::size_t i = 0; i < fmts_len && info.SupportedVideoFormat[i] != ASI_IMG_END; ++i)
    {
        types::ImageFormat format = types::ImageFormat::RAW8;
        if (types::imageFormatFromType(static_cast<int>(info.SupportedVideoFormat[i]), format))
            desc.supported_formats.push_back(format);
    }

    return desc;
}

/// Rebuild the SDK's camera list and enumerate every camera. The caller must already hold discoveryMtx().
types::OperationResult enumerateCamerasLocked(types::CameraDescriptorList& list)
{
    list.clear();

    // Rebuilds the SDK's internal device list as a side effect; camera properties are unreadable until it has run.
    const int count = ASIGetNumOfConnectedCameras();
    if (count <= 0)
        return types::OperationResult::OPERATION_OK;

    list.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index)
    {
        // Note the addressing asymmetry: this vendor entry point takes an enumeration INDEX, while every other one
        // takes a CameraID. They are not interchangeable.
        ASI_CAMERA_INFO info{};
        const ASI_ERROR_CODE err = ASIGetCameraProperty(&info, index);
        if (err != ASI_SUCCESS)
            return categoryFromAsi(static_cast<int>(err));
        list.push_back(descriptorFromInfo(info));
    }
    return types::OperationResult::OPERATION_OK;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------

types::OperationResult enumerateCameras(types::CameraDescriptorList& list)
{
    const std::lock_guard<std::mutex> lock(discoveryMtx());
    return enumerateCamerasLocked(list);
}

types::OperationResult enumerateByModel(const std::string& model, types::CameraDescriptorList& list)
{
    types::CameraDescriptorList all;
    {
        const std::lock_guard<std::mutex> lock(discoveryMtx());
        const types::OperationResult res = enumerateCamerasLocked(all);
        if (res != types::OperationResult::OPERATION_OK)
            return res;
    }

    list.clear();
    for (const types::CameraDescriptor& desc : all)
    {
        if (nameMatchesModel(desc.name, model))
            list.push_back(desc);
    }
    return types::OperationResult::OPERATION_OK;
}

types::OperationResult findCameraById(types::CameraId id, types::CameraDescriptor& out_desc)
{
    types::CameraDescriptorList all;
    {
        const std::lock_guard<std::mutex> lock(discoveryMtx());
        const types::OperationResult res = enumerateCamerasLocked(all);
        if (res != types::OperationResult::OPERATION_OK)
            return res;
    }

    for (const types::CameraDescriptor& desc : all)
    {
        if (desc.id == id)
        {
            out_desc = desc;
            return types::OperationResult::OPERATION_OK;
        }
    }
    return types::OperationResult::DEVICE_NOT_FOUND;
}

bool nameMatchesModel(const std::string& name, const std::string& model)
{
    if (model.empty() || model.size() > name.size())
        return false;

    const auto ci_equal = [](char a, char b)
    {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    };
    return std::search(name.cbegin(), name.cend(), model.cbegin(), model.cend(), ci_equal) != name.cend();
}

std::string getSdkVersion()
{
    // The vendor returns a non-const pointer into its own static storage: copy it out at once, never free or write it.
    const char* version = ASIGetSDKVersion();
    return (version != nullptr) ? std::string(version) : std::string();
}

// ---------------------------------------------------------------------------------------------------------------------

}} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
