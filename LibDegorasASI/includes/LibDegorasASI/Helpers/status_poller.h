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

#pragma once

// C++ INCLUDES
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

// PROJECT INCLUDES
#include "LibDegorasASI/Common/common_types.h"


// NAMESPACES
namespace dpasi
{

// ---------------------------------------------------------------------------------------------------------------------

/**
 * @brief Generic background status-polling worker, device- and SDK-agnostic.
 * @tparam StatusT Plain status payload produced each poll and delivered to the sink.
 *
 * @details Each poll interval the worker calls a @ref Producer to fill a @p StatusT and then hands the
 *          (result, status) pair to a @ref Sink. Both callables are supplied at start() and copied into the
 *          worker, so the poller stores no pointer to the owning device.
 *
 * Lifetime & threading contract:
 *  - The sink is always invoked on the worker thread, never while holding any poller lock. Non-OK results are
 *    delivered to the sink (not silently dropped) so the owner can surface a lost connection.
 *  - Cancellation is prompt: stop() signals a condition variable, so the worker wakes immediately instead of
 *    sleeping out the remaining interval.
 *  - stop() performs a BOUNDED join: if the worker does not finish within @p join_timeout (e.g. it is wedged in a
 *    blocking SDK call on unplugged hardware) it is DETACHED rather than joined, so a destructor can never hang.
 *  - Self-destruct safe: if the owner is destroyed from inside the sink callback, stop() runs on the worker thread;
 *    it detaches instead of self-joining (which would deadlock). The worker holds its own shared_ptr to the internal
 *    State, so its post-callback stop-check and notification remain valid even after the poller is destroyed.
 *
 * @warning A detached, wedged worker leaks for the remainder of the process. This is bounded to genuinely stuck
 *          hardware and is deliberately preferred over a hanging shutdown.
 *
 * @note This poller drives camera TELEMETRY (sensor temperature, dropped-frame count, exposure state), not frame
 *       delivery: the acquisition loop is owned by the caller, matching the vendor's own guidance that frames be
 *       retrieved from a dedicated thread.
 * @note Generic, project-agnostic infrastructure: candidate to migrate into LibDegorasBase if shared across the
 *       Degoras libraries, rather than reimplemented per project.
 */
template <class StatusT>
class StatusPoller
{
public:

    using Producer = std::function<types::OperationResult(StatusT&)>;          ///< Fills a status; returns its result.
    using Sink     = std::function<void(types::OperationResult, const StatusT&)>;  ///< Receives each poll outcome.

    StatusPoller() = default;

    /// @brief Stops the worker (bounded join / detach) before destruction.
    ~StatusPoller() { this->stop(); }

    StatusPoller(const StatusPoller&) = delete;
    StatusPoller& operator=(const StatusPoller&) = delete;
    StatusPoller(StatusPoller&&) = delete;
    StatusPoller& operator=(StatusPoller&&) = delete;

    /**
     * @brief Start the polling worker.
     * @param producer Called each interval to produce a status.
     * @param sink Called each interval with the produced (result, status).
     * @param interval Delay between polls.
     * @param join_timeout Maximum time stop() waits for the worker before detaching it.
     * @return OPERATION_OK on success, WORKER_ALREADY_RUNNING if already polling, WORKER_START_ERROR on failure.
     */
    types::OperationResult start(Producer producer,
                                 Sink sink,
                                 std::chrono::milliseconds interval,
                                 std::chrono::milliseconds join_timeout = std::chrono::milliseconds(2000))
    {
        const std::lock_guard<std::mutex> lock(this->wk_mtx_);
        if (this->worker_.joinable())
            return types::OperationResult::WORKER_ALREADY_RUNNING;

        this->join_timeout_ = join_timeout;
        this->state_ = std::make_shared<State>();   // Fresh state per run; any detached worker keeps its own copy.
        std::shared_ptr<State> st = this->state_;
        st->running = true;

        try
        {
            this->worker_ = std::thread(&StatusPoller::run, st, std::move(producer), std::move(sink), interval);
        }
        catch (...)
        {
            st->running = false;
            return types::OperationResult::WORKER_START_ERROR;
        }
        return types::OperationResult::OPERATION_OK;
    }

    /**
     * @brief Stop the polling worker.
     * @return OPERATION_OK if the worker stopped (or was detached after self-destruct), WORKER_NOT_RUNNING if it was
     *         not running, OPERATION_TIMEOUT if it had to be detached after exceeding the join timeout.
     */
    types::OperationResult stop()
    {
        const std::lock_guard<std::mutex> lock(this->wk_mtx_);
        if (!this->worker_.joinable())
            return types::OperationResult::WORKER_NOT_RUNNING;

        {
            const std::lock_guard<std::mutex> slock(this->state_->m);
            this->state_->stop = true;
        }
        this->state_->cv.notify_all();

        if (this->worker_.get_id() == std::this_thread::get_id())
        {
            // Called from within the worker (owner destroyed inside its own callback): a self-join would deadlock.
            this->worker_.detach();
            return types::OperationResult::OPERATION_OK;
        }

        std::unique_lock<std::mutex> dlock(this->state_->m);
        const bool finished = this->state_->cv.wait_for(dlock, this->join_timeout_,
                                                        [this]{ return this->state_->done; });
        dlock.unlock();

        if (finished)
        {
            this->worker_.join();
            return types::OperationResult::OPERATION_OK;
        }

        // Worker is wedged (e.g. blocking SDK call on dead hardware): detach instead of hanging shutdown.
        this->worker_.detach();
        return types::OperationResult::OPERATION_TIMEOUT;
    }

    /**
     * @brief True while a worker thread is active.
     * @note Safe to call from inside the sink callback. It deliberately does NOT take the worker-lifecycle lock:
     *       stop() holds that lock while waiting for the worker to finish, so an observer that took it would block
     *       against a concurrent stop() for the whole join timeout -- and stop() would then DETACH a perfectly healthy
     *       worker, leaving it running against a device its owner is about to tear down.
     * @warning This is a deliberate divergence from the same helper in the sibling reference library, which has the
     *          identical latent hazard; the fix belongs upstream when this file migrates to LibDegorasBase.
     */
    bool isRunning() const
    {
        return this->state_->running.load();
    }

private:

    /// Shared sync block, kept alive by both the poller and the (possibly detached) worker.
    struct State
    {
        std::mutex m;
        std::condition_variable cv;
        bool stop = false;
        bool done = false;
        std::atomic<bool> running{false};   ///< Observable without the worker-lifecycle lock.
    };

    static void run(std::shared_ptr<State> st, Producer producer, Sink sink, std::chrono::milliseconds interval)
    {
        for (;;)
        {
            {
                const std::lock_guard<std::mutex> lock(st->m);
                if (st->stop)
                    break;
            }

            StatusT status{};
            const types::OperationResult res = producer(status);
            sink(res, status);   // Outside all locks; non-OK is delivered, never dropped.

            std::unique_lock<std::mutex> lock(st->m);
            st->cv.wait_for(lock, interval, [&]{ return st->stop; });
            if (st->stop)
                break;
        }

        st->running = false;
        const std::lock_guard<std::mutex> lock(st->m);
        st->done = true;
        st->cv.notify_all();
    }

    mutable std::mutex wk_mtx_;                                  ///< Guards worker lifecycle (start/stop/join).
    std::thread worker_;                                        ///< The polling worker (joinable == running).
    std::shared_ptr<State> state_ = std::make_shared<State>();  ///< Current run's shared sync block.
    std::chrono::milliseconds join_timeout_{2000};              ///< Bound on stop()'s join before detaching.
};

// ---------------------------------------------------------------------------------------------------------------------

} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
