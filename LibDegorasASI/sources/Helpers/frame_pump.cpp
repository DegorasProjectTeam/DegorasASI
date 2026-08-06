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
#include <utility>

// PROJECT INCLUDES
#include "LibDegorasASI/Helpers/frame_pump.h"


// NAMESPACES
namespace dpasi
{

// ---------------------------------------------------------------------------------------------------------------------

FramePump::~FramePump()
{
    this->stop();
}

types::OperationResult FramePump::start(Producer producer, Sink sink, std::chrono::milliseconds join_timeout)
{
    const std::lock_guard<std::mutex> lock(this->wk_mtx_);
    if (this->worker_.joinable())
        return types::OperationResult::WORKER_ALREADY_RUNNING;

    if (!producer || !sink)
        return types::OperationResult::INVALID_PARAMETER;

    this->join_timeout_ = join_timeout;

    // Fresh state per run, so the counters start from zero and any detached worker keeps its own copy.
    std::shared_ptr<State> st = std::make_shared<State>();
    st->running = true;
    {
        const std::lock_guard<std::mutex> plock(this->state_ptr_mtx_);
        this->state_ = st;
    }

    try
    {
        this->worker_ = std::thread(&FramePump::run, st, std::move(producer), std::move(sink));
    }
    catch (...)
    {
        st->running = false;
        return types::OperationResult::WORKER_START_ERROR;
    }
    return types::OperationResult::OPERATION_OK;
}

std::shared_ptr<FramePump::State> FramePump::currentState() const
{
    const std::lock_guard<std::mutex> plock(this->state_ptr_mtx_);
    return this->state_;
}

types::OperationResult FramePump::stop()
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

    // The worker may be inside a capture that the vendor SDK will not cancel, so this wait is bounded by the join
    // timeout rather than by the stop signal.
    std::unique_lock<std::mutex> dlock(this->state_->m);
    const bool finished = this->state_->cv.wait_for(dlock, this->join_timeout_,
                                                    [this]{ return this->state_->done; });
    dlock.unlock();

    if (finished)
    {
        this->worker_.join();
        return types::OperationResult::OPERATION_OK;
    }

    // Worker is wedged (e.g. a capture blocked on dead hardware): detach instead of hanging shutdown.
    this->worker_.detach();
    return types::OperationResult::OPERATION_TIMEOUT;
}

// The three observers below deliberately avoid wk_mtx_, so they remain callable from inside the sink callback: stop()
// holds wk_mtx_ while waiting for the worker, so an observer taking it would block against a concurrent stop() for the
// whole join timeout and end up forcing the worker to be detached.

bool FramePump::isRunning() const
{
    return this->currentState()->running.load();
}

std::uint64_t FramePump::deliveredCount() const
{
    return this->currentState()->delivered.load();
}

std::uint64_t FramePump::failedCount() const
{
    return this->currentState()->failed.load();
}

namespace
{

/// Floor between iterations after a FAILED capture. The success path never waits.
constexpr int kErrorBackoffMs = 5;

} // namespace

void FramePump::run(std::shared_ptr<State> st, Producer producer, Sink sink)
{
    // Whatever happens below -- including an exception escaping the producer or the sink -- the worker MUST mark itself
    // done and signal, or stop() would wait out its whole join timeout and detach a thread that has already died.
    struct DoneSignal
    {
        std::shared_ptr<State> st;
        ~DoneSignal()
        {
            st->running = false;
            const std::lock_guard<std::mutex> lock(st->m);
            st->done = true;
            st->cv.notify_all();
        }
    } done_signal{st};

    // ONE frame for the whole run. The producer only reallocates its buffer when the geometry changes, so a steady
    // stream costs no per-frame allocation, and the sink receives it by reference so nothing is copied on delivery.
    types::Frame frame;

    for (;;)
    {
        {
            const std::lock_guard<std::mutex> lock(st->m);
            if (st->stop)
                break;
        }

        // An exception must not escape a thread entry point: that is std::terminate, i.e. the whole application dies
        // because one frame could not be captured or queued. Both callables are therefore contained. A throwing
        // producer counts as a failed capture; a throwing sink costs that one delivery.
        types::OperationResult res = types::OperationResult::ACQUISITION_FAILED;
        try
        {
            res = producer(frame);
        }
        catch (...)
        {
            res = types::OperationResult::ACQUISITION_FAILED;
        }

        // Counted BEFORE delivery and via atomics, so a callback that reads the counters sees this frame included and
        // needs no lock that stop() could be holding.
        if (res == types::OperationResult::OPERATION_OK)
            ++st->delivered;
        else
            ++st->failed;

        try
        {
            sink(res, frame);   // Outside all locks; non-OK is delivered, never dropped.
        }
        catch (...)
        {
            // Swallowed deliberately: the pump has no channel through which to report a consumer's failure, and
            // killing the acquisition -- or the process -- because one callback threw would be far worse than
            // losing one frame.
        }

        // On SUCCESS there is no wait: the producer blocks until a frame arrives, so looping straight back is what
        // keeps up with the stream. On FAILURE there must be a floor. The producer is only *documented* to block, and
        // several real outcomes return instantly -- a zero timeout, a geometry that cannot be established, a camera
        // closed underneath us -- which without a floor would spin a core at ~10^6 iterations/s and flood the callback
        // at the same rate. The wait is on the very condition variable stop() signals, so cancellation stays immediate.
        std::unique_lock<std::mutex> lock(st->m);
        if (res != types::OperationResult::OPERATION_OK && !st->stop)
            st->cv.wait_for(lock, std::chrono::milliseconds(kErrorBackoffMs), [&]{ return st->stop; });
        if (st->stop)
            break;
    }
}

// ---------------------------------------------------------------------------------------------------------------------

} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
