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
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

// PROJECT INCLUDES (module aggregators)
#include <LibDegorasASI/Modules/Common>
#include <LibDegorasASI/Modules/ASI>


using namespace dpasi;
using namespace dpasi::asi;
using namespace dpasi::types;

// ---------------------------------------------------------------------------------------------------------------------
// No-hardware self-check for the process-wide camera ownership registry and the lock registries. Neither touches a
// camera, so both are testable without hardware. The ownership registry is what gives ZWO the aliasing guarantee the
// SDK itself does not provide: verified on hardware, opening an already-open camera returns success, so without this
// two device objects would each believe they owned the same camera.
// ---------------------------------------------------------------------------------------------------------------------

namespace
{

void testClaimIsExclusive()
{
    const CameraId id = CameraId{7};
    assert(!isCameraClaimed(id));

    assert(tryClaimCamera(id));      // first claim wins
    assert(isCameraClaimed(id));
    assert(!tryClaimCamera(id));     // second claim on the same camera is refused
    assert(!tryClaimCamera(id));     // and stays refused

    releaseCamera(id);
    assert(!isCameraClaimed(id));
    assert(tryClaimCamera(id));      // claimable again once released
    releaseCamera(id);
    assert(!isCameraClaimed(id));
}

void testClaimsAreIndependentPerCamera()
{
    const CameraId first = CameraId{0};
    const CameraId second = CameraId{1};

    assert(tryClaimCamera(first));
    assert(tryClaimCamera(second));   // a different camera is unaffected by the first claim
    assert(isCameraClaimed(first) && isCameraClaimed(second));

    releaseCamera(first);
    assert(!isCameraClaimed(first));
    assert(isCameraClaimed(second));  // releasing one must not release the other

    releaseCamera(second);
    assert(!isCameraClaimed(second));
}

void testReleaseIsIdempotent()
{
    const CameraId id = CameraId{42};

    // Releasing an unclaimed camera is a no-op, so a teardown path can call it unconditionally.
    releaseCamera(id);
    releaseCamera(id);
    assert(!isCameraClaimed(id));

    assert(tryClaimCamera(id));
    releaseCamera(id);
    releaseCamera(id);
    assert(!isCameraClaimed(id));
}

void testConcurrentClaimsElectExactlyOneWinner()
{
    // The registry's whole purpose is to be raced. Many threads claim the same camera at once; exactly one may win.
    const CameraId id = CameraId{99};
    constexpr int kThreads = 16;

    std::atomic<int> winners{0};
    std::vector<std::thread> racers;
    racers.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i)
        racers.emplace_back([&]{ if (tryClaimCamera(id)) ++winners; });

    for (std::thread& racer : racers)
        racer.join();

    assert(winners.load() == 1);
    assert(isCameraClaimed(id));

    releaseCamera(id);
    assert(!isCameraClaimed(id));
}

void testLockRegistriesAreStableAndDistinct()
{
    // The same camera must always yield the same mutex, so two adapter objects addressing one camera share it.
    assert(&cameraMtx(CameraId{0}) == &cameraMtx(CameraId{0}));
    assert(&acquisitionMtx(CameraId{0}) == &acquisitionMtx(CameraId{0}));

    // Different cameras must yield different mutexes, so independent cameras are not serialised against each other.
    assert(&cameraMtx(CameraId{0}) != &cameraMtx(CameraId{1}));
    assert(&acquisitionMtx(CameraId{0}) != &acquisitionMtx(CameraId{1}));

    // The control lock and the acquisition lock are DISTINCT for the same camera. This is the property that keeps a
    // short control call from waiting out a blocking frame retrieval.
    assert(&cameraMtx(CameraId{0}) != &acquisitionMtx(CameraId{0}));
    assert(&cameraMtx(CameraId{5}) != &acquisitionMtx(CameraId{5}));

    // The discovery lock is a single process-wide mutex, distinct from any per-camera lock.
    assert(&discoveryMtx() == &discoveryMtx());
    assert(&discoveryMtx() != &cameraMtx(CameraId{0}));
    assert(&discoveryMtx() != &acquisitionMtx(CameraId{0}));

    // References stay valid as the registry grows: a rehash must not relocate a mutex a caller may be holding.
    std::mutex& early = cameraMtx(CameraId{1000});
    for (int i = 1001; i < 1200; ++i)
        (void)cameraMtx(CameraId{i});
    assert(&early == &cameraMtx(CameraId{1000}));

    // And the mutexes are usable, not merely distinct.
    {
        const std::lock_guard<std::mutex> control_lock(cameraMtx(CameraId{3}));
        const std::lock_guard<std::mutex> acq_lock(acquisitionMtx(CameraId{3}));
    }
}

void testConcurrentLockLookupIsSafe()
{
    // Many threads resolving mutexes for overlapping cameras must not corrupt the registry.
    constexpr int kThreads = 8;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back([t]
        {
            for (int i = 0; i < 200; ++i)
            {
                const CameraId id = CameraId{(t * 7 + i) % 32};
                const std::lock_guard<std::mutex> lock(cameraMtx(id));
            }
        });
    }

    for (std::thread& worker : workers)
        worker.join();

    assert(&cameraMtx(CameraId{0}) == &cameraMtx(CameraId{0}));
}

void testModelNameMatching()
{
    // Pure string check, safe before connecting: the vendor brands its model names, and the exact prefix is not
    // guaranteed, so matching is by case-insensitive containment.
    assert(nameMatchesModel("ZWO ASI224MC", "ASI224MC"));
    assert(nameMatchesModel("ZWO ASI224MC", "asi224mc"));
    assert(nameMatchesModel("ASI224MC", "ASI224MC"));
    assert(nameMatchesModel("ZWO ASI224MC", "ZWO"));

    assert(!nameMatchesModel("ZWO ASI224MC", "ASI183MM"));
    assert(!nameMatchesModel("ZWO ASI224MC", ""));            // an empty model matches nothing
    assert(!nameMatchesModel("", "ASI224MC"));
    assert(!nameMatchesModel("ASI", "ASI224MC"));             // model longer than the name

    // A different camera's name must not match, even where the models share a prefix.
    assert(!nameMatchesModel("ZWO ASI224MM", "ASI224MC"));
    assert(nameMatchesModel("ZWO ASI2600MC Pro", "ASI2600MC"));
}

} // namespace

int main()
{
    testClaimIsExclusive();
    testClaimsAreIndependentPerCamera();
    testReleaseIsIdempotent();
    testConcurrentClaimsElectExactlyOneWinner();
    testLockRegistriesAreStableAndDistinct();
    testConcurrentLockLookupIsSafe();
    testModelNameMatching();

    std::cout << "UT_CameraRegistry: ALL CHECKS PASSED" << std::endl;
    return 0;
}
