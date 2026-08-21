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
#include <iostream>

// PROJECT INCLUDES (module aggregators)
#include <DegorasASI/Modules/ASI>
#include <DegorasASI/Modules/Devices>


// Example: enumerate the connected ZWO ASI cameras and print their capabilities. No camera is opened, so this is the
// first thing to run against new hardware: it answers "what is attached and what can it do?".

using namespace dpasi;
using namespace dpasi::asi;
using dpasi::types::OperationResult;

/// The camera model this example is demonstrated on. One class drives every ASI model; point this at another model
/// and the same code drives that camera unchanged.
constexpr const char* kModel = "ASI224MC";

namespace
{

void printDescriptor(const types::CameraDescriptor& desc)
{
    std::cout << "  [id " << types::toType(desc.id) << "] " << desc.name << "\n";
    std::cout << "      sensor    : " << desc.max_width << "x" << desc.max_height
              << ", " << desc.pixel_size_um << " um pixels, " << desc.bit_depth << "-bit ADC"
              << ", " << desc.electrons_per_adu << " e-/ADU\n";
    std::cout << "      colour    : " << (desc.is_colour ? "yes" : "no");
    if (desc.is_colour)
        std::cout << " (Bayer " << types::toString(desc.bayer_pattern) << ")";
    std::cout << "\n";

    std::cout << "      bins      :";
    for (const int bin : desc.supported_bins)
        std::cout << " " << bin;
    std::cout << "\n";

    std::cout << "      formats   :";
    for (const types::ImageFormat format : desc.supported_formats)
        std::cout << " " << types::toString(format);
    std::cout << "\n";

    std::cout << "      features  : "
              << (desc.has_mechanical_shutter ? "shutter " : "")
              << (desc.has_st4_port ? "ST4 " : "")
              << (desc.is_cooled ? "cooler " : "")
              << (desc.is_trigger_camera ? "trigger " : "")
              << (desc.is_usb3_camera ? "usb3-camera " : "usb2-camera ")
              << (desc.is_usb3_host ? "usb3-host" : "usb2-host") << "\n";

    // A USB3 camera on a USB2 host still works, but at a fraction of its rated throughput, and nothing else reports it.
    if (!desc.isLinkFullSpeed())
        std::cout << "      WARNING   : USB3 camera on a USB2 host -- throughput is degraded, expect dropped frames\n";
}

void printControls(types::CameraId id)
{
    // Control capabilities require an open camera, so this opens one briefly. Which controls exist varies by model and
    // must never be assumed: the AsiCamera, for instance, has no gamma control at all.
    AsiCameraController ctrl(id);

    types::DeviceError err = ctrl.open();
    if (!err.ok())
    {
        std::cout << "      controls  : <could not open: " << err.toString() << ">\n";
        return;
    }

    err = ctrl.initialise();
    if (!err.ok())
    {
        std::cout << "      controls  : <could not initialise: " << err.toString() << ">\n";
        ctrl.close();
        return;
    }

    types::CameraSN serial;
    if (ctrl.readSerialNumber(serial).ok())
        std::cout << "      serial    : " << serial << "\n";

    types::RoiFormat roi;
    if (ctrl.readRoiFormat(roi).ok())
        std::cout << "      power-on  : " << roi.width << "x" << roi.height << " bin" << roi.bin
                  << " " << types::toString(roi.format)
                  << " (" << types::frameBufferSize(roi) << " bytes per frame)\n";

    types::ControlCapsList caps;
    err = ctrl.readControlCaps(caps);
    if (!err.ok())
    {
        std::cout << "      controls  : <could not read: " << err.toString() << ">\n";
        ctrl.close();
        return;
    }

    std::cout << "      controls  : " << caps.size() << "\n";
    for (const types::ControlCaps& cap : caps)
    {
        std::cout << "        " << types::toString(cap.type)
                  << " (" << cap.name << ")"
                  << " range [" << cap.min_value << ", " << cap.max_value << "]"
                  << " default " << cap.default_value
                  << (cap.is_writable ? " writable" : " read-only")
                  << (cap.is_auto_supported ? " auto-capable" : "")
                  << "\n";
    }

    ctrl.close();
}

} // namespace

int main()
{
    std::cout << "ZWO ASI SDK version: " << getSdkVersion() << "\n\n";

    types::CameraDescriptorList cameras;
    const OperationResult res = enumerateCameras(cameras);
    std::cout << "Discovery: " << types::toString(res) << " (" << cameras.size() << " camera(s) found)\n";

    if (cameras.empty())
    {
        std::cout << "No cameras found. Connect a ZWO ASI camera and retry.\n";
        return 0;
    }

    for (const types::CameraDescriptor& desc : cameras)
    {
        printDescriptor(desc);
        printControls(desc.id);
        std::cout << "\n";
    }

    types::CameraDescriptorList asi224mc;
    if (AsiCamera::getDeviceList(asi224mc) == OperationResult::OPERATION_OK)
        std::cout << "Cameras this library ships a driver for (" << kModel << "): "
                  << asi224mc.size() << "\n";

    return 0;
}
