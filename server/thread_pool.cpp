#include "thread_pool.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace minidb {
namespace {

void add_saturated(std::uint64_t &counter, std::uint64_t amount = 1) noexcept {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    counter = amount > maximum - counter ? maximum : counter + amount;
}

} // namespace

thread_local const ThreadPool *ThreadPool::current_pool_ = nullptr;

ThreadPool::ThreadPool(std::size_t workers, std::size_t max_queue,
                       std::size_t max_queued_bytes)
    : max_queue_(max_queue), max_queued_bytes_(max_queued_bytes) {
    if (workers == 0 || max_queue == 0 || max_queued_bytes == 0) {
        throw std::invalid_argument("thread pool limits must be positive");
    }
    stats_.workers = workers;
    workers_.reserve(workers);
    try {
        for (std::size_t index = 0; index < workers; ++index) {
            workers_.emplace_back([this, index] { worker_loop(index); });
        }
    } catch (...) {
        // A failed constructor has no destructor to join already started threads.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        for (auto &worker : workers_) {
            worker.join();
        }
        throw;
    }
}

ThreadPool::~ThreadPool() noexcept {
    try {
        shutdown();
    } catch (...) {
        // A live worker must never continue through a destroyed pool object.
        std::terminate();
    }
}

ThreadPool::SubmitResult ThreadPool::try_submit(Task task, std::size_t bytes) {
    if (!task) {
        throw std::invalid_argument("thread pool task must not be empty");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            add_saturated(stats_.rejected);
            return SubmitResult::Stopped;
        }
        // Subtract before comparing so a size_t addition cannot wrap around.
        if (queue_.size() >= max_queue_ || bytes > max_queued_bytes_ ||
            queued_bytes_ > max_queued_bytes_ - bytes) {
            add_saturated(stats_.rejected);
            return SubmitResult::Full;
        }
        queue_.push_back(QueuedTask{std::move(task), bytes, Clock::now()});
        queued_bytes_ += bytes;
        add_saturated(stats_.accepted);
    }
    ready_.notify_one();
    return SubmitResult::Accepted;
}

void ThreadPool::shutdown() {
    // Check before taking shutdown_mutex_: an external caller might hold it
    // while joining the worker that is making this call.
    if (current_pool_ == this) {
        throw std::logic_error("a thread pool worker cannot shut down its own pool");
    }
    std::lock_guard<std::mutex> shutdown_lock(shutdown_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    for (auto &worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

PoolStats ThreadPool::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = stats_;
    result.queued = queue_.size();
    result.queued_bytes = queued_bytes_;
    result.stopped = stopping_;
    return result;
}

void ThreadPool::worker_loop(std::size_t worker_index) noexcept {
    current_pool_ = this;
    for (;;) {
        QueuedTask entry;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                // Stopping workers still drain every accepted queue entry.
                break;
            }
            entry = std::move(queue_.front());
            queue_.pop_front();
            queued_bytes_ -= entry.bytes;
            ++stats_.active;
            stats_.peak_active = std::max(stats_.peak_active, stats_.active);
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now() - entry.enqueued_at).count();
            const auto wait = elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
            add_saturated(stats_.queue_wait_us, wait);
            stats_.max_queue_wait_us = std::max(stats_.max_queue_wait_us, wait);
        }

        bool failed = false;
        try {
            entry.task(worker_index);
        } catch (...) {
            failed = true;
        }
        // Release user captures outside the queue mutex as well as executing them
        // outside it; a capture's destructor may itself call a pool operation.
        entry.task = {};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --stats_.active;
            add_saturated(stats_.completed);
            if (failed) {
                add_saturated(stats_.failed_tasks);
            }
        }
    }
    current_pool_ = nullptr;
}

} // namespace minidb
