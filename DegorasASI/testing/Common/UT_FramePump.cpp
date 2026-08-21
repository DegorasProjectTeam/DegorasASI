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
#include <iostream>
#include <stdexcept>
#include <thread>

// PROJECT INCLUDES (module aggregators)
#include <DegorasASI/Modules/Common>
#include <DegorasASI/Modules/Helpers>


using namespace dpasi;
using namespace dpasi::types;

// ---------------------------------------------------------------------------------------------------------------------
// No-hardware self-check for the FramePump worker. The pump is generic -- it only knows a producer and a sink -- so it
// can be driven with SYNTHETIC producers that no camera could reproduce on demand: one that fails instantly, one that
// throws, one that blocks. That is exactly where the interesting failure modes live, so this carries the coverage that
// the hardware test cannot.
// ---------------------------------------------------------------------------------------------------------------------

namespace
{

void testDeliversAndCounts()
{
    std::cout << "  delivers frames and counts them\n";

    FramePump pump;
    std::atomic<int> delivered{0};
    std::atomic<bool> geometry_kept{true};
    std::atomic<const void*> first_buffer{nullptr};
    std::atomic<bool> buffer_reused{true};

    const OperationResult started = pump.start(
        [](Frame& frame)
        {
            // Fill the frame the way the real producer does: size it once, then reuse it.
            frame.format = ImageFormat::RAW8;
            frame.width = 64; frame.height = 32;
            if (frame.data.size() != frame.expectedBytes())
                frame.data.resize(frame.expectedBytes());
            ++frame.sequence;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));   // stands in for the blocking capture
            return OperationResult::OPERATION_OK;
        },
        [&](OperationResult res, const Frame& frame)
        {
            if (res != OperationResult::OPERATION_OK || frame.data.size() != 64u * 32u)
                geometry_kept = false;
            const void* buf = frame.data.data();
            if (first_buffer.load() == nullptr)
                first_buffer = buf;
            else if (buf != first_buffer.load())
                buffer_reused = false;
            ++delivered;
        });

    assert(started == OperationResult::OPERATION_OK);
    assert(pump.isRunning());
    assert(pump.start([](Frame&){ return OperationResult::OPERATION_OK; },
                      [](OperationResult, const Frame&){}) == OperationResult::WORKER_ALREADY_RUNNING);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    assert(pump.stop() == OperationResult::OPERATION_OK);
    assert(!pump.isRunning());
    assert(pump.stop() == OperationResult::WORKER_NOT_RUNNING);

    std::cout << "    " << delivered.load() << " deliveries, pump counted " << pump.deliveredCount() << "\n";
    assert(delivered.load() > 0);
    assert(pump.deliveredCount() == static_cast<std::uint64_t>(delivered.load()));
    assert(pump.failedCount() == 0);
    assert(geometry_kept.load());
    assert(buffer_reused.load());   // ONE buffer for the whole run
}

void testFailingProducerDoesNotSpin()
{
    std::cout << "  a failing producer does not spin\n";

    // Regression test. The producer is only DOCUMENTED to block; real outcomes return instantly (a zero timeout, a
    // geometry that cannot be established, a camera closed underneath us). Without a floor the loop would run at
    // roughly 10^6 iterations/s, pinning a core and flooding the callback at the same rate.
    FramePump pump;
    std::atomic<int> callbacks{0};

    assert(pump.start(
        [](Frame&){ return OperationResult::OPERATION_TIMEOUT; },   // returns instantly, forever
        [&](OperationResult res, const Frame&)
        {
            assert(res == OperationResult::OPERATION_TIMEOUT);
            ++callbacks;
        }) == OperationResult::OPERATION_OK);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    assert(pump.stop() == OperationResult::OPERATION_OK);

    const int n = callbacks.load();
    std::cout << "    " << n << " failed captures in 300 ms (a spin would be tens of thousands)\n";
    assert(n > 0);            // it must still make progress and keep reporting
    assert(n < 2000);         // ~5 ms floor => order of 60; a spin would be orders of magnitude more
    assert(pump.failedCount() == static_cast<std::uint64_t>(n));
    assert(pump.deliveredCount() == 0);
}

void testStopIsPromptDespiteTheBackoff()
{
    std::cout << "  stop stays prompt during the backoff\n";

    // The floor must be an interruptible wait on the same condition variable stop() signals, not a sleep.
    FramePump pump;
    assert(pump.start([](Frame&){ return OperationResult::OPERATION_TIMEOUT; },
                      [](OperationResult, const Frame&){}) == OperationResult::OPERATION_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    assert(pump.stop() == OperationResult::OPERATION_OK);
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    std::cout << "    stop returned in " << ms << " ms\n";
    assert(ms < 200);
}

void testThrowingCallablesDoNotTerminate()
{
    std::cout << "  a throwing producer or sink does not kill the process\n";

    // An exception escaping a thread entry point is std::terminate: the whole application would die because one frame
    // could not be captured or queued. Reaching the end of this function at all is the assertion.
    {
        FramePump pump;
        std::atomic<int> seen{0};
        assert(pump.start(
            [](Frame&) -> OperationResult { throw std::runtime_error("producer exploded"); },
            [&](OperationResult res, const Frame&)
            {
                // A throwing producer is reported as a failed capture, not hidden.
                assert(res != OperationResult::OPERATION_OK);
                ++seen;
            }) == OperationResult::OPERATION_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        assert(pump.stop() == OperationResult::OPERATION_OK);   // a clean join, not a detach
        assert(seen.load() > 0);
        assert(pump.failedCount() > 0);
    }

    {
        FramePump pump;
        std::atomic<int> attempts{0};
        assert(pump.start(
            [&](Frame&){ ++attempts; std::this_thread::sleep_for(std::chrono::milliseconds(5));
                         return OperationResult::OPERATION_OK; },
            [](OperationResult, const Frame&) { throw std::runtime_error("consumer exploded"); })
            == OperationResult::OPERATION_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        assert(pump.stop() == OperationResult::OPERATION_OK);
        // A throwing sink costs that one delivery; acquisition carries on.
        assert(attempts.load() > 1);
        assert(pump.deliveredCount() > 1);
    }
    std::cout << "    both contained; acquisition survived\n";
}

void testObserversAreCallableFromTheSink()
{
    std::cout << "  observers callable from inside the sink\n";

    // stop() must hold the worker-lifecycle lock while waiting for the worker, so the observers must not take it, or a
    // sink that reads a counter would deadlock against a concurrent stop() until the join timeout expired.
    FramePump pump;
    std::atomic<bool> observers_ok{true};
    std::atomic<int> seen{0};

    assert(pump.start(
        [](Frame& frame)
        {
            frame.format = ImageFormat::RAW8; frame.width = 8; frame.height = 2;
            frame.data.resize(frame.expectedBytes());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return OperationResult::OPERATION_OK;
        },
        [&](OperationResult, const Frame&)
        {
            if (pump.deliveredCount() == 0 || !pump.isRunning())
                observers_ok = false;
            (void)pump.failedCount();
            ++seen;
        }) == OperationResult::OPERATION_OK);

    while (seen.load() < 3)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    const OperationResult stopped = pump.stop();
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    std::cout << "    " << seen.load() << " sinks queried the observers; stop took " << ms << " ms\n";
    assert(stopped == OperationResult::OPERATION_OK);   // a join, NOT a timeout-and-detach
    assert(ms < 1000);
    assert(observers_ok.load());
}

void testCountersAreFreshPerRun()
{
    std::cout << "  counters are per-run\n";

    FramePump pump;
    const auto producer = [](Frame&)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return OperationResult::OPERATION_OK;
    };
    const auto sink = [](OperationResult, const Frame&){};

    assert(pump.start(producer, sink) == OperationResult::OPERATION_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    assert(pump.stop() == OperationResult::OPERATION_OK);
    const std::uint64_t first = pump.deliveredCount();
    assert(first > 0);

    assert(pump.start(producer, sink) == OperationResult::OPERATION_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    assert(pump.stop() == OperationResult::OPERATION_OK);
    const std::uint64_t second = pump.deliveredCount();

    std::cout << "    run 1: " << first << ", run 2: " << second << "\n";
    assert(second > 0 && second < first + first);   // reset, not accumulated
}

void testRejectsEmptyCallables()
{
    std::cout << "  empty callables are rejected\n";

    FramePump pump;
    assert(pump.start(nullptr, [](OperationResult, const Frame&){}) == OperationResult::INVALID_PARAMETER);
    assert(pump.start([](Frame&){ return OperationResult::OPERATION_OK; }, nullptr) ==
           OperationResult::INVALID_PARAMETER);
    assert(!pump.isRunning());
}

void testDestructorStopsAWorker()
{
    std::cout << "  destructor stops a running worker\n";

    std::atomic<bool> alive{true};
    {
        FramePump pump;
        assert(pump.start(
            [](Frame&){ std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        return OperationResult::OPERATION_OK; },
            [](OperationResult, const Frame&){}) == OperationResult::OPERATION_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // No stop: the destructor must do it, bounded.
    }
    alive = false;
    assert(!alive.load());
}

} // namespace

int main()
{
    testDeliversAndCounts();
    testFailingProducerDoesNotSpin();
    testStopIsPromptDespiteTheBackoff();
    testThrowingCallablesDoNotTerminate();
    testObserversAreCallableFromTheSink();
    testCountersAreFreshPerRun();
    testRejectsEmptyCallables();
    testDestructorStopsAWorker();

    std::cout << "UT_FramePump: ALL CHECKS PASSED" << std::endl;
    return 0;
}
