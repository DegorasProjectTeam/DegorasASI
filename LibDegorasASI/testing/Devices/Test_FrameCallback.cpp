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
// ASSERTIONS MUST BE LIVE IN EVERY BUILD TYPE.
//
// This suite is assert-based and has no framework, so assert() IS the test. Release defines NDEBUG, which makes
// assert() expand to nothing and discards the WHOLE expression -- including any call inside it. Measured on this
// suite before the fix: objdump found zero references to _assert in all eleven Release objects, so the entire
// assertion layer was dead, and 610 of the 716 assert sites wrap a call whose side effect went with it. The visible
// symptoms were a suite that passed while doing nothing (Test_AsiCameraCapture reported "serial: (empty), 0
// controls, sensor 0x0" and still declared success, because assert(camera.doConnect() == ...) never opened the
// camera) and two tests that hung outright (UT_FramePump, Test_FrameCallback: assert(pump.start(...)) never started
// the pump, so a later loop waited on a counter that could not advance).
//
// BOTH LINES ARE REQUIRED, in this order, above the first #include. The bare #undef is NOT enough: if anything has
// already pulled in <cassert> while NDEBUG was defined, assert is already expanded away and stays dead. Re-including
// the header re-arms it, because assert.h does #undef assert and redefines the macro on every inclusion and
// <cassert> deliberately has no include guard. Verified by compiling both forms with -O3 -DNDEBUG: the two-line
// form fires, the bare #undef placed after an earlier <cassert> produces zero _assert references.
#undef NDEBUG
#include <cassert>


// C++ INCLUDES
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
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
// Integration test for callback-driven frame acquisition against real hardware: delivery, the guards around starting
// and stopping the worker, buffer reuse, and every teardown path (explicit stop, stopping the stream, disconnecting,
// and the destructor). SELF-SKIPS with success when no camera is attached.
// ---------------------------------------------------------------------------------------------------------------------

namespace
{

void configure(AsiCamera& camera)
{
    RoiFormat roi;
    roi.width = 640; roi.height = 480; roi.bin = 1; roi.format = ImageFormat::RAW8;
    assert(camera.doSetRoi(roi, RoiPosition()) == OperationResult::OPERATION_OK);
    assert(camera.doSetExposure(std::chrono::milliseconds(20)) == OperationResult::OPERATION_OK);
}

void testStartGuards(AsiCamera& camera)
{
    std::cout << "  start guards\n";

    // The worker pumps an existing stream; it never starts one itself, so that the acquisition state machine keeps a
    // single owner.
    assert(camera.getAcquisitionMode() == AcquisitionMode::IDLE);
    assert(camera.startFrameAcquisition() == OperationResult::INVALID_SEQUENCE);
    assert(!camera.isFrameAcquisitionRunning());

    assert(camera.doStartVideoCapture() == OperationResult::OPERATION_OK);
    assert(camera.startFrameAcquisition(std::chrono::milliseconds(1000)) == OperationResult::OPERATION_OK);
    assert(camera.isFrameAcquisitionRunning());

    // Idempotence is NOT silent here: a second start is an error, so a caller cannot accidentally believe it replaced
    // the callback or the timeout of a running worker.
    assert(camera.startFrameAcquisition() == OperationResult::WORKER_ALREADY_RUNNING);

    assert(camera.stopFrameAcquisition() == OperationResult::OPERATION_OK);
    assert(!camera.isFrameAcquisitionRunning());
    assert(camera.stopFrameAcquisition() == OperationResult::WORKER_NOT_RUNNING);

    assert(camera.doStopVideoCapture() == OperationResult::OPERATION_OK);
}

void testFramesAreDelivered(AsiCamera& camera)
{
    std::cout << "  frames are delivered to the callback\n";

    std::atomic<int> ok_frames{0};
    std::atomic<int> bad_frames{0};
    std::atomic<std::uint64_t> last_seq{0};
    std::atomic<bool> geometry_ok{true};
    std::atomic<bool> size_ok{true};
    std::atomic<bool> monotonic{true};
    std::atomic<const void*> first_buffer{nullptr};
    std::atomic<bool> buffer_reused{true};

    assert(camera.setNewFrameCb([&](OperationResult res, const Frame& frame)
    {
        if (res != OperationResult::OPERATION_OK)
        {
            ++bad_frames;
            return;
        }
        if (frame.width != 640 || frame.height != 480 || frame.format != ImageFormat::RAW8)
            geometry_ok = false;
        if (frame.data.size() != frame.expectedBytes() || frame.data.size() != 640u * 480u)
            size_ok = false;
        if (frame.sequence <= last_seq.load())
            monotonic = false;
        last_seq = frame.sequence;

        // The worker must reuse ONE buffer across the whole run: a steady stream costs no per-frame allocation.
        const void* buf = frame.data.data();
        const void* expected = first_buffer.load();
        if (expected == nullptr)
            first_buffer = buf;
        else if (buf != expected)
            buffer_reused = false;

        ++ok_frames;
    }) == OperationResult::OPERATION_OK);

    assert(camera.doStartVideoCapture() == OperationResult::OPERATION_OK);
    assert(camera.startFrameAcquisition(std::chrono::milliseconds(1000)) == OperationResult::OPERATION_OK);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    assert(camera.stopFrameAcquisition() == OperationResult::OPERATION_OK);

    const std::uint64_t delivered = camera.getAcquiredFrameCount();
    const std::uint64_t failed = camera.getFailedFrameCount();
    std::cout << "    " << ok_frames.load() << " frames to the callback in 2 s ("
              << delivered << " counted by the pump, " << failed << " failures)\n";

    assert(ok_frames.load() > 0);
    assert(geometry_ok.load());
    assert(size_ok.load());
    assert(monotonic.load());
    assert(buffer_reused.load());

    // The pump's own counters must agree with what the callback observed.
    assert(delivered == static_cast<std::uint64_t>(ok_frames.load()));
    assert(failed == static_cast<std::uint64_t>(bad_frames.load()));

    assert(camera.doStopVideoCapture() == OperationResult::OPERATION_OK);
}

void testCountersResetPerRun(AsiCamera& camera)
{
    std::cout << "  counters reset on each start\n";

    assert(camera.setNewFrameCb([](OperationResult, const Frame&){}) == OperationResult::OPERATION_OK);
    assert(camera.doStartVideoCapture() == OperationResult::OPERATION_OK);

    assert(camera.startFrameAcquisition(std::chrono::milliseconds(1000)) == OperationResult::OPERATION_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    assert(camera.stopFrameAcquisition() == OperationResult::OPERATION_OK);
    const std::uint64_t first_run = camera.getAcquiredFrameCount();
    assert(first_run > 0);

    assert(camera.startFrameAcquisition(std::chrono::milliseconds(1000)) == OperationResult::OPERATION_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    assert(camera.stopFrameAcquisition() == OperationResult::OPERATION_OK);
    const std::uint64_t second_run = camera.getAcquiredFrameCount();

    // A fresh run must not accumulate on top of the previous one.
    assert(second_run > 0);
    assert(second_run < first_run + first_run);
    std::cout << "    run 1: " << first_run << " frames, run 2: " << second_run << " frames\n";

    assert(camera.doStopVideoCapture() == OperationResult::OPERATION_OK);
}

void testStoppingTheStreamStopsTheWorker(AsiCamera& camera)
{
    std::cout << "  stopping the stream stops the worker\n";

    assert(camera.setNewFrameCb([](OperationResult, const Frame&){}) == OperationResult::OPERATION_OK);
    assert(camera.doStartVideoCapture() == OperationResult::OPERATION_OK);
    assert(camera.startFrameAcquisition(std::chrono::milliseconds(1000)) == OperationResult::OPERATION_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    assert(camera.isFrameAcquisitionRunning());

    // Leaving the worker running against a stopped stream would flood the callback with timeouts, so stopping the
    // stream must stop it.
    assert(camera.doStopVideoCapture() == OperationResult::OPERATION_OK);
    assert(!camera.isFrameAcquisitionRunning());
    assert(camera.getAcquisitionMode() == AcquisitionMode::IDLE);
}

void testDisconnectStopsTheWorker(CameraId id)
{
    std::cout << "  disconnect stops the worker\n";

    AsiCamera camera(id);
    assert(camera.doConnect() == OperationResult::OPERATION_OK);
    configure(camera);

    std::atomic<int> frames{0};
    assert(camera.setNewFrameCb([&frames](OperationResult res, const Frame&)
    {
        if (res == OperationResult::OPERATION_OK)
            ++frames;
    }) == OperationResult::OPERATION_OK);

    assert(camera.doStartVideoCapture() == OperationResult::OPERATION_OK);
    assert(camera.startFrameAcquisition(std::chrono::milliseconds(1000)) == OperationResult::OPERATION_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    assert(frames.load() > 0);

    // Disconnecting straight from a running worker: it must stop the worker, wait out the capture in flight, and only
    // then close. No explicit stop first.
    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    assert(camera.doDisconnect() == OperationResult::OPERATION_OK);
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    assert(!camera.isFrameAcquisitionRunning());
    assert(!camera.isConnected());
    assert(!asi::isCameraClaimed(id));
    assert(ms < 5000);   // bounded
    std::cout << "    disconnect from a running worker took " << ms << " ms\n";

    // The callback must not fire after the disconnect returns.
    const int settled = frames.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    assert(frames.load() == settled);
}

void testDestructorStopsTheWorker(CameraId id)
{
    std::cout << "  destructor stops the worker\n";

    {
        AsiCamera camera(id);
        assert(camera.doConnect() == OperationResult::OPERATION_OK);
        configure(camera);
        assert(camera.setNewFrameCb([](OperationResult, const Frame&){}) == OperationResult::OPERATION_OK);
        assert(camera.doStartVideoCapture() == OperationResult::OPERATION_OK);
        assert(camera.startFrameAcquisition(std::chrono::milliseconds(1000)) == OperationResult::OPERATION_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        // No stop, no disconnect: the destructor must do all of it without hanging.
    }

    assert(!asi::isCameraClaimed(id));

    // Fully usable afterwards, which proves the teardown really completed.
    AsiCamera after(id);
    assert(after.doConnect() == OperationResult::OPERATION_OK);
    assert(after.doDisconnect() == OperationResult::OPERATION_OK);
    std::cout << "    camera reusable after destructor teardown\n";
}

void testCallbackMayBeReplacedAndCleared(AsiCamera& camera)
{
    std::cout << "  callback can be replaced while running\n";

    std::atomic<int> first{0};
    std::atomic<int> second{0};

    assert(camera.setNewFrameCb([&first](OperationResult res, const Frame&)
    {
        if (res == OperationResult::OPERATION_OK) ++first;
    }) == OperationResult::OPERATION_OK);

    assert(camera.doStartVideoCapture() == OperationResult::OPERATION_OK);
    assert(camera.startFrameAcquisition(std::chrono::milliseconds(1000)) == OperationResult::OPERATION_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const int first_before_swap = first.load();
    assert(first_before_swap > 0);

    // Swapped mid-run: the worker reads the callback afresh for every frame.
    assert(camera.setNewFrameCb([&second](OperationResult res, const Frame&)
    {
        if (res == OperationResult::OPERATION_OK) ++second;
    }) == OperationResult::OPERATION_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    assert(second.load() > 0);
    std::cout << "    first callback " << first.load() << " frames, second " << second.load() << "\n";

    // Clearing it must not break the worker: acquisition keeps running, frames are simply not delivered.
    assert(camera.setNewFrameCb(nullptr) == OperationResult::OPERATION_OK);
    const std::uint64_t before = camera.getAcquiredFrameCount();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    assert(camera.getAcquiredFrameCount() > before);

    assert(camera.stopFrameAcquisition() == OperationResult::OPERATION_OK);
    assert(camera.doStopVideoCapture() == OperationResult::OPERATION_OK);
}

void testObserversAreSafeFromInsideTheCallback(AsiCamera& camera)
{
    std::cout << "  observers callable from inside the callback\n";

    // Regression test. The observers must NOT take the worker-lifecycle lock: stop() holds that lock while waiting for
    // the worker, so an observer that took it would block against a concurrent stop() for the whole join timeout and
    // force the worker to be detached. Reading a counter from inside the callback is the natural thing to do, so it
    // must be free of that hazard.
    std::atomic<int> observed{0};
    std::atomic<bool> observers_agreed{true};

    assert(camera.setNewFrameCb([&](OperationResult res, const Frame&)
    {
        if (res != OperationResult::OPERATION_OK)
            return;
        // All three observers, from the worker thread, while a stop may be racing us.
        const std::uint64_t delivered = camera.getAcquiredFrameCount();
        const std::uint64_t failed = camera.getFailedFrameCount();
        const bool running = camera.isFrameAcquisitionRunning();
        // The count is incremented before delivery, so this frame is already included.
        if (delivered == 0 || !running || failed > delivered)
            observers_agreed = false;
        ++observed;
    }) == OperationResult::OPERATION_OK);

    assert(camera.doStartVideoCapture() == OperationResult::OPERATION_OK);
    assert(camera.startFrameAcquisition(std::chrono::milliseconds(1000)) == OperationResult::OPERATION_OK);

    // Let deliveries start, then stop while the callback is actively hammering the observers.
    while (observed.load() < 3)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    const OperationResult stopped = camera.stopFrameAcquisition();
    const long long stop_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    std::cout << "    " << observed.load() << " callbacks queried the observers; stop took " << stop_ms << " ms\n";

    // A clean join, NOT a timeout-and-detach: the join bound is capture timeout + 1 s = 2 s here, so a deadlocked
    // observer would show up as ~2 s and OPERATION_TIMEOUT.
    assert(stopped == OperationResult::OPERATION_OK);
    assert(stop_ms < 1500);
    assert(observers_agreed.load());
    assert(observed.load() > 0);

    assert(camera.doStopVideoCapture() == OperationResult::OPERATION_OK);
}

void testPollingAndCallbackStylesAgree(AsiCamera& camera)
{
    std::cout << "  callback and polling styles drive the same path\n";

    // The manual style still works after the worker has been used, and both report the same geometry.
    assert(camera.doStartVideoCapture() == OperationResult::OPERATION_OK);

    Frame manual;
    bool got_manual = false;
    for (int i = 0; i < 10 && !got_manual; ++i)
        got_manual = (camera.doGetVideoFrame(manual, std::chrono::milliseconds(1000)) == OperationResult::OPERATION_OK);
    assert(got_manual);

    Frame from_cb;
    std::atomic<bool> got_cb{false};
    assert(camera.setNewFrameCb([&](OperationResult res, const Frame& frame)
    {
        if (res == OperationResult::OPERATION_OK && !got_cb.load())
        {
            from_cb = frame;   // copy out, as a real consumer must
            got_cb = true;
        }
    }) == OperationResult::OPERATION_OK);

    assert(camera.startFrameAcquisition(std::chrono::milliseconds(1000)) == OperationResult::OPERATION_OK);
    for (int i = 0; i < 40 && !got_cb.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(camera.stopFrameAcquisition() == OperationResult::OPERATION_OK);
    assert(got_cb.load());

    assert(from_cb.width == manual.width && from_cb.height == manual.height);
    assert(from_cb.format == manual.format && from_cb.bin == manual.bin);
    assert(from_cb.data.size() == manual.data.size());
    std::cout << "    both styles yielded " << manual.width << "x" << manual.height << " "
              << toString(manual.format) << ", " << manual.data.size() << " bytes\n";

    assert(camera.doStopVideoCapture() == OperationResult::OPERATION_OK);
}

} // namespace

int main(int argc, char* argv[])
{
    std::cout << "Test_FrameCallback (ZWO ASI SDK " << asi::getSdkVersion() << ")\n";

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

    {
        AsiCamera camera(id);
        assert(camera.doConnect() == OperationResult::OPERATION_OK);
        configure(camera);

        testStartGuards(camera);
        testFramesAreDelivered(camera);
        testCountersResetPerRun(camera);
        testStoppingTheStreamStopsTheWorker(camera);
        testCallbackMayBeReplacedAndCleared(camera);
        testObserversAreSafeFromInsideTheCallback(camera);
        testPollingAndCallbackStylesAgree(camera);

        assert(camera.doDisconnect() == OperationResult::OPERATION_OK);
    }

    testDisconnectStopsTheWorker(id);
    testDestructorStopsTheWorker(id);

    std::cout << "Test_FrameCallback: ALL CHECKS PASSED" << std::endl;
    return 0;
}
