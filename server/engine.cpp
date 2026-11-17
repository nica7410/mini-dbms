#include "engine.hpp"

#include "c_api.hpp"
#include "writer_preferred_gate.hpp"

#ifdef MINIDB_BENCH_SERIAL
#include "benchmarks/serial_gate.hpp"
#elif defined(MINIDB_BENCH_FAIR)
#include "benchmarks/fair_gate.hpp"
#endif

#ifdef MINIDB_TEST_ACCESS
#include "tests/test_access.hpp"
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <stdexcept>
#include <utility>

namespace minidb {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t microseconds_since(Clock::time_point start) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}

#ifdef MINIDB_BENCH_SERIAL
using EngineGate = detail::SerialGate;
#elif defined(MINIDB_BENCH_FAIR)
using EngineGate = detail::FairGate;
#else
using EngineGate = detail::WriterPreferredGate;
#endif

struct TableDeleter {
    void operator()(Table* table) const noexcept {
        // table_close releases the object even on error. Never retry its pointer.
        if (table != nullptr) (void)table_close(table, nullptr);
    }
};

using TableHandle = std::unique_ptr<Table, TableDeleter>;

TableHandle open_table(const std::string& directory) {
    Table* raw = nullptr;
    DbError error{};
    if (table_open(directory.c_str(), &raw, &error) != 0) {
        throw std::runtime_error(error.msg);
    }
    return TableHandle(raw);
}

struct ReaderDeleter {
    void operator()(TableReader* reader) const noexcept {
        if (reader != nullptr) (void)table_reader_close(reader, nullptr);
    }
};

using ReaderHandle = std::unique_ptr<TableReader, ReaderDeleter>;

ReaderHandle open_reader(Table* table) {
    TableReader* raw = nullptr;
    DbError error{};
    if (table_reader_open(table, &raw, &error) != 0) {
        throw std::runtime_error(error.msg);
    }
    return ReaderHandle(raw);
}

struct StatementOwner {
    Statement statement{};
    ~StatementOwner() { statement_free(&statement); }
    StatementOwner() = default;
    StatementOwner(const StatementOwner&) = delete;
    StatementOwner& operator=(const StatementOwner&) = delete;
};

struct LockMetrics {
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint64_t> total_us{0};
    std::atomic<std::uint64_t> max_us{0};
};

struct Metrics {
    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::uint64_t> successful_inserts{0};
    std::atomic<std::uint64_t> successful_selects{0};
    std::atomic<std::uint64_t> errors{0};
    std::atomic<std::uint64_t> current_executions{0};
    std::atomic<std::uint64_t> peak_executions{0};
    std::atomic<std::uint64_t> lock_wait_us{0};
    std::array<LockMetrics, static_cast<std::size_t>(LockKind::Count)> lock_wait;
    std::atomic<std::uint64_t> execution_us{0};
};

class ExecutionScope {
public:
    explicit ExecutionScope(Metrics& metrics) noexcept
        : metrics_(metrics), started_(Clock::now()) {
        const auto current = metrics_.current_executions.fetch_add(1, std::memory_order_relaxed) + 1;
        auto peak = metrics_.peak_executions.load(std::memory_order_relaxed);
        while (peak < current && !metrics_.peak_executions.compare_exchange_weak(
                   peak, current, std::memory_order_relaxed)) {}
    }

    ~ExecutionScope() {
        metrics_.execution_us.fetch_add(microseconds_since(started_), std::memory_order_relaxed);
        metrics_.current_executions.fetch_sub(1, std::memory_order_relaxed);
    }

private:
    Metrics& metrics_;
    Clock::time_point started_;
};

void fail(QueryResult& result, QueryStatus status, const char* message) {
    result.status = status;
    result.rows.clear();
    result.error = message;
}

bool valid_insert_row(const InsertRow& row) noexcept {
    if (row.name == nullptr) return false;
    const auto length = std::strlen(row.name);
    return length > 0 && length <= NAME_MAX_BYTES &&
           std::strpbrk(row.name, "\t\r\n") == nullptr &&
           row.age >= AGE_MIN && row.age <= AGE_MAX;
}

bool valid_insert_values(const Statement& statement) noexcept {
    if (statement.rows == nullptr || statement.row_count == 0 ||
        statement.row_count > INSERT_MAX_ROWS) {
        return false;
    }
    for (std::size_t i = 0; i < statement.row_count; ++i) {
        if (!valid_insert_row(statement.rows[i])) return false;
    }
    return true;
}

extern "C" {
static int record_insert(std::int64_t id, void* opaque) noexcept {
    auto& result = *static_cast<QueryResult*>(opaque);
    // This callback must not allocate or fail after the C engine commits.
    // It runs once per stored row, in order.
    if (result.inserted_count == 0) result.inserted_id = id;
    result.last_inserted_id = id;
    ++result.inserted_count;
    result.committed = true;
    return 0;
}
}

enum class CollectionFailure { None, Limit, Allocation };

struct RowCollector {
    QueryResult& result;
    const EngineOptions& options;
    std::size_t bytes = 256;
    CollectionFailure failure = CollectionFailure::None;
};

extern "C" {
static int collect_row(const Row* row, void* opaque) noexcept {
    auto& collector = *static_cast<RowCollector*>(opaque);
    std::size_t escaped = 0;
    for (const auto* p = reinterpret_cast<const unsigned char*>(row->name); *p != 0; ++p) {
        escaped += *p < 0x20 ? 6 : (*p == '"' || *p == '\\' ? 2 : 1);
    }
    const std::size_t cost = 64 + escaped;
    if (collector.result.rows.size() >= collector.options.max_rows ||
        collector.bytes > collector.options.max_result_bytes ||
        cost > collector.options.max_result_bytes - collector.bytes) {
        collector.failure = CollectionFailure::Limit;
        return -1;
    }
    try {
        // Row and Row::name are borrowed stack storage owned by the C callback.
        collector.result.rows.push_back(OwnedRow{row->id, row->name, row->age});
        collector.bytes += cost;
        return 0;
    } catch (...) {
        collector.failure = CollectionFailure::Allocation;
        return -1;
    }
}
}

} // namespace

struct Engine::Impl {
    // A worker's own file cursor over the shared Table. The B+ tree index is
    // shared: it is read only under the shared gate and changed only by INSERT
    // under the exclusive gate.
    struct Reader {
        explicit Reader(Table* shared) : handle(open_reader(shared)) {}
        ReaderHandle handle;
        std::mutex mutex;
    };

    explicit Impl(const EngineOptions& supplied) : options(supplied) {
        if (options.data_dir.empty() || options.data_dir.find('\0') != std::string::npos) {
            throw std::invalid_argument("data_dir must be a nonempty path without NUL bytes");
        }
        if (options.readers == 0 || options.max_sql_bytes == 0 ||
            options.max_rows == 0 || options.max_result_bytes == 0) {
            throw std::invalid_argument("engine reader count and limits must be positive");
        }
        table = open_table(options.data_dir);
        readers.reserve(options.readers);
        for (std::size_t i = 0; i < options.readers; ++i) {
            readers.push_back(std::make_unique<Reader>(table.get()));
        }
    }

    int run_insert(const Statement& statement, const ResultSink& sink, DbError& error) {
        ExecutionScope execution(metrics);
        return executor_run(table.get(), &statement, &sink, &error);
    }

    int run_select(Reader& reader, const Statement& statement, const ResultSink& sink, DbError& error) {
        ExecutionScope execution(metrics);
        return executor_run_read(reader.handle.get(), &statement, &sink, &error);
    }

    void add_wait(Clock::time_point started, LockKind kind) noexcept {
        const auto elapsed = microseconds_since(started);
        auto& metric = metrics.lock_wait[static_cast<std::size_t>(kind)];
        metric.count.fetch_add(1, std::memory_order_relaxed);
        metric.total_us.fetch_add(elapsed, std::memory_order_relaxed);
        auto maximum = metric.max_us.load(std::memory_order_relaxed);
        while (maximum < elapsed && !metric.max_us.compare_exchange_weak(
                   maximum, elapsed, std::memory_order_relaxed)) {}
        metrics.lock_wait_us.fetch_add(elapsed, std::memory_order_relaxed);
    }

    bool available(QueryResult& result) const {
        if (is_healthy.load(std::memory_order_acquire)) return true;
        fail(result, QueryStatus::Unavailable, "database requires validation and restart after a storage failure");
        return false;
    }

    void finish_select(int rc, RowCollector& collector, const DbError& error) {
        if (collector.failure == CollectionFailure::Limit) {
            fail(collector.result, QueryStatus::ResultTooLarge, "query result exceeds the configured row or byte limit");
        } else if (collector.failure == CollectionFailure::Allocation) {
            fail(collector.result, QueryStatus::InternalError, "unable to allocate query results");
        } else if (rc != 0) {
            is_healthy.store(false, std::memory_order_release);
            fail(collector.result, QueryStatus::StorageError, error.msg);
        } else {
            collector.result.status = QueryStatus::Ok;
        }
    }

    void run(std::size_t worker_index, const std::string& sql, Clock::time_point deadline,
             QueryResult& result) {
        if (worker_index >= readers.size()) {
            fail(result, QueryStatus::InternalError, "invalid SQL worker index");
            return;
        }
        if (!available(result)) return;
        if (sql.size() > options.max_sql_bytes) {
            fail(result, QueryStatus::InvalidSql, "SQL exceeds the configured byte limit");
            return;
        }
        if (sql.find('\0') != std::string::npos) {
            fail(result, QueryStatus::InvalidSql, "SQL must not contain NUL bytes");
            return;
        }

        StatementOwner parsed;
        DbError error{};
        std::size_t position = 0;
        const int first = parser_next(sql.c_str(), &position, &parsed.statement, &error);
        if (first != 1) {
            fail(result, QueryStatus::InvalidSql,
                 first == 0 ? "exactly one SQL statement is required" : error.msg);
            return;
        }
        // Parse the entire input before executing anything. In particular, an
        // INSERT followed by another statement or malformed suffix writes nothing.
        StatementOwner extra;
        const int trailing = parser_next(sql.c_str(), &position, &extra.statement, &error);
        if (trailing != 0) {
            fail(result, QueryStatus::InvalidSql,
                 trailing == 1 ? "only one SQL statement per request is supported" : error.msg);
            return;
        }

        const auto& statement = parsed.statement;
        result.is_insert = statement.kind == STMT_INSERT;
        if (result.is_insert) {
            // Mirror the C executor's public schema rules only to classify a
            // client error; executor_run still performs its own validation.
            if (!valid_insert_values(statement)) {
                fail(result, QueryStatus::InvalidValues,
                     "name must contain 1-255 bytes without tabs/newlines; age must be 0-150");
                return;
            }
            auto wait_started = Clock::now();
            std::unique_lock<EngineGate> write_gate(gate);
            add_wait(wait_started, LockKind::WriteGate);
            if (!available(result)) return;
            // The wait for the gate counts toward the task's expiry budget.
            if (Clock::now() > deadline) {
                fail(result, QueryStatus::Expired, "task expired before SQL execution");
                return;
            }

            const ResultSink sink{record_insert, nullptr, &result};
            const int rc = run_insert(statement, sink, error);
            if (rc == 0) {
                result.status = QueryStatus::Ok;
                return;
            }

            // All stored IDs are > 0. This public-API probe checks table_usable
            // first, then misses in the index without touching the FILE cursor.
            // It exposes write_failed, not general file health or commit status.
            DbError health_error{};
            if (table_find_id(table.get(), 0, nullptr, nullptr, &health_error) != 0) {
                is_healthy.store(false, std::memory_order_release);
            }
            fail(result, QueryStatus::StorageError, error.msg);
            return;
        }

        if (options.max_result_bytes < 256) {
            fail(result, QueryStatus::ResultTooLarge, "query result byte budget is smaller than the response envelope");
            return;
        }
        auto& reader = *readers[worker_index];
        auto wait_started = Clock::now();
        std::unique_lock<std::mutex> reader_lock(reader.mutex);
        add_wait(wait_started, LockKind::ReaderMutex);
        wait_started = Clock::now();
        std::shared_lock<EngineGate> read_gate(gate);
        add_wait(wait_started, LockKind::ReadGate);
        if (!available(result)) return;

        RowCollector collector{result, options};
        const ResultSink sink{nullptr, collect_row, &collector};
        // The shared index is always current, so a miss is final.
        const int rc = run_select(reader, statement, sink, error);
        finish_select(rc, collector, error);
    }

    QueryResult execute(std::size_t worker_index, const std::string& sql, Clock::time_point deadline) {
        metrics.requests.fetch_add(1, std::memory_order_relaxed);
        QueryResult result;
        try {
            run(worker_index, sql, deadline, result);
        } catch (const std::bad_alloc&) {
            fail(result, QueryStatus::InternalError, "out of memory");
        } catch (...) {
            fail(result, QueryStatus::InternalError, "engine failure");
        }
        if (result.status == QueryStatus::Ok) {
            (result.is_insert ? metrics.successful_inserts : metrics.successful_selects)
                .fetch_add(1, std::memory_order_relaxed);
        } else {
            metrics.errors.fetch_add(1, std::memory_order_relaxed);
        }
        return result;
    }

    EngineOptions options;
    // Declared before readers so every reader closes before the Table.
    TableHandle table;
    std::vector<std::unique_ptr<Reader>> readers;
    EngineGate gate;
    std::atomic<bool> is_healthy{true};
    Metrics metrics;
};

#ifdef MINIDB_TEST_ACCESS
GateSnapshot EngineTestAccess::snapshot(Engine& engine) {
    return detail::GateTestAccess::snapshot(engine.impl_->gate);
}
#endif

Engine::Engine(const EngineOptions& options) : impl_(std::make_unique<Impl>(options)) {}
Engine::~Engine() = default;

QueryResult Engine::execute(std::size_t worker_index, const std::string& sql, Deadline deadline) {
    return impl_->execute(worker_index, sql, deadline);
}

bool Engine::healthy() const {
    return impl_->is_healthy.load(std::memory_order_acquire);
}

EngineStats Engine::stats() const {
    const auto& metrics = impl_->metrics;
    EngineStats out;
    out.requests = metrics.requests.load(std::memory_order_relaxed);
    out.successful_inserts = metrics.successful_inserts.load(std::memory_order_relaxed);
    out.successful_selects = metrics.successful_selects.load(std::memory_order_relaxed);
    out.errors = metrics.errors.load(std::memory_order_relaxed);
    out.current_executions = metrics.current_executions.load(std::memory_order_relaxed);
    out.peak_executions = metrics.peak_executions.load(std::memory_order_relaxed);
    out.lock_wait_us = metrics.lock_wait_us.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < out.lock_wait.size(); ++i) {
        out.lock_wait[i].count = metrics.lock_wait[i].count.load(std::memory_order_relaxed);
        out.lock_wait[i].total_us = metrics.lock_wait[i].total_us.load(std::memory_order_relaxed);
        out.lock_wait[i].max_us = metrics.lock_wait[i].max_us.load(std::memory_order_relaxed);
    }
    out.execution_us = metrics.execution_us.load(std::memory_order_relaxed);
    return out;
}

} // namespace minidb
