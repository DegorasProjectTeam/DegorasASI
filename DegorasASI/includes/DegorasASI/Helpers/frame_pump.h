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
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

// PROJECT INCLUDES
#include "DegorasASI/Global/degorasasi_export.h"
#include "DegorasASI/Common/common_types.h"


// NAMESPACES
namespace dpasi
{

// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Background frame-acquisition worker that delivers each captured frame to a callback.
 *
 * @details Runs a dedicated thread that repeatedly calls a @ref Producer to capture one frame and hands it to a
 *          @ref Sink. Both callables are supplied at start() and copied into the worker, so the pump stores no pointer
 *          to the owning device. It exists so a consumer can process frames on its own terms — write them to disk,
 *          push them to a display, feed a pipeline — without owning an acquisition loop.
 *
 * Why this is not @ref StatusPoller:
 *  - The poller SLEEPS between polls; a frame pump must not, because its producer already blocks until a frame arrives.
 *  - The poller constructs a fresh payload each round. A frame pump reuses ONE frame across the whole run, so a steady
 *    stream costs no per-frame allocation, which is the entire point of the frame buffer's reuse design.
 *  - The sink receives the frame BY CONST REFERENCE, so delivering a multi-megabyte image copies nothing.
 *
 * Lifetime & threading contract (identical in spirit to @ref StatusPoller):
 *  - The sink is always invoked on the worker thread, never while holding any pump lock. Non-OK results ARE delivered
 *    (not silently dropped), so a consumer sees a timeout or a lost camera rather than just silence.
 *  - Cancellation is prompt on the pump's side: stop() signals a condition variable. It cannot, however, cancel a
 *    capture already blocked inside the vendor SDK, so the worst-case stop latency is one frame timeout.
 *  - stop() performs a BOUNDED join: if the worker does not finish within the join timeout it is DETACHED rather than
 *    joined, so a destructor can never hang.
 *  - Self-destruct safe: if the owner is destroyed from inside the sink callback, stop() runs on the worker thread and
 *    detaches instead of self-joining (which would deadlock). The worker holds its own shared_ptr to the internal
 *    State, so its post-callback stop-check remains valid even after the pump is destroyed.
 *
 * @warning A detached, wedged worker keeps calling its producer, which captured the owning device. Give start() a join
 *          timeout comfortably LONGER than the producer's own timeout so a healthy worker always joins and detaching
 *          stays reserved for genuinely stuck hardware. This is the same trade the reference library makes for its
 *          status worker: a bounded leak is deliberately preferred over a hanging shutdown.
 *
 * @note Generic, project-agnostic apart from its use of the Frame value type: a candidate to migrate into
 *       LibDegorasBase alongside StatusPoller and waitForCondition if it is ever shared across the Degoras libraries.
 */
class DEGORASASI_EXPORT FramePump
{
public:

    /// Captures one frame into @p frame and returns the outcome. Expected to BLOCK until a frame arrives or times out.
    using Producer = std::function<types::OperationResult(types::Frame&)>;

    /// Receives each capture's outcome and the frame. The frame is a reference into the pump's reusable buffer and is
    /// only valid for the duration of the call: copy what you need before returning.
    using Sink = std::function<void(types::OperationResult, const types::Frame&)>;

    /// @brief Establishes an idle pump: no worker, zeroed counters and the library's default join bound.
    FramePump();

    /// @brief Stops the worker (bounded join / detach) before destruction.
    ~FramePump();

    FramePump(const FramePump&) = delete;
    FramePump& operator=(const FramePump&) = delete;
    FramePump(FramePump&&) = delete;
    FramePump& operator=(FramePump&&) = delete;

    /**
     * @brief Start the acquisition worker with the library's default join bound.
     * @param producer Called in a loop to capture a frame. Must block rather than spin.
     * @param sink Called after each capture with the (result, frame).
     * @return OPERATION_OK on success, WORKER_ALREADY_RUNNING if already pumping, WORKER_START_ERROR on failure.
     *
     * @note An OVERLOAD, not a defaulted parameter, and the same applies everywhere in this library. A default
     *       argument is compiled into the CALLER, so revising it later would leave every already-built client running
     *       the old value until it is recompiled; a value carried by an overload lives in the library and travels with
     *       the shared object.
     */
    types::OperationResult start(Producer producer, Sink sink);

    /**
     * @brief Start the acquisition worker.
     * @param producer Called in a loop to capture a frame. Must block rather than spin.
     * @param sink Called after each capture with the (result, frame).
     * @param join_timeout Maximum time stop() waits for the worker before detaching it. Make it longer than the
     *                     producer's own timeout.
     * @return OPERATION_OK on success, WORKER_ALREADY_RUNNING if already pumping, WORKER_START_ERROR on failure.
     */
    types::OperationResult start(Producer producer, Sink sink, std::chrono::milliseconds join_timeout);

    /**
     * @brief Stop the acquisition worker.
     * @return OPERATION_OK if the worker stopped (or was detached after self-destruct), WORKER_NOT_RUNNING if it was
     *         not running, OPERATION_TIMEOUT if it had to be detached after exceeding the join timeout.
     * @note May block for up to one frame timeout: a capture already inside the vendor SDK cannot be cancelled.
     */
    types::OperationResult stop();

    /// @brief True while a worker thread is active.
    bool isRunning() const;

    /// @brief Frames delivered to the sink with an OK result since the last start().
    std::uint64_t deliveredCount() const;

    /// @brief Captures that returned a non-OK result since the last start() (typically timeouts).
    std::uint64_t failedCount() const;

    // The three observers above are SAFE TO CALL FROM INSIDE THE SINK CALLBACK, which is where a consumer most
    // naturally wants them. That is not free: stop() necessarily holds the worker-lifecycle lock while waiting for the
    // worker to finish, so an observer that took that same lock would block against a concurrent stop() until the join
    // timeout expired and the worker were detached. They therefore touch only the shared state block -- through
    // atomics, and reaching it through a dedicated pointer lock that is never held across a wait.

private:

    /// Shared sync block, kept alive by both the pump and the (possibly detached) worker.
    struct State
    {
        /// @brief Establishes a block for a run not yet started: not stopping, not done, not running, counters zero.
        State();

        std::mutex m;
        std::condition_variable cv;
        bool stop;
        bool done;
        std::atomic<bool> running;               ///< Observable without the worker-lifecycle lock.
        std::atomic<std::uint64_t> delivered;
        std::atomic<std::uint64_t> failed;
    };

    static void run(std::shared_ptr<State> st, Producer producer, Sink sink);

    /// @brief The current run's state block. Takes only @ref state_ptr_mtx_, so it never contends with stop()'s wait.
    std::shared_ptr<State> currentState() const;

    mutable std::mutex wk_mtx_;                    ///< Guards worker lifecycle (start/stop/join).
    mutable std::mutex state_ptr_mtx_;             ///< Guards ONLY the state_ pointer; never held across a wait.
    std::thread worker_;                           ///< The acquisition worker (joinable == running).
    std::shared_ptr<State> state_;                 ///< Current run's shared sync block.
    std::chrono::milliseconds join_timeout_;       ///< Bound on stop()'s join before detaching.
};

// ---------------------------------------------------------------------------------------------------------------------

} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
