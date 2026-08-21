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

// =====================================================================================================================
//  HARDWARE GATE FOR THE INTEGRATION TESTS (Test_*)
//
//  THE PROBLEM THIS FIXES. All three Test_* executables opened with the same block:
//
//      if (getDeviceList(...) != OPERATION_OK || cameras.empty())
//      {
//          std::cout << "SKIPPED: no " << kModel << " attached.\n";
//          return 0;                       // <-- CTest reads this as PASSED
//      }
//
//  Exit status 0 is success, so on a machine with no camera the suite reported a full green run having tested
//  nothing at all. That is the same failure as the assertions being compiled out in Release: a result that looks
//  like a result and is not one.
//
//  THREE STATES, not two. A camera can be absent, present and free, or present and HELD BY ANOTHER PROCESS --
//  the last of which used to be invisible. The ASI SDK reports no exclusivity at all (a second process opens and
//  initialises an already-streaming camera with ASI_SUCCESS), so the two then fight and every run fails at a
//  different assertion. asi_camera_lock.h adds the host-wide claim that makes the state observable, and this gate
//  refuses to run rather than produce a random red suite. The message names the holder, e.g.
//  "held by pid 19340 (Example_asi_camera_live.exe)".
//
//  THE TWO MODES, because both are wanted, and they apply to a missing camera and a held one alike.
//
//    * DEFAULT -- no camera means SKIPPED, and it says so. Exit status 77, which CTest reports as "Skipped" when
//      the test carries SKIP_RETURN_CODE 77 (set for every Test_* in the subgroup CMakeLists). Not a pass, not a
//      failure, and visible as its own count in the ctest summary. Right for a developer machine or a build that
//      only means to check the code compiles and the unit tests hold.
//
//    * DPASI_REQUIRE_CAMERA=1 -- no camera is a FAILURE. Exit status 1. Right for the station and for any run whose
//      purpose is to certify the whole suite: there, silently skipping the only tests that touch the vendor SDK
//      would defeat the point of running it.
//
//  An environment variable rather than a CMake option deliberately: it needs no reconfigure, it can differ between
//  two runs of the same build tree, and CI sets it in one place. Any value other than "0" or the empty string
//  counts as set, so DPASI_REQUIRE_CAMERA=1, =yes and =true all work.
// =====================================================================================================================

#pragma once

// C++ INCLUDES
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

// PROJECT INCLUDES
#include <DegorasASI/Modules/Devices>
#include <DegorasASI/Modules/ASI>

// =====================================================================================================================

namespace dpasi_test
{

/// CTest reads this exit status as "Skipped" for any test carrying the SKIP_RETURN_CODE property.
constexpr int kCTestSkip = 77;

/// True when the caller has demanded that hardware be present.
inline bool cameraIsRequired()
{
    const char* raw = std::getenv("DPASI_REQUIRE_CAMERA");
    if (raw == nullptr)
        return false;
    const std::string value(raw);
    return !value.empty() && value != "0";
}

/// Finds the camera an integration test needs, or ends the process with the right verdict.
///
/// Never returns when there is no camera: it exits 77 (skip) or 1 (failure) according to DPASI_REQUIRE_CAMERA.
/// Returning a sentinel instead would only push the decision into every caller, which is how the old copies of this
/// logic came to agree on the wrong answer.
///
/// \param test_name  Name of the calling test, for the message.
/// \param model      Camera model to look for, or nullptr for any camera.
/// \param argc,argv  The test's own arguments; a first argument overrides the camera id.
inline dpasi::types::CameraId requireCamera(const char* test_name, const char* model, int argc, char* argv[])
{
    using dpasi::AsiCamera;
    using dpasi::types::CameraDescriptorList;
    using dpasi::types::CameraId;
    using dpasi::types::OperationResult;

    CameraDescriptorList cameras;
    const OperationResult res = (model != nullptr) ? AsiCamera::getDeviceList(model, cameras)
                                                  : AsiCamera::getDeviceList(cameras);

    if (res != OperationResult::OPERATION_OK || cameras.empty())
    {
        const std::string what = (model != nullptr) ? std::string("no ") + model + " attached"
                                                    : std::string("no ASI camera attached");

        if (cameraIsRequired())
        {
            std::cout << test_name << ": FAILED -- " << what
                      << ", and DPASI_REQUIRE_CAMERA is set.\n"
                      << "  This test exercises the vendor SDK against real hardware. Connect the camera, or unset "
                      << "DPASI_REQUIRE_CAMERA to let it skip instead." << std::endl;
            std::exit(1);
        }

        std::cout << test_name << ": SKIPPED -- " << what << ".\n"
                  << "  Set DPASI_REQUIRE_CAMERA=1 to make this a failure instead, for a run that must cover the "
                  << "hardware path." << std::endl;
        std::exit(kCTestSkip);
    }

    CameraId id = cameras.front().id;
    if (argc > 1)
        id = static_cast<CameraId>(std::atoi(argv[1]));

    // ANOTHER PROCESS MAY HOLD IT, and the SDK will not say so: measured, a second process enumerates, opens and
    // initialises an already-streaming camera with ASI_SUCCESS throughout, after which the two fight and the
    // failures are random. The host-wide claim in asi_camera_lock.h is what makes the question answerable, so ask
    // it BEFORE running a suite that would otherwise fail at a different assertion every time.
    const std::string holder = dpasi::asi::describeCameraHolder(id);
    if (!holder.empty())
    {
        if (cameraIsRequired())
        {
            std::cout << test_name << ": FAILED -- camera " << dpasi::types::toType(id)
                      << " is held by " << holder << ", and DPASI_REQUIRE_CAMERA is set.\n"
                      << "  Close that program and retry." << std::endl;
            std::exit(1);
        }

        std::cout << test_name << ": SKIPPED -- camera " << dpasi::types::toType(id)
                  << " is held by " << holder << ".\n"
                  << "  Close that program to run the hardware tests." << std::endl;
        std::exit(kCTestSkip);
    }

    std::cout << "Using camera id " << dpasi::types::toType(id) << "\n";
    return id;
}

}   // namespace dpasi_test

// **********************************************************************************************************************
