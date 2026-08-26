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

// C++ INCLUDES
#include <algorithm>
#include <cmath>
#include <utility>

// PROJECT INCLUDES
#include "live_model.h"


using namespace dpasi;
using dpasi::types::OperationResult;

// NAMESPACES
namespace live
{

namespace
{

// How long one grab is allowed to wait. It is NOT the exposure limit: in video mode the stream runs on its own and a
// timed-out grab simply means no frame has arrived yet, so the loop asks again and nothing is restarted. Keeping it
// short is what lets stop() finish promptly instead of waiting out a ten-second exposure.
constexpr int kGrabTimeoutMs = 500;

// Below this, a progress bar is noise: it would refill several times a second and tell nobody anything.
constexpr double kProgressUsefulMs = 400.0;

// How often the controls and the dropped-frame counter are re-read. Each one is a USB round trip, so doing it per
// frame would tax the same link that is carrying the images.
constexpr double kTelemetryPeriodMs = 1000.0;

double millisSince(const std::chrono::steady_clock::time_point& then)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - then).count();
}

}   // namespace

// ---------------------------------------------------------------------------------------------------------------------

ModelState::ModelState() :
    exposure_us(0),
    gain(0),
    wb_red(0),
    wb_blue(0),
    has_wb(false),
    fps(0.0),
    dropped(0),
    sequence(0),
    next_frame_progress(0.0),
    expected_interval_ms(0.0),
    progress_is_useful(false),
    last_error(),
    temperature_c(0.0),
    temperature_valid(false)
{
}

// ---------------------------------------------------------------------------------------------------------------------

LiveModel::LiveModel(AsiCamera& camera, bool has_wb) :
    camera_(camera),
    has_wb_(has_wb),
    worker_(),
    running_(false),
    streaming_(false),
    frame_mtx_(),
    ready_(),
    ready_valid_(false),
    state_mtx_(),
    state_(),
    request_mtx_(),
    requests_(),
    last_frame_at_(std::chrono::steady_clock::now()),
    expected_interval_ms_(0.0)
{
    this->state_.has_wb = has_wb;
    this->refreshControls();
}

LiveModel::~LiveModel()
{
    this->stop();
}

// ---------------------------------------------------------------------------------------------------------------------

bool LiveModel::start()
{
    if (this->running_.load())
        return true;

    if (this->camera_.doStartVideoCapture() != OperationResult::OPERATION_OK)
        return false;

    this->streaming_ = true;
    this->last_frame_at_ = std::chrono::steady_clock::now();
    this->running_.store(true);
    this->worker_ = std::thread(&LiveModel::run, this);
    return true;
}

void LiveModel::stop()
{
    // The thread first, then the stream. The other order would have the thread grabbing from a stopped stream, which
    // is not harmful but does produce a burst of failures that would land in last_error for no reason.
    this->running_.store(false);
    if (this->worker_.joinable())
        this->worker_.join();

    if (this->streaming_)
    {
        this->camera_.doStopVideoCapture();
        this->streaming_ = false;
    }
}

// ---------------------------------------------------------------------------------------------------------------------

bool LiveModel::takeFrame(types::Frame& out)
{
    const std::lock_guard<std::mutex> lock(this->frame_mtx_);
    if (!this->ready_valid_)
        return false;

    // Swapped, not copied. The caller's previous buffer goes back to the model and becomes the next scratch, so
    // three buffers cycle between the two threads and a steady stream allocates nothing.
    std::swap(out, this->ready_);
    this->ready_valid_ = false;
    return true;
}

ModelState LiveModel::state() const
{
    const std::lock_guard<std::mutex> lock(this->state_mtx_);
    ModelState copy = this->state_;

    // Progress is computed HERE, on the caller's thread, rather than being published by the acquisition thread. That
    // thread is blocked inside the grab for the whole exposure, so anything it published would freeze exactly when
    // the bar needs to move. The estimate is the measured frame interval, floored at the exposure, since a frame can
    // never arrive sooner than its own exposure.
    const double expected = std::max(this->expected_interval_ms_, copy.exposure_us / 1000.0);
    if (expected > 0.0)
    {
        copy.next_frame_progress = std::min(1.0, millisSince(this->last_frame_at_) / expected);
        copy.expected_interval_ms = expected;
        copy.progress_is_useful = (expected >= kProgressUsefulMs);
    }
    return copy;
}

// ---------------------------------------------------------------------------------------------------------------------

void LiveModel::requestExposure(long long microseconds)
{
    const std::lock_guard<std::mutex> lock(this->request_mtx_);
    this->requests_.push_back([this, microseconds]()
    {
        this->writeControl(types::ControlType::EXPOSURE, microseconds, false);
    });
}

void LiveModel::requestExposureScale(double factor)
{
    const std::lock_guard<std::mutex> lock(this->request_mtx_);
    this->requests_.push_back([this, factor]()
    {
        types::ControlValue current;
        if (this->camera_.getControl(types::ControlType::EXPOSURE, current) != OperationResult::OPERATION_OK)
            return;
        const long long target = std::max<long long>(1, std::llround(current.value * factor));
        this->writeControl(types::ControlType::EXPOSURE, target, false);
    });
}

void LiveModel::requestGain(long long value)
{
    const std::lock_guard<std::mutex> lock(this->request_mtx_);
    this->requests_.push_back([this, value]()
    {
        this->writeControl(types::ControlType::GAIN, value, false);
    });
}

void LiveModel::requestGainDelta(long long delta)
{
    const std::lock_guard<std::mutex> lock(this->request_mtx_);
    this->requests_.push_back([this, delta]()
    {
        this->writeControl(types::ControlType::GAIN, delta, true);
    });
}

void LiveModel::requestWbRedDelta(long long delta)
{
    if (!this->has_wb_)
        return;
    const std::lock_guard<std::mutex> lock(this->request_mtx_);
    this->requests_.push_back([this, delta]()
    {
        this->writeControl(types::ControlType::WB_RED, delta, true);
    });
}

void LiveModel::requestResetControls()
{
    const std::lock_guard<std::mutex> lock(this->request_mtx_);
    this->requests_.push_back([this]()
    {
        // WHY THIS EXISTS. Connecting does not put the camera in a known state: the vendor's driver restores whatever
        // the last session left, and a session that walked the white balance down to its minimum leaves the next one
        // with a picture that is pure green. Measured on the ASI224MC in one process: at WB 1/1 the channel means
        // were 2.2 / 220.4 / 2.6, and at the reported defaults of 52/95 they were 230.7 / 220.4 / 224.7.
        //
        // The PICTURE controls only, and deliberately: resetting the bandwidth or the binning as well would silently
        // change the frame rate and the image size, so a command that means "put the colours back" would be a trap.
        const types::ControlTypeList picture = {
            types::ControlType::EXPOSURE,
            types::ControlType::GAIN,
            types::ControlType::OFFSET,
            types::ControlType::WB_RED,
            types::ControlType::WB_BLUE,
        };

        // The library's own operation rather than a loop here: it is the one that knows to skip a control this model
        // does not expose, and every application wants this, so it belongs next to the controls and not copied into
        // each example.
        this->camera_.doResetControlsToDefaults(picture);
    });
}

void LiveModel::requestWbBlueDelta(long long delta)
{
    if (!this->has_wb_)
        return;
    const std::lock_guard<std::mutex> lock(this->request_mtx_);
    this->requests_.push_back([this, delta]()
    {
        this->writeControl(types::ControlType::WB_BLUE, delta, true);
    });
}

// ---------------------------------------------------------------------------------------------------------------------

bool LiveModel::exposureRange(long long& minimum, long long& maximum) const
{
    types::ControlCaps caps;
    if (!this->camera_.hasControl(types::ControlType::EXPOSURE))
        return false;
    if (this->camera_.getControlCaps(types::ControlType::EXPOSURE, caps) != OperationResult::OPERATION_OK)
        return false;
    minimum = caps.min_value;
    maximum = caps.max_value;
    return true;
}

bool LiveModel::gainMaximum(long long& maximum) const
{
    types::ControlCaps caps;
    if (!this->camera_.hasControl(types::ControlType::GAIN))
        return false;
    if (this->camera_.getControlCaps(types::ControlType::GAIN, caps) != OperationResult::OPERATION_OK)
        return false;
    maximum = caps.max_value;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------

void LiveModel::run()
{
    types::Frame scratch;
    std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_telemetry = window_start;
    int frames_in_window = 0;

    while (this->running_.load())
    {
        // Before the grab, never during it: this is what gives a control change its "applies to the next exposure"
        // meaning, and it keeps every camera call on this one thread.
        this->applyRequests();

        const OperationResult res =
            this->camera_.doGetVideoFrame(scratch, std::chrono::milliseconds(kGrabTimeoutMs));

        if (!this->running_.load())
            break;

        if (res == OperationResult::OPERATION_TIMEOUT)
        {
            // Expected, and not an error. An exposure longer than the grab timeout simply needs more turns of this
            // loop; the stream keeps running underneath and nothing is restarted by asking again.
            continue;
        }

        if (res != OperationResult::OPERATION_OK)
        {
            {
                const std::lock_guard<std::mutex> lock(this->state_mtx_);
                this->state_.last_error = types::toString(res);
            }
            // A failure that repeats would otherwise spin this loop at full speed against a sick camera.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Read what the view will want BEFORE the swap, while the new frame is still in scratch.
        const std::uint64_t sequence = scratch.sequence;

        {
            const std::lock_guard<std::mutex> lock(this->frame_mtx_);
            std::swap(this->ready_, scratch);
            this->ready_valid_ = true;
        }

        ++frames_in_window;
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

        {
            const std::lock_guard<std::mutex> lock(this->state_mtx_);
            const double gap = std::chrono::duration<double, std::milli>(now - this->last_frame_at_).count();
            // Exponentially smoothed, seeded by the first measurement. Smoothed because the estimate feeds a
            // progress bar, and an unfiltered interval makes it jump about; the weight is mild so that a real change
            // of exposure is reflected within a few frames.
            this->expected_interval_ms_ =
                (this->expected_interval_ms_ <= 0.0) ? gap : (0.7 * this->expected_interval_ms_ + 0.3 * gap);
            this->last_frame_at_ = now;
            this->state_.sequence = sequence;
            this->state_.last_error.clear();
        }

        const double window_ms = std::chrono::duration<double, std::milli>(now - window_start).count();
        if (window_ms >= kTelemetryPeriodMs)
        {
            const double measured_fps = frames_in_window * 1000.0 / window_ms;
            frames_in_window = 0;
            window_start = now;

            int dropped = 0;
            this->camera_.getDroppedFrames(dropped);

            const std::lock_guard<std::mutex> lock(this->state_mtx_);
            this->state_.fps = measured_fps;
            this->state_.dropped = dropped;
        }

        if (millisSince(last_telemetry) >= kTelemetryPeriodMs)
        {
            this->refreshControls();
            last_telemetry = std::chrono::steady_clock::now();
        }
    }
}

void LiveModel::applyRequests()
{
    std::vector<std::function<void()>> pending;
    {
        const std::lock_guard<std::mutex> lock(this->request_mtx_);
        if (this->requests_.empty())
            return;
        pending.swap(this->requests_);
    }

    // Invoked with the lock released: each one is a USB round trip, and holding the queue during them would block
    // the main thread every time it tried to add another.
    for (const std::function<void()>& request : pending)
        request();

    this->refreshControls();
}

long long LiveModel::writeControl(types::ControlType control, long long value, bool relative)
{
    if (!this->camera_.hasControl(control) || !this->camera_.isControlWritable(control))
        return -1;

    types::ControlValue current;
    if (this->camera_.getControl(control, current) != OperationResult::OPERATION_OK)
        return -1;

    types::ControlCaps caps;
    if (this->camera_.getControlCaps(control, caps) != OperationResult::OPERATION_OK)
        return -1;

    types::ControlValue wanted;
    wanted.value = std::clamp<long long>(relative ? current.value + value : value, caps.min_value, caps.max_value);
    wanted.is_auto = false;

    if (this->camera_.doSetControl(control, wanted) != OperationResult::OPERATION_OK)
        return -1;

    // Read back rather than trust the write: the SDK clamps silently, so what was asked for and what took effect are
    // not always the same number, and the view must show the second one.
    types::ControlValue applied;
    if (this->camera_.getControl(control, applied) != OperationResult::OPERATION_OK)
        return -1;
    return applied.value;
}

void LiveModel::refreshControls()
{
    types::ControlValue value;
    long long exposure = -1;
    long long gain = -1;
    long long red = -1;
    long long blue = -1;
    double temperature = 0.0;
    bool temperature_ok = false;

    if (this->camera_.getControl(types::ControlType::EXPOSURE, value) == OperationResult::OPERATION_OK)
        exposure = value.value;
    if (this->camera_.getControl(types::ControlType::GAIN, value) == OperationResult::OPERATION_OK)
        gain = value.value;
    if (this->has_wb_)
    {
        if (this->camera_.getControl(types::ControlType::WB_RED, value) == OperationResult::OPERATION_OK)
            red = value.value;
        if (this->camera_.getControl(types::ControlType::WB_BLUE, value) == OperationResult::OPERATION_OK)
            blue = value.value;
    }

    // TEMPERATURE is reported in TENTHS of a degree Celsius, which is the one unit in this enum that catches
    // everybody, and it is read-only on every camera seen so far. hasControl() is asked first because an uncooled
    // camera may not expose it at all, and a missing control must read as "no reading" and not as 0 degrees.
    if (this->camera_.hasControl(types::ControlType::TEMPERATURE) &&
        this->camera_.getControl(types::ControlType::TEMPERATURE, value) == OperationResult::OPERATION_OK)
    {
        temperature = static_cast<double>(value.value) / 10.0;
        temperature_ok = true;
    }

    const std::lock_guard<std::mutex> lock(this->state_mtx_);
    if (exposure >= 0) this->state_.exposure_us = exposure;
    if (gain >= 0)     this->state_.gain = gain;
    if (red >= 0)      this->state_.wb_red = red;
    if (blue >= 0)     this->state_.wb_blue = blue;

    this->state_.temperature_valid = temperature_ok;
    if (temperature_ok)
        this->state_.temperature_c = temperature;
}

// ---------------------------------------------------------------------------------------------------------------------

}   // END NAMESPACE

// ---------------------------------------------------------------------------------------------------------------------
