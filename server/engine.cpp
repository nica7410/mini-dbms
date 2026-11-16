#include "engine.hpp"

#include "c_api.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
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

// Unlike std::shared_mutex, this gate explicitly prevents new readers from
// bypassing an already waiting writer. Existing readers finish their statements.
class WriterPreferredGate {
public:
    void lock_shared() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !writer_ && waiting_writers_ == 0; });
        ++readers_;
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
    std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t readers_ = 0;
    std::size_t waiting_writers_ = 0;
    bool writer_ = false;
};

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

struct StatementOwner {
    Statement statement{};
    ~StatementOwner() { statement_free(&statement); }
    StatementOwner() = default;
    StatementOwner(const StatementOwner&) = delete;
    StatementOwner& operator=(const StatementOwner&) = delete;
};

struct Metrics {
    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::uint64_t> successful_inserts{0};
    std::atomic<std::uint64_t> successful_selects{0};
    std::atomic<std::uint64_t> errors{0};
    std::atomic<std::uint64_t> fallbacks{0};
    std::atomic<std::uint64_t> current_executions{0};
    std::atomic<std::uint64_t> peak_executions{0};
    std::atomic<std::uint64_t> lock_wait_us{0};
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

bool valid_insert_values(const Statement& statement) noexcept {
    if (statement.name == nullptr) return false;
    const auto length = std::strlen(statement.name);
    return length > 0 && length <= NAME_MAX_BYTES &&
           std::strpbrk(statement.name, "\t\r\n") == nullptr &&
           statement.age >= AGE_MIN && statement.age <= AGE_MAX;
}

extern "C" {
static int record_insert(std::int64_t id, void* opaque) noexcept {
    auto& result = *static_cast<QueryResult*>(opaque);
    // This callback must not allocate or fail after the C engine commits.
    result.inserted_id = id;
    result.committed = true;
    return 0;
}
}

enum class CollectionFailure { None, Limit, Allocation };

struct RowCollector {
    QueryResult& result;
    const EngineOptions& options;
    std::size_t bytes = 256;
    std::size_t calls = 0;
    CollectionFailure failure = CollectionFailure::None;
};

extern "C" {
static int collect_row(const Row* row, void* opaque) noexcept {
    auto& collector = *static_cast<RowCollector*>(opaque);
    ++collector.calls;
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
    struct Reader {
        explicit Reader(const std::string& directory) : table(open_table(directory)) {}
        TableHandle table;
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
        writer = open_table(options.data_dir);
        readers.reserve(options.readers);
        for (std::size_t i = 0; i < options.readers; ++i) {
            readers.push_back(std::make_unique<Reader>(options.data_dir));
        }
    }

    int run_c(Table* table, const Statement& statement, const ResultSink& sink, DbError& error) {
        ExecutionScope execution(metrics);
        return executor_run(table, &statement, &sink, &error);
    }

    void add_wait(Clock::time_point started) noexcept {
        metrics.lock_wait_us.fetch_add(microseconds_since(started), std::memory_order_relaxed);
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
            // A genuine reader error is never treated as a stale-index miss.
            is_healthy.store(false, std::memory_order_release);
            fail(collector.result, QueryStatus::StorageError, error.msg);
        } else {
            collector.result.status = QueryStatus::Ok;
        }
    }

    void run(std::size_t worker_index, const std::string& sql, QueryResult& result) {
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
            std::unique_lock<WriterPreferredGate> write_gate(gate);
            add_wait(wait_started);
            wait_started = Clock::now();
            std::unique_lock<std::mutex> writer_lock(writer_mutex);
            add_wait(wait_started);
            if (!available(result)) return;

            const ResultSink sink{record_insert, nullptr, &result};
            const int rc = run_c(writer.get(), statement, sink, error);
            if (rc == 0) {
                result.status = QueryStatus::Ok;
                return;
            }

            // All stored IDs are > 0. This public-API probe checks table_usable
            // first, then misses in the index without touching the FILE cursor.
            // It exposes write_failed, not general file health or commit status.
            DbError health_error{};
            if (table_find_id(writer.get(), 0, nullptr, nullptr, &health_error) != 0) {
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
        add_wait(wait_started);
        wait_started = Clock::now();
        std::shared_lock<WriterPreferredGate> read_gate(gate);
        add_wait(wait_started);
        if (!available(result)) return;

        RowCollector collector{result, options};
        const ResultSink sink{nullptr, collect_row, &collector};
        int rc = run_c(reader.table.get(), statement, sink, error);
        if (rc == 0 && statement.where == WHERE_ID && collector.calls == 0) {
            // Reader indexes are immutable snapshots. An append cannot invalidate
            // their hits; a miss must consult the authoritative writer index.
            // Always take locks in gate -> writer_mutex order.
            wait_started = Clock::now();
            std::unique_lock<std::mutex> writer_lock(writer_mutex);
            add_wait(wait_started);
            if (!available(result)) return;
            metrics.fallbacks.fetch_add(1, std::memory_order_relaxed);
            rc = run_c(writer.get(), statement, sink, error);
        }
        finish_select(rc, collector, error);
    }

    QueryResult execute(std::size_t worker_index, const std::string& sql) {
        metrics.requests.fetch_add(1, std::memory_order_relaxed);
        QueryResult result;
        try {
            run(worker_index, sql, result);
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
    TableHandle writer;
    std::vector<std::unique_ptr<Reader>> readers;
    WriterPreferredGate gate;
    std::mutex writer_mutex;
    std::atomic<bool> is_healthy{true};
    Metrics metrics;
};

Engine::Engine(const EngineOptions& options) : impl_(std::make_unique<Impl>(options)) {}
Engine::~Engine() = default;

QueryResult Engine::execute(std::size_t worker_index, const std::string& sql) {
    return impl_->execute(worker_index, sql);
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
    out.fallbacks = metrics.fallbacks.load(std::memory_order_relaxed);
    out.current_executions = metrics.current_executions.load(std::memory_order_relaxed);
    out.peak_executions = metrics.peak_executions.load(std::memory_order_relaxed);
    out.lock_wait_us = metrics.lock_wait_us.load(std::memory_order_relaxed);
    out.execution_us = metrics.execution_us.load(std::memory_order_relaxed);
    return out;
}

} // namespace minidb
