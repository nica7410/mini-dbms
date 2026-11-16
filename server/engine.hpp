#pragma once

#include <cstddef>
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
    InternalError
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
    bool committed = false;
    std::int64_t inserted_id = 0;
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

struct EngineStats {
    std::uint64_t requests = 0;
    std::uint64_t successful_inserts = 0;
    std::uint64_t successful_selects = 0;
    std::uint64_t errors = 0;
    std::uint64_t fallbacks = 0;
    // These count executor_run calls, excluding parsing and lock waiting.
    std::uint64_t current_executions = 0;
    std::uint64_t peak_executions = 0;
    std::uint64_t lock_wait_us = 0;
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

    // Exactly one statement is accepted; trailing whitespace/comments are OK.
    // Each SQL worker should retain its own worker_index in [0, readers).
    // The engine must outlive all calls; drain/join workers before destruction.
    QueryResult execute(std::size_t worker_index, const std::string& sql);
    bool healthy() const;
    // Atomic counters are read independently, so an in-flight snapshot can skew.
    EngineStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace minidb
