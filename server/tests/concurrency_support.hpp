#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace minidb::test {
inline void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

class Event {
public:
    void set() {
        std::lock_guard<std::mutex> lock(mutex_);
        set_ = true;
        changed_.notify_all();
    }
    bool is_set() {
        std::lock_guard<std::mutex> lock(mutex_);
        return set_;
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        require(changed_.wait_for(lock, std::chrono::seconds(5), [&] { return set_; }),
                "event deadline exceeded");
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool set_ = false;
};

template <typename Predicate>
void await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline, "state observation deadline exceeded");
        std::this_thread::yield();
    }
}

// Unblock before joining even when an assertion or thread creation throws.
class Threads {
public:
    explicit Threads(std::function<void()> unblock) : unblock_(std::move(unblock)) {}
    ~Threads() {
        unblock_();
        join();
    }
    void start(std::function<void()> function) {
        std::packaged_task<void()> task(std::move(function));
        results_.push_back(task.get_future());
        threads_.emplace_back(std::move(task));
    }
    void finish() {
        join();
        for (auto& result : results_) result.get();
    }
private:
    void join() {
        for (auto& thread : threads_) if (thread.joinable()) thread.join();
    }
    std::function<void()> unblock_;
    std::vector<std::thread> threads_;
    std::vector<std::future<void>> results_;
};
} // namespace minidb::test
