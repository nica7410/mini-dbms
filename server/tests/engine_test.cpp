#include "engine.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <csignal>
#include <sys/resource.h>
#endif

namespace {

using minidb::Engine;
using minidb::EngineOptions;
using minidb::QueryStatus;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class TempDirectory {
public:
    TempDirectory() {
        static std::atomic<unsigned> serial{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("minidb-engine-test-" + std::to_string(stamp) + "-" +
                std::to_string(serial.fetch_add(1)));
        std::filesystem::create_directories(path);
    }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    void seed(std::size_t count, std::size_t name_length = 0) const {
        std::ofstream file(path / "users.tsv", std::ios::binary);
        for (std::size_t id = 1; id <= count; ++id) {
            file << id << '\t';
            if (name_length == 0) file << "user" << id;
            else file << std::string(name_length, 'x');
            file << '\t' << id % 151 << '\n';
        }
        file.close();
        require(static_cast<bool>(file), "unable to seed the test database");
    }
    EngineOptions options(std::size_t readers = 4) const {
        EngineOptions out;
        out.data_dir = path.string();
        out.readers = readers;
        return out;
    }
    std::filesystem::path path;
};

class StartBarrier {
public:
    explicit StartBarrier(std::size_t count) : remaining_(count) {}
    void arrive() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (--remaining_ == 0) cv_.notify_all();
        else cv_.wait(lock, [this] { return remaining_ == 0; });
    }
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t remaining_;
};

void parallel(std::size_t count, const std::function<void(std::size_t)>& function) {
    StartBarrier barrier(count);
    std::vector<std::thread> threads;
    std::mutex error_mutex;
    std::string failure;
    for (std::size_t worker = 0; worker < count; ++worker) {
        threads.emplace_back([&, worker] {
            barrier.arrive();
            try {
                function(worker);
            } catch (const std::exception& error) {
                std::lock_guard<std::mutex> lock(error_mutex);
                if (failure.empty()) failure = error.what();
            }
        });
    }
    for (auto& thread : threads) thread.join();
    if (!failure.empty()) throw std::runtime_error(failure);
}

void test_new_rows_visible_and_eof() {
    TempDirectory directory;
    directory.seed(2);
    const auto options = directory.options(2);
    {
        Engine engine(options);
        auto old = engine.execute(0, "SELECT * FROM users WHERE id = 1;");
        require(old.status == QueryStatus::Ok && old.rows.size() == 1, "initial index hit failed");
        require(old.rows[0].name == "user1", "initial row name differs");

        auto eof = engine.execute(0, "SELECT * FROM users;");
        require(eof.status == QueryStatus::Ok && eof.rows.size() == 2, "initial EOF scan failed");
        auto inserted = engine.execute(1, "INSERT INTO users (name, age) VALUES ('O''Reilly', 37);");
        require(inserted.status == QueryStatus::Ok && inserted.committed && inserted.inserted_id == 3,
                "insert did not expose its committed ID");

        auto fresh = engine.execute(0, "SELECT * FROM users WHERE id = 3;");
        require(fresh.status == QueryStatus::Ok && fresh.rows.size() == 1 &&
                fresh.rows[0].name == "O'Reilly", "another worker did not see the shared index entry");
        auto rescan = engine.execute(0, "SELECT * FROM users;");
        require(rescan.status == QueryStatus::Ok && rescan.rows.size() == 3,
                "reader that reached EOF did not observe the appended row");
        auto missing = engine.execute(1, "SELECT * FROM users WHERE id = 900;");
        require(missing.status == QueryStatus::Ok && missing.rows.empty(), "missing ID is not an empty success");
        require(old.rows[0].name == "user1", "result retained a borrowed C row buffer");
        require(engine.healthy(), "successful requests damaged health state");
    }
    // The destructor must release every handle, and the original C open path
    // must rebuild the index from committed rows without any new file format.
    Engine reopened(options);
    auto fresh = reopened.execute(1, "SELECT * FROM users WHERE id = 3;");
    require(fresh.status == QueryStatus::Ok && fresh.rows.size() == 1, "reopen lost the committed row");
}

void test_gap_ids_and_new_rows() {
    TempDirectory directory;
    {
        // IDs only need to increase, so a gap (3, 4) is a valid stored state.
        std::ofstream file(directory.path / "users.tsv", std::ios::binary);
        file << "1\ta\t1\n2\tb\t2\n5\tc\t5\n";
        file.close();
        require(static_cast<bool>(file), "unable to write the gap fixture");
    }
    Engine engine(directory.options(2));
    const auto lookup = [&](std::size_t worker, std::int64_t id) {
        return engine.execute(worker, "SELECT * FROM users WHERE id = " + std::to_string(id) + ";");
    };
    const auto expect_empty = [&](std::size_t worker, std::int64_t id, const char* message) {
        const auto result = lookup(worker, id);
        require(result.status == QueryStatus::Ok && result.rows.empty(), message);
    };

    expect_empty(0, 3, "gap ID was found before any insert");
    expect_empty(1, 900, "future ID was found before any insert");
    expect_empty(0, 0, "ID 0 was found");

    auto inserted = engine.execute(1, "INSERT INTO users (name, age) VALUES ('new', 6);");
    require(inserted.status == QueryStatus::Ok && inserted.inserted_id == 6, "insert after a gap failed");
    auto second = engine.execute(0, "INSERT INTO users (name, age) VALUES ('newer', 7);");
    require(second.status == QueryStatus::Ok && second.inserted_id == 7, "second insert failed");

    expect_empty(0, 4, "gap ID was found after inserts");
    expect_empty(1, -1, "negative ID was found");

    auto old = lookup(1, 5);
    require(old.status == QueryStatus::Ok && old.rows.size() == 1 && old.rows[0].name == "c",
            "startup row was not found");

    // Rows inserted through one worker are visible to every worker at once.
    auto fresh = lookup(0, 6);
    require(fresh.status == QueryStatus::Ok && fresh.rows.size() == 1 && fresh.rows[0].name == "new",
            "first inserted ID was not found");
    fresh = lookup(1, 7);
    require(fresh.status == QueryStatus::Ok && fresh.rows.size() == 1 && fresh.rows[0].name == "newer",
            "later inserted ID was not found");
    expect_empty(0, 8, "unassigned ID was found");
    expect_empty(1, 999999, "future ID was found");

    // A previously missing ID must become visible as soon as it is inserted.
    auto third = engine.execute(0, "INSERT INTO users (name, age) VALUES ('now-present', 8);");
    require(third.status == QueryStatus::Ok && third.inserted_id == 8, "third insert failed");
    fresh = lookup(1, 8);
    require(fresh.status == QueryStatus::Ok && fresh.rows.size() == 1 &&
            fresh.rows[0].name == "now-present", "an earlier miss hid a later insert");
    require(engine.healthy(), "index misses damaged health state");
}

void test_insert_at_int64_limit() {
    TempDirectory directory;
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    {
        std::ofstream file(directory.path / "users.tsv", std::ios::binary);
        file << maximum - 1 << "\tprevious\t1\n";
        file.close();
        require(static_cast<bool>(file), "unable to write the INT64 boundary fixture");
    }
    const auto options = directory.options(2);
    const std::string last_query = "SELECT * FROM users WHERE id = " + std::to_string(maximum) + ";";
    {
        Engine engine(options);
        auto missing = engine.execute(0, last_query);
        require(missing.status == QueryStatus::Ok && missing.rows.empty(), "unassigned INT64_MAX was found");
        auto inserted = engine.execute(1, "INSERT INTO users (name, age) VALUES ('last', 2);");
        require(inserted.status == QueryStatus::Ok && inserted.committed && inserted.inserted_id == maximum,
                "last representable ID was not inserted");
        auto found = engine.execute(0, last_query);
        require(found.status == QueryStatus::Ok && found.rows.size() == 1 && found.rows[0].name == "last",
                "row with ID INT64_MAX was not found");

        const auto exhausted = engine.execute(0, "INSERT INTO users (name, age) VALUES ('overflow', 3);");
        require(exhausted.status == QueryStatus::StorageError && !exhausted.committed && engine.healthy(),
                "ID exhaustion changed the storage-health contract");
        found = engine.execute(1, last_query);
        require(found.status == QueryStatus::Ok && found.rows.size() == 1 && found.rows[0].id == maximum,
                "failed insert damaged the stored row");
        missing = engine.execute(0, "SELECT * FROM users WHERE id = 0;");
        require(missing.status == QueryStatus::Ok && missing.rows.empty(), "ID 0 was found after exhaustion");
    }
    Engine reopened(options);
    auto found = reopened.execute(1, last_query);
    require(found.status == QueryStatus::Ok && found.rows.size() == 1,
            "restart did not rebuild the index entry for the last row");
    const auto exhausted = reopened.execute(0, "INSERT INTO users (name, age) VALUES ('still-full', 4);");
    require(exhausted.status == QueryStatus::StorageError && !exhausted.committed, "reopen allowed ID overflow");
    auto missing = reopened.execute(0, "SELECT * FROM users WHERE id = 0;");
    require(missing.status == QueryStatus::Ok && missing.rows.empty(), "ID 0 was found after reopen");
}

void test_input_is_validated_before_writes() {
    TempDirectory directory;
    directory.seed(1);
    Engine engine(directory.options(1));
    const std::string insert = "INSERT INTO users (name, age) VALUES ('must-not-exist', 20);";
    for (const auto& sql : std::vector<std::string>{
             insert + " SELECT * FROM users;", insert + " malformed suffix", insert + std::string(1, '\0'),
             std::string(), std::string("/* comment only */")}) {
        auto result = engine.execute(0, sql);
        require(result.status == QueryStatus::InvalidSql && !result.committed,
                "invalid or multiple statements were executed");
    }
    auto invalid = engine.execute(0, "INSERT INTO users (name, age) VALUES ('', 20);");
    require(invalid.status == QueryStatus::InvalidValues && !invalid.committed, "empty name was not rejected");
    invalid = engine.execute(0, "INSERT INTO users (name, age) VALUES ('invalid', 151);");
    require(invalid.status == QueryStatus::InvalidValues && !invalid.committed, "invalid age was not rejected");
    auto missing = engine.execute(0, "SELECT * FROM users WHERE id = 2;");
    require(missing.status == QueryStatus::Ok && missing.rows.empty(),
            "a rejected input created a row");
    auto result = engine.execute(0, "SELECT * FROM users;");
    require(result.status == QueryStatus::Ok && result.rows.size() == 1,
            "an invalid request partially inserted data");
    result = engine.execute(0, "INSERT INTO users (name, age) VALUES ('valid', 20); -- trailing comment\n");
    require(result.status == QueryStatus::Ok && result.inserted_id == 2, "trailing comment was rejected");
    result = engine.execute(0, "SELECT * FROM users WHERE id = 2;");
    require(result.status == QueryStatus::Ok && result.rows.size() == 1 && result.rows[0].name == "valid",
            "first valid insert after rejected inputs was hidden");
    result = engine.execute(1, "SELECT * FROM users;");
    require(result.status == QueryStatus::InternalError, "invalid worker index was accepted");
    require(engine.healthy(), "client errors poisoned the database");

    auto small_options = directory.options(1);
    small_options.max_sql_bytes = 8;
    Engine small(small_options);
    result = small.execute(0, insert);
    require(result.status == QueryStatus::InvalidSql && !result.committed, "oversized SQL was executed");
}

void test_result_limits_are_not_storage_errors() {
    TempDirectory directory;
    directory.seed(2, 255);
    auto options = directory.options(1);
    options.max_rows = 1;
    {
        Engine engine(options);
        auto result = engine.execute(0, "SELECT * FROM users;");
        require(result.status == QueryStatus::ResultTooLarge && result.rows.empty(),
                "row limit returned a partial success");
        require(engine.healthy(), "row cap poisoned the database");
        result = engine.execute(0, "SELECT * FROM users WHERE id = 1;");
        require(result.status == QueryStatus::Ok && result.rows.size() == 1,
                "a capped scan damaged its reader cursor");
    }
    options.max_rows = 10;
    options.max_result_bytes = 512;
    Engine bytes(options);
    const auto result = bytes.execute(0, "SELECT * FROM users WHERE id = 1;");
    require(result.status == QueryStatus::ResultTooLarge && result.rows.empty(), "byte cap was ignored");
    require(bytes.healthy(), "byte cap poisoned the database");
}

void test_lock_wait_categories() {
    TempDirectory directory;
    directory.seed(1);
    Engine engine(directory.options(2));
    for (const auto* sql : {"SELECT * FROM users WHERE id = 1;",
                            "SELECT * FROM users WHERE id = 100;",
                            "INSERT INTO users (name, age) VALUES ('new', 2);",
                            "SELECT * FROM users WHERE id = 2;"}) {
        require(engine.execute(0, sql).status == QueryStatus::Ok, "metric fixture query failed");
    }
    require(engine.execute(1, "invalid").status == QueryStatus::InvalidSql, "invalid SQL was accepted");
    const auto stats = engine.stats();
    // reader_mutex, read_gate, write_gate
    const std::uint64_t expected[] = {3, 3, 1};
    static_assert(sizeof expected / sizeof expected[0] == static_cast<std::size_t>(minidb::LockKind::Count),
                  "expected counts must cover every lock category");
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < stats.lock_wait.size(); ++i) {
        const auto& metric = stats.lock_wait[i];
        require(metric.count == expected[i], "lock acquisitions assigned to the wrong category");
        require(metric.max_us <= metric.total_us, "maximum wait exceeds total wait");
        total += metric.total_us;
    }
    require(total == stats.lock_wait_us, "quiescent wait totals disagree");
}

void test_parallel_readers() {
    TempDirectory directory;
    constexpr std::size_t count = 6000;
    directory.seed(count);
    Engine engine(directory.options(4));
    parallel(4, [&](std::size_t worker) {
        for (int iteration = 0; iteration < 12; ++iteration) {
            const auto id = 1 + (worker * 701 + static_cast<std::size_t>(iteration) * 101) % count;
            auto point = engine.execute(worker, "SELECT * FROM users WHERE id = " + std::to_string(id) + ";");
            require(point.status == QueryStatus::Ok && point.rows.size() == 1 &&
                    point.rows[0].id == static_cast<std::int64_t>(id), "parallel point query read another cursor");
            auto scan = engine.execute(worker, "SELECT * FROM users;");
            require(scan.status == QueryStatus::Ok && scan.rows.size() == count,
                    "parallel scan skipped or duplicated rows");
            for (std::size_t index = 0; index < count; ++index) {
                require(scan.rows[index].id == static_cast<std::int64_t>(index + 1),
                        "parallel scan returned out-of-order IDs");
            }
        }
    });
    const auto stats = engine.stats();
    require(stats.requests == 96 && stats.successful_selects == 96 && stats.errors == 0,
            "parallel execution statistics lost requests");
    require(stats.current_executions == 0, "completed readers retained execution state");
    // Scheduling is OS-dependent. Report the observed overlap instead of making
    // this correctness test fail merely because a runner schedules sequentially.
    std::cout << "observed concurrent C executor calls: " << stats.peak_executions << '\n';
}

void test_parallel_inserts_and_worker_reuse() {
    TempDirectory directory;
    Engine engine(directory.options(4));
    std::mutex ids_mutex;
    std::vector<std::int64_t> ids;
    parallel(4, [&](std::size_t worker) {
        for (int item = 0; item < 50; ++item) {
            auto result = engine.execute(worker, "INSERT INTO users (name, age) VALUES ('parallel', 25);");
            require(result.status == QueryStatus::Ok && result.committed, "parallel insert failed");
            {
                std::lock_guard<std::mutex> lock(ids_mutex);
                ids.push_back(result.inserted_id);
            }
            auto found = engine.execute(worker, "SELECT * FROM users WHERE id = " +
                                               std::to_string(result.inserted_id) + ";");
            require(found.status == QueryStatus::Ok && found.rows.size() == 1 &&
                    found.rows[0].id == result.inserted_id, "read-after-write missed a parallel insert");
        }
    });
    std::sort(ids.begin(), ids.end());
    require(ids.size() == 200, "parallel inserts lost responses");
    for (std::size_t i = 0; i < ids.size(); ++i) {
        require(ids[i] == static_cast<std::int64_t>(i + 1), "parallel inserts generated duplicate IDs");
    }
    for (const auto id : {1, 200}) {
        auto found = engine.execute(0, "SELECT * FROM users WHERE id = " + std::to_string(id) + ";");
        require(found.status == QueryStatus::Ok && found.rows.size() == 1 && found.rows[0].id == id,
                "concurrent inserts lost the first or last row");
    }
    auto missing = engine.execute(1, "SELECT * FROM users WHERE id = 201;");
    require(missing.status == QueryStatus::Ok && missing.rows.empty(),
            "concurrent inserts created an unexpected ID");
    auto scan = engine.execute(0, "SELECT * FROM users;");
    require(scan.status == QueryStatus::Ok && scan.rows.size() == ids.size(), "parallel inserts lost rows");
    // Defensive reader ownership also protects an accidentally reused index.
    parallel(4, [&](std::size_t) {
        for (int i = 0; i < 10; ++i) {
            auto result = engine.execute(0, "SELECT * FROM users;");
            require(result.status == QueryStatus::Ok && result.rows.size() == 200,
                    "concurrent reuse of a reader corrupted its cursor");
        }
    });
    require(engine.healthy(), "concurrent valid queries poisoned the database");
    require(engine.stats().successful_inserts == 200, "insert statistics differ from committed rows");
}

void test_write_failure_is_global() {
#if defined(__linux__)
    TempDirectory directory;
    Engine engine(directory.options(2));
    rlimit previous{};
    require(getrlimit(RLIMIT_FSIZE, &previous) == 0, "getrlimit failed");
    const auto old_handler = std::signal(SIGXFSZ, SIG_IGN);
    require(old_handler != SIG_ERR, "unable to suppress SIGXFSZ in fault test");
    rlimit limited = previous;
    limited.rlim_cur = 0;
    if (setrlimit(RLIMIT_FSIZE, &limited) != 0) {
        std::signal(SIGXFSZ, old_handler);
        throw std::runtime_error("unable to inject a file-size write failure");
    }
    const auto failed = engine.execute(0, "INSERT INTO users (name, age) VALUES ('disk-failure', 5);");
    const int restored = setrlimit(RLIMIT_FSIZE, &previous);
    std::signal(SIGXFSZ, old_handler);
    require(restored == 0, "unable to restore the file-size limit");
    require(failed.status == QueryStatus::StorageError && !failed.committed && !engine.healthy(),
            "writer failure was not propagated to engine health");
    const auto reader = engine.execute(1, "SELECT * FROM users;");
    require(reader.status == QueryStatus::Unavailable, "independent reader ignored the writer failure");
    for (const auto id : {0, 900}) {
        const auto missing = engine.execute(1, "SELECT * FROM users WHERE id = " + std::to_string(id) + ";");
        require(missing.status == QueryStatus::Unavailable,
                "an index miss bypassed the global write failure");
    }
#endif
}

} // namespace

int main() {
    try {
        test_new_rows_visible_and_eof();
        test_gap_ids_and_new_rows();
        test_insert_at_int64_limit();
        test_input_is_validated_before_writes();
        test_result_limits_are_not_storage_errors();
        test_lock_wait_categories();
        test_parallel_readers();
        test_parallel_inserts_and_worker_reuse();
        test_write_failure_is_global();
        std::cout << "engine tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "engine test failure: " << error.what() << '\n';
        return 1;
    }
}
