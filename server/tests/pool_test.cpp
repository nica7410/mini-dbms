#include "thread_pool.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Pool = minidb::ThreadPool;
using Submit = Pool::SubmitResult;
using namespace std::chrono_literals;

void check(bool condition, const char *expression, int line) {
    if (!condition) {
        throw std::runtime_error(std::string("line ") + std::to_string(line) +
                                 ": " + expression);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

class Gate {
public:
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        ++arrived_;
        changed_.notify_all();
        changed_.wait(lock, [this] { return open_; });
    }

    bool await_arrivals(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, 5s, [this, count] { return arrived_ >= count; });
    }

    void open() {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t arrived_ = 0;
    bool open_ = false;
};

class ScopeExit {
public:
    explicit ScopeExit(std::function<void()> fn) : fn_(std::move(fn)) {}
    ~ScopeExit() { fn_(); }
    ScopeExit(const ScopeExit &) = delete;
    ScopeExit &operator=(const ScopeExit &) = delete;

private:
    std::function<void()> fn_;
};

template <typename Predicate>
bool eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

void test_queue_limits_with_running_workers() {
    Gate gate;
    std::atomic<unsigned> ran{0};
    Pool pool(2, 2, 10);
    ScopeExit release([&] { gate.open(); });
    auto block = [&](std::size_t) { gate.wait(); ++ran; };
    CHECK(pool.try_submit(block) == Submit::Accepted);
    CHECK(pool.try_submit(block) == Submit::Accepted);
    CHECK(gate.await_arrivals(2));

    auto task = [&](std::size_t) { ++ran; };
    CHECK(pool.try_submit(task, 11) == Submit::Full);
    CHECK(pool.try_submit(task, 6) == Submit::Accepted);
    CHECK(pool.try_submit(task, 5) == Submit::Full);
    CHECK(pool.try_submit(task, 4) == Submit::Accepted);
    CHECK(pool.try_submit(task, 0) == Submit::Full);
    const auto blocked = pool.stats();
    CHECK(blocked.workers == 2 && blocked.active == 2 && blocked.peak_active == 2);
    CHECK(blocked.queued == 2 && blocked.queued_bytes == 10);
    CHECK(blocked.accepted == 4 && blocked.rejected == 3);

    gate.open();
    pool.shutdown();
    CHECK(ran == 4);
    CHECK(pool.stats().completed == 4 && pool.stats().failed_tasks == 0);
    CHECK(pool.stats().queued == 0 && pool.stats().queued_bytes == 0);
    CHECK(pool.stats().active == 0 && pool.stats().stopped);
    CHECK(pool.try_submit(task) == Submit::Stopped);
    CHECK(pool.stats().rejected == 4);
}

void test_queue_byte_overflow() {
    const auto maximum = std::numeric_limits<std::size_t>::max();
    Gate gate;
    Pool pool(1, 3, maximum);
    ScopeExit release([&] { gate.open(); });
    CHECK(pool.try_submit([&](std::size_t) { gate.wait(); }) == Submit::Accepted);
    CHECK(gate.await_arrivals(1));
    CHECK(pool.try_submit([](std::size_t) {}, maximum - 1) == Submit::Accepted);
    CHECK(pool.try_submit([](std::size_t) {}, 2) == Submit::Full);
    CHECK(pool.try_submit([](std::size_t) {}, 1) == Submit::Accepted);
    CHECK(pool.stats().queued_bytes == maximum);
    gate.open();
    pool.shutdown();
    CHECK(pool.stats().queued_bytes == 0 && pool.stats().completed == 3);
}

void test_fifo_single_worker() {
    Gate gate;
    std::vector<std::size_t> order;
    Pool pool(1, 16, 16);
    ScopeExit release([&] { gate.open(); });
    CHECK(pool.try_submit([&](std::size_t) { gate.wait(); }) == Submit::Accepted);
    CHECK(gate.await_arrivals(1));
    for (std::size_t index = 0; index < 12; ++index) {
        CHECK(pool.try_submit([&, index](std::size_t) { order.push_back(index); }, 1) ==
              Submit::Accepted);
    }
    gate.open();
    pool.shutdown();
    CHECK(order.size() == 12);
    for (std::size_t index = 0; index < order.size(); ++index) {
        CHECK(order[index] == index);
    }
}

void test_concurrent_shutdown_drains_and_joins() {
    Gate gate;
    std::atomic<unsigned> ran{0};
    std::atomic<unsigned> shutdown_started{0};
    std::atomic<unsigned> shutdown_finished{0};
    Pool pool(2, 8, 8);
    std::thread first;
    std::thread second;
    ScopeExit cleanup([&] {
        gate.open();
        if (first.joinable()) first.join();
        if (second.joinable()) second.join();
    });
    for (unsigned index = 0; index < 2; ++index) {
        CHECK(pool.try_submit([&](std::size_t) { gate.wait(); ++ran; }) == Submit::Accepted);
    }
    CHECK(gate.await_arrivals(2));
    for (unsigned index = 0; index < 3; ++index) {
        CHECK(pool.try_submit([&](std::size_t) { ++ran; }, 1) == Submit::Accepted);
    }
    auto shutdown = [&] {
        ++shutdown_started;
        pool.shutdown();
        ++shutdown_finished;
    };
    first = std::thread(shutdown);
    second = std::thread(shutdown);
    CHECK(eventually([&] { return shutdown_started == 2 && pool.stats().stopped; }));
    CHECK(shutdown_finished == 0);
    CHECK(pool.try_submit([](std::size_t) {}) == Submit::Stopped);
    gate.open();
    first.join();
    second.join();
    CHECK(shutdown_finished == 2 && ran == 5);
    CHECK(pool.stats().completed == 5 && pool.stats().active == 0);
    pool.shutdown();
    CHECK(pool.stats().completed == 5);
}

void test_concurrent_producers_execute_once() {
    constexpr std::size_t producer_count = 4;
    constexpr std::size_t per_producer = 300;
    constexpr std::size_t total = producer_count * per_producer;
    std::vector<std::atomic<unsigned>> seen(total);
    for (auto &value : seen) value.store(0);
    std::atomic<unsigned> rejected{0};
    std::atomic<unsigned> wrong_worker{0};
    std::array<std::thread::id, 4> worker_threads{};
    std::mutex identities_mutex;
    Pool pool(4, total, total);
    std::vector<std::thread> producers;
    ScopeExit join_producers([&] {
        for (auto &producer : producers) {
            if (producer.joinable()) producer.join();
        }
    });
    for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            for (std::size_t local = 0; local < per_producer; ++local) {
                const auto id = producer * per_producer + local;
                const auto result = pool.try_submit([&, id](std::size_t worker) {
                    ++seen[id];
                    std::lock_guard<std::mutex> lock(identities_mutex);
                    if (worker >= worker_threads.size()) {
                        ++wrong_worker;
                        return;
                    }
                    auto &identity = worker_threads[worker];
                    if (identity == std::thread::id{}) {
                        identity = std::this_thread::get_id();
                    } else if (identity != std::this_thread::get_id()) {
                        ++wrong_worker;
                    }
                }, 1);
                if (result != Submit::Accepted) ++rejected;
            }
        });
    }
    for (auto &producer : producers) producer.join();
    pool.shutdown();
    CHECK(rejected == 0 && wrong_worker == 0);
    for (const auto &value : seen) CHECK(value == 1);
    const auto done = pool.stats();
    CHECK(done.accepted == total && done.completed == total && done.failed_tasks == 0);
    CHECK(done.queued == 0 && done.queued_bytes == 0 && done.active == 0);
}

void test_exception_survival_and_self_shutdown_guard() {
    std::atomic<unsigned> after_exception{0};
    std::atomic<unsigned> guarded{0};
    Pool pool(1, 4, 4);
    CHECK(pool.try_submit([](std::size_t) { throw std::runtime_error("task failure"); }) ==
          Submit::Accepted);
    CHECK(pool.try_submit([&](std::size_t) {
        try {
            pool.shutdown();
        } catch (const std::logic_error &) {
            ++guarded;
        }
    }) == Submit::Accepted);
    CHECK(pool.try_submit([&](std::size_t) { ++after_exception; }) == Submit::Accepted);
    pool.shutdown();
    CHECK(guarded == 1 && after_exception == 1);
    CHECK(pool.stats().completed == 3 && pool.stats().failed_tasks == 1);
}

void test_invalid_configuration() {
    for (const auto limits : {std::array<std::size_t, 3>{0, 1, 1},
                              std::array<std::size_t, 3>{1, 0, 1},
                              std::array<std::size_t, 3>{1, 1, 0}}) {
        bool rejected = false;
        try {
            Pool invalid(limits[0], limits[1], limits[2]);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        CHECK(rejected);
    }
    Pool pool(1, 1, 1);
    bool rejected = false;
    try {
        pool.try_submit({});
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    CHECK(rejected && pool.stats().accepted == 0);
}

} // namespace

int main() {
    try {
        test_queue_limits_with_running_workers();
        test_queue_byte_overflow();
        test_fifo_single_worker();
        test_concurrent_shutdown_drains_and_joins();
        test_concurrent_producers_execute_once();
        test_exception_survival_and_self_shutdown_guard();
        test_invalid_configuration();
        std::cout << "Thread pool: 7 tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "Thread pool test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
