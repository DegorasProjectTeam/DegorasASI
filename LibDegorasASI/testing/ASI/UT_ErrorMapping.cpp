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
#include <cassert>
#include <iostream>
#include <string>

// PROJECT INCLUDES (module aggregators)
#include <LibDegorasASI/Modules/Common>
#include <LibDegorasASI/Modules/ASI>


using namespace dpasi;
using namespace dpasi::asi;
using namespace dpasi::types;

// ---------------------------------------------------------------------------------------------------------------------
// No-hardware self-check for the raw-vendor-code -> category mapping and the error-construction helpers. The mapping is
// deliberately pure, so it is fully testable without a camera or the vendor SDK.
// ---------------------------------------------------------------------------------------------------------------------

namespace
{

void testSuccessMapping()
{
    assert(categoryFromAsi(static_cast<int>(AsiError::SUCCESS)) == OperationResult::OPERATION_OK);
}

void testConnectivityMapping()
{
    // A closed camera is "not connected"; an absent or unplugged one is "not found". Keeping them distinct is what lets
    // a caller tell "you forgot to connect" from "the hardware went away".
    assert(categoryFromAsi(static_cast<int>(AsiError::CAMERA_CLOSED)) == OperationResult::NOT_CONNECTED);
    assert(categoryFromAsi(static_cast<int>(AsiError::INVALID_INDEX)) == OperationResult::DEVICE_NOT_FOUND);
    assert(categoryFromAsi(static_cast<int>(AsiError::INVALID_ID)) == OperationResult::DEVICE_NOT_FOUND);
    assert(categoryFromAsi(static_cast<int>(AsiError::CAMERA_REMOVED)) == OperationResult::DEVICE_NOT_FOUND);
}

void testCapabilityMapping()
{
    assert(categoryFromAsi(static_cast<int>(AsiError::INVALID_CONTROL_TYPE)) ==
           OperationResult::UNSUPPORTED_CAPABILITY);
    assert(categoryFromAsi(static_cast<int>(AsiError::GPS_NOT_SUPPORTED)) == OperationResult::UNSUPPORTED_CAPABILITY);
    assert(categoryFromAsi(static_cast<int>(AsiError::GPS_VER_ERR)) == OperationResult::UNSUPPORTED_CAPABILITY);
}

void testParameterMapping()
{
    assert(categoryFromAsi(static_cast<int>(AsiError::INVALID_SIZE)) == OperationResult::INVALID_PARAMETER);
    assert(categoryFromAsi(static_cast<int>(AsiError::OUTOF_BOUNDARY)) == OperationResult::INVALID_PARAMETER);
    assert(categoryFromAsi(static_cast<int>(AsiError::INVALID_PATH)) == OperationResult::INVALID_PARAMETER);
    assert(categoryFromAsi(static_cast<int>(AsiError::INVALID_FILEFORMAT)) == OperationResult::INVALID_PARAMETER);

    // The vendor documents INVALID_IMGTYPE as also covering bad geometry or binning, so it is grouped with the other
    // parameter faults rather than reported as purely an unsupported format.
    assert(categoryFromAsi(static_cast<int>(AsiError::INVALID_IMGTYPE)) == OperationResult::INVALID_PARAMETER);
}

void testSequenceMapping()
{
    // Every "you called this in the wrong state" code collapses to one category; the raw code preserves which one.
    assert(categoryFromAsi(static_cast<int>(AsiError::INVALID_SEQUENCE)) == OperationResult::INVALID_SEQUENCE);
    assert(categoryFromAsi(static_cast<int>(AsiError::VIDEO_MODE_ACTIVE)) == OperationResult::INVALID_SEQUENCE);
    assert(categoryFromAsi(static_cast<int>(AsiError::EXPOSURE_IN_PROGRESS)) == OperationResult::INVALID_SEQUENCE);
    assert(categoryFromAsi(static_cast<int>(AsiError::INVALID_MODE)) == OperationResult::INVALID_SEQUENCE);
}

void testOtherMapping()
{
    assert(categoryFromAsi(static_cast<int>(AsiError::BUFFER_TOO_SMALL)) == OperationResult::BUFFER_TOO_SMALL);
    assert(categoryFromAsi(static_cast<int>(AsiError::TIMEOUT)) == OperationResult::OPERATION_TIMEOUT);
    assert(categoryFromAsi(static_cast<int>(AsiError::GENERAL_ERROR)) == OperationResult::ZWO_INTERNAL_ERROR);
    assert(categoryFromAsi(static_cast<int>(AsiError::GPS_FPGA_ERR)) == OperationResult::READ_FAILED);
    assert(categoryFromAsi(static_cast<int>(AsiError::GPS_DATA_INVALID)) == OperationResult::READ_FAILED);
}

void testUnknownCodesAreGenericNotSuccess()
{
    // The critical property: a code this library does not model must NEVER be mistaken for success. A newer SDK adding
    // an error code must degrade to a generic failure, not to a silent pass.
    assert(categoryFromAsi(23) == OperationResult::ZWO_INTERNAL_ERROR);
    assert(categoryFromAsi(99) == OperationResult::ZWO_INTERNAL_ERROR);
    assert(categoryFromAsi(-1) == OperationResult::ZWO_INTERNAL_ERROR);
    assert(categoryFromAsi(1000000) == OperationResult::ZWO_INTERNAL_ERROR);

    // Only the literal success code maps to success.
    for (int raw = 1; raw <= 40; ++raw)
        assert(categoryFromAsi(raw) != OperationResult::OPERATION_OK);
}

void testErrorStringification()
{
    assert(asiErrorToString(0) == "ASI_SUCCESS");
    assert(asiErrorToString(4) == "ASI_ERROR_CAMERA_CLOSED");
    assert(asiErrorToString(13) == "ASI_ERROR_BUFFER_TOO_SMALL");
    assert(asiErrorToString(22) == "ASI_ERROR_GPS_DATA_INVALID");

    // The vendor's header and its PDF disagree on the terminating ordinal, so anything past the modelled range must
    // stringify as unknown rather than be trusted.
    assert(asiErrorToString(23) == "ASI_UNKNOWN_ERROR");
    assert(asiErrorToString(-5) == "ASI_UNKNOWN_ERROR");

    // Every modelled code stringifies to something meaningful.
    for (int raw = 0; raw <= 22; ++raw)
        assert(asiErrorToString(raw) != "ASI_UNKNOWN_ERROR");
}

void testErrorConstruction()
{
    // An error built from a vendor code carries BOTH the mapped category and the raw code, so mapping to a general
    // category never loses information.
    const DeviceError from_asi = errorFromAsi(static_cast<int>(AsiError::TIMEOUT), "ASIGetVideoData(id=0)");
    assert(!from_asi.ok());
    assert(from_asi.category == OperationResult::OPERATION_TIMEOUT);
    assert(from_asi.asi_code == 11);
    assert(from_asi.context == "ASIGetVideoData(id=0)");
    assert(from_asi.toString().find("asi=11") != std::string::npos);

    // An error the library detected itself made no vendor call, so it reports a zero raw code.
    const DeviceError from_lib = errorFromLibrary(OperationResult::INVALID_PARAMETER, "writeRoiFormat(id=0)");
    assert(!from_lib.ok());
    assert(from_lib.category == OperationResult::INVALID_PARAMETER);
    assert(from_lib.asi_code == 0);
    assert(from_lib.context == "writeRoiFormat(id=0)");

    // A vendor success code produces an ok error, and so does the explicit success helper.
    assert(errorFromAsi(0, "ASIOpenCamera(id=0)").ok());
    const DeviceError ok = successError();
    assert(ok.ok() && ok.asi_code == 0 && ok.context.empty());
}

} // namespace

int main()
{
    testSuccessMapping();
    testConnectivityMapping();
    testCapabilityMapping();
    testParameterMapping();
    testSequenceMapping();
    testOtherMapping();
    testUnknownCodesAreGenericNotSuccess();
    testErrorStringification();
    testErrorConstruction();

    std::cout << "UT_ErrorMapping: ALL CHECKS PASSED" << std::endl;
    return 0;
}
