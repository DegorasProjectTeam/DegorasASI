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
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// PROJECT INCLUDES
#include "DegorasASI/Global/degorasasi_export.h"


// NAMESPACES
namespace dpasi
{
namespace types
{

// ---------------------------------------------------------------------------------------------------------------------
// TYPE ALIASES

/**
 * @brief Opaque ASI camera identifier, as assigned by the SDK.
 * @note A scoped enumeration with no enumerators is used as a zero-overhead strong type: it prevents an arbitrary int
 *       being passed where a camera is expected, while still comparing and ordering like its underlying type. Construct
 *       it from a discovery result (@ref CameraDescriptor::id) or explicitly, e.g. CameraId{0}.
 * @warning This is the SDK's addressing key, NOT a stable hardware identity: it is derived from enumeration order and
 *          may change when cameras are attached or detached. The stable identity is the hardware serial number, which
 *          the SDK only reveals once a camera is open (see @ref CameraInfo::serial_number).
 */
enum class CameraId : int
{
};

using CameraSN = std::string;              ///< ZWO camera hardware serial number, rendered as 16 hexadecimal digits.
using PixelByte = std::uint8_t;            ///< One byte of image data.
using ControlRaw = std::int64_t;           ///< Raw control value, widened so it is platform-invariant (see note below).

// The ASI C API types every control value as `long`, which is 4 bytes on Windows (LLP64) and 8 bytes on Linux/macOS
// (LP64). Widening to a fixed 64-bit type at the public boundary keeps this library's ABI identical on every platform
// and confines the vendor's `long` to the Layer-2 adapter translation unit.

// ---------------------------------------------------------------------------------------------------------------------
// ENUMERATIONS

/**
 * @brief High-level, hardware-agnostic result of a library operation.
 * @note Every enumerator has an explicit, stable value so it is safe to log or compare across versions.
 *       When the category is ZWO_INTERNAL_ERROR, the raw vendor code is preserved in DeviceError::asi_code.
 */
enum class OperationResult : std::uint8_t
{
    OPERATION_OK           = 0,   ///< Operation completed successfully.
    NOT_CONNECTED          = 1,   ///< The camera is not connected.
    DEVICE_NOT_FOUND       = 2,   ///< The requested camera was not found during discovery.
    ALREADY_CONNECTED      = 3,   ///< This object already owns an open connection (idempotent re-connect).
    CAMERA_IN_USE          = 4,   ///< Another live object already owns this camera.
    OPERATION_TIMEOUT      = 5,   ///< A bounded wait expired before its condition was met.
    ZWO_INTERNAL_ERROR     = 6,   ///< A ZWO/ASI call failed; the raw code is in DeviceError::asi_code.
    WORKER_ALREADY_RUNNING = 7,   ///< The status-polling worker was already running.
    WORKER_NOT_RUNNING     = 8,   ///< The status-polling worker was not running.
    WORKER_START_ERROR     = 9,   ///< The status-polling worker thread could not be started.
    READ_FAILED            = 10,  ///< A read failed or comms were lost; the value is NOT valid.
    INVALID_PARAMETER      = 11,  ///< A parameter was rejected by the library before reaching the SDK.
    UNSUPPORTED_CAPABILITY = 12,  ///< This camera does not expose the requested control or image format.
    INVALID_SEQUENCE       = 13,  ///< The operation is illegal in the camera's current acquisition state.
    BUFFER_TOO_SMALL       = 14,  ///< The destination buffer cannot hold one full frame.
    ACQUISITION_FAILED     = 15   ///< The sensor reported a failed exposure; start a new one.
};

/**
 * @brief Human-readable name for an OperationResult.
 * @param r The result code to stringify.
 * @return The enumerator name, or "UNKNOWN_OPERATION_RESULT" for an unrecognised value.
 */
DEGORASASI_EXPORT std::string toString(OperationResult r);

/**
 * @brief Pixel layout of a frame, matching the ASI SDK's output formats.
 * @note Values coincide with the vendor's ASI_IMG_TYPE ordinals so the mapping is a pure cast, but callers must never
 *       rely on that: use @ref toType / @ref imageFormatFromType.
 * @warning The vendor documents neither the RGB24 channel order nor, for a sensor narrower than 16 bits, whether RAW16
 *          words are MSB- or LSB-justified. This library therefore never reinterprets pixels: a @ref Frame carries its
 *          format and the raw bytes exactly as the sensor delivered them, and interpretation is the consumer's choice.
 */
enum class ImageFormat : std::uint8_t
{
    RAW8  = 0,   ///< 8-bit raw sensor data (Bayer-mosaiced on a colour camera). 1 byte per pixel.
    RGB24 = 1,   ///< 24-bit colour, debayered by the SDK. 3 bytes per pixel.
    RAW16 = 2,   ///< 16-bit raw sensor data (Bayer-mosaiced on a colour camera). 2 bytes per pixel.
    Y8    = 3    ///< 8-bit monochrome (luminance). 1 byte per pixel.
};

/// @brief Human-readable name for an ImageFormat.
DEGORASASI_EXPORT std::string toString(ImageFormat format);

/**
 * @brief Speed the camera's USB link is actually running at.
 * @note This is the NEGOTIATED speed, not a capability: a USB3 camera in a USB2 port runs at USB2 and delivers a small
 *       fraction of its rated frame rate, with nothing but this to say so. The vendor's own capture software shows it
 *       in its title bar for the same reason.
 */
enum class UsbLinkSpeed : std::uint8_t
{
    USB2 = 0,   ///< High-Speed link. Either the camera or the port is USB2.
    USB3 = 1    ///< SuperSpeed link. Both the camera and the port are USB3.
};

/// @brief Human-readable name for a UsbLinkSpeed, e.g. "USB3.0".
DEGORASASI_EXPORT std::string toString(UsbLinkSpeed speed);

/// Colour-filter mosaic order of the sensor's top-left 2x2 cell.
enum class BayerPattern : std::uint8_t
{
    RG = 0,   ///< Red, Green / Green, Blue.
    BG = 1,   ///< Blue, Green / Green, Red.
    GR = 2,   ///< Green, Red / Blue, Green.
    GB = 3    ///< Green, Blue / Red, Green.
};

/// @brief Human-readable name for a BayerPattern.
DEGORASASI_EXPORT std::string toString(BayerPattern pattern);

/**
 * @brief Camera control (adjustable parameter) selector.
 * @note Values coincide with the vendor's ASI_CONTROL_TYPE ordinals. A camera exposes only a SUBSET of these: the set
 *       must be discovered per camera (see @ref ControlCaps), never assumed. For example the ASI224MC exposes 14 of
 *       them and has no GAMMA control at all.
 */
enum class ControlType : std::uint8_t
{
    GAIN                    = 0,   ///< Sensor gain, in the camera's own gain units.
    EXPOSURE                = 1,   ///< Exposure time in MICROSECONDS.
    GAMMA                   = 2,   ///< Gamma correction.
    WB_RED                  = 3,   ///< White balance, red channel.
    WB_BLUE                 = 4,   ///< White balance, blue channel.
    OFFSET                  = 5,   ///< Black level, lifted so read noise is not clipped at zero (was BRIGHTNESS).
    BANDWIDTH_OVERLOAD      = 6,   ///< USB bandwidth usage, as a percentage. Lower it if frames are dropped.
    OVERCLOCK               = 7,   ///< Push the sensor clock past its rated speed: more noise, not always supported.
    TEMPERATURE             = 8,   ///< Sensor temperature in TENTHS of a degree Celsius (usually read-only).
    FLIP                    = 9,   ///< Image flip; the value is a @ref FlipMode.
    AUTO_MAX_GAIN           = 10,  ///< Auto-exposure algorithm's gain ceiling.
    AUTO_MAX_EXPOSURE       = 11,  ///< Auto-exposure algorithm's exposure ceiling in MILLISECONDS.
    AUTO_TARGET_BRIGHTNESS  = 12,  ///< Auto-exposure algorithm's target brightness.
    HARDWARE_BIN            = 13,  ///< Bin in the readout, not in software: faster, less read noise, fewer formats.
    HIGH_SPEED_MODE         = 14,  ///< Faster readout at reduced bit depth (typically 8-bit instead of 12).
    COOLER_POWER_PERC       = 15,  ///< Cooler duty cycle, as a percentage (cooled cameras only).
    TARGET_TEMPERATURE      = 16,  ///< Cooler set point in WHOLE degrees Celsius (cooled cameras only).
    COOLER_ON               = 17,  ///< Enable the thermoelectric cooler (cooled cameras only).
    MONO_BIN                = 18,  ///< Bin a colour sensor as if mono: no Bayer grid artefact, no colour.
    FAN_ON                  = 19,  ///< Enable the cooling fan.
    PATTERN_ADJUST          = 20,  ///< Fixed-pattern-noise adjustment.
    ANTI_DEW_HEATER         = 21,  ///< Enable the window anti-dew heater.
    FAN_ADJUST              = 22,  ///< Fan speed adjustment.
    PWR_LED_BRIGHTNESS      = 23,  ///< Power-LED brightness.
    USB_HUB_RESET           = 24,  ///< Reset the internal USB hub.
    GPS_SUPPORT             = 25,  ///< Whether the GPS module is present/enabled (GPS cameras only).
    GPS_START_LINE          = 26,  ///< First sensor line stamped with GPS time (GPS cameras only).
    GPS_END_LINE            = 27,  ///< Last sensor line stamped with GPS time (GPS cameras only).
    ROLLING_INTERVAL        = 28   ///< Rolling-shutter line-to-line delay in MICROSECONDS: the skew of one frame.
};

// The vendor header additionally defines ASI_BRIGHTNESS and ASI_AUTO_MAX_BRIGHTNESS as preprocessor ALIASES of
// ASI_OFFSET and ASI_AUTO_TARGET_BRIGHTNESS respectively, not as distinct controls. Only the canonical names appear
// above: listing both would make any switch over this enum a duplicate-case compile error.
//
// AUTO_MAX_EXPOSURE is documented as microseconds in the vendor header but is in fact MILLISECONDS: the control
// reports itself as "AutoExpMaxExpMS" with a 1..60000 range on real hardware.

/// @brief Human-readable name for a ControlType.
DEGORASASI_EXPORT std::string toString(ControlType type);

/// Image flip applied by the camera before delivery.
enum class FlipMode : std::uint8_t
{
    NONE       = 0,   ///< No flip (original orientation).
    HORIZONTAL = 1,   ///< Flip horizontally.
    VERTICAL   = 2,   ///< Flip vertically.
    BOTH       = 3    ///< Flip both horizontally and vertically.
};

/// Progress of a single-frame (snapshot) exposure.
enum class ExposureState : std::uint8_t
{
    IDLE    = 0,   ///< No exposure in progress; a new one may be started.
    WORKING = 1,   ///< Exposing.
    SUCCESS = 2,   ///< Exposure finished; the frame is ready to download.
    FAILED  = 3    ///< Exposure failed; it must be started again.
};

/// @brief Human-readable name for an ExposureState.
DEGORASASI_EXPORT std::string toString(ExposureState state);

/**
 * @brief The library's view of what a camera is currently doing.
 * @note This is NOT an SDK concept: the ASI SDK exposes no acquisition-state query, and it accepts some calls that are
 *       illegal for the state it is actually in (notably it accepts a ROI/format change during streaming, silently
 *       invalidating the frame geometry a caller has already sized a buffer for). The library therefore tracks the
 *       state itself and rejects those transitions with OperationResult::INVALID_SEQUENCE.
 */
enum class AcquisitionMode : std::uint8_t
{
    IDLE     = 0,   ///< Not acquiring; configuration changes are allowed.
    VIDEO    = 1,   ///< Streaming; frames are retrieved with the video path.
    SNAPSHOT = 2    ///< A single-frame exposure is in progress.
};

/// @brief Human-readable name for an AcquisitionMode.
DEGORASASI_EXPORT std::string toString(AcquisitionMode mode);

// ---------------------------------------------------------------------------------------------------------------------
// DATA STRUCTURES

/**
 * @brief Region-of-interest geometry and pixel format.
 * @note @ref width and @ref height are the dimensions AFTER binning, matching the SDK. The vendor requires
 *       width % 8 == 0 and height % 2 == 0; the library validates this before calling the SDK (verified on hardware:
 *       a violation is rejected with ASI_ERROR_INVALID_SIZE and leaves the previous ROI intact).
 */
struct DEGORASASI_EXPORT RoiFormat
{
    /// @brief Establishes an empty, unbinned RAW8 geometry: no ROI is implied until one is read from a camera.
    RoiFormat();

    /// @brief Serialise the format to a compact, single-line JSON string.
    std::string toJsonStr() const;

    /**
     * @brief Serialise the format to a JSON string, pretty-printed when @p pretty is true.
     * @note The compact form is a separate overload rather than a defaulted argument, here and everywhere else in this
     *       header: a default argument in a shared library is compiled into the CALLER, so changing it later would
     *       need every client rebuilt, whereas an overload lives in the library and ships with it.
     */
    std::string toJsonStr(bool pretty) const;

    /// @brief Parse a format from a JSON string produced by toJsonStr(); missing fields keep their defaults.
    static RoiFormat fromJsonStr(const std::string& json);

    int width;            ///< ROI width in pixels, after binning. Must be a multiple of 8.
    int height;           ///< ROI height in pixels, after binning. Must be a multiple of 2.
    int bin;              ///< Binning factor (1 = no binning). Must be one the camera supports.
    ImageFormat format;   ///< Pixel layout of the delivered frame.
};

/**
 * @brief Top-left origin of the region of interest, in binned pixel coordinates.
 * @warning Changing the ROI format RECENTRES the origin on the sensor (verified on hardware: a 1304x976 -> 640x480
 *          format change moved the origin from (0,0) to (332,248)). The library therefore always re-applies and reads
 *          back the intended position after a format change.
 */
struct DEGORASASI_EXPORT RoiPosition
{
    /// @brief Establishes the origin at the sensor's top-left corner, (0, 0).
    RoiPosition();

    /// @brief Serialise the position to a compact, single-line JSON string.
    std::string toJsonStr() const;

    /// @brief Serialise the position to a JSON string, pretty-printed when @p pretty is true.
    std::string toJsonStr(bool pretty) const;

    /// @brief Parse a position from a JSON string produced by toJsonStr(); missing fields keep their defaults.
    static RoiPosition fromJsonStr(const std::string& json);

    int start_x;   ///< Horizontal origin in binned pixels.
    int start_y;   ///< Vertical origin in binned pixels.
};

/**
 * @brief Discovered capabilities and limits of one camera control.
 * @note Obtained per camera; the set of available controls varies by model and must never be assumed.
 */
struct DEGORASASI_EXPORT ControlCaps
{
    /// @brief Establishes an empty, non-writable description of the GAIN control, with a zero range.
    ControlCaps();

    /// @brief Serialise the capabilities to a compact, single-line JSON string.
    std::string toJsonStr() const;

    /// @brief Serialise the capabilities to a JSON string, pretty-printed when @p pretty is true.
    std::string toJsonStr(bool pretty) const;

    /// @brief Parse capabilities from a JSON string produced by toJsonStr(); missing fields keep their defaults.
    static ControlCaps fromJsonStr(const std::string& json);

    ControlType type;          ///< Which control this describes.
    std::string name;          ///< Vendor's short name, e.g. "Exposure".
    std::string description;   ///< Vendor's human-readable description.
    ControlRaw min_value;      ///< Smallest accepted value.
    ControlRaw max_value;      ///< Largest accepted value.
    ControlRaw default_value;  ///< Value the camera powers up with.
    bool is_auto_supported;    ///< Whether the camera can drive this control automatically.
    bool is_writable;          ///< Whether the control can be set at all (e.g. TEMPERATURE is read-only).
};

using ControlCapsList = std::vector<ControlCaps>;   ///< All controls a camera exposes.
using ControlTypeList = std::vector<ControlType>;   ///< A selection of controls, for operations that take several.

/// Current value of one camera control.
struct DEGORASASI_EXPORT ControlValue
{
    /// @brief Establishes a zero value that is not being driven automatically.
    ControlValue();

    /// @brief Serialise the value to a compact, single-line JSON string.
    std::string toJsonStr() const;

    /// @brief Serialise the value to a JSON string, pretty-printed when @p pretty is true.
    std::string toJsonStr(bool pretty) const;

    /// @brief Parse a value from a JSON string produced by toJsonStr(); missing fields keep their defaults.
    static ControlValue fromJsonStr(const std::string& json);

    ControlRaw value;   ///< The control's raw value, in the control's own units.
    bool is_auto;       ///< Whether the camera is currently driving this control automatically.
};

/**
 * @brief Identity and fixed capabilities of a camera, obtainable WITHOUT opening it.
 * @note This is what discovery returns, so a consumer can choose a camera before connecting to it.
 */
struct DEGORASASI_EXPORT CameraDescriptor
{
    /// @brief Establishes an empty descriptor: camera 0, no name, zeroed sensor geometry and no declared capability.
    CameraDescriptor();

    /// @brief Serialise the descriptor to a compact, single-line JSON string.
    std::string toJsonStr() const;

    /// @brief Serialise the descriptor to a JSON string, pretty-printed when @p pretty is true.
    std::string toJsonStr(bool pretty) const;

    /// @brief Parse a descriptor from a JSON string produced by toJsonStr(); missing fields keep their defaults.
    static CameraDescriptor fromJsonStr(const std::string& json);

    /// @brief Whether the camera supports @p format.
    bool supportsFormat(ImageFormat format) const;

    /// @brief Whether the camera supports the @p bin binning factor.
    bool supportsBin(int bin) const;

    /**
     * @brief Whether the camera's USB link is running at its full rated speed.
     * @return False when a USB3 camera is attached to a USB2 host, which silently degrades throughput.
     * @note The only way to detect a degraded link; worth reporting alongside dropped-frame diagnostics.
     */
    bool isLinkFullSpeed() const;

    /// @brief The speed the link is actually negotiated at: USB3 only when BOTH the camera and the host are USB3.
    UsbLinkSpeed linkSpeed() const;

    CameraId id;                                 ///< SDK addressing key. Volatile across attach/detach.
    std::string name;                            ///< Vendor model string, e.g. "ZWO ASI224MC".
    int max_width;                               ///< Sensor width in pixels (unbinned).
    int max_height;                              ///< Sensor height in pixels (unbinned).
    double pixel_size_um;                        ///< Physical pixel pitch in micrometres.
    int bit_depth;                               ///< Sensor ADC bit depth.
    double electrons_per_adu;                    ///< Sensor gain in electrons per ADU.
    bool is_colour;                              ///< Whether the sensor has a colour-filter mosaic.
    BayerPattern bayer_pattern;                  ///< Mosaic order; meaningful only when @ref is_colour.
    std::vector<int> supported_bins;             ///< Binning factors this camera accepts.
    std::vector<ImageFormat> supported_formats;  ///< Pixel formats this camera can deliver.
    bool has_mechanical_shutter;                 ///< Whether a mechanical shutter is fitted.
    bool has_st4_port;                           ///< Whether the ST4 autoguider port is fitted.
    bool is_cooled;                              ///< Whether a regulated thermoelectric cooler is fitted.
    bool is_usb3_camera;                         ///< Whether the camera itself is USB3-capable.
    bool is_usb3_host;                           ///< Whether the host port it is attached to is USB3.
    bool is_trigger_camera;                      ///< Whether hardware/software triggering is supported.
};

using CameraDescriptorList = std::vector<CameraDescriptor>;   ///< Result of a discovery sweep.

/**
 * @brief One acquired image and the geometry it was acquired with.
 *
 * @details Owns its pixel bytes. The library sizes @ref data itself from the camera's live ROI, so a caller can never
 *          under-size the destination: the vendor's frame-retrieval calls perform no bounds check and are documented to
 *          crash on a short buffer.
 *
 * @note Reuse one Frame across a streaming loop: the library only reallocates when the geometry changes, so a steady
 *       stream costs no per-frame allocation.
 * @warning The bytes are exactly what the sensor delivered, in @ref format. This library does not debayer, shift or
 *          reorder them, because the vendor documents neither the RGB24 channel order nor the RAW16 bit justification.
 */
struct DEGORASASI_EXPORT Frame
{
    /// @brief Establishes an empty frame: no pixels, unbinned RAW8 geometry at the origin and sequence 0.
    Frame();

    /// @brief True when the frame holds no image data.
    bool empty() const;

    /// @brief Number of image bytes the current geometry requires.
    std::size_t expectedBytes() const;

    /// @brief Serialise the frame's METADATA to a compact, single-line JSON string (never the pixel bytes).
    std::string toJsonStr() const;

    /// @brief Serialise the frame's METADATA (never the pixel bytes), pretty-printed when @p pretty is true.
    std::string toJsonStr(bool pretty) const;

    ImageFormat format;                                  ///< Pixel layout of @ref data.
    int width;                                           ///< Frame width in pixels.
    int height;                                          ///< Frame height in pixels.
    int bin;                                             ///< Binning factor the frame was acquired with.

    /// @brief Where on the sensor the frame's first pixel came from, in post-binning coordinates.
    /// @note Carried so a frame is self-describing: without it a windowed capture cannot be matched against a dark or
    ///       a flat, and -- on a colour camera -- an ODD origin shifts the Bayer mosaic by one photosite, which
    ///       silently mis-colours anything that demosaics the frame. The vendor accepts an odd origin without
    ///       complaint, so the offset has to travel with the pixels rather than be assumed to be zero.
    int start_x;
    int start_y;                                         ///< See @ref start_x.
    std::uint64_t sequence;                              ///< Monotonic counter, incremented per delivered frame.
    std::chrono::system_clock::time_point timestamp;     ///< Host time at which the frame was retrieved.
    std::vector<PixelByte> data;                         ///< The image bytes.
};

/**
 * @brief Telemetry snapshot of a camera: what CHANGES while it runs.
 * @note The fixed properties live in @ref CameraDescriptor and the adjustable settings are read through the control
 *       API; this carries only the moving parts. It is what the background telemetry poller produces each interval and
 *       what getDeviceStatus() returns on demand.
 * @warning A value that could not be read is FLAGGED rather than defaulted: @ref temperature_valid is false when the
 *          sensor temperature could not be read, so a reading of 0 degrees is never mistaken for a real one.
 */
struct DEGORASASI_EXPORT CameraStatus
{
    /// @brief Establishes a disconnected, idle snapshot with no valid temperature reading and a default ROI.
    CameraStatus();

    /// @brief Serialise the status to a compact, single-line JSON string.
    std::string toJsonStr() const;

    /// @brief Serialise the status to a JSON string, pretty-printed when @p pretty is true.
    std::string toJsonStr(bool pretty) const;

    /// @brief Parse a status from a JSON string produced by toJsonStr(); missing fields keep their defaults.
    static CameraStatus fromJsonStr(const std::string& json);

    CameraId id;                    ///< Identifier of the camera this snapshot describes.
    bool connected;                 ///< Whether the camera was connected when the snapshot was taken.
    AcquisitionMode acq_mode;       ///< What the camera was doing.
    ExposureState exposure_state;   ///< Progress of a single-frame exposure; meaningful in SNAPSHOT mode.
    bool temperature_valid;         ///< Whether @ref temperature_c holds a real reading.
    double temperature_c;           ///< Sensor temperature in degrees Celsius (already unscaled from the raw tenths).
    int dropped_frames;             ///< Frames dropped in the current streaming session.
    RoiFormat roi;                  ///< The live ROI geometry and pixel format.
};

/**
 * @brief Self-contained error returned by adapter-level calls.
 * @note Returned by value and complete on its own: it carries both the high-level @ref OperationResult category and the
 *       raw ZWO/ASI code. The library keeps NO shared or per-instance "last error" state, so a returned DeviceError can
 *       never be aliased or overwritten by a concurrent operation.
 */
struct DEGORASASI_EXPORT DeviceError
{
    OperationResult category;   ///< High-level result category.
    int asi_code;               ///< Raw ZWO/ASI return code (ASI_ERROR_*).
    std::string context;        ///< Human context, e.g. "ASIOpenCamera(id=0)".

    /// @brief Establishes a success: category OPERATION_OK, no raw vendor code and an empty context.
    DeviceError();

    /// @brief True when the operation succeeded.
    bool ok() const;

    /// @brief Human-readable form: "<category>: asi=<code> (<context>)".
    std::string toString() const;
};

// ---------------------------------------------------------------------------------------------------------------------
// IMAGE GEOMETRY (pure, hardware-free)

/**
 * @brief Bytes occupied by one pixel in the given format.
 * @param format The pixel layout.
 * @return 1 for RAW8/Y8, 2 for RAW16, 3 for RGB24; 0 for an unrecognised value.
 */
DEGORASASI_EXPORT std::size_t bytesPerPixel(ImageFormat format);

/**
 * @brief Bytes required to hold exactly one frame of the given geometry.
 * @param roi The region-of-interest geometry and format.
 * @return width * height * bytesPerPixel(format), or 0 if the geometry is not positive.
 * @note This is the single place the frame-buffer size is computed. The vendor's retrieval calls do not bounds-check
 *       and are documented to crash on a short buffer, so every call site sizes its buffer from here.
 */
DEGORASASI_EXPORT std::size_t frameBufferSize(const RoiFormat& roi);

/**
 * @brief Whether a ROI geometry satisfies the vendor's alignment rules.
 * @param roi The geometry to check (dimensions are post-binning).
 * @return True when width and height are positive, width % 8 == 0, height % 2 == 0 and bin >= 1.
 * @note Verified on hardware: a violation is rejected by the SDK with ASI_ERROR_INVALID_SIZE, so the library checks
 *       first and reports OperationResult::INVALID_PARAMETER without a round trip.
 */
DEGORASASI_EXPORT bool isRoiAligned(const RoiFormat& roi);

// ---------------------------------------------------------------------------------------------------------------------
// SDK NUMERIC ADAPTERS
// Map library enums to (and from) the raw numeric values the ASI C API uses. These are pure, validated conversions with
// no vendor-header dependency, so they belong in the device-agnostic layer; the ASI_* struct translations live in the
// Layer-2 adapter.

DEGORASASI_EXPORT int toType(CameraId id);

DEGORASASI_EXPORT int toType(ImageFormat format);

DEGORASASI_EXPORT int toType(ControlType type);

DEGORASASI_EXPORT int toType(FlipMode mode);

/// @brief Map a raw ASI_IMG_TYPE ordinal to an ImageFormat. @return False if @p raw is not a format this library knows.
DEGORASASI_EXPORT bool imageFormatFromType(int raw, ImageFormat& out_format);

/// @brief Map a raw ASI_BAYER_PATTERN ordinal to a BayerPattern. @return False if @p raw is out of range.
DEGORASASI_EXPORT bool bayerPatternFromType(int raw, BayerPattern& out_pattern);

/// @brief Map a raw ASI_CONTROL_TYPE ordinal to a ControlType. @return False if @p raw is not a known control.
DEGORASASI_EXPORT bool controlTypeFromType(int raw, ControlType& out_type);

/// @brief Map a raw ASI_EXPOSURE_STATUS ordinal to an ExposureState. @return False if @p raw is out of range.
DEGORASASI_EXPORT bool exposureStateFromType(int raw, ExposureState& out_state);

// ---------------------------------------------------------------------------------------------------------------------

}} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
