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


// ---------------------------------------------------------------------------------------------------------------------
// Compile-time guard for the library's central architectural invariant: THE VENDOR SDK NEVER LEAKS INTO A PUBLIC HEADER.
//
// This translation unit includes every public header, individually and through every module aggregator. It is built as a
// test target, and test targets do NOT receive the vendor SDK on their include path -- the SDK is linked PRIVATE to the
// library, so only the library's own translation units can see it. Therefore, if any public header ever starts to
// include ASICamera2.h (or otherwise depend on a vendor type), THIS FILE FAILS TO COMPILE.
//
// It also proves each header is self-sufficient: every one is included first in its own right, so a header that silently
// relied on another being included before it would fail here too.
//
// The runtime body is deliberately near-empty. The value is in the compilation, not the assertions.
// ---------------------------------------------------------------------------------------------------------------------

// PROJECT INCLUDES (every public header, individually)
#include "DegorasASI/Global/degorasasi_export.h"
#include "DegorasASI/Common/common_types.h"
#include "DegorasASI/Helpers/json_utils.h"
#include "DegorasASI/Helpers/frame_pump.h"
#include "DegorasASI/Helpers/status_poller.h"
#include "DegorasASI/Helpers/wait_for.h"
#include "DegorasASI/ASI/asi_api_lock.h"
#include "DegorasASI/ASI/asi_camera_controller.h"
#include "DegorasASI/ASI/asi_camera_registry.h"
#include "DegorasASI/ASI/asi_discovery.h"
#include "DegorasASI/ASI/asi_error.h"
#include "DegorasASI/Devices/asi_camera.h"

// PROJECT INCLUDES (every module aggregator, which is how consumers are expected to include the library)
#include <DegorasASI/Modules/Common>
#include <DegorasASI/Modules/Helpers>
#include <DegorasASI/Modules/ASI>
#include <DegorasASI/Modules/Devices>

// C++ INCLUDES
#include <cassert>
#include <iostream>


int main()
{
    // A handful of touches so the includes above are not merely parsed but actually instantiated and linked: the value
    // of this test is that it COMPILED and LINKED with no vendor SDK in sight.
    dpasi::types::Frame frame;
    assert(frame.empty());

    dpasi::StatusPoller<dpasi::types::CameraStatus> poller;
    assert(!poller.isRunning());

    const dpasi::AsiCamera camera(dpasi::types::CameraId{0});
    assert(camera.getExpectedModel().empty());   // no required model -> any ASI camera is accepted

    const dpasi::asi::AsiCameraController controller(dpasi::types::CameraId{0});
    assert(controller.cameraId() == dpasi::types::CameraId{0});

    std::cout << "UT_HeaderHygiene: ALL CHECKS PASSED (public headers are free of the vendor SDK)" << std::endl;
    return 0;
}
