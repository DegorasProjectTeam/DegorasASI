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
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

// PROJECT INCLUDES (module aggregators)
#include <LibDegorasASI/Modules/ASI>
#include <LibDegorasASI/Modules/Devices>


// Example: the connection lifecycle and the guarantees around it -- connect, idempotent reconnect, exclusive ownership,
// telemetry callback, and deterministic disconnect. Takes an optional camera id and otherwise uses the first discovered
// AsiCamera. No image is acquired.

using namespace dpasi;
using dpasi::types::OperationResult;

/// The camera model this example is demonstrated on. One class drives every ASI model; point this at another model
/// and the same code drives that camera unchanged.
constexpr const char* kModel = "ASI224MC";

int main(int argc, char* argv[])
{
    types::CameraDescriptorList cameras;
    if (AsiCamera::getDeviceList(kModel, cameras) != OperationResult::OPERATION_OK || cameras.empty())
    {
        std::cout << "No " << kModel << " found. Connect one and retry.\n";
        return 1;
    }

    types::CameraId id = cameras.front().id;
    if (argc > 1)
        id = static_cast<types::CameraId>(std::atoi(argv[1]));

    // A user-supplied camera can be checked against a model WITHOUT opening anything, straight from discovery.
    types::CameraDescriptor descriptor;
    for (const types::CameraDescriptor& desc : cameras)
        if (desc.id == id)
            descriptor = desc;
    std::cout << "Camera id " << types::toType(id) << " reports itself as \"" << descriptor.name << "\"\n";
    std::cout << "Matches the model this run expects (" << kModel << "): "
              << (asi::nameMatchesModel(descriptor.name, kModel) ? "yes" : "no") << "\n\n";

    // Requiring a model is OPTIONAL. AsiCamera camera(id) would accept whatever camera sits at that identifier -- one
    // class drives every ASI model, so there is nothing model-specific to select. Passing a model instead makes
    // doConnect() refuse anything else, which is what a program written against one specific camera wants.
    AsiCamera camera(id, kModel);

    // Nothing has touched hardware yet: the constructor performs no device I/O, so operations are refused until connect.
    std::cout << "before connect -> isConnected=" << (camera.isConnected() ? "true" : "false") << "\n";
    double celsius = 0;
    std::cout << "before connect -> getSensorTemperature: "
              << types::toString(camera.getSensorTemperature(celsius)) << "\n\n";

    std::cout << "doConnect      : " << types::toString(camera.doConnect()) << "\n";
    std::cout << "doConnect again: " << types::toString(camera.doConnect())
              << "  (the owning object's reconnect is idempotent)\n";

    // A second object addressing the same camera is refused. The SDK gives no such protection of its own -- opening an
    // already-open camera returns success -- so the library tracks ownership itself.
    AsiCamera rival(id);
    std::cout << "second object  : " << types::toString(rival.doConnect())
              << "  (exclusive ownership is enforced by the library)\n\n";

    types::CameraSN serial;
    if (camera.getSerialNumber(serial) == OperationResult::OPERATION_OK)
        std::cout << "serial number  : " << serial << "  (only readable once open)\n";
    if (camera.getSensorTemperature(celsius) == OperationResult::OPERATION_OK)
        std::cout << "temperature    : " << celsius << " C\n";

    types::ControlCapsList caps;
    if (camera.getControlCaps(caps) == OperationResult::OPERATION_OK)
        std::cout << "controls found : " << caps.size() << "  (discovered at connect, never assumed)\n\n";

    // Background telemetry, reusing the library's generic status poller. Frame acquisition is deliberately NOT done this
    // way: the acquisition loop belongs to the caller.
    camera.setNewStatusCb([](OperationResult res, const types::CameraStatus& status)
    {
        std::cout << "  [telemetry] " << types::toString(res)
                  << " mode=" << types::toString(status.acq_mode)
                  << " temp=" << (status.temperature_valid ? std::to_string(status.temperature_c) : "<invalid>")
                  << " roi=" << status.roi.width << "x" << status.roi.height << "\n";
    });

    std::cout << "startStatusPolling: " << types::toString(camera.startStatusPolling()) << "\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(1600));
    std::cout << "stopStatusPolling : " << types::toString(camera.stopStatusPolling()) << "\n\n";

    std::cout << "doDisconnect      : " << types::toString(camera.doDisconnect()) << "\n";
    std::cout << "doDisconnect again: " << types::toString(camera.doDisconnect()) << "\n";

    // Now that the camera is released, another object can take it.
    std::cout << "rival after release: " << types::toString(rival.doConnect()) << "\n";
    rival.doDisconnect();

    return 0;
}
