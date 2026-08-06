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
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <numeric>
#include <thread>

// PROJECT INCLUDES (module aggregators)
#include <LibDegorasASI/Modules/Devices>


// Example: CALLBACK-DRIVEN frame acquisition. The library runs the acquisition loop on its own worker and hands each
// frame to a callback, so the application never blocks a thread of its own on a capture call.
//
// It shows the two things that matter when doing this for real:
//   1. The callback runs on the acquisition worker, so it must be SHORT. Whatever time it spends is time not spent
//      capturing, and it shows up directly as dropped frames.
//   2. The frame handed to the callback is a reference into the worker's reusable buffer and dies when the callback
//      returns. Anything kept beyond that must be copied -- so the realistic pattern is "copy into a queue, process on
//      your own thread", which is exactly what the second half of this example does.
//
// Takes an optional camera id and otherwise uses the first discovered camera.

using namespace dpasi;
using dpasi::types::OperationResult;

/// The camera model this example is demonstrated on. One class drives every ASI model; point this at another model
/// and the same code drives that camera unchanged.
constexpr const char* kModel = "ASI224MC";

namespace
{

/// Mean pixel value: a cheap stand-in for whatever real processing a consumer would do.
double meanByte(const types::Frame& frame)
{
    if (frame.data.empty())
        return 0.0;
    const std::uint64_t sum = std::accumulate(frame.data.cbegin(), frame.data.cend(), std::uint64_t(0),
                                              [](std::uint64_t acc, types::PixelByte b)
                                              { return acc + static_cast<std::uint64_t>(b); });
    return static_cast<double>(sum) / static_cast<double>(frame.data.size());
}

/**
 * @brief A hand-off queue: the acquisition callback copies frames in, a worker thread processes them out.
 * @note This is the pattern to copy for real work. The callback stays short (one copy) and heavy processing happens
 *       off the acquisition thread, so a slow consumer costs latency instead of dropped frames. The queue is bounded
 *       and drops the OLDEST frame when full, so a consumer that cannot keep up degrades predictably rather than
 *       growing without limit.
 */
class FrameQueue
{
public:

    explicit FrameQueue(std::size_t capacity) : capacity_(capacity) {}

    void push(const types::Frame& frame)
    {
        {
            const std::lock_guard<std::mutex> lock(this->mtx_);
            if (this->queue_.size() >= this->capacity_)
            {
                this->queue_.pop_front();
                ++this->discarded_;
            }
            this->queue_.push_back(frame);   // The copy that decouples the two threads.
        }
        this->cv_.notify_one();
    }

    /// @brief Pop a frame, or return false when the queue is closed and drained.
    bool pop(types::Frame& out)
    {
        std::unique_lock<std::mutex> lock(this->mtx_);
        this->cv_.wait(lock, [this]{ return !this->queue_.empty() || this->closed_; });
        if (this->queue_.empty())
            return false;
        out = std::move(this->queue_.front());
        this->queue_.pop_front();
        return true;
    }

    void close()
    {
        {
            const std::lock_guard<std::mutex> lock(this->mtx_);
            this->closed_ = true;
        }
        this->cv_.notify_all();
    }

    std::size_t discarded() const
    {
        const std::lock_guard<std::mutex> lock(this->mtx_);
        return this->discarded_;
    }

private:

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<types::Frame> queue_;
    std::size_t capacity_;
    std::size_t discarded_ = 0;
    bool closed_ = false;
};

} // namespace

int main(int argc, char* argv[])
{
    types::CameraDescriptorList cameras;
    if (AsiCamera::getDeviceList(kModel, cameras) != OperationResult::OPERATION_OK || cameras.empty())
    {
        std::cout << "No " << kModel << " found. Connect one and retry.\n";
        return 1;
    }

    types::CameraId id = cameras.front().id;
    if (argc > 1)
        id = static_cast<types::CameraId>(std::atoi(argv[1]));

    AsiCamera camera(id);
    if (camera.doConnect() != OperationResult::OPERATION_OK)
    {
        std::cout << "Could not connect to camera id " << types::toType(id) << "\n";
        return 1;
    }

    types::RoiFormat roi;
    roi.width = 640;
    roi.height = 480;
    roi.bin = 1;
    roi.format = types::ImageFormat::RAW8;
    camera.doSetRoi(roi, types::RoiPosition());
    camera.doSetExposure(std::chrono::milliseconds(20));

    // -- Part 1: process directly inside the callback ------------------------------------------------------------------
    // Fine when the work is trivial, as here. The callback runs on the acquisition worker.
    std::cout << "-- part 1: processing inside the callback --\n";

    std::atomic<int> seen{0};
    camera.setNewFrameCb([&seen](OperationResult res, const types::Frame& frame)
    {
        if (res != OperationResult::OPERATION_OK)
        {
            std::cout << "  capture failed: " << types::toString(res) << "\n";
            return;
        }
        const int n = ++seen;
        if (n <= 3)
            std::cout << "  frame " << frame.sequence << ": " << frame.width << "x" << frame.height
                      << " " << types::toString(frame.format)
                      << ", " << frame.data.size() << " bytes, mean " << meanByte(frame) << "\n";
    });

    // The stream is started explicitly; the worker only pumps it. Keeping these separate means starting a worker never
    // silently reconfigures the hardware.
    camera.doStartVideoCapture();
    std::cout << "  startFrameAcquisition: "
              << types::toString(camera.startFrameAcquisition(std::chrono::milliseconds(1000))) << "\n";

    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::cout << "  stopFrameAcquisition:  " << types::toString(camera.stopFrameAcquisition()) << "\n";
    std::cout << "  delivered " << camera.getAcquiredFrameCount() << " frames, "
              << camera.getFailedFrameCount() << " failures, " << seen.load() << " seen by the callback\n";

    // -- Part 2: hand off to a processing thread -----------------------------------------------------------------------
    // The pattern for real work: the callback only copies, a separate thread does the processing.
    std::cout << "\n-- part 2: handing frames to a processing thread --\n";

    FrameQueue queue(8);
    std::atomic<int> processed{0};

    std::thread consumer([&queue, &processed]
    {
        types::Frame frame;
        while (queue.pop(frame))
        {
            // Stand-in for real processing (debayering, stacking, writing to disk...). Deliberately slower than the
            // frame rate, to show the queue absorbing the mismatch instead of the camera dropping frames.
            volatile double sink = meanByte(frame);
            (void)sink;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            ++processed;
        }
    });

    camera.setNewFrameCb([&queue](OperationResult res, const types::Frame& frame)
    {
        // Short and predictable: one copy, then straight back to capturing. The frame reference dies when this returns,
        // which is precisely why the copy is necessary.
        if (res == OperationResult::OPERATION_OK)
            queue.push(frame);
    });

    camera.startFrameAcquisition(std::chrono::milliseconds(1000));
    std::this_thread::sleep_for(std::chrono::seconds(3));
    camera.stopFrameAcquisition();

    queue.close();
    consumer.join();

    int dropped = 0;
    camera.getDroppedFrames(dropped);
    std::cout << "  acquired " << camera.getAcquiredFrameCount() << " frames"
              << ", processed " << processed.load()
              << ", queue discarded " << queue.discarded()
              << ", camera dropped " << dropped << "\n";

    // -- Teardown ------------------------------------------------------------------------------------------------------
    // Stopping the stream also stops the worker, and so does disconnecting, and so does the destructor. Each is bounded.
    std::cout << "\n-- teardown --\n";
    std::cout << "  doStopVideoCapture: " << types::toString(camera.doStopVideoCapture()) << "\n";
    std::cout << "  frame worker running after stop: "
              << (camera.isFrameAcquisitionRunning() ? "yes" : "no") << "\n";
    std::cout << "  doDisconnect: " << types::toString(camera.doDisconnect()) << "\n";

    return 0;
}
