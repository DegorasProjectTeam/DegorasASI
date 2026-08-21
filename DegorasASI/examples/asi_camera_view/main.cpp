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
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

// PROJECT INCLUDES (module aggregators)
#include <DegorasASI/Modules/Helpers>
#include <DegorasASI/Modules/Devices>


// Example: SEE what the camera produced, with no image-processing dependency anywhere.
//
// This is the one to run first on new hardware. It prints a brightness map straight to the terminal -- so the very
// first question, "is the camera seeing anything at all?", is answered without opening a file -- and then writes real
// images to disk in each of the camera's formats so they can be inspected properly.
//
// Two facts about the pixel data drive everything here, neither of them documented by the vendor and both measured on
// hardware (see the README's SDK-behaviour table):
//   * RGB24 arrives as B,G,R -- the same order BMP stores, so writing one is a straight copy.
//   * RAW16 already spans the full 16-bit range on a 12-bit sensor, so it needs no shifting.
//
// Takes an optional camera id and otherwise uses the first discovered camera.

using namespace dpasi;
using dpasi::types::OperationResult;

namespace
{

/// Capture one still frame in the given format and both preview and save it.
void captureAndShow(AsiCamera& camera, types::ImageFormat format, const std::string& stem, bool preview)
{
    if (!camera.supportsFormat(format))
    {
        std::cout << "  " << types::toString(format) << ": not supported by this camera, skipping\n";
        return;
    }

    // Full sensor, so the saved files are the real picture rather than a crop.
    const OperationResult roi_res = camera.doSetFullFrameRoi(format, 1);
    if (roi_res != OperationResult::OPERATION_OK)
    {
        std::cout << "  " << types::toString(format) << ": doSetFullFrameRoi -> " << types::toString(roi_res) << "\n";
        return;
    }

    types::Frame frame;
    const OperationResult res = camera.doCaptureSingleFrame(frame, std::chrono::seconds(10));
    if (res != OperationResult::OPERATION_OK)
    {
        std::cout << "  " << types::toString(format) << ": capture -> " << types::toString(res) << "\n";
        return;
    }

    const std::string path = stem + "." + imgio::extensionFor(format);
    const bool written = imgio::writeFrame(frame, path);

    std::cout << "  " << types::toString(format) << ": " << frame.width << "x" << frame.height
              << ", " << frame.data.size() << " bytes -> " << (written ? path : std::string("WRITE FAILED")) << "\n";

    if (preview)
    {
        std::cout << "\n" << imgio::framePreview(frame, 72) << "\n";
    }
}

} // namespace

int main(int argc, char* argv[])
{
    types::CameraDescriptorList cameras;
    if (AsiCamera::getDeviceList(cameras) != OperationResult::OPERATION_OK || cameras.empty())
    {
        std::cout << "No ASI camera found. Connect one and retry.\n";
        return 1;
    }

    types::CameraId id = cameras.front().id;
    if (argc > 1)
        id = static_cast<types::CameraId>(std::atoi(argv[1]));

    AsiCamera camera(id);
    if (camera.doConnect() != OperationResult::OPERATION_OK)
    {
        std::cout << "Could not connect to camera id " << types::toType(id) << "\n";
        return 1;
    }

    types::CameraDescriptor desc;
    camera.getDescriptor(desc);
    std::cout << desc.name << " -- " << desc.max_width << "x" << desc.max_height
              << (desc.is_colour ? ", colour" : ", mono")
              << ", " << types::toString(camera.getUsbLinkSpeed()) << "\n\n";

    // Enough exposure and gain to see something indoors. Adjust to taste; this is a bring-up aid, not photometry.
    camera.doSetExposure(std::chrono::milliseconds(50));
    types::ControlValue gain;
    gain.value = 300;
    camera.doSetControl(types::ControlType::GAIN, gain);

    // -- Colour, the one worth looking at --------------------------------------------------------------------------
    // The SDK demosaics RGB24 for us, so this is a real picture. The raw formats below are the sensor's own mosaic.
    std::cout << "-- still frames --\n";
    captureAndShow(camera, types::ImageFormat::RGB24, "asi_frame_rgb24", true);

    // -- The raw formats, saved but not previewed --------------------------------------------------------------------
    // On a colour camera these are Bayer-mosaiced: useful for processing, not for looking at directly.
    captureAndShow(camera, types::ImageFormat::RAW8, "asi_frame_raw8", false);
    captureAndShow(camera, types::ImageFormat::RAW16, "asi_frame_raw16", false);
    captureAndShow(camera, types::ImageFormat::Y8, "asi_frame_y8", false);

    // -- A short stream, saving only the last frame --------------------------------------------------------------------
    // Proof that the same data comes out of the video path, and a place to hang a real viewer later.
    std::cout << "-- streaming burst --\n";
    if (camera.supportsFormat(types::ImageFormat::RGB24) &&
        camera.doSetFullFrameRoi(types::ImageFormat::RGB24, 1) == OperationResult::OPERATION_OK &&
        camera.doStartVideoCapture() == OperationResult::OPERATION_OK)
    {
        types::Frame frame;
        int received = 0;
        for (int i = 0; i < 20; ++i)
            if (camera.doGetVideoFrame(frame, std::chrono::milliseconds(2000)) == OperationResult::OPERATION_OK)
                ++received;

        int dropped = 0;
        camera.getDroppedFrames(dropped);
        camera.doStopVideoCapture();

        if (received > 0)
        {
            const bool written = imgio::writeBmp(frame, "asi_stream_last.bmp");
            std::cout << "  " << received << "/20 frames, " << dropped << " dropped -> "
                      << (written ? "asi_stream_last.bmp" : "WRITE FAILED") << "\n";
        }
    }

    camera.doDisconnect();

    std::cout << "\nOpen the .bmp files directly; the .pgm files open in any image editor or astronomy tool.\n";
    std::cout << "For a live view, hand a Frame to your GUI toolkit -- no conversion is needed with Qt:\n";
    std::cout << "  RGB24 -> QImage(frame.data.data(), w, h, QImage::Format_BGR888)\n";
    std::cout << "  RAW8  -> QImage(frame.data.data(), w, h, QImage::Format_Grayscale8)\n";
    std::cout << "  RAW16 -> QImage(frame.data.data(), w, h, QImage::Format_Grayscale16)\n";
    return 0;
}
