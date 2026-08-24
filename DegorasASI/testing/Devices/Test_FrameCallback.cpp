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
#include <DegorasASI/Modules/ASI>
#include <DegorasASI/Modules/Devices>

// TESTING INCLUDES
#include <dpasi_test_camera.h>


using namespace dpasi;
using namespace dpasi::types;

/// The camera model these checks are written against. One class drives every ASI model, so this is only the model
/// the run is VALIDATED on; point it at another model and the same code exercises that camera unchanged.
constexpr const char* kModel = "ASI224MC";

// ---------------------------------------------------------------------------------------------------------------------
// TIME WINDOWS: WAIT FOR THE CONDITION, NOT FOR THE CLOCK.
//
// Every wait in this file used to be a fixed sleep -- two seconds here, six hundred milliseconds there -- followed by
// an assertion that something had happened in the meantime. That is wrong in both directions at once: it wastes two
// seconds on a link that delivers in two hundred milliseconds, and it fails for no good reason on a busy machine or a
// USB2 host where the frame simply had not arrived yet. Neither outcome tells you anything about the code under test.
//
// So the windows below are LIMITS, not durations: each wait returns as soon as the thing it is waiting for is true,
// and only the limit is a number. The limits are derived from the exposure the test configures, so pointing this
// suite at a camera or a link with different timing does not mean revisiting a table of magic constants.
//
// One wait cannot work this way and is marked where it appears: proving that the callback does NOT fire after a
// disconnect is a wait for a NON-event, and the only honest way to bound that is to wait longer than a frame would
// have taken.
// ---------------------------------------------------------------------------------------------------------------------

/// The exposure every check below runs at. Short, because none of them is about image quality.
constexpr std::chrono::milliseconds kExposure{20};

/// A generous allowance for ONE frame to come back: exposure, readout and the USB transfer. Wide on purpose -- this
/// suite also runs on a USB2 host, where a 640x480 frame is not instantaneous.
constexpr std::chrono::milliseconds kFrameBudget{400};

/// How many frames a "frames are flowing" wait insists on. More than one, so a single lucky frame cannot satisfy a
/// check that is really about a stream being pumped.
constexpr int kFramesWanted = 5;

/// @brief Blocks until a predicate holds, or until the limit runs out.
/// @return Whether it came true. Polled rather than condition-variable driven because the things being waited on are
///         plain atomics written by the worker thread, and a two-millisecond poll is far finer than a frame.
template <typename Predicate>
bool waitFor(Predicate ready, std::chrono::milliseconds limit)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (ready())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return ready();
}

/// @brief Blocks until a counter has advanced by @p wanted frames, or until that many frame budgets have passed.
template <typename Counter>
bool waitForFrames(const Counter& counter, int wanted)
{
    const auto start = counter.load();
    return waitFor([&counter, start, wanted]() { return counter.load() - start >= wanted; },
                   kFrameBudget * wanted);
}

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
    assert(camera.doSetExposure(kExposure) == OperationResult::OPERATION_OK);
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
    const bool flowed = waitForFrames(ok_frames, kFramesWanted);
    assert(camera.stopFrameAcquisition() == OperationResult::OPERATION_OK);

    const std::uint64_t delivered = camera.getAcquiredFrameCount();
    const std::uint64_t failed = camera.getFailedFrameCount();
    std::cout << "    " << ok_frames.load() << " frames to the callback ("
              << delivered << " counted by the pump, " << failed << " failures)\n";

    assert(flowed);
    assert(ok_frames.load() >= kFramesWanted);
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

    // The wait counts frames THROUGH THE CALLBACK rather than through the pump's own counter, and that matters
    // here: the whole point of this check is whether the pump counter resets, so waiting on it would make the second
    // run's wait succeed instantly on a stale value and hide exactly the bug being looked for.
    std::atomic<int> seen{0};
    assert(camera.setNewFrameCb([&seen](OperationResult res, const Frame&)
    {
        if (res == OperationResult::OPERATION_OK)
            ++seen;
    }) == OperationResult::OPERATION_OK);
    assert(camera.doStartVideoCapture() == OperationResult::OPERATION_OK);

    assert(camera.startFrameAcquisition(std::chrono::milliseconds(1000)) == OperationResult::OPERATION_OK);
    assert(waitForFrames(seen, kFramesWanted));
    assert(camera.stopFrameAcquisition() == OperationResult::OPERATION_OK);
    const std::uint64_t first_run = camera.getAcquiredFrameCount();
    assert(first_run > 0);

    assert(camera.startFrameAcquisition(std::chrono::milliseconds(1000)) == OperationResult::OPERATION_OK);
    assert(waitForFrames(seen, kFramesWanted));
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

    std::atomic<int> seen{0};
    assert(camera.setNewFrameCb([&seen](OperationResult res, const Frame&)
    {
        if (res == OperationResult::OPERATION_OK)
            ++seen;
    }) == OperationResult::OPERATION_OK);
    assert(camera.doStartVideoCapture() == OperationResult::OPERATION_OK);
    assert(camera.startFrameAcquisition(std::chrono::milliseconds(1000)) == OperationResult::OPERATION_OK);

    // Waiting for frames rather than for a clock also makes the precondition stronger: the worker is not merely
    // flagged as running, it is demonstrably pumping when the stream is pulled out from under it.
    assert(waitForFrames(seen, kFramesWanted));
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
    assert(waitForFrames(frames, kFramesWanted));

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

    // The callback must not fire after the disconnect returns. THE ONE FIXED SLEEP IN THIS FILE, and unavoidable:
    // this is a wait for a non-event, so there is no condition to wait on. Bounded by a frame budget, which is the
    // honest length -- if the worker were still alive, a frame would have arrived inside it.
    const int settled = frames.load();
    std::this_thread::sleep_for(kFrameBudget);
    assert(frames.load() == settled);
}

void testDestructorStopsTheWorker(CameraId id)
{
    std::cout << "  destructor stops the worker\n";

    {
        AsiCamera camera(id);
        assert(camera.doConnect() == OperationResult::OPERATION_OK);
        configure(camera);
        std::atomic<int> seen{0};
        assert(camera.setNewFrameCb([&seen](OperationResult res, const Frame&)
        {
            if (res == OperationResult::OPERATION_OK)
                ++seen;
        }) == OperationResult::OPERATION_OK);
        assert(camera.doStartVideoCapture() == OperationResult::OPERATION_OK);
        assert(camera.startFrameAcquisition(std::chrono::milliseconds(1000)) == OperationResult::OPERATION_OK);
        assert(waitForFrames(seen, kFramesWanted));
        // No stop, no disconnect: the destructor must do all of it without hanging, and with frames actually in
        // flight rather than merely a worker that was started a moment ago.
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
    assert(waitForFrames(first, kFramesWanted));
    const int first_before_swap = first.load();
    assert(first_before_swap > 0);

    // Swapped mid-run: the worker reads the callback afresh for every frame.
    assert(camera.setNewFrameCb([&second](OperationResult res, const Frame&)
    {
        if (res == OperationResult::OPERATION_OK) ++second;
    }) == OperationResult::OPERATION_OK);
    assert(waitForFrames(second, kFramesWanted));

    assert(second.load() > 0);
    std::cout << "    first callback " << first.load() << " frames, second " << second.load() << "\n";

    // Clearing it must not break the worker: acquisition keeps running, frames are simply not delivered.
    assert(camera.setNewFrameCb(nullptr) == OperationResult::OPERATION_OK);
    const std::uint64_t before = camera.getAcquiredFrameCount();
    assert(waitFor([&camera, before]() { return camera.getAcquiredFrameCount() > before; }, kFrameBudget * 2));

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

    // Let deliveries start, then stop while the callback is actively hammering the observers. BOUNDED: this used to
    // be a bare while-loop with no way out, so a camera that stopped delivering hung the test instead of failing it.
    assert(waitForFrames(observed, 3));

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
    const bool arrived = waitFor([&got_cb]() { return got_cb.load(); }, kFrameBudget * kFramesWanted);
    assert(camera.stopFrameAcquisition() == OperationResult::OPERATION_OK);
    assert(arrived);
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

    // The gate lives in ../dpasi_test_camera.h. This block used to print SKIPPED and return 0 when no
    // camera was attached, and exit status 0 is what CTest reads as PASSED. See that header for the two modes.
    const CameraId id = dpasi_test::requireCamera("Test_FrameCallback", kModel, argc, argv);

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
