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

// ASSERTIONS MUST BE LIVE IN EVERY BUILD TYPE. See any other test in this suite for the full reasoning; the short
// version is that Release defines NDEBUG, assert() then expands to nothing and takes the whole expression with it,
// and both lines below are needed because a bare #undef is silently useless if <cassert> came in earlier.
#undef NDEBUG
#include <cassert>

// C++ INCLUDES
#include <cstdlib>
#include <iostream>
#include <string>

// PROJECT INCLUDES (module aggregators)
#include <DegorasASI/Modules/ASI>


using namespace dpasi;
using namespace dpasi::types;

// ---------------------------------------------------------------------------------------------------------------------
// This test needs NO CAMERA. The host-wide claim is a lock on a file named after a camera id; nothing ever opens the
// device, so a made-up id exercises the mechanism exactly as a real one would. That is the point of testing it here
// rather than inside the hardware tests: the property under test is mutual exclusion between processes, and mixing
// that with a USB device would make a failure ambiguous.
//
// Mutual exclusion cannot be tested within one process -- both the in-process registry and the OS lock would
// correctly consider the second attempt to be the same owner -- so the test RE-EXECUTES ITSELF as a child and reads
// the child's exit status. Hence the two modes below.
// ---------------------------------------------------------------------------------------------------------------------

namespace
{

/// A camera id no hardware will have. Kept high so a real camera can never collide with it.
constexpr int kFakeId = 4242;

/// Exit codes the child uses to report what happened, chosen not to collide with 0, 1 or CTest's 77.
constexpr int kChildAcquired = 10;
constexpr int kChildRefused  = 20;

int runChild(int id)
{
    const CameraId camera = static_cast<CameraId>(id);

    const std::string holder = asi::describeCameraHolder(camera);
    std::cout << "    child: holder reported as '" << holder << "'\n";

    if (!asi::tryLockCameraOnHost(camera))
        return kChildRefused;

    asi::unlockCameraOnHost(camera);
    return kChildAcquired;
}

/// Runs this same executable as a child, in child mode, and returns its exit status.
int spawnChild(const char* self, int id)
{
    // Quoted: the build directory path contains a drive letter and may contain spaces.
    const std::string cmd = std::string("\"") + self + "\" --child " + std::to_string(id);
    return std::system(cmd.c_str());
}

void testTheClaimIsHeldAgainstAnotherProcess(const char* self)
{
    std::cout << "  the claim is held against another process\n";

    const CameraId camera = static_cast<CameraId>(kFakeId);

    // Belt and braces: a previous crashed run cannot have left this held -- the OS releases the lock when a process
    // dies, however it dies -- but assert it, because the whole test depends on starting from free.
    assert(asi::describeCameraHolder(camera).empty());

    const bool got = asi::tryLockCameraOnHost(camera);
    assert(got);

    // Our own claim is deliberately NOT reported as a holder: the question a caller asks is always "who else".
    assert(asi::describeCameraHolder(camera).empty());

    const int refused = spawnChild(self, kFakeId);
    std::cout << "    child exit status while we hold it: " << refused << "\n";
    assert(refused == kChildRefused);

    asi::unlockCameraOnHost(camera);

    const int acquired = spawnChild(self, kFakeId);
    std::cout << "    child exit status after we release it: " << acquired << "\n";
    assert(acquired == kChildAcquired);
}

void testReClaimingFromTheSameProcessSucceeds()
{
    std::cout << "  re-claiming from the same process succeeds\n";

    // The in-process registry is what refuses a second object in this process, and it runs first, so the host-wide
    // lock must not also refuse -- otherwise a legitimate disconnect/reconnect cycle would fail.
    const CameraId camera = static_cast<CameraId>(kFakeId + 1);
    assert(asi::tryLockCameraOnHost(camera));
    assert(asi::tryLockCameraOnHost(camera));
    asi::unlockCameraOnHost(camera);
}

void testReleaseIsIdempotent()
{
    std::cout << "  release is idempotent and safe on a claim never taken\n";

    const CameraId never = static_cast<CameraId>(kFakeId + 2);
    asi::unlockCameraOnHost(never);
    asi::unlockCameraOnHost(never);

    const CameraId taken = static_cast<CameraId>(kFakeId + 3);
    assert(asi::tryLockCameraOnHost(taken));
    asi::unlockCameraOnHost(taken);
    asi::unlockCameraOnHost(taken);
}

void testAReleasedClaimIsAvailableAgain(const char* self)
{
    std::cout << "  a released claim is available again, repeatedly\n";

    const CameraId camera = static_cast<CameraId>(kFakeId + 4);
    for (int round = 1; round <= 3; ++round)
    {
        assert(asi::tryLockCameraOnHost(camera));
        assert(spawnChild(self, kFakeId + 4) == kChildRefused);
        asi::unlockCameraOnHost(camera);
        assert(spawnChild(self, kFakeId + 4) == kChildAcquired);
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    if (argc == 3 && std::string(argv[1]) == "--child")
        return runChild(std::atoi(argv[2]));

    std::cout << "UT_CameraLock (no camera required)\n";

    testTheClaimIsHeldAgainstAnotherProcess(argv[0]);
    testReClaimingFromTheSameProcessSucceeds();
    testReleaseIsIdempotent();
    testAReleasedClaimIsAvailableAgain(argv[0]);

    std::cout << "UT_CameraLock: ALL CHECKS PASSED" << std::endl;
    return 0;
}

// **********************************************************************************************************************
