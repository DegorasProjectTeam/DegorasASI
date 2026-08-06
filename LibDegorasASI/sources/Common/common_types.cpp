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
#include <algorithm>
#include <sstream>

// PROJECT INCLUDES
#include "LibDegorasASI/Common/common_types.h"
#include "LibDegorasASI/Helpers/json_utils.h"


// NAMESPACES
namespace dpasi
{
namespace types
{

// ---------------------------------------------------------------------------------------------------------------------
// SDK NUMERIC ADAPTERS

int toType(CameraId id)
{
    return static_cast<int>(id);
}

int toType(ImageFormat format)
{
    return static_cast<int>(format);
}

int toType(ControlType type)
{
    return static_cast<int>(type);
}

int toType(FlipMode mode)
{
    return static_cast<int>(mode);
}

bool imageFormatFromType(int raw, ImageFormat& out_format)
{
    switch (raw)
    {
        case 0: out_format = ImageFormat::RAW8;  return true;
        case 1: out_format = ImageFormat::RGB24; return true;
        case 2: out_format = ImageFormat::RAW16; return true;
        case 3: out_format = ImageFormat::Y8;    return true;
        default: break;
    }
    return false;
}

bool bayerPatternFromType(int raw, BayerPattern& out_pattern)
{
    switch (raw)
    {
        case 0: out_pattern = BayerPattern::RG; return true;
        case 1: out_pattern = BayerPattern::BG; return true;
        case 2: out_pattern = BayerPattern::GR; return true;
        case 3: out_pattern = BayerPattern::GB; return true;
        default: break;
    }
    return false;
}

bool controlTypeFromType(int raw, ControlType& out_type)
{
    // The vendor's ASI_CONTROL_TYPE is a dense range [0, ROLLING_INTERVAL]; anything outside it is a control this
    // library does not model (e.g. one added by a newer SDK) and is reported as unknown rather than cast blindly.
    if (raw < 0 || raw > static_cast<int>(ControlType::ROLLING_INTERVAL))
        return false;
    out_type = static_cast<ControlType>(raw);
    return true;
}

bool exposureStateFromType(int raw, ExposureState& out_state)
{
    switch (raw)
    {
        case 0: out_state = ExposureState::IDLE;    return true;
        case 1: out_state = ExposureState::WORKING; return true;
        case 2: out_state = ExposureState::SUCCESS; return true;
        case 3: out_state = ExposureState::FAILED;  return true;
        default: break;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// STRINGIFICATION

std::string toString(OperationResult r)
{
    switch (r)
    {
        case OperationResult::OPERATION_OK:           return "OPERATION_OK";
        case OperationResult::NOT_CONNECTED:          return "NOT_CONNECTED";
        case OperationResult::DEVICE_NOT_FOUND:       return "DEVICE_NOT_FOUND";
        case OperationResult::ALREADY_CONNECTED:      return "ALREADY_CONNECTED";
        case OperationResult::CAMERA_IN_USE:          return "CAMERA_IN_USE";
        case OperationResult::OPERATION_TIMEOUT:      return "OPERATION_TIMEOUT";
        case OperationResult::ZWO_INTERNAL_ERROR:     return "ZWO_INTERNAL_ERROR";
        case OperationResult::WORKER_ALREADY_RUNNING: return "WORKER_ALREADY_RUNNING";
        case OperationResult::WORKER_NOT_RUNNING:     return "WORKER_NOT_RUNNING";
        case OperationResult::WORKER_START_ERROR:     return "WORKER_START_ERROR";
        case OperationResult::READ_FAILED:            return "READ_FAILED";
        case OperationResult::INVALID_PARAMETER:      return "INVALID_PARAMETER";
        case OperationResult::UNSUPPORTED_CAPABILITY: return "UNSUPPORTED_CAPABILITY";
        case OperationResult::INVALID_SEQUENCE:       return "INVALID_SEQUENCE";
        case OperationResult::BUFFER_TOO_SMALL:       return "BUFFER_TOO_SMALL";
        case OperationResult::ACQUISITION_FAILED:     return "ACQUISITION_FAILED";
    }
    return "UNKNOWN_OPERATION_RESULT";
}

std::string toString(ImageFormat format)
{
    switch (format)
    {
        case ImageFormat::RAW8:  return "RAW8";
        case ImageFormat::RGB24: return "RGB24";
        case ImageFormat::RAW16: return "RAW16";
        case ImageFormat::Y8:    return "Y8";
    }
    return "UNKNOWN_IMAGE_FORMAT";
}

std::string toString(UsbLinkSpeed speed)
{
    switch (speed)
    {
        case UsbLinkSpeed::USB2: return "USB2.0";
        case UsbLinkSpeed::USB3: return "USB3.0";
    }
    return "UNKNOWN_USB_LINK_SPEED";
}

std::string toString(BayerPattern pattern)
{
    switch (pattern)
    {
        case BayerPattern::RG: return "RG";
        case BayerPattern::BG: return "BG";
        case BayerPattern::GR: return "GR";
        case BayerPattern::GB: return "GB";
    }
    return "UNKNOWN_BAYER_PATTERN";
}

std::string toString(ControlType type)
{
    switch (type)
    {
        case ControlType::GAIN:                   return "GAIN";
        case ControlType::EXPOSURE:               return "EXPOSURE";
        case ControlType::GAMMA:                  return "GAMMA";
        case ControlType::WB_RED:                 return "WB_RED";
        case ControlType::WB_BLUE:                return "WB_BLUE";
        case ControlType::OFFSET:                 return "OFFSET";
        case ControlType::BANDWIDTH_OVERLOAD:     return "BANDWIDTH_OVERLOAD";
        case ControlType::OVERCLOCK:              return "OVERCLOCK";
        case ControlType::TEMPERATURE:            return "TEMPERATURE";
        case ControlType::FLIP:                   return "FLIP";
        case ControlType::AUTO_MAX_GAIN:          return "AUTO_MAX_GAIN";
        case ControlType::AUTO_MAX_EXPOSURE:      return "AUTO_MAX_EXPOSURE";
        case ControlType::AUTO_TARGET_BRIGHTNESS: return "AUTO_TARGET_BRIGHTNESS";
        case ControlType::HARDWARE_BIN:           return "HARDWARE_BIN";
        case ControlType::HIGH_SPEED_MODE:        return "HIGH_SPEED_MODE";
        case ControlType::COOLER_POWER_PERC:      return "COOLER_POWER_PERC";
        case ControlType::TARGET_TEMPERATURE:     return "TARGET_TEMPERATURE";
        case ControlType::COOLER_ON:              return "COOLER_ON";
        case ControlType::MONO_BIN:               return "MONO_BIN";
        case ControlType::FAN_ON:                 return "FAN_ON";
        case ControlType::PATTERN_ADJUST:         return "PATTERN_ADJUST";
        case ControlType::ANTI_DEW_HEATER:        return "ANTI_DEW_HEATER";
        case ControlType::FAN_ADJUST:             return "FAN_ADJUST";
        case ControlType::PWR_LED_BRIGHTNESS:     return "PWR_LED_BRIGHTNESS";
        case ControlType::USB_HUB_RESET:          return "USB_HUB_RESET";
        case ControlType::GPS_SUPPORT:            return "GPS_SUPPORT";
        case ControlType::GPS_START_LINE:         return "GPS_START_LINE";
        case ControlType::GPS_END_LINE:           return "GPS_END_LINE";
        case ControlType::ROLLING_INTERVAL:       return "ROLLING_INTERVAL";
    }
    return "UNKNOWN_CONTROL_TYPE";
}

std::string toString(ExposureState state)
{
    switch (state)
    {
        case ExposureState::IDLE:    return "IDLE";
        case ExposureState::WORKING: return "WORKING";
        case ExposureState::SUCCESS: return "SUCCESS";
        case ExposureState::FAILED:  return "FAILED";
    }
    return "UNKNOWN_EXPOSURE_STATE";
}

std::string toString(AcquisitionMode mode)
{
    switch (mode)
    {
        case AcquisitionMode::IDLE:     return "IDLE";
        case AcquisitionMode::VIDEO:    return "VIDEO";
        case AcquisitionMode::SNAPSHOT: return "SNAPSHOT";
    }
    return "UNKNOWN_ACQUISITION_MODE";
}

// ---------------------------------------------------------------------------------------------------------------------
// IMAGE GEOMETRY

std::size_t bytesPerPixel(ImageFormat format)
{
    switch (format)
    {
        case ImageFormat::RAW8:  return 1;
        case ImageFormat::Y8:    return 1;
        case ImageFormat::RAW16: return 2;
        case ImageFormat::RGB24: return 3;
    }
    return 0;
}

std::size_t frameBufferSize(const RoiFormat& roi)
{
    if (roi.width <= 0 || roi.height <= 0)
        return 0;
    return static_cast<std::size_t>(roi.width) * static_cast<std::size_t>(roi.height) * bytesPerPixel(roi.format);
}

bool isRoiAligned(const RoiFormat& roi)
{
    return roi.width > 0 && roi.height > 0 && roi.bin >= 1 && (roi.width % 8) == 0 && (roi.height % 2) == 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// DATA STRUCTURES

RoiFormat::RoiFormat() :
    width(0),
    height(0),
    bin(1),
    format(ImageFormat::RAW8)
{}

std::string RoiFormat::toJsonStr(bool pretty) const
{
    std::ostringstream ss;
    ss << "{";
    ss << "\"width\": " << this->width << ",";
    ss << "\"height\": " << this->height << ",";
    ss << "\"bin\": " << this->bin << ",";
    ss << "\"format\": " << static_cast<int>(this->format);
    ss << "}";
    return pretty ? json::prettify(ss.str()) : ss.str();
}

RoiFormat RoiFormat::fromJsonStr(const std::string& json_str)
{
    RoiFormat roi;
    roi.width = json::getInt(json_str, "width");
    roi.height = json::getInt(json_str, "height");
    roi.bin = json::getInt(json_str, "bin", 1);
    roi.format = static_cast<ImageFormat>(json::getInt(json_str, "format", static_cast<int>(ImageFormat::RAW8)));
    return roi;
}

RoiPosition::RoiPosition() :
    start_x(0),
    start_y(0)
{}

std::string RoiPosition::toJsonStr(bool pretty) const
{
    std::ostringstream ss;
    ss << "{";
    ss << "\"start_x\": " << this->start_x << ",";
    ss << "\"start_y\": " << this->start_y;
    ss << "}";
    return pretty ? json::prettify(ss.str()) : ss.str();
}

RoiPosition RoiPosition::fromJsonStr(const std::string& json_str)
{
    RoiPosition pos;
    pos.start_x = json::getInt(json_str, "start_x");
    pos.start_y = json::getInt(json_str, "start_y");
    return pos;
}

ControlCaps::ControlCaps() :
    type(ControlType::GAIN),
    name(),
    description(),
    min_value(0),
    max_value(0),
    default_value(0),
    is_auto_supported(false),
    is_writable(false)
{}

std::string ControlCaps::toJsonStr(bool pretty) const
{
    std::ostringstream ss;
    ss << "{";
    ss << "\"type\": " << static_cast<int>(this->type) << ",";
    ss << "\"name\": \"" << this->name << "\",";
    ss << "\"description\": \"" << this->description << "\",";
    ss << "\"min_value\": " << this->min_value << ",";
    ss << "\"max_value\": " << this->max_value << ",";
    ss << "\"default_value\": " << this->default_value << ",";
    ss << "\"is_auto_supported\": " << (this->is_auto_supported ? "true" : "false") << ",";
    ss << "\"is_writable\": " << (this->is_writable ? "true" : "false");
    ss << "}";
    return pretty ? json::prettify(ss.str()) : ss.str();
}

ControlCaps ControlCaps::fromJsonStr(const std::string& json_str)
{
    ControlCaps caps;
    caps.type = static_cast<ControlType>(json::getInt(json_str, "type", static_cast<int>(ControlType::GAIN)));
    caps.name = json::getString(json_str, "name");
    caps.description = json::getString(json_str, "description");
    caps.min_value = json::getInt64(json_str, "min_value");
    caps.max_value = json::getInt64(json_str, "max_value");
    caps.default_value = json::getInt64(json_str, "default_value");
    caps.is_auto_supported = json::getBool(json_str, "is_auto_supported");
    caps.is_writable = json::getBool(json_str, "is_writable");
    return caps;
}

ControlValue::ControlValue() :
    value(0),
    is_auto(false)
{}

std::string ControlValue::toJsonStr(bool pretty) const
{
    std::ostringstream ss;
    ss << "{";
    ss << "\"value\": " << this->value << ",";
    ss << "\"is_auto\": " << (this->is_auto ? "true" : "false");
    ss << "}";
    return pretty ? json::prettify(ss.str()) : ss.str();
}

ControlValue ControlValue::fromJsonStr(const std::string& json_str)
{
    ControlValue val;
    val.value = json::getInt64(json_str, "value");
    val.is_auto = json::getBool(json_str, "is_auto");
    return val;
}

CameraDescriptor::CameraDescriptor() :
    id(CameraId{0}),
    name(),
    max_width(0),
    max_height(0),
    pixel_size_um(0),
    bit_depth(0),
    electrons_per_adu(0),
    is_colour(false),
    bayer_pattern(BayerPattern::RG),
    supported_bins(),
    supported_formats(),
    has_mechanical_shutter(false),
    has_st4_port(false),
    is_cooled(false),
    is_usb3_camera(false),
    is_usb3_host(false),
    is_trigger_camera(false)
{}

bool CameraDescriptor::supportsFormat(ImageFormat format) const
{
    return std::find(this->supported_formats.cbegin(), this->supported_formats.cend(), format) !=
           this->supported_formats.cend();
}

bool CameraDescriptor::supportsBin(int bin) const
{
    return std::find(this->supported_bins.cbegin(), this->supported_bins.cend(), bin) != this->supported_bins.cend();
}

bool CameraDescriptor::isLinkFullSpeed() const
{
    // A USB2 camera is always at full speed; only a USB3 camera on a USB2 host is degraded.
    return !this->is_usb3_camera || this->is_usb3_host;
}

UsbLinkSpeed CameraDescriptor::linkSpeed() const
{
    // SuperSpeed needs BOTH ends: a USB3 camera in a USB2 port negotiates USB2, and vice versa.
    return (this->is_usb3_camera && this->is_usb3_host) ? UsbLinkSpeed::USB3 : UsbLinkSpeed::USB2;
}

std::string CameraDescriptor::toJsonStr(bool pretty) const
{
    std::ostringstream ss;
    ss << "{";
    ss << "\"id\": " << static_cast<int>(this->id) << ",";
    ss << "\"name\": \"" << this->name << "\",";
    ss << "\"max_width\": " << this->max_width << ",";
    ss << "\"max_height\": " << this->max_height << ",";
    ss << "\"pixel_size_um\": " << this->pixel_size_um << ",";
    ss << "\"bit_depth\": " << this->bit_depth << ",";
    ss << "\"electrons_per_adu\": " << this->electrons_per_adu << ",";
    ss << "\"is_colour\": " << (this->is_colour ? "true" : "false") << ",";
    ss << "\"bayer_pattern\": " << static_cast<int>(this->bayer_pattern) << ",";

    ss << "\"supported_bins\": [";
    for (std::size_t i = 0; i < this->supported_bins.size(); ++i)
        ss << (i ? "," : "") << this->supported_bins[i];
    ss << "],";

    ss << "\"supported_formats\": [";
    for (std::size_t i = 0; i < this->supported_formats.size(); ++i)
        ss << (i ? "," : "") << static_cast<int>(this->supported_formats[i]);
    ss << "],";

    ss << "\"has_mechanical_shutter\": " << (this->has_mechanical_shutter ? "true" : "false") << ",";
    ss << "\"has_st4_port\": " << (this->has_st4_port ? "true" : "false") << ",";
    ss << "\"is_cooled\": " << (this->is_cooled ? "true" : "false") << ",";
    ss << "\"is_usb3_camera\": " << (this->is_usb3_camera ? "true" : "false") << ",";
    ss << "\"is_usb3_host\": " << (this->is_usb3_host ? "true" : "false") << ",";
    ss << "\"is_trigger_camera\": " << (this->is_trigger_camera ? "true" : "false");
    ss << "}";
    return pretty ? json::prettify(ss.str()) : ss.str();
}

CameraDescriptor CameraDescriptor::fromJsonStr(const std::string& json_str)
{
    CameraDescriptor desc;
    desc.id = static_cast<CameraId>(json::getInt(json_str, "id"));
    desc.name = json::getString(json_str, "name");
    desc.max_width = json::getInt(json_str, "max_width");
    desc.max_height = json::getInt(json_str, "max_height");
    desc.pixel_size_um = json::getDouble(json_str, "pixel_size_um");
    desc.bit_depth = json::getInt(json_str, "bit_depth");
    desc.electrons_per_adu = json::getDouble(json_str, "electrons_per_adu");
    desc.is_colour = json::getBool(json_str, "is_colour");
    desc.bayer_pattern =
        static_cast<BayerPattern>(json::getInt(json_str, "bayer_pattern", static_cast<int>(BayerPattern::RG)));
    desc.supported_bins = json::getIntArray(json_str, "supported_bins");

    const std::vector<int> raw_formats = json::getIntArray(json_str, "supported_formats");
    for (const int raw : raw_formats)
    {
        ImageFormat format = ImageFormat::RAW8;
        if (imageFormatFromType(raw, format))
            desc.supported_formats.push_back(format);
    }

    desc.has_mechanical_shutter = json::getBool(json_str, "has_mechanical_shutter");
    desc.has_st4_port = json::getBool(json_str, "has_st4_port");
    desc.is_cooled = json::getBool(json_str, "is_cooled");
    desc.is_usb3_camera = json::getBool(json_str, "is_usb3_camera");
    desc.is_usb3_host = json::getBool(json_str, "is_usb3_host");
    desc.is_trigger_camera = json::getBool(json_str, "is_trigger_camera");
    return desc;
}

Frame::Frame() :
    format(ImageFormat::RAW8),
    width(0),
    height(0),
    bin(1),
    sequence(0),
    timestamp(),
    data()
{}

bool Frame::empty() const
{
    return this->data.empty();
}

std::size_t Frame::expectedBytes() const
{
    RoiFormat roi;
    roi.width = this->width;
    roi.height = this->height;
    roi.bin = this->bin;
    roi.format = this->format;
    return frameBufferSize(roi);
}

std::string Frame::toJsonStr(bool pretty) const
{
    const std::chrono::system_clock::duration since_epoch = this->timestamp.time_since_epoch();
    const std::int64_t stamp_us = std::chrono::duration_cast<std::chrono::microseconds>(since_epoch).count();

    std::ostringstream ss;
    ss << "{";
    ss << "\"format\": " << static_cast<int>(this->format) << ",";
    ss << "\"width\": " << this->width << ",";
    ss << "\"height\": " << this->height << ",";
    ss << "\"bin\": " << this->bin << ",";
    ss << "\"sequence\": " << this->sequence << ",";
    ss << "\"timestamp_us\": " << stamp_us << ",";
    ss << "\"bytes\": " << this->data.size();
    ss << "}";
    return pretty ? json::prettify(ss.str()) : ss.str();
}


CameraStatus::CameraStatus() :
    id(CameraId{0}),
    connected(false),
    acq_mode(AcquisitionMode::IDLE),
    exposure_state(ExposureState::IDLE),
    temperature_valid(false),
    temperature_c(0),
    dropped_frames(0),
    roi(RoiFormat())
{}

std::string CameraStatus::toJsonStr(bool pretty) const
{
    std::ostringstream ss;
    ss << "{";
    ss << "\"id\": " << static_cast<int>(this->id) << ",";
    ss << "\"connected\": " << (this->connected ? "true" : "false") << ",";
    ss << "\"acq_mode\": " << static_cast<int>(this->acq_mode) << ",";
    ss << "\"exposure_state\": " << static_cast<int>(this->exposure_state) << ",";
    ss << "\"temperature_valid\": " << (this->temperature_valid ? "true" : "false") << ",";
    ss << "\"temperature_c\": " << this->temperature_c << ",";
    ss << "\"dropped_frames\": " << this->dropped_frames << ",";
    ss << "\"roi\": " << this->roi.toJsonStr();
    ss << "}";
    return pretty ? json::prettify(ss.str()) : ss.str();
}

CameraStatus CameraStatus::fromJsonStr(const std::string& json_str)
{
    CameraStatus status;
    status.id = static_cast<CameraId>(json::getInt(json_str, "id"));
    status.connected = json::getBool(json_str, "connected");
    status.acq_mode =
        static_cast<AcquisitionMode>(json::getInt(json_str, "acq_mode", static_cast<int>(AcquisitionMode::IDLE)));
    status.exposure_state =
        static_cast<ExposureState>(json::getInt(json_str, "exposure_state", static_cast<int>(ExposureState::IDLE)));
    status.temperature_valid = json::getBool(json_str, "temperature_valid");
    status.temperature_c = json::getDouble(json_str, "temperature_c");
    status.dropped_frames = json::getInt(json_str, "dropped_frames");
    status.roi = RoiFormat::fromJsonStr(json::getObject(json_str, "roi"));
    return status;
}

// ---------------------------------------------------------------------------------------------------------------------

std::string DeviceError::toString() const
{
    std::ostringstream ss;
    ss << dpasi::types::toString(this->category)
       << ": asi=" << this->asi_code
       << " (" << this->context << ")";
    return ss.str();
}

bool DeviceError::ok() const
{
    return this->category == OperationResult::OPERATION_OK;
}

}} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
