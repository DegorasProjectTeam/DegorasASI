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
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

// PROJECT INCLUDES
#include "DegorasASI/Global/degorasasi_export.h"
#include "DegorasASI/Common/common_types.h"
#include "DegorasASI/ASI/asi_camera_controller.h"
#include "DegorasASI/Helpers/frame_pump.h"
#include "DegorasASI/Helpers/status_poller.h"


// NAMESPACES
namespace dpasi
{

// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Connection configuration for a camera.
 * @note @ref disable_dark_subtract defaults to true on purpose: the vendor's dark-frame subtraction PERSISTS in the
 *       Windows registry across process lifetimes, so a previous run or an unrelated application can leave it enabled
 *       and silently alter every frame this library delivers. Clearing it at connect makes a fresh connection mean the
 *       same thing every time. Set it to false only if an external tool is deliberately managing that setting.
 * @note @ref reset_flip defaults to true for the same reason, and matters more than it looks. The vendor's FLIP
 *       control also persists, and ASIStudio sets it from a menu, so a camera can arrive delivering vertically
 *       mirrored buffers because of something a different program did days ago. That does not merely turn pictures
 *       upside down: on a colour sensor a vertical flip moves the Bayer mosaic by one row, so every raw frame is
 *       demosaiced wrong and reds come out green -- and nothing in the frame reveals it. Set it to false only if the
 *       flip is being managed deliberately, and then expect to correct the mosaic yourself.
 */
struct DeviceConfig
{
    bool disable_dark_subtract = true;   ///< Clear the vendor's persistent dark-subtraction setting at connect.
    bool reset_flip = true;              ///< Clear the vendor's persistent image flip at connect.
    int telemetry_rate_ms = 500;         ///< Interval of the background telemetry poller, when started.
};

/**
 * @brief Driver for any ZWO ASI camera.
 *
 * @details ONE class covers every model, because the vendor gives every model the SAME C API: the differences between
 *          an ASI224MC and an ASI2600MM are entirely DATA the SDK reports at run time — which controls exist, and the
 *          flags in @ref types::CameraDescriptor — not differences in structure or behaviour. Supporting another model
 *          therefore needs no new code at all.
 *
 *          That is the opposite of the sibling reference library, which does have a class per device. Its vendor SDK
 *          exposes a DIFFERENT C API per module and its devices differ structurally (one axis versus two), so there a
 *          per-device class composes different adapters and has something real to model. Here it would only duplicate
 *          the same logic behind a different name.
 *
 * Capabilities are DISCOVERED, never assumed. The control set varies by model — the ASI224MC exposes 14 of the SDK's
 * controls and has no gamma control at all — so the set is read once at connect and every request is checked against
 * it. Ask for something this camera does not have and you get UNSUPPORTED_CAPABILITY, never a fabricated value. Query
 * the capabilities first (@ref hasControl, @ref hasCooler, @ref isColour) if you need to decide what to offer a user.
 *
 * Acquisition: the two workflows are mutually exclusive in hardware, and configuration is illegal while either runs, so
 *              this class tracks the acquisition mode and rejects an illegal transition with INVALID_SEQUENCE. That
 *              guard is not decoration: the SDK accepts a ROI/format change during streaming and silently changes the
 *              frame geometry a caller has already sized a buffer for.
 *
 * Ownership & lifetime: no device I/O in the constructor; deterministic and bounded shutdown; non-copyable and
 *                       non-movable. The destructor disconnects, which stops both background workers, waits out any
 *                       frame retrieval already in flight, and only then closes the camera.
 */
class DEGORASASI_EXPORT AsiCamera final
{
public:

    /// Callback delivering each poll's (result, status). Invoked on the telemetry worker thread, holding no lock.
    using NewStatusCb = std::function<void(types::OperationResult, const types::CameraStatus&)>;

    /// Callback delivering each captured frame. Invoked on the acquisition worker thread, holding no lock.
    using NewFrameCb = std::function<void(types::OperationResult, const types::Frame&)>;

    /// @brief Bind to a camera by identifier, accepting whatever model it turns out to be.
    explicit AsiCamera(types::CameraId id);

    /**
     * @brief Bind to a camera by identifier, requiring it to be a particular model.
     * @param id The camera to target.
     * @param expected_model Model the camera must report, e.g. "ASI2600MM". Matched case-insensitively by containment
     *        against the vendor's name string, which is brand-prefixed ("ZWO ASI2600MM").
     * @note doConnect() then fails with DEVICE_NOT_FOUND if the camera is a different model. Use this when a program is
     *       written against one specific camera and connecting to the wrong one would be worse than not connecting.
     */
    AsiCamera(types::CameraId id, std::string expected_model);

    ~AsiCamera();

    AsiCamera(const AsiCamera&) = delete;
    AsiCamera& operator=(const AsiCamera&) = delete;
    AsiCamera(AsiCamera&&) = delete;
    AsiCamera& operator=(AsiCamera&&) = delete;

    // -- Identity and connection --

    types::CameraId getCameraId() const;          ///< Identifier of the camera this object targets.
    std::string getExpectedModel() const;         ///< The required model, or empty if any model is accepted.
    bool isConnected() const;                     ///< Whether the camera currently has an open connection.

    /**
     * @brief Open and initialise the camera. Idempotent for the object that owns the connection.
     * @param cfg Optional connection settings.
     * @return OPERATION_OK on success; ALREADY_CONNECTED if this object already owns it; CAMERA_IN_USE if another
     *         object holds it; DEVICE_NOT_FOUND if the camera is absent or is not the required model;
     *         ZWO_INTERNAL_ERROR if the SDK refused the open or the initialisation.
     * @note Discovers the control set, leaves the camera idle, and performs no acquisition. Constructors do no device
     *       I/O, so this is the first call that touches hardware.
     */
    types::OperationResult doConnect(const DeviceConfig& cfg = DeviceConfig{});

    /**
     * @brief Stop any acquisition, close and release the camera. Returns NOT_CONNECTED if it was not connected.
     * @note Deterministic and bounded: it stops both background workers, waits out any frame retrieval already in
     *       flight, then closes. It never hangs, and it is also run by the destructor.
     */
    types::OperationResult doDisconnect();

    // -- Capabilities (discovered at connect) --

    /// @brief The camera's fixed properties: sensor size, pixel pitch, supported bins and formats, feature flags.
    types::OperationResult getDescriptor(types::CameraDescriptor& descriptor) const;

    /// @brief The camera's stable hardware serial number, as 16 hexadecimal digits.
    /// @note Requires a connection: the SDK only reveals the serial number of an open camera.
    types::OperationResult getSerialNumber(types::CameraSN& serial) const;

    /// @brief Every control this camera exposes, with its limits and flags.
    types::OperationResult getControlCaps(types::ControlCapsList& caps) const;

    /// @brief The limits and flags of one control. @return UNSUPPORTED_CAPABILITY if this camera lacks it.
    types::OperationResult getControlCaps(types::ControlType type, types::ControlCaps& caps) const;

    // The predicates below answer "can this camera do X?" without an error round-trip, so a user interface can decide
    // what to offer before attempting anything. All return false when not connected, since nothing is known yet.

    bool hasControl(types::ControlType type) const;            ///< Whether the camera exposes this control at all.
    bool isControlWritable(types::ControlType type) const;     ///< Whether it can be set (e.g. TEMPERATURE cannot).
    bool isControlAutoCapable(types::ControlType type) const;  ///< Whether the camera can drive it automatically.
    bool hasCooler() const;                                    ///< Whether a regulated cooler is fitted.
    bool isColour() const;                                     ///< Whether the sensor has a colour-filter mosaic.

    /**
     * @brief The speed the USB link is actually negotiated at.
     * @return USB3 only when both the camera and the host port are USB3; USB2 otherwise, including when not connected.
     * @note Worth surfacing prominently: a USB3 camera in a USB2 port works but delivers a fraction of its rated frame
     *       rate, and nothing else reports it. Pair it with @ref isLinkFullSpeed and @ref getDroppedFrames when
     *       diagnosing throughput.
     */
    types::UsbLinkSpeed getUsbLinkSpeed() const;

    /// @brief Whether the link runs at the camera's full rated speed. False for a USB3 camera on a USB2 host.
    bool isLinkFullSpeed() const;
    bool supportsFormat(types::ImageFormat format) const;      ///< Whether it can deliver this pixel format.
    bool supportsBin(int bin) const;                           ///< Whether it accepts this binning factor.

    // -- Controls --

    /// @brief Read one control's value and auto flag. @return UNSUPPORTED_CAPABILITY if this camera lacks it.
    types::OperationResult getControl(types::ControlType type, types::ControlValue& value);

    /**
     * @brief Write one control's value and auto flag.
     * @return UNSUPPORTED_CAPABILITY if this camera lacks the control or it is read-only; INVALID_PARAMETER if the
     *         value is outside the control's discovered limits.
     * @note The range is checked against the discovered capabilities first, because the SDK silently clamps an
     *       out-of-range value and reports success -- which would leave the caller believing a write it never got.
     */
    types::OperationResult doSetControl(types::ControlType type, const types::ControlValue& value);

    // Typed conveniences. Provided ONLY where the SDK's unit is not the natural one, so each is the single place its
    // conversion happens and no consumer has to remember it. Everything else goes through the generic pair above.

    /// @brief Set the exposure time. @note The SDK's unit for this control is microseconds; this takes a duration.
    types::OperationResult doSetExposure(std::chrono::microseconds exposure);

    /// @brief Read the exposure time, converted from the SDK's microseconds.
    types::OperationResult getExposure(std::chrono::microseconds& exposure);

    /**
     * @brief Read the sensor temperature in degrees Celsius.
     * @return UNSUPPORTED_CAPABILITY if this camera has no temperature sensor.
     * @note The SDK reports this control in TENTHS of a degree; the division happens here, once. Note the asymmetry
     *       with @ref doSetCoolerTarget, which the SDK takes in WHOLE degrees -- which is exactly why temperature is
     *       not handled by one generic conversion.
     */
    types::OperationResult getSensorTemperature(double& celsius);

    /// @brief Set the image flip applied by the camera. @note Wraps the raw FLIP control value as a typed enum.
    types::OperationResult doSetFlip(types::FlipMode mode);

    /// @brief Read the image flip applied by the camera.
    types::OperationResult getFlip(types::FlipMode& mode);

    // USB traffic. The share of the bus this camera is allowed to occupy is the main lever over dropped frames, and it
    // is the one a capture application puts in front of the user, so it gets named accessors rather than living only
    // behind the generic control API. The slider bounds come from the usual place:
    // getControlCaps(ControlType::BANDWIDTH_OVERLOAD, caps) yields min, max and default for this camera.

    /**
     * @brief Set the share of USB bandwidth this camera may occupy.
     * @param percent Bandwidth share. Valid range is per-camera; read it from the control's capabilities.
     * @param automatic Let the camera manage the share itself, as the vendor software's "Auto" box does. When true,
     *        @p percent is the starting point the camera adjusts from.
     * @return UNSUPPORTED_CAPABILITY if the camera has no such control, or if @p automatic is asked of a camera that
     *         cannot drive it automatically; INVALID_PARAMETER if @p percent is outside the camera's range.
     * @note LOWERING this is the first remedy for dropped frames, especially on a degraded USB2 link; raising it trades
     *       bus headroom for frame rate. It does not change image quality, unlike @ref doSetHighSpeedMode.
     */
    types::OperationResult doSetUsbBandwidth(int percent, bool automatic = false);

    /// @brief Read the current USB bandwidth share and whether the camera is managing it automatically.
    types::OperationResult getUsbBandwidth(int& percent, bool& automatic);

    /**
     * @brief Enable the sensor's high-speed readout mode.
     * @note Raises the achievable frame rate at the cost of image quality (the vendor trades ADC bit depth for speed).
     *       Distinct from @ref doSetUsbBandwidth, which only changes how much of the bus the camera may use.
     */
    types::OperationResult doSetHighSpeedMode(bool enable);

    /// @brief Whether the sensor's high-speed readout mode is enabled.
    types::OperationResult isHighSpeedMode(bool& enabled);

    // Cooler control, for the models that have one (check @ref hasCooler first). On a camera without a cooler every
    // one of these returns UNSUPPORTED_CAPABILITY rather than pretending to work.

    /// @brief Turn the thermoelectric cooler on or off.
    types::OperationResult doEnableCooler(bool enable);

    /// @brief Whether the cooler is currently enabled.
    types::OperationResult isCoolerEnabled(bool& enabled);

    /// @brief Set the cooler set point. @warning In WHOLE degrees Celsius -- NOT the tenths getSensorTemperature uses.
    types::OperationResult doSetCoolerTarget(int celsius);

    /// @brief Read the cooler set point, in whole degrees Celsius.
    types::OperationResult getCoolerTarget(int& celsius);

    /// @brief Read the cooler's current duty cycle, as a percentage.
    types::OperationResult getCoolerPower(int& percent);

    // -- Region of interest and image format --

    /// @brief Read the live ROI geometry and pixel format.
    types::OperationResult getRoiFormat(types::RoiFormat& roi) const;

    /**
     * @brief Set the ROI geometry, pixel format and origin together.
     * @param roi Geometry and format. Dimensions are post-binning; width must be a multiple of 8 and height of 2.
     * @param pos Origin on the sensor, in binned pixels.
     * @return INVALID_SEQUENCE if an acquisition is running; UNSUPPORTED_CAPABILITY if this camera cannot deliver the
     *         format or the binning factor; INVALID_PARAMETER if the geometry is misaligned or does not fit the sensor.
     * @note Geometry and origin are set in ONE call because they are not independent: changing the format recentres the
     *       origin on the sensor, so setting them separately would leave the origin wherever the SDK moved it.
     */
    types::OperationResult doSetRoi(const types::RoiFormat& roi, const types::RoiPosition& pos);

    /// @brief Read the live ROI origin, in binned pixel coordinates.
    types::OperationResult getRoiPosition(types::RoiPosition& pos) const;

    /// @brief Move the ROI origin. Legal while streaming, since it does not change the frame geometry.
    types::OperationResult doSetRoiPosition(const types::RoiPosition& pos);

    /**
     * @brief Configure the ROI to the full sensor at a given binning and pixel format.
     * @return INVALID_SEQUENCE if an acquisition is running; UNSUPPORTED_CAPABILITY if the camera does not support the
     *         format or the binning factor.
     * @note Encodes the full-frame geometry once: the sensor size divided by the binning factor, aligned down to the
     *       vendor's width-multiple-of-8 and height-multiple-of-2 rules, at origin (0,0).
     */
    types::OperationResult doSetFullFrameRoi(types::ImageFormat format, int bin);

    // -- Acquisition, common --

    /// @brief What the camera is currently doing, as tracked by the library.
    types::AcquisitionMode getAcquisitionMode() const;

    // -- Acquisition: video streaming --

    /// @brief Begin streaming. @return INVALID_SEQUENCE if a snapshot exposure is in progress.
    types::OperationResult doStartVideoCapture();

    /// @brief Stop streaming, and the frame worker with it. @return INVALID_SEQUENCE if the camera was not streaming.
    types::OperationResult doStopVideoCapture();

    /**
     * @brief Retrieve the next streamed frame, blocking until one arrives or @p timeout elapses.
     * @param[out] frame Filled with the image and its geometry. Reuse one frame across a loop: its buffer is
     *                   reallocated only when the geometry changes, so a steady stream costs no per-frame allocation.
     * @param timeout How long to wait. Clamped to a bounded maximum; "wait forever" is never passed to the SDK.
     * @return OPERATION_OK on success; INVALID_SEQUENCE if not streaming; OPERATION_TIMEOUT if no frame arrived.
     * @note BLOCKS. This is the caller-driven style; see @ref setNewFrameCb for the callback-driven one.
     */
    types::OperationResult doGetVideoFrame(types::Frame& frame, std::chrono::milliseconds timeout);

    /// @brief Frames dropped in the current streaming session. @note Read it before stopping capture.
    types::OperationResult getDroppedFrames(int& dropped);

    // -- Acquisition: single frame (snapshot) --

    /**
     * @brief Begin a single exposure. Non-blocking; poll @ref getExposureState, then call @ref doGetExposureFrame.
     * @param dark True to keep a mechanical shutter closed; ignored on a camera without one.
     * @return INVALID_SEQUENCE if streaming is running or an exposure is already in progress.
     * @warning The camera's automatic exposure and gain are INERT on this path: they only act while streaming. Setting
     *          a control to auto and then taking a snapshot yields neither auto behaviour nor an error.
     */
    types::OperationResult doStartExposure(bool dark = false);

    /// @brief Cancel an exposure in progress. @return INVALID_SEQUENCE if no exposure was in progress.
    types::OperationResult doStopExposure();

    /// @brief Poll a single exposure's progress. @return READ_FAILED if the state could not be read.
    types::OperationResult getExposureState(types::ExposureState& state);

    /**
     * @brief Download the frame produced by a completed exposure.
     * @return OPERATION_OK on success; INVALID_SEQUENCE if no exposure is in progress; ACQUISITION_FAILED if the sensor
     *         reported the exposure failed; OPERATION_TIMEOUT if the exposure has not finished yet.
     */
    types::OperationResult doGetExposureFrame(types::Frame& frame);

    /**
     * @brief Take one frame: start an exposure, wait for it, download it and return the camera to idle.
     * @param timeout Maximum total time to wait for the exposure to complete.
     * @param dark True to keep a mechanical shutter closed; ignored on a camera without one.
     * @return OPERATION_OK on success; OPERATION_TIMEOUT if the exposure did not finish in time; ACQUISITION_FAILED if
     *         the sensor reported a failure; INVALID_SEQUENCE if an acquisition was already running.
     * @note BLOCKS for up to @p timeout. A convenience over the four-step snapshot sequence; it always leaves the
     *       camera idle, including on failure.
     */
    types::OperationResult doCaptureSingleFrame(types::Frame& frame,
                                                std::chrono::milliseconds timeout,
                                                bool dark = false);

    // -- Callback-driven frame acquisition --
    //
    // The alternative to owning the acquisition loop yourself: the library runs the loop on a worker thread and hands
    // each frame to a callback, so a consumer can process frames independently -- write them out, push them to a
    // display, feed a pipeline -- without blocking its own thread on doGetVideoFrame().
    //
    // Both styles drive the SAME acquisition path and the same guards, so pick whichever suits the application; they
    // are simply two ways in. Do not mix them on one camera at the same time: two consumers pulling from one frame
    // stream would each see an arbitrary half of it.

    /**
     * @brief Register the frame callback.
     * @warning It only fires while startFrameAcquisition() is active. The frame is a reference into the worker's
     *          reusable buffer and is ONLY valid for the duration of the call: copy what you need before returning.
     * @warning Keep the callback short. It runs between captures, so time spent in it is time not spent retrieving,
     *          and a slow callback shows up as dropped frames. Hand heavy work to your own queue.
     */
    types::OperationResult setNewFrameCb(NewFrameCb cb);

    /**
     * @brief Start delivering frames to the callback on a background worker.
     * @param timeout How long each capture waits for a frame before reporting a timeout to the callback. Clamped to
     *        the same bounds the capture path enforces.
     * @return OPERATION_OK; NOT_CONNECTED; INVALID_SEQUENCE if the camera is not streaming; WORKER_ALREADY_RUNNING.
     * @note Requires video capture to be running: call doStartVideoCapture() first. Deliberately NOT implicit, so the
     *       acquisition state machine has exactly one owner and starting a worker never silently reconfigures hardware.
     */
    types::OperationResult startFrameAcquisition(std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));

    /**
     * @brief Stop the background frame worker. Bounded; never blocks indefinitely.
     * @note May take up to one frame timeout to return: a capture already inside the vendor SDK cannot be cancelled.
     *       Also performed automatically by doStopVideoCapture(), doDisconnect() and the destructor.
     */
    types::OperationResult stopFrameAcquisition();

    bool isFrameAcquisitionRunning() const;          ///< Whether the background frame worker is currently running.
    std::uint64_t getAcquiredFrameCount() const;     ///< Frames delivered OK since the last startFrameAcquisition().
    std::uint64_t getFailedFrameCount() const;       ///< Captures that failed (typically timeouts) since that start.

    // -- Telemetry --

    /// @brief Read a telemetry snapshot of the camera.
    types::OperationResult getDeviceStatus(types::CameraStatus& status);

    /// @brief Register the telemetry callback. @warning It only fires while startStatusPolling() is active.
    types::OperationResult setNewStatusCb(NewStatusCb cb);
    types::OperationResult startStatusPolling();   ///< Start the background worker that drives the status callback.
    types::OperationResult stopStatusPolling();    ///< Stop the background status worker (bounded; never blocks).
    bool isStatusPollingRunning() const;           ///< Whether the status worker is currently running.

    // -- Discovery --

    /// @brief Enumerate every connected ASI camera. Opens none, so a consumer can choose before connecting.
    static types::OperationResult getDeviceList(types::CameraDescriptorList& list);

    /// @brief Enumerate the connected ASI cameras of one model, e.g. "ASI224MC". Opens none.
    static types::OperationResult getDeviceList(const std::string& model, types::CameraDescriptorList& list);

private:

    /// @brief OPERATION_OK if connected, otherwise NOT_CONNECTED. Caller must hold @ref state_mtx_.
    types::OperationResult checkConnectedLocked() const;

    /// @brief OPERATION_OK if the camera is idle, otherwise INVALID_SEQUENCE. Caller must hold @ref state_mtx_.
    types::OperationResult checkIdleLocked() const;

    /// @brief Look up a discovered control's capabilities. Caller must hold @ref state_mtx_.
    /// @return False if this camera does not expose @p type.
    bool findCapsLocked(types::ControlType type, types::ControlCaps& out_caps) const;

    /// @brief Shared body of the capability predicates: connected AND the control present, under the lock.
    bool queryCaps(types::ControlType type, types::ControlCaps& out_caps) const;

    /// @brief Read one whole-number control, mapping absence to UNSUPPORTED_CAPABILITY.
    types::OperationResult getIntControl(types::ControlType type, int& out_value);

    /// @brief Write one whole-number control, mapping absence to UNSUPPORTED_CAPABILITY.
    types::OperationResult setIntControl(types::ControlType type, int value);

    /// @brief Fill a telemetry snapshot. Used by both getDeviceStatus() and the poller's producer.
    types::OperationResult fillStatus(types::CameraStatus& status);

    /// @brief Release the camera without touching the workers. Caller must NOT hold @ref state_mtx_.
    void releaseConnection() noexcept;

    types::CameraId id_;
    std::string expected_model_;                       ///< Required model, or empty to accept any.
    mutable std::mutex state_mtx_;                     ///< Guards the connection/acquisition state and the caches.
    bool connected_;                                   ///< Whether this object currently holds an open camera.
    bool i_own_claim_;                                 ///< True if THIS object claimed the camera (vs aliasing it).
    int telemetry_rate_ms_;                            ///< Poll interval from the DeviceConfig supplied to doConnect().
    types::AcquisitionMode acq_mode_;                  ///< Library-tracked acquisition state; the SDK has no query.
    types::CameraDescriptor descriptor_;               ///< Cached at connect: the camera's fixed properties.
    types::ControlCapsList control_caps_;              ///< Cached at connect: measured to be stable while streaming.
    asi::AsiCameraController ctrl_;
    mutable std::mutex cb_mtx_;
    NewStatusCb cb_;
    NewFrameCb frame_cb_;
    // Both workers are declared LAST so they are destroyed FIRST: each is stopped before ctrl_ and the caches it uses
    // go away. The frame pump is listed after the poller so it is torn down first, since it is the one that can be
    // blocked inside a vendor capture.
    StatusPoller<types::CameraStatus> poller_;
    FramePump pump_;
};

using AsiCameraPtr = std::shared_ptr<AsiCamera>;

// ---------------------------------------------------------------------------------------------------------------------

} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
