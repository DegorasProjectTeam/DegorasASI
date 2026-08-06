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
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

// PROJECT INCLUDES (module aggregators)
#include <LibDegorasASI/Modules/ASI>
#include <LibDegorasASI/Modules/Devices>


using namespace dpasi;
using namespace dpasi::types;

/// The camera model these checks are written against. One class drives every ASI model, so this is only the model
/// the run is VALIDATED on; point it at another model and the same code exercises that camera unchanged.
constexpr const char* kModel = "ASI224MC";

// ---------------------------------------------------------------------------------------------------------------------
// Integration test for the concurrency model against real hardware. It asserts the two properties the library's
// two-lock design rests on, both of which were measured on this SDK before the design was fixed:
//
//   1. A short control call issued while another thread is blocked in a frame retrieval must make progress. That is why
//      the blocking retrieval takes the per-camera ACQUISITION lock and never the per-camera CONTROL lock: covering the
//      blocking call with the control lock would stall every control read behind a frame wait.
//
//   2. Stopping capture does NOT cancel a retrieval already blocked. So a disconnect cannot avoid an in-flight read by
//      stopping first -- it must WAIT it out. The acquisition lock is what makes that wait happen, instead of the camera
//      being closed underneath a pending vendor call.
//
// SELF-SKIPS with success when no camera is attached.
// ---------------------------------------------------------------------------------------------------------------------

namespace
{

/// Aliasing: many threads racing to connect the same camera; exactly one may win.
void testConcurrentConnectElectsOneOwner(CameraId id)
{
    std::cout << "  concurrent connects elect exactly one owner\n";

    constexpr int kThreads = 8;
    std::vector<std::unique_ptr<AsiCamera>> devices;
    devices.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i)
        devices.push_back(std::make_unique<AsiCamera>(id));

    std::atomic<int> connected{0};
    std::atomic<int> refused{0};
    std::vector<std::thread> racers;
    racers.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i)
    {
        racers.emplace_back([&devices, &connected, &refused, i]
        {
            const OperationResult res = devices[static_cast<std::size_t>(i)]->doConnect();
            if (res == OperationResult::OPERATION_OK)
                ++connected;
            else if (res == OperationResult::CAMERA_IN_USE)
                ++refused;
        });
    }
    for (std::thread& racer : racers)
        racer.join();

    assert(connected.load() == 1);
    assert(refused.load() == kThreads - 1);
    std::cout << "    1 owner, " << refused.load() << " refused with CAMERA_IN_USE\n";

    devices.clear();   // Destructors disconnect; the claim must be gone afterwards.
    assert(!asi::isCameraClaimed(id));
}

/// Property 1: control calls progress while a frame retrieval is blocked on the same camera.
void testControlCallsProgressDuringStreaming(AsiCamera& camera)
{
    std::cout << "  control calls progress during streaming\n";

    assert(camera.doSetExposure(std::chrono::milliseconds(20)) == OperationResult::OPERATION_OK);
    assert(camera.doStartVideoCapture() == OperationResult::OPERATION_OK);

    std::atomic<bool> run{true};
    std::atomic<long> frames{0};
    std::atomic<long> frame_errors{0};
    std::atomic<long> control_ok{0};
    std::atomic<long> control_errors{0};

    std::thread grabber([&]
    {
        Frame frame;
        while (run.load())
        {
            if (camera.doGetVideoFrame(frame, std::chrono::milliseconds(1000)) == OperationResult::OPERATION_OK)
                ++frames;
            else
                ++frame_errors;
        }
    });

    std::thread controller([&]
    {
        while (run.load())
        {
            ControlValue value;
            if (camera.getControl(ControlType::GAIN, value) == OperationResult::OPERATION_OK)
                ++control_ok;
            else
                ++control_errors;

            double celsius = 0;
            camera.getSensorTemperature(celsius);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(5));
    run = false;
    grabber.join();
    controller.join();

    std::cout << "    5 s: " << frames.load() << " frames (" << frame_errors.load() << " errors), "
              << control_ok.load() << " control reads (" << control_errors.load() << " errors)\n";

    // Both threads must have made real progress, and neither may have failed.
    assert(frames.load() > 0);
    assert(control_ok.load() > 0);
    assert(frame_errors.load() == 0);
    assert(control_errors.load() == 0);

    int dropped = 0;
    assert(camera.getDroppedFrames(dropped) == OperationResult::OPERATION_OK);
    assert(camera.doStopVideoCapture() == OperationResult::OPERATION_OK);
}

/// Property 2: a disconnect concurrent with a blocked retrieval waits it out rather than closing underneath it.
void testDisconnectWaitsOutAnInFlightRetrieval(CameraId id)
{
    std::cout << "  disconnect waits out an in-flight retrieval\n";

    AsiCamera camera(id);
    assert(camera.doConnect() == OperationResult::OPERATION_OK);

    assert(camera.doStartVideoCapture() == OperationResult::OPERATION_OK);

    // Drain whatever is already queued, then switch to a long exposure and drain again so the new exposure is really in
    // force. Without this, the next retrieval could be satisfied by a frame captured under the old, short exposure and
    // would not block at all.
    Frame drain;
    for (int i = 0; i < 5; ++i)
        camera.doGetVideoFrame(drain, std::chrono::milliseconds(500));
    assert(camera.doSetExposure(std::chrono::milliseconds(1500)) == OperationResult::OPERATION_OK);
    for (int i = 0; i < 2; ++i)
        camera.doGetVideoFrame(drain, std::chrono::milliseconds(2500));

    // ONE retrieval, so there is no loop to re-enter the vendor call and confuse the observation.
    std::atomic<bool> retrieval_entered{false};
    std::atomic<bool> retrieval_returned{false};

    std::thread grabber([&]
    {
        Frame frame;
        retrieval_entered = true;
        camera.doGetVideoFrame(frame, std::chrono::milliseconds(2500));
        retrieval_returned = true;
    });

    // Wait until the retrieval has actually entered the call, then give it a moment to reach the vendor.
    while (!retrieval_entered.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const bool in_flight_at_disconnect = !retrieval_returned.load();

    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    const OperationResult disconnected = camera.doDisconnect();
    const long long waited_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

    assert(disconnected == OperationResult::OPERATION_OK);
    grabber.join();
    assert(retrieval_returned.load());

    std::cout << "    disconnect took " << waited_ms << " ms"
              << " (retrieval was still in flight when it began: " << (in_flight_at_disconnect ? "yes" : "no") << ")\n";

    // THE invariant, asserted through DURATION rather than through a flag. Closing the camera under a pending retrieval
    // is unsafe, and stopping capture does not cancel one, so a disconnect issued while a retrieval is in flight can
    // only return by waiting on the acquisition lock -- which takes the rest of the exposure, hundreds of milliseconds.
    // Without that wait it would return in single-digit milliseconds.
    //
    // Note what must NOT be asserted here: that `retrieval_returned` is set the moment the disconnect returns. The
    // grabber sets that flag AFTER doGetVideoFrame returns, i.e. after the acquisition lock is released, so a disconnect
    // can legitimately win the race to that lock and finish first. Asserting on the flag looks stronger but is racy;
    // asserting on the duration tests the same property and cannot flake.
    if (in_flight_at_disconnect)
        assert(waited_ms > 100);

    // And the wait must be BOUNDED by the frame timeout, never unbounded.
    assert(waited_ms < 5000);

    assert(!camera.isConnected());
    assert(!asi::isCameraClaimed(id));

    // Retrievals attempted after the disconnect must be refused cleanly, never crash or touch a closed camera.
    Frame after_close;
    const OperationResult refused = camera.doGetVideoFrame(after_close, std::chrono::milliseconds(50));
    assert(refused == OperationResult::NOT_CONNECTED);

    // And the camera must be fully usable afterwards.
    AsiCamera after(id);
    assert(after.doConnect() == OperationResult::OPERATION_OK);
    assert(after.doDisconnect() == OperationResult::OPERATION_OK);
}

/// Telemetry polling must coexist with streaming: it is exactly the control-during-retrieval case, via the poller.
void testTelemetryPollingDuringStreaming(AsiCamera& camera)
{
    std::cout << "  telemetry polling during streaming\n";

    std::atomic<int> polls{0};
    std::atomic<int> poll_errors{0};
    assert(camera.setNewStatusCb([&](OperationResult res, const CameraStatus& status)
    {
        if (res == OperationResult::OPERATION_OK && status.connected)
            ++polls;
        else
            ++poll_errors;
    }) == OperationResult::OPERATION_OK);

    assert(camera.doSetExposure(std::chrono::milliseconds(20)) == OperationResult::OPERATION_OK);
    assert(camera.startStatusPolling() == OperationResult::OPERATION_OK);
    assert(camera.doStartVideoCapture() == OperationResult::OPERATION_OK);

    Frame frame;
    int received = 0;
    for (int i = 0; i < 40; ++i)
        if (camera.doGetVideoFrame(frame, std::chrono::milliseconds(1000)) == OperationResult::OPERATION_OK)
            ++received;

    assert(camera.doStopVideoCapture() == OperationResult::OPERATION_OK);
    assert(camera.stopStatusPolling() == OperationResult::OPERATION_OK);

    std::cout << "    " << received << " frames alongside " << polls.load() << " telemetry polls ("
              << poll_errors.load() << " poll errors)\n";
    assert(received > 0);
    assert(polls.load() > 0);
    assert(poll_errors.load() == 0);
}

} // namespace

int main(int argc, char* argv[])
{
    std::cout << "Test_Concurrency (ZWO ASI SDK " << asi::getSdkVersion() << ")\n";

    CameraDescriptorList cameras;
    if (AsiCamera::getDeviceList(kModel, cameras) != OperationResult::OPERATION_OK || cameras.empty())
    {
        std::cout << "SKIPPED: no " << kModel << " attached." << std::endl;
        return 0;
    }

    CameraId id = cameras.front().id;
    if (argc > 1)
        id = static_cast<CameraId>(std::atoi(argv[1]));
    std::cout << "Using camera id " << toType(id) << "\n";

    testConcurrentConnectElectsOneOwner(id);

    {
        AsiCamera camera(id);
        assert(camera.doConnect() == OperationResult::OPERATION_OK);

        RoiFormat roi;
        roi.width = 640; roi.height = 480; roi.bin = 1; roi.format = ImageFormat::RAW8;
        assert(camera.doSetRoi(roi, RoiPosition()) == OperationResult::OPERATION_OK);

        testControlCallsProgressDuringStreaming(camera);
        testTelemetryPollingDuringStreaming(camera);

        assert(camera.doDisconnect() == OperationResult::OPERATION_OK);
    }

    testDisconnectWaitsOutAnInFlightRetrieval(id);

    std::cout << "Test_Concurrency: ALL CHECKS PASSED" << std::endl;
    return 0;
}
