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
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <string>

// PROJECT INCLUDES (module aggregators)
#include <DegorasASI/Modules/Devices>


// Example: drive an AsiCamera through both acquisition workflows. Configures a ROI, takes one still frame via the
// snapshot path, then streams a short burst via the video path, printing frame statistics as it goes. Accepts an
// optional camera id on the command line and otherwise uses the first discovered camera.

using namespace dpasi;
using dpasi::types::OperationResult;

/// The camera model this example is demonstrated on. One class drives every ASI model; point this at another model
/// and the same code drives that camera unchanged.
constexpr const char* kModel = "ASI224MC";

namespace
{

/// Mean pixel value of a frame's bytes; a cheap sanity check that real data arrived.
double meanByte(const types::Frame& frame)
{
    if (frame.data.empty())
        return 0.0;
    const std::uint64_t sum = std::accumulate(frame.data.cbegin(), frame.data.cend(), std::uint64_t(0),
                                              [](std::uint64_t acc, types::PixelByte b)
                                              { return acc + static_cast<std::uint64_t>(b); });
    return static_cast<double>(sum) / static_cast<double>(frame.data.size());
}

void printFrame(const std::string& label, const types::Frame& frame)
{
    std::cout << "  " << label << ": " << frame.width << "x" << frame.height << " bin" << frame.bin
              << " " << types::toString(frame.format)
              << ", " << frame.data.size() << " bytes"
              << ", seq " << frame.sequence
              << ", mean pixel " << meanByte(frame) << "\n";
}

bool check(const std::string& what, OperationResult res)
{
    std::cout << "  " << what << ": " << types::toString(res) << "\n";
    return res == OperationResult::OPERATION_OK;
}

} // namespace

int main(int argc, char* argv[])
{
    // Pick the camera: an explicit id on the command line, else the first discovered camera.
    types::CameraDescriptorList cameras;
    if (AsiCamera::getDeviceList(kModel, cameras) != OperationResult::OPERATION_OK || cameras.empty())
    {
        std::cout << "No " << kModel << " found. Connect one and retry.\n";
        return 1;
    }

    types::CameraId id = cameras.front().id;
    if (argc > 1)
        id = static_cast<types::CameraId>(std::atoi(argv[1]));

    std::cout << "Using camera id " << types::toType(id) << "\n\n";

    AsiCamera camera(id);   // No device I/O in the constructor.

    std::cout << "-- connect --\n";
    if (!check("doConnect", camera.doConnect()))
        return 1;

    types::CameraSN serial;
    if (camera.getSerialNumber(serial) == OperationResult::OPERATION_OK)
        std::cout << "  serial: " << serial << "\n";

    double celsius = 0;
    if (camera.getSensorTemperature(celsius) == OperationResult::OPERATION_OK)
        std::cout << "  sensor temperature: " << celsius << " C\n";

    // The negotiated USB link, and the bandwidth lever over it. A USB3 camera in a USB2 port works but delivers a
    // fraction of its rated frame rate, and nothing else in the system reports it.
    std::cout << "  usb link: " << types::toString(camera.getUsbLinkSpeed())
              << (camera.isLinkFullSpeed() ? " (full speed)" : " (DEGRADED: USB3 camera on a USB2 host)") << "\n";
    int bw = 0;
    bool bw_auto = false;
    if (camera.getUsbBandwidth(bw, bw_auto) == OperationResult::OPERATION_OK)
        std::cout << "  usb traffic: " << bw << "%" << (bw_auto ? " (auto)" : "") << "\n";

    // -- Configure ---------------------------------------------------------------------------------------------------
    std::cout << "\n-- configure --\n";

    // Geometry and origin are set together: changing the format recentres the origin, so setting them separately would
    // leave the origin wherever the SDK moved it.
    types::RoiFormat roi;
    roi.width = 640;
    roi.height = 480;
    roi.bin = 1;
    roi.format = types::ImageFormat::RAW8;
    if (!check("doSetRoi(640x480 bin1 RAW8 @ 0,0)", camera.doSetRoi(roi, types::RoiPosition())))
        return 1;

    check("doSetExposure(20 ms)", camera.doSetExposure(std::chrono::milliseconds(20)));

    types::ControlValue gain;
    gain.value = 200;
    gain.is_auto = false;
    check("doSetControl(GAIN, 200)", camera.doSetControl(types::ControlType::GAIN, gain));

    // Lowering the USB bandwidth share is the usual remedy for dropped frames, especially on a degraded link.
    check("doSetUsbBandwidth(80)", camera.doSetUsbBandwidth(80));

    // This camera has no gamma control at all, so asking for one is refused rather than silently ignored.
    types::ControlValue gamma;
    gamma.value = 50;
    check("doSetControl(GAMMA) -- expected UNSUPPORTED_CAPABILITY",
          camera.doSetControl(types::ControlType::GAMMA, gamma));

    types::RoiFormat applied;
    if (camera.getRoiFormat(applied) == OperationResult::OPERATION_OK)
        std::cout << "  live ROI: " << applied.width << "x" << applied.height << " bin" << applied.bin
                  << " " << types::toString(applied.format)
                  << " (" << types::frameBufferSize(applied) << " bytes per frame)\n";

    // -- Snapshot ----------------------------------------------------------------------------------------------------
    std::cout << "\n-- snapshot (single frame) --\n";

    types::Frame still;
    if (check("doCaptureSingleFrame", camera.doCaptureSingleFrame(still, std::chrono::seconds(5))))
        printFrame("still", still);

    std::cout << "  acquisition mode after capture: " << types::toString(camera.getAcquisitionMode()) << "\n";

    // -- Video -------------------------------------------------------------------------------------------------------
    std::cout << "\n-- video (streaming burst) --\n";

    if (!check("doStartVideoCapture", camera.doStartVideoCapture()))
        return 1;

    // Illegal while streaming, and refused by the library rather than by the SDK: the SDK would ACCEPT this and
    // silently change the frame geometry underneath us.
    check("doSetRoi while streaming -- expected INVALID_SEQUENCE", camera.doSetRoi(roi, types::RoiPosition()));

    // One frame object reused across the loop: its buffer is reallocated only if the geometry changes, so a steady
    // stream costs no per-frame allocation.
    types::Frame frame;
    int received = 0;
    constexpr int kWanted = 30;
    for (int i = 0; i < kWanted; ++i)
    {
        const OperationResult res = camera.doGetVideoFrame(frame, std::chrono::milliseconds(1000));
        if (res == OperationResult::OPERATION_OK)
        {
            ++received;
            if (received <= 3)
                printFrame("frame", frame);
        }
        else
        {
            std::cout << "  frame " << i << ": " << types::toString(res) << "\n";
        }
    }

    // Sampled BEFORE stopping: the vendor's documentation disagrees with itself about when the counter is cleared, so
    // treat it as a per-session figure.
    int dropped = 0;
    camera.getDroppedFrames(dropped);
    std::cout << "  received " << received << "/" << kWanted << " frames, dropped " << dropped << "\n";

    check("doStopVideoCapture", camera.doStopVideoCapture());

    // -- Telemetry ---------------------------------------------------------------------------------------------------
    std::cout << "\n-- telemetry --\n";

    types::CameraStatus status;
    if (camera.getDeviceStatus(status) == OperationResult::OPERATION_OK)
        std::cout << status.toJsonStr(true) << "\n";

    std::cout << "\n-- disconnect --\n";
    check("doDisconnect", camera.doDisconnect());   // Also handled by the destructor.

    return 0;
}
