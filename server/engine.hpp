#pragma once

#include <cstddef>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace minidb {

enum class QueryStatus {
    Ok,
    InvalidSql,
    InvalidValues,
    ResultTooLarge,
    StorageError,
    Unavailable,
    InternalError,
    // An INSERT whose deadline passed while it waited for the exclusive gate.
    // Nothing was written.
    Expired
};

struct OwnedRow {
    std::int64_t id;
    std::string name;
    int age;
};

struct QueryResult {
    QueryStatus status = QueryStatus::InternalError;
    bool is_insert = false;
    // A later response/transport failure never rolls an INSERT back.
    // True once at least one row of the INSERT was stored. With a multi-row
    // INSERT and a storage failure this can be true while status is an error.
    bool committed = false;
    // IDs within one INSERT statement are consecutive: the whole statement runs
    // under the exclusive gate. inserted_id is the first ID.
    std::int64_t inserted_id = 0;
    std::int64_t last_inserted_id = 0;
    std::size_t inserted_count = 0;
    std::vector<OwnedRow> rows;
    std::string error;
};

struct EngineOptions {
    std::string data_dir;
    std::size_t readers = 1;
    std::size_t max_sql_bytes = 65536;
    std::size_t max_rows = 10000;
    // Conservative compact-JSON budget: 256 bytes for the response envelope,
    // then 64 bytes plus the escaped name length per returned row.
    std::size_t max_result_bytes = 8388608;
};

enum class LockKind : std::size_t {
    ReaderMutex, ReadGate, WriteGate, Count
};

struct LockWaitStats {
    std::uint64_t count = 0;
    std::uint64_t total_us = 0;
    std::uint64_t max_us = 0;
};

struct EngineStats {
    std::uint64_t requests = 0;
    std::uint64_t successful_inserts = 0;
    std::uint64_t successful_selects = 0;
    std::uint64_t errors = 0;
    // These count C executor calls (INSERT and SELECT), excluding parsing and lock waiting.
    std::uint64_t current_executions = 0;
    std::uint64_t peak_executions = 0;
    std::uint64_t lock_wait_us = 0;
    // Successful acquisitions, including uncontended ones; time excludes holding.
    std::array<LockWaitStats, static_cast<std::size_t>(LockKind::Count)> lock_wait{};
    std::uint64_t execution_us = 0;
};

class Engine {
public:
    explicit Engine(const EngineOptions& options);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    using Deadline = std::chrono::steady_clock::time_point;

    // Exactly one statement is accepted; trailing whitespace/comments are OK.
    // Each SQL worker should retain its own worker_index in [0, readers).
    // The engine must outlive all calls; drain/join workers before destruction.
    // An INSERT checks deadline once more after acquiring the exclusive gate and
    // returns Expired without writing if it has passed. SELECT ignores it.
    QueryResult execute(std::size_t worker_index, const std::string& sql,
                        Deadline deadline = Deadline::max());
    bool healthy() const;
    // Atomic counters are read independently, so an in-flight snapshot can skew.
    EngineStats stats() const;

private:
    friend struct EngineTestAccess;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace minidb
