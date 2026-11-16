#include "engine.hpp"
#include "thread_pool.hpp"

#include <drogon/drogon.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
using Callback = std::function<void(const drogon::HttpResponsePtr &)>;
std::atomic<bool> stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free,
              "Shutdown requires a signal-safe lock-free atomic flag");

// Signal handlers only set a flag. The event loop performs shutdown work.
extern "C" void request_stop(int) { stop_requested.store(true, std::memory_order_relaxed); }

struct Options {
    std::string data_dir = "data";
    std::string host = "127.0.0.1";
    std::size_t port = 8080;
    std::size_t workers = 4;
    std::size_t io_threads = 2;
    std::size_t queue_capacity = 256;
    std::size_t queue_bytes = 16 * 1024 * 1024;
    std::size_t max_sql_bytes = 65536;
    std::size_t max_rows = 10000;
    std::size_t max_result_bytes = 8 * 1024 * 1024;
    std::size_t queue_timeout_ms = 5000;
    std::size_t max_connections = 1024;
};

void usage() {
    std::cout
        << "Mini DBMS Phase 3 (C17 engine + C++17 API)\n"
        << "Usage: minidb_server [options]\n"
        << "  --data-dir PATH          Database directory (data)\n"
        << "  --host ADDRESS          Listen address (127.0.0.1)\n"
        << "  --port N                TCP port (8080)\n"
        << "  --workers N             SQL workers/read contexts (4)\n"
        << "  --io-threads N          HTTP event loops (2)\n"
        << "  --queue-capacity N      Maximum waiting SQL tasks (256)\n"
        << "  --queue-bytes N         Maximum waiting request bytes (16777216)\n"
        << "  --max-sql-bytes N       SQL UTF-8 byte limit (65536)\n"
        << "  --max-rows N            SELECT result row limit (10000)\n"
        << "  --max-result-bytes N    JSON result byte limit, >= 512 (8388608)\n"
        << "  --queue-timeout-ms N    Expiry before SQL execution (5000)\n"
        << "  --max-connections N     HTTP connection limit (1024)\n"
        << "  --help                  Show this message\n";
}

std::size_t positive_number(const std::string &text) {
    std::size_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0) {
        throw std::invalid_argument("expected a positive integer: " + text);
    }
    return value;
}

Options parse_options(int argc, char **argv) {
    Options out;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (i + 1 == argc) throw std::invalid_argument("missing value for " + key);
        const std::string value = argv[++i];
        if (key == "--data-dir") out.data_dir = value;
        else if (key == "--host") out.host = value;
        else if (key == "--port") out.port = positive_number(value);
        else if (key == "--workers") out.workers = positive_number(value);
        else if (key == "--io-threads") out.io_threads = positive_number(value);
        else if (key == "--queue-capacity") out.queue_capacity = positive_number(value);
        else if (key == "--queue-bytes") out.queue_bytes = positive_number(value);
        else if (key == "--max-sql-bytes") out.max_sql_bytes = positive_number(value);
        else if (key == "--max-rows") out.max_rows = positive_number(value);
        else if (key == "--max-result-bytes") out.max_result_bytes = positive_number(value);
        else if (key == "--queue-timeout-ms") out.queue_timeout_ms = positive_number(value);
        else if (key == "--max-connections") out.max_connections = positive_number(value);
        else throw std::invalid_argument("unknown option: " + key);
    }
    if (out.port > 65535 || out.workers > 256 || out.io_threads > 256 ||
        out.max_sql_bytes > 16 * 1024 * 1024 || out.max_result_bytes < 512 ||
        out.queue_timeout_ms > 86400000 || out.data_dir.empty() || out.host.empty()) {
        throw std::invalid_argument("option out of range (see --help; workers/io-threads <= 256, SQL <= 16 MiB)");
    }
    return out;
}

// Prevent two Phase 3 servers from independently assigning IDs in one database.
// The original CLI does not participate in this lock; use it only while stopped.
class DataDirectoryLock {
public:
    explicit DataDirectoryLock(const std::string &directory) {
        std::filesystem::create_directories(directory);
        const auto path = std::filesystem::path(directory) / ".minidb-server.lock";
#ifdef _WIN32
        handle_ = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("database directory is locked or its lock file is inaccessible");
        }
#else
        descriptor_ = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (descriptor_ < 0) throw std::runtime_error("cannot open database lock file");
        if (flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error("database directory is already in use by another server");
        }
#endif
    }
    ~DataDirectoryLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (descriptor_ >= 0) close(descriptor_);
#endif
    }
    DataDirectoryLock(const DataDirectoryLock &) = delete;
    DataDirectoryLock &operator=(const DataDirectoryLock &) = delete;
private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
};

bool valid_utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) continue;
        unsigned count;
        std::uint32_t code;
        std::uint32_t minimum;
        if (first >= 0xc2 && first <= 0xdf) { count = 1; code = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; code = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; code = first & 7; minimum = 0x10000; }
        else return false;
        if (text.size() - i < count) return false;
        while (count-- != 0) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}

std::string serialize(const Json::Value &value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    writer["emitUTF8"] = true;
    return Json::writeString(writer, value);
}

drogon::HttpResponsePtr body_response(int status, std::string body) {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    response->addHeader("Cache-Control", "no-store");
    response->setBody(std::move(body));
    return response;
}

drogon::HttpResponsePtr error_response(int status, const std::string &id,
                                      const std::string &code, const std::string &message,
                                      const std::string &write_outcome = "") {
    Json::Value value(Json::objectValue);
    value["ok"] = false;
    if (!id.empty()) value["request_id"] = id;
    value["error"]["code"] = code;
    value["error"]["message"] = valid_utf8(message) ? message : "DB engine returned an error";
    if (!write_outcome.empty()) value["write_outcome"] = write_outcome;
    auto response = body_response(status, serialize(value));
    if (status == 503) response->addHeader("Retry-After", "1");
    return response;
}

std::pair<int, const char *> query_error(minidb::QueryStatus status) {
    using Status = minidb::QueryStatus;
    switch (status) {
        case Status::InvalidSql: return {400, "INVALID_SQL"};
        case Status::InvalidValues: return {422, "INVALID_VALUES"};
        case Status::ResultTooLarge: return {422, "RESULT_TOO_LARGE"};
        case Status::StorageError: return {500, "STORAGE_ERROR"};
        case Status::Unavailable: return {503, "DATABASE_UNAVAILABLE"};
        default: return {500, "INTERNAL_ERROR"};
    }
}

drogon::HttpResponsePtr query_response(const minidb::QueryResult &result,
                                     const std::string &id, std::size_t byte_limit) {
    if (result.status != minidb::QueryStatus::Ok) {
        const auto mapped = query_error(result.status);
        const std::string outcome = result.committed ? "applied" :
            (result.is_insert && result.status == minidb::QueryStatus::StorageError ? "unknown" : "");
        return error_response(mapped.first, id, mapped.second, result.error, outcome);
    }
    Json::Value value(Json::objectValue);
    value["ok"] = true;
    value["request_id"] = id;
    if (result.is_insert) {
        value["type"] = "insert";
        value["inserted_id"] = std::to_string(result.inserted_id);
        value["affected_rows"] = 1;
        value["write_outcome"] = "applied";
    } else {
        value["type"] = "select";
        value["rows"] = Json::Value(Json::arrayValue);
        for (const auto &row : result.rows) {
            if (!valid_utf8(row.name)) {
                return error_response(500, id, "INVALID_STORED_TEXT", "Stored name is not valid UTF-8");
            }
            Json::Value item(Json::objectValue);
            // IDs can reach INT64_MAX; strings avoid loss in JavaScript clients.
            item["id"] = std::to_string(row.id);
            item["name"] = row.name;
            item["age"] = row.age;
            value["rows"].append(std::move(item));
        }
        value["row_count"] = Json::UInt64(result.rows.size());
    }
    auto body = serialize(value);
    if (body.size() > byte_limit) {
        return error_response(422, id, "RESULT_TOO_LARGE", "Serialized result exceeds the response byte limit",
                              result.committed ? "applied" : "");
    }
    return body_response(200, std::move(body));
}

bool decode_request(std::string_view body, Json::Value &value) {
    if (!valid_utf8(body)) return false;
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    builder["allowComments"] = false;
    builder["allowTrailingCommas"] = false;
    builder["strictRoot"] = true;
    builder["failIfExtra"] = true;
    builder["rejectDupKeys"] = true;
    builder["allowSpecialFloats"] = false;
    builder["stackLimit"] = 32;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errors;
    try {
        return reader->parse(body.data(), body.data() + body.size(), &value, &errors) &&
               value.isObject() && value.size() == 1 && value["sql"].isString();
    } catch (const Json::RuntimeError &) {
        // JsonCpp throws on nesting beyond stackLimit rather than returning false.
        return false;
    }
}

int serve(const Options &options) {
    DataDirectoryLock database_lock(options.data_dir);
    minidb::EngineOptions engine_options;
    engine_options.data_dir = options.data_dir;
    engine_options.readers = options.workers;
    engine_options.max_sql_bytes = options.max_sql_bytes;
    engine_options.max_rows = options.max_rows;
    engine_options.max_result_bytes = options.max_result_bytes;
    minidb::Engine engine(engine_options);
    minidb::ThreadPool pool(options.workers, options.queue_capacity, options.queue_bytes);
    std::atomic<bool> stopping{false};
    std::atomic<std::uint64_t> next_request{1};
    std::atomic<std::uint64_t> expired{0};
    auto &app = drogon::app();
    // Account for worst-case JSON escaping of an otherwise valid SQL string.
    const std::size_t body_limit = options.max_sql_bytes * 6 + 1024;
    app.setThreadNum(options.io_threads)
        .addListener(options.host, static_cast<std::uint16_t>(options.port))
        .setClientMaxBodySize(body_limit)
        .setClientMaxMemoryBodySize(body_limit)
        .setMaxConnectionNum(options.max_connections)
        .setPipeliningRequestsNumber(16)
        .setIdleConnectionTimeout(30)
        .setFileTypes({})
        .disableSession()
        .disableSigtermHandling()
        .setLogLevel(trantor::Logger::kWarn);
    app.setCustomErrorHandler([](drogon::HttpStatusCode status, const drogon::HttpRequestPtr &) {
        return error_response(static_cast<int>(status), "", "HTTP_ERROR", "HTTP request rejected");
    });

    app.registerHandler("/query", [&](const drogon::HttpRequestPtr &request, Callback &&callback) {
        const auto id = std::to_string(next_request.fetch_add(1, std::memory_order_relaxed));
        try {
            if (stopping.load(std::memory_order_acquire)) {
                callback(error_response(503, id, "SHUTTING_DOWN", "Server is draining accepted SQL tasks", "not_started"));
                return;
            }
            if (request->contentType() != drogon::CT_APPLICATION_JSON) {
                callback(error_response(415, id, "CONTENT_TYPE", "Use application/json"));
                return;
            }
            Json::Value input;
            if (!decode_request(request->getBody(), input)) {
                callback(error_response(400, id, "INVALID_REQUEST", "Expected a UTF-8 JSON object containing only a string sql field"));
                return;
            }
            std::string sql = input["sql"].asString();
            if (sql.size() > options.max_sql_bytes) {
                callback(error_response(413, id, "SQL_TOO_LARGE", "SQL byte limit exceeded"));
                return;
            }
            if (!valid_utf8(sql)) {
                callback(error_response(400, id, "INVALID_REQUEST", "SQL must be valid UTF-8"));
                return;
            }
            const auto queued_at = Clock::now();
            // Keep a separate callback copy for admission failures. Accepted tasks
            // own the SQL and callback; no request-local references enter the queue.
            const auto submitted = pool.try_submit(
                [&, sql = std::move(sql), id, queued_at, done = callback](std::size_t worker) {
                    bool applied = false;
                    drogon::HttpResponsePtr response;
                    try {
                        if (Clock::now() - queued_at > std::chrono::milliseconds(options.queue_timeout_ms)) {
                            expired.fetch_add(1, std::memory_order_relaxed);
                            response = error_response(503, id, "QUEUE_TIMEOUT", "Task expired before SQL execution", "not_started");
                        } else {
                            auto result = engine.execute(worker, sql);
                            applied = result.committed;
                            response = query_response(result, id, options.max_result_bytes);
                        }
                    } catch (const std::exception &) {
                        response = error_response(500, id, "INTERNAL_ERROR", "Request processing failed",
                                                  applied ? "applied" : "unknown");
                    }
                    // Drogon hands the response back to the connection's I/O loop.
                    // Transmission failure never rolls back an already applied INSERT.
                    done(response);
                }, request->getBody().size());
            if (submitted != minidb::ThreadPool::SubmitResult::Accepted) {
                callback(error_response(503, id,
                    submitted == minidb::ThreadPool::SubmitResult::Full ? "QUEUE_FULL" : "SHUTTING_DOWN",
                    "SQL task was not accepted", "not_started"));
            }
        } catch (const std::exception &) {
            callback(error_response(500, id, "INTERNAL_ERROR", "Could not accept the request", "not_started"));
        }
    }, {drogon::Post});

    app.registerHandler("/health", [&](const drogon::HttpRequestPtr &, Callback &&callback) {
        const bool available = engine.healthy() && !stopping.load(std::memory_order_acquire);
        Json::Value result(Json::objectValue);
        result["ok"] = available;
        result["status"] = available ? "ready" : "unavailable";
        callback(body_response(available ? 200 : 503, serialize(result)));
    }, {drogon::Get});

    app.registerHandler("/metrics", [&](const drogon::HttpRequestPtr &, Callback &&callback) {
        const auto workers = pool.stats();
        const auto database = engine.stats();
        Json::Value result(Json::objectValue);
        auto &queue = result["pool"];
        queue["workers"] = Json::UInt64(workers.workers);
        queue["queued"] = Json::UInt64(workers.queued);
        queue["queued_bytes"] = Json::UInt64(workers.queued_bytes);
        queue["active"] = Json::UInt64(workers.active);
        queue["peak_active"] = Json::UInt64(workers.peak_active);
        queue["accepted"] = Json::UInt64(workers.accepted);
        queue["completed"] = Json::UInt64(workers.completed);
        queue["rejected"] = Json::UInt64(workers.rejected);
        queue["failed_tasks"] = Json::UInt64(workers.failed_tasks);
        queue["queue_wait_us"] = Json::UInt64(workers.queue_wait_us);
        queue["max_queue_wait_us"] = Json::UInt64(workers.max_queue_wait_us);
        queue["expired"] = Json::UInt64(expired.load(std::memory_order_relaxed));
        auto &db = result["engine"];
        db["healthy"] = engine.healthy();
        db["requests"] = Json::UInt64(database.requests);
        db["successful_inserts"] = Json::UInt64(database.successful_inserts);
        db["successful_selects"] = Json::UInt64(database.successful_selects);
        db["errors"] = Json::UInt64(database.errors);
        db["fallbacks"] = Json::UInt64(database.fallbacks);
        db["fallbacks_skipped"] = Json::UInt64(database.fallbacks_skipped);
        db["current_executions"] = Json::UInt64(database.current_executions);
        db["peak_executions"] = Json::UInt64(database.peak_executions);
        db["lock_wait_us"] = Json::UInt64(database.lock_wait_us);
        db["execution_us"] = Json::UInt64(database.execution_us);
        callback(body_response(200, serialize(result)));
    }, {drogon::Get});

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    app.getLoop()->runEvery(0.05, [&] {
        if (stop_requested.load(std::memory_order_relaxed) &&
            !stopping.exchange(true, std::memory_order_acq_rel)) {
            // This is the main loop, not a SQL worker or an HTTP I/O loop.
            // HTTP loops stay alive to deliver worker callbacks during the drain.
            pool.shutdown();
            app.quit();
        }
    });
    std::cout << "Mini DBMS listening on " << options.host << ':' << options.port
              << " with " << options.workers << " SQL workers\n" << std::flush;
    app.run();
    stopping.store(true, std::memory_order_release);
    pool.shutdown();
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        usage();
        return 0;
    }
    try {
        return serve(parse_options(argc, argv));
    } catch (const std::exception &error) {
        std::cerr << "minidb_server: " << error.what() << '\n';
        return 1;
    }
}
