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

// =====================================================================================================================
// Implementation of the StatusPoller<StatusT> template declared in status_poller.h, split out so that the public
// header carries declarations and doxygen only. Never included directly: status_poller.h includes it at its end. The
// contract, the rationale for every design decision and the doxygen all live in the declaring header.
// =====================================================================================================================

#pragma once

// C++ INCLUDES
#include <utility>

// PROJECT INCLUDES
#include "DegorasASI/Helpers/status_poller.h"


// NAMESPACES
namespace dpasi
{

namespace detail
{

// How long stop() waits for the worker before detaching, when the caller does not say. Named because it is used
// TWICE -- by the constructor and by the short start() overload -- and two independent literals would silently
// disagree the first time one of them was revised. frame_pump.cpp names its own equivalent for the same reason.
constexpr int kDefaultJoinTimeoutMs = 2000;

}   // namespace detail

// ---------------------------------------------------------------------------------------------------------------------

template <class StatusT>
StatusPoller<StatusT>::State::State() :
    stop(false),
    done(false),
    running(false)
{}

template <class StatusT>
StatusPoller<StatusT>::StatusPoller() :
    state_(std::make_shared<State>()),
    join_timeout_(std::chrono::milliseconds(detail::kDefaultJoinTimeoutMs))
{}

template <class StatusT>
StatusPoller<StatusT>::~StatusPoller()
{
    this->stop();
}

template <class StatusT>
types::OperationResult StatusPoller<StatusT>::start(Producer producer, Sink sink, std::chrono::milliseconds interval)
{
    // Two seconds is the bound the constructor also establishes, and was this parameter's default argument.
    return this->start(std::move(producer), std::move(sink), interval,
                       std::chrono::milliseconds(detail::kDefaultJoinTimeoutMs));
}

template <class StatusT>
types::OperationResult StatusPoller<StatusT>::start(Producer producer,
                                                    Sink sink,
                                                    std::chrono::milliseconds interval,
                                                    std::chrono::milliseconds join_timeout)
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

template <class StatusT>
types::OperationResult StatusPoller<StatusT>::stop()
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

template <class StatusT>
bool StatusPoller<StatusT>::isRunning() const
{
    return this->state_->running.load();
}

template <class StatusT>
void StatusPoller<StatusT>::run(std::shared_ptr<State> st,
                                Producer producer,
                                Sink sink,
                                std::chrono::milliseconds interval)
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

// ---------------------------------------------------------------------------------------------------------------------

} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
