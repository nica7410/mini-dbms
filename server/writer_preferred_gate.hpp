#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace minidb::detail {

// Unlike std::shared_mutex, this gate explicitly prevents new readers from
// bypassing an already waiting writer. Existing readers finish their statements.
class WriterPreferredGate {
public:
    void lock_shared() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return can_read(); });
        ++readers_;
    }

    bool try_lock_shared() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!can_read()) return false;
        ++readers_;
        return true;
    }

    void unlock_shared() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (--readers_ == 0) cv_.notify_all();
    }

    void lock() {
        std::unique_lock<std::mutex> lock(mutex_);
        ++waiting_writers_;
        try {
            cv_.wait(lock, [this] { return !writer_ && readers_ == 0; });
        } catch (...) {
            --waiting_writers_;
            cv_.notify_all();
            throw;
        }
        --waiting_writers_;
        writer_ = true;
    }

    void unlock() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        writer_ = false;
        cv_.notify_all();
    }

private:
    friend struct GateTestAccess;
    bool can_read() const noexcept { return !writer_ && waiting_writers_ == 0; }
    std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t readers_ = 0;
    std::size_t waiting_writers_ = 0;
    bool writer_ = false;
};


} // namespace minidb::detail
