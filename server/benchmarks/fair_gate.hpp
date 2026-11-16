#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace minidb::detail {

// Benchmark-only candidate: arrival-order (ticket) reader/writer gate.
// Every lock request takes a ticket when it registers. Requests are admitted
// strictly in ticket order, so a later reader cannot pass a waiting writer and
// a later writer cannot pass a waiting reader. Consecutive readers are admitted
// together. A 64-bit ticket counter does not wrap in practice.
class FairGate {
public:
    void lock_shared() {
        std::unique_lock<std::mutex> lock(mutex_);
        const std::uint64_t ticket = next_ticket_++;
        cv_.wait(lock, [&] { return serving_ == ticket && !writer_; });
        ++serving_;
        ++readers_;
        cv_.notify_all();  // the next ticket may be another reader
    }

    bool try_lock_shared() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (writer_ || serving_ != next_ticket_) return false;  // someone is waiting
        ++next_ticket_;
        ++serving_;
        ++readers_;
        return true;
    }

    void unlock_shared() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (--readers_ == 0) cv_.notify_all();
    }

    void lock() {
        std::unique_lock<std::mutex> lock(mutex_);
        const std::uint64_t ticket = next_ticket_++;
        cv_.wait(lock, [&] { return serving_ == ticket && !writer_ && readers_ == 0; });
        ++serving_;
        writer_ = true;
    }

    void unlock() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        writer_ = false;
        cv_.notify_all();
    }

private:
    friend struct FairGateTestAccess;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::uint64_t next_ticket_ = 0;
    std::uint64_t serving_ = 0;
    std::uint64_t readers_ = 0;
    bool writer_ = false;
};

} // namespace minidb::detail
