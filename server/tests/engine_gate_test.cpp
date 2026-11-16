#include "c_api.hpp"
#include "test_access.hpp"
#include "concurrency_support.hpp"

#include <atomic>
#include <filesystem>
#include <iostream>

using namespace minidb::test;
namespace {
Event scan_entered, release_scan, insert_entered;
std::atomic<bool> intercept_scan{true};
}

extern "C" int __real_executor_run(Table*, const Statement*, const ResultSink*, DbError*);
extern "C" int __wrap_executor_run(Table* table, const Statement* statement,
                                   const ResultSink* sink, DbError* error) {
    if (statement->kind == STMT_SELECT && statement->where == WHERE_NONE && intercept_scan.exchange(false)) {
        scan_entered.set();
        release_scan.wait();
    }
    if (statement->kind == STMT_INSERT) insert_entered.set();
    return __real_executor_run(table, statement, sink, error);
}

int main() {
    const auto path = std::filesystem::temp_directory_path() /
        ("minidb-gate-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(path);
        {
            minidb::EngineOptions options;
            options.data_dir = path.string();
            options.readers = 2;
            minidb::Engine engine(options);
            minidb::QueryResult scanned, inserted;
            Threads threads([&] { release_scan.set(); });
            threads.start([&] { scanned = engine.execute(0, "SELECT * FROM users;"); });
            scan_entered.wait();
            require(minidb::EngineTestAccess::snapshot(engine).readers == 1,
                    "SELECT released its gate before the C executor call");
            threads.start([&] {
                inserted = engine.execute(1, "INSERT INTO users (name, age) VALUES ('after-scan', 1);");
            });
            await([&] { return minidb::EngineTestAccess::snapshot(engine).waiting_writers == 1; });
            require(!insert_entered.is_set(), "INSERT entered while a SELECT held the gate");
            release_scan.set();
            threads.finish();
            require(scanned.status == minidb::QueryStatus::Ok && scanned.rows.empty(),
                    "scan observed a write that should have waited");
            require(inserted.status == minidb::QueryStatus::Ok && inserted.inserted_id == 1,
                    "waiting INSERT did not complete");
            auto after = engine.execute(0, "SELECT * FROM users WHERE id = 1;");
            require(after.status == minidb::QueryStatus::Ok && after.rows.size() == 1,
                    "completed INSERT is not visible");
        }
        std::filesystem::remove_all(path);
        std::cout << "Engine gate integration passed\n";
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
