#ifndef MINIDB_SERVER_THREAD_POOL_HPP
#define MINIDB_SERVER_THREAD_POOL_HPP

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace minidb {

struct PoolStats {
    std::size_t workers = 0;
    std::size_t queued = 0;
    std::size_t queued_bytes = 0;
    std::size_t active = 0;
    std::size_t peak_active = 0;
    std::uint64_t accepted = 0;
    // Completed includes failed tasks. Failed tasks are also counted separately.
    std::uint64_t completed = 0;
    std::uint64_t rejected = 0;
    std::uint64_t failed_tasks = 0;
    // Sum and maximum of enqueue-to-start waits, in microseconds.
    std::uint64_t queue_wait_us = 0;
    std::uint64_t max_queue_wait_us = 0;
    bool stopped = false;
};

class ThreadPool {
public:
    // An index identifies one stable worker thread for the pool's lifetime.
    using Task = std::function<void(std::size_t)>;

    enum class SubmitResult { Accepted, Full, Stopped };

    // All limits must be positive. Queue limits exclude already running tasks.
    ThreadPool(std::size_t workers, std::size_t max_queue,
               std::size_t max_queued_bytes);
    ~ThreadPool() noexcept;

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;
    ThreadPool(ThreadPool &&) = delete;
    ThreadPool &operator=(ThreadPool &&) = delete;

    // Does not wait for queue capacity. bytes is the caller's accounting weight.
    // Empty tasks are invalid. Allocation failures propagate without acceptance.
    SubmitResult try_submit(Task task, std::size_t bytes = 0);

    // Stop acceptance, drain accepted tasks, then join workers. External callers
    // may call this concurrently or repeatedly. Calling from this pool's own
    // worker throws logic_error before changing pool state, avoiding self-join.
    // The owner must outlive callers/tasks; destroying it on a worker is invalid.
    void shutdown();

    PoolStats stats() const;

private:
    using Clock = std::chrono::steady_clock;

    struct QueuedTask {
        Task task;
        std::size_t bytes = 0;
        Clock::time_point enqueued_at;
    };

    void worker_loop(std::size_t worker_index) noexcept;

    const std::size_t max_queue_;
    const std::size_t max_queued_bytes_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<QueuedTask> queue_;
    std::size_t queued_bytes_ = 0;
    bool stopping_ = false;
    PoolStats stats_;
    std::vector<std::thread> workers_;
    // Keep concurrent join operations away from the same std::thread objects.
    std::mutex shutdown_mutex_;

    static thread_local const ThreadPool *current_pool_;
};

} // namespace minidb

#endif
