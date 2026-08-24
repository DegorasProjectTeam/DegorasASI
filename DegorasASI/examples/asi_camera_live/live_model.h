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
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// PROJECT INCLUDES
#include <DegorasASI/Modules/Devices>


// NAMESPACES
namespace live
{

// ---------------------------------------------------------------------------------------------------------------------
// THE MODEL. It owns the camera and the ONLY thread that ever talks to it.
//
// WHY THIS EXISTS AT ALL. The previous version of this example was one loop on the main thread: grab a frame, show
// it, call waitKey, repeat. doGetVideoFrame() blocks for at least the exposure, and waitKey() is what pumps the GUI,
// so with a two-second exposure the window went unresponsive for two seconds at a time -- no repaint, no slider
// movement, nothing. At ten seconds it looked broken. Moving the grab to its own thread is the whole fix: the main
// thread then runs the view at a steady rate whatever the exposure is doing.
//
// TWO CONSEQUENCES OF THAT SPLIT, both deliberate.
//
//   * CONTROL CHANGES ARE REQUESTS, not calls. The view pushes them from the main thread and the acquisition thread
//     applies them between grabs, which means a change made during a long exposure takes effect on the NEXT one.
//     That is not a limitation being papered over; it is what the hardware does, and saying so in the interface is
//     better than blocking the caller until the current exposure ends. Every request method returns immediately.
//   * FRAMES ARE HANDED OVER BY SWAP, not by copy. A 16-bit full-frame image is megabytes, and copying one per
//     displayed frame would undo the point. takeFrame() exchanges buffers under the lock, so the steady state costs
//     no allocation once both sides have grown their vectors.
//
// Everything public here is safe to call from the main thread while the acquisition thread runs.
// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Everything the view needs to draw, captured in one consistent snapshot.
 * @note Taken under a single lock, so the numbers on screen always describe the same instant rather than being
 *       assembled from several reads that could straddle a frame.
 */
struct ModelState
{
    /// @brief Establishes a snapshot describing a camera that has produced nothing yet.
    ModelState();

    long long exposure_us;        ///< Exposure last read back from the camera, in microseconds.
    long long gain;               ///< Gain last read back from the camera.
    long long wb_red;             ///< White-balance red, when the camera has one.
    long long wb_blue;            ///< White-balance blue, when the camera has one.
    bool has_wb;                  ///< Whether white balance exists on this camera at all.
    double fps;                   ///< Delivered frames per second, averaged over the last reporting window.
    int dropped;                  ///< Frames the SDK reports as dropped since streaming started.
    std::uint64_t sequence;       ///< Sequence number of the most recent frame.
    double next_frame_progress;   ///< 0..1 estimate of how far along the wait for the next frame is.
    double expected_interval_ms;  ///< Estimated wait for one frame, so a bar can say how many seconds remain.
    bool progress_is_useful;      ///< True when that interval is long enough for a progress bar to mean something.
    std::string last_error;       ///< Text of the most recent acquisition failure, or empty.
};

/**
 * @brief Owns the camera and the acquisition thread, and publishes what the view draws.
 * @note Not copyable or movable: it owns a running thread and hands out references to its own state.
 */
class LiveModel
{
public:

    /**
     * @brief Binds the model to an already connected and configured camera.
     * @param camera A connected camera whose ROI, format and initial controls are already set. Must outlive this.
     * @param has_wb Whether the white-balance controls exist, decided once by the caller from the descriptor.
     * @note Does not start streaming; call start() for that.
     */
    LiveModel(dpasi::AsiCamera& camera, bool has_wb);

    /// @brief Stops the thread and the stream if they are still running.
    ~LiveModel();

    LiveModel(const LiveModel&) = delete;
    LiveModel& operator=(const LiveModel&) = delete;
    LiveModel(LiveModel&&) = delete;
    LiveModel& operator=(LiveModel&&) = delete;

    /**
     * @brief Starts video capture and the acquisition thread.
     * @return True when both started; false when the camera refused to stream, in which case nothing was started.
     */
    bool start();

    /**
     * @brief Stops the acquisition thread and the stream, in that order.
     * @note Idempotent, and safe from a teardown path. Waits for the thread, which cannot take longer than one
     *       frame timeout because the grab is bounded.
     */
    void stop();

    /**
     * @brief Takes the newest frame, if one has arrived since the last call.
     * @param out Receives the frame by SWAP; its previous contents go back to the model as the next scratch buffer.
     * @return True when @p out now holds a new frame, false when nothing new has arrived.
     */
    bool takeFrame(dpasi::types::Frame& out);

    /**
     * @brief A consistent snapshot of everything the view draws.
     * @return The current state.
     */
    ModelState state() const;

    /**
     * @brief Requests an absolute exposure, in microseconds.
     * @param microseconds The value to write. The camera clamps it, and the state reports what actually took.
     * @note Returns immediately; applied by the acquisition thread before the next grab.
     */
    void requestExposure(long long microseconds);

    /**
     * @brief Requests an exposure multiplied by a factor.
     * @param factor Multiplier, e.g. 1.1 to lengthen by ten percent.
     * @note Multiplicative because exposure spans microseconds to seconds, where a fixed step is useless at one end
     *       and imperceptible at the other.
     */
    void requestExposureScale(double factor);

    /**
     * @brief Requests an absolute gain.
     * @param value The value to write, clamped by the camera.
     */
    void requestGain(long long value);

    /**
     * @brief Requests a gain change relative to whatever is current.
     * @param delta Amount to add, which may be negative.
     */
    void requestGainDelta(long long delta);

    /**
     * @brief Requests a white-balance red change relative to whatever is current.
     * @param delta Amount to add, which may be negative. Ignored on a camera without white balance.
     */
    void requestWbRedDelta(long long delta);

    /**
     * @brief Requests a white-balance blue change relative to whatever is current.
     * @param delta Amount to add, which may be negative. Ignored on a camera without white balance.
     */
    void requestWbBlueDelta(long long delta);

    /**
     * @brief Puts the picture controls back to the values the camera reports as its own defaults.
     * @note Exposure, gain, offset and white balance only -- not the bandwidth, the binning or the high-speed mode,
     *       which change throughput and geometry rather than what the picture looks like.
     * @note Needed because the vendor's driver hands back whatever the previous session left behind: this camera
     *       opens with WB_RED and WB_BLUE at 1 against defaults of 52 and 95, which is a picture with no red and no
     *       blue in it at all. Measured, not guessed -- see the note in the implementation.
     */
    void requestResetControls();

    /**
     * @brief The camera's accepted range for the exposure control.
     * @param minimum Receives the smallest accepted value, in microseconds.
     * @param maximum Receives the largest accepted value, in microseconds.
     * @return True when the camera reported a range, false when it has no writable exposure control.
     */
    bool exposureRange(long long& minimum, long long& maximum) const;

    /**
     * @brief The camera's accepted maximum for the gain control.
     * @param maximum Receives the largest accepted value.
     * @return True when the camera reported a range, false when it has no writable gain control.
     */
    bool gainMaximum(long long& maximum) const;

private:

    /// @brief The acquisition loop: apply pending requests, grab one frame, publish it, repeat.
    void run();

    /// @brief Applies and clears every queued request. Runs on the acquisition thread only.
    void applyRequests();

    /// @brief Writes one control, clamped to its caps, and returns what the camera actually took.
    long long writeControl(dpasi::types::ControlType control, long long value, bool relative);

    /// @brief Re-reads the controls the view displays. Runs on the acquisition thread only.
    void refreshControls();

    dpasi::AsiCamera& camera_;                                  ///< Not owned; supplied connected by the caller.
    bool has_wb_;                                               ///< Whether white balance exists on this camera.

    std::thread worker_;                                        ///< The only thread that touches the camera.
    std::atomic<bool> running_;                                 ///< Cleared to ask the loop to finish.
    bool streaming_;                                            ///< Whether doStartVideoCapture() succeeded.

    mutable std::mutex frame_mtx_;                              ///< Guards the handover slot and its flag.
    dpasi::types::Frame ready_;                                 ///< The newest frame, waiting to be taken.
    bool ready_valid_;                                          ///< Whether ready_ holds a frame not yet taken.

    mutable std::mutex state_mtx_;                              ///< Guards the published snapshot.
    ModelState state_;                                          ///< What state() hands out.

    mutable std::mutex request_mtx_;                            ///< Guards the pending request queue.
    std::vector<std::function<void()>> requests_;               ///< Drained by the acquisition thread before a grab.

    std::chrono::steady_clock::time_point last_frame_at_;       ///< When the previous frame arrived.
    double expected_interval_ms_;                               ///< Moving estimate of the gap between frames.
};

// ---------------------------------------------------------------------------------------------------------------------

}   // END NAMESPACE

// ---------------------------------------------------------------------------------------------------------------------
