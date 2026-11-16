#pragma once

#include <mutex>

namespace minidb::detail {
// Benchmark-only baseline: same W readers, but all DB executions serialize.
// This is not a single-Table memory/startup baseline.
class SerialGate {
public:
    void lock() { mutex_.lock(); }
    void unlock() { mutex_.unlock(); }
    void lock_shared() { lock(); }
    void unlock_shared() { unlock(); }
private:
    std::mutex mutex_;
};
} // namespace minidb::detail
