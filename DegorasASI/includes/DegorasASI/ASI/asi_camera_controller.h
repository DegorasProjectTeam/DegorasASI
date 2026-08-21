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

#pragma once

// C++ INCLUDES
#include <chrono>
#include <string>

// PROJECT INCLUDES
#include "DegorasASI/Global/degorasasi_export.h"
#include "DegorasASI/Common/common_types.h"


// NAMESPACES
namespace dpasi
{
namespace asi
{

// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Adapter binding one ASI camera (by identifier) to the ASICamera2 C API.
 *
 * @details This is the ONLY class that knows the ZWO ASI camera C API, and its translation unit is the only one besides
 *          discovery that includes the vendor header. An ASI camera is addressed by identifier alone, so this adapter
 *          holds nothing but that identifier: no vendor handle, no vendor type, and hence no need for a PIMPL to keep
 *          the SDK out of this header. A device personality composes exactly one of these. Every method returns a
 *          self-contained DeviceError by value; there is no stored "last error".
 *
 * Threading:
 *  - Camera-scoped lifecycle calls (open/initialise/close) take the global discoveryMtx(), because they mutate the
 *    SDK's process-global camera/connection state.
 *  - Short steady-state calls take cameraMtx(id), so two different cameras run concurrently.
 *  - The BLOCKING frame retrievals take acquisitionMtx(id) instead, never cameraMtx(id). Measured on hardware: a short
 *    control call issued while another thread is blocked in a frame retrieval is safe and both progress, so the control
 *    lock must not cover the blocking call. No method holds cameraMtx(id) across a wait.
 *
 * Frame buffers: every retrieval sizes its destination from the camera's LIVE ROI, read back from the SDK, and refuses
 *                with BUFFER_TOO_SMALL rather than calling the vendor with a short buffer -- the vendor's retrievals do
 *                no bounds check and are documented to crash in that case.
 *
 * @warning This adapter is a thin translation layer: it does NOT enforce the acquisition state machine. The SDK accepts
 *          some calls that are illegal for the state it is in (notably it accepts a ROI/format change while streaming,
 *          silently changing the geometry a caller has already sized a buffer for). Guarding that is the device layer's
 *          job, which tracks the acquisition mode; a caller using this adapter directly must guard it itself.
 */
class DEGORASASI_EXPORT AsiCameraController
{
public:

    explicit AsiCameraController(types::CameraId id);

    types::CameraId cameraId() const;

    // -- Camera-scoped lifecycle --
    /// @brief ASIOpenCamera. @return DEVICE_NOT_FOUND if the camera is absent or was unplugged.
    types::DeviceError open();
    /// @brief ASIInitCamera. Blocks a while and disturbs a camera that is capturing; never call it on a live stream.
    types::DeviceError initialise();
    /// @brief ASICloseCamera. Takes both the global lock and this camera's control lock, so it cannot run while a short
    ///        control call is in flight on the same camera.
    /// @warning It does NOT wait for a frame RETRIEVAL, which is guarded by the acquisition lock instead. A caller
    ///          tearing a camera down must hold acquisitionMtx(id) across the close, as the device layer does.
    types::DeviceError close();

    // -- Identity --
    /// @brief Read the stable hardware serial number as 16 hexadecimal digits. Requires the camera to be OPEN.
    types::DeviceError readSerialNumber(types::CameraSN& out_serial) const;

    // -- Controls --
    /// @brief Enumerate every control this camera exposes, with its limits and flags. Requires the camera to be open.
    types::DeviceError readControlCaps(types::ControlCapsList& out_caps) const;
    /// @brief Read one control's current value and auto flag.
    types::DeviceError readControl(types::ControlType type, types::ControlValue& out_value) const;
    /// @brief Write one control's value and auto flag.
    /// @warning The SDK silently CLAMPS an out-of-range value and reports success, so validate against the control's
    ///          discovered limits before calling if an out-of-range write must be an error.
    types::DeviceError writeControl(types::ControlType type, const types::ControlValue& value);

    // -- Region of interest and image format --
    /// @brief Read the live ROI geometry and pixel format.
    types::DeviceError readRoiFormat(types::RoiFormat& out_roi) const;
    /// @brief Write the ROI geometry and pixel format.
    /// @return INVALID_PARAMETER without contacting the camera if @p roi violates the vendor's alignment rules.
    /// @warning Succeeds while streaming, silently changing the frame geometry; and it RECENTRES the ROI origin on the
    ///          sensor. Apply the intended origin with writeRoiPosition() afterwards.
    types::DeviceError writeRoiFormat(const types::RoiFormat& roi);
    /// @brief Read the live ROI origin, in binned pixel coordinates.
    types::DeviceError readRoiPosition(types::RoiPosition& out_pos) const;
    /// @brief Write the ROI origin. Legal while streaming. @return INVALID_PARAMETER if it puts the ROI off-sensor.
    types::DeviceError writeRoiPosition(const types::RoiPosition& pos);

    // -- Video streaming --
    types::DeviceError startVideoCapture();          ///< ASIStartVideoCapture (idempotent).
    types::DeviceError stopVideoCapture() noexcept;  ///< ASIStopVideoCapture (idempotent, teardown-safe).

    /**
     * @brief Retrieve the next streamed frame, blocking until one arrives or @p timeout elapses.
     * @param[out] out_frame Resized to the live ROI only when the geometry changed, then filled; its metadata is set.
     * @param timeout How long to wait for a frame. Clamped to a bounded maximum; the SDK's "wait forever" is unused.
     * @return OPERATION_OK on success, OPERATION_TIMEOUT if no frame arrived, BUFFER_TOO_SMALL if the live geometry
     *         could not be established, NOT_CONNECTED if the camera closed.
     * @note BLOCKS for up to @p timeout while holding acquisitionMtx(id). Reuse one Frame across a loop: the buffer is
     *       reallocated only on a geometry change.
     * @warning Stopping capture does NOT cancel a retrieval already blocked here (measured): it runs to its full
     *          timeout. Keep @p timeout modest, because it bounds how long a concurrent disconnect must wait.
     */
    types::DeviceError readVideoFrame(types::Frame& out_frame, std::chrono::milliseconds timeout);

    /// @brief Frames dropped in the current capture session (USB congestion or slow consumption).
    /// @note Sample it BEFORE stopping capture: the vendor's documentation disagrees with itself about when the counter
    ///       is cleared, so treat it as a per-session figure rather than a cumulative one.
    types::DeviceError readDroppedFrames(int& out_dropped) const;

    // -- Single-frame (snapshot) exposure --
    /// @brief Begin one exposure. @param dark True to keep a mechanical shutter closed (ignored if none is fitted).
    /// @return INVALID_SEQUENCE if video capture is running.
    types::DeviceError startExposure(bool dark);
    /// @brief Cancel an exposure in progress (teardown-safe).
    types::DeviceError stopExposure() noexcept;
    /// @brief Poll the exposure's progress.
    types::DeviceError readExposureState(types::ExposureState& out_state) const;

    /**
     * @brief Download the frame produced by a completed exposure.
     * @param[out] out_frame Resized to the live ROI only when the geometry changed, then filled.
     * @return OPERATION_OK on success, BUFFER_TOO_SMALL if the live geometry could not be established.
     * @note Only call it once the exposure state reports success. The call takes no timeout and blocks for an
     *       unspecified period, so it is issued under acquisitionMtx(id) like the streaming retrieval.
     */
    types::DeviceError readExposureFrame(types::Frame& out_frame);

    // -- Miscellaneous --
    /// @brief Disable the vendor's dark-frame subtraction.
    /// @note Worth calling at connect: the setting PERSISTS in the Windows registry across process lifetimes, so a
    ///       previous run or another application can leave it enabled and silently alter every frame.
    types::DeviceError disableDarkSubtract();

private:

    /// @brief Read the live ROI and grow @p frame to fit it, setting its geometry metadata.
    /// @return BUFFER_TOO_SMALL if the required size could not be established, so no vendor call is made with a short
    ///         buffer. This is the single gate protecting every frame retrieval.
    types::DeviceError prepareFrame(types::Frame& out_frame);

    types::CameraId id_;
};

// ---------------------------------------------------------------------------------------------------------------------

}} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
