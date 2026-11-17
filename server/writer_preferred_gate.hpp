#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace minidb::detail {

// Unlike std::shared_mutex, this gate explicitly prevents new readers from
// bypassing an already waiting writer. Existing readers finish their statements.
//
// Readers and writers wait on separate condition variables so a release wakes
// only threads that can make progress: one writer when a writer is waiting,
// otherwise every waiting reader. Notifications are sent after the mutex is
// released so a woken thread does not immediately block on it again.
class WriterPreferredGate {
public:
    void lock_shared() {
        std::unique_lock<std::mutex> lock(mutex_);
        readers_cv_.wait(lock, [this] { return can_read(); });
        ++readers_;
    }

    bool try_lock_shared() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!can_read()) return false;
        ++readers_;
        return true;
    }

    void unlock_shared() noexcept {
        bool wake_writer;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Readers only wait while a writer is active or waiting, and a writer
            // cannot be active here, so only a waiting writer needs waking.
            wake_writer = --readers_ == 0 && waiting_writers_ > 0;
        }
        if (wake_writer) writers_cv_.notify_one();
    }

    void lock() {
        std::unique_lock<std::mutex> lock(mutex_);
        ++waiting_writers_;
        try {
            writers_cv_.wait(lock, [this] { return !writer_ && readers_ == 0; });
        } catch (...) {
            --waiting_writers_;
            const bool wake_readers = waiting_writers_ == 0 && !writer_;
            lock.unlock();
            // This thread may have consumed a notify_one meant for a writer.
            writers_cv_.notify_one();
            if (wake_readers) readers_cv_.notify_all();
            throw;
        }
        --waiting_writers_;
        writer_ = true;
    }

    void unlock() noexcept {
        bool wake_writer;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            writer_ = false;
            wake_writer = waiting_writers_ > 0;
        }
        // Every unlock with waiting writers wakes one, so a writer that lost the
        // race to a newly arriving writer is woken again by that writer's unlock.
        if (wake_writer) writers_cv_.notify_one();
        else readers_cv_.notify_all();
    }

private:
    friend struct GateTestAccess;
    bool can_read() const noexcept { return !writer_ && waiting_writers_ == 0; }
    std::mutex mutex_;
    std::condition_variable readers_cv_;
    std::condition_variable writers_cv_;
    std::size_t readers_ = 0;
    std::size_t waiting_writers_ = 0;
    bool writer_ = false;
};


} // namespace minidb::detail
