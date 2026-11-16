// Linked only into minidb_server_test. No control interface exists in production.
#include "c_api.hpp"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <unistd.h>

// SELECT runs through executor_run_read (worker reader handle).
extern "C" int __real_executor_run_read(TableReader*, const Statement*, const ResultSink*, DbError*);
extern "C" int __wrap_executor_run_read(TableReader* reader, const Statement* statement,
                                        const ResultSink* sink, DbError* error) {
    static std::atomic<bool> first_scan{true};
    const char* enter = std::getenv("MINIDB_TEST_ENTER_FD");
    const char* release = std::getenv("MINIDB_TEST_RELEASE_FD");
    if (enter && release && statement->where == WHERE_NONE && first_scan.exchange(false)) {
        char token = 'x';
        ssize_t count;
        do { count = write(std::atoi(enter), &token, 1); } while (count < 0 && errno == EINTR);
        if (count == 1) {
            do { count = read(std::atoi(release), &token, 1); } while (count < 0 && errno == EINTR);
        }
        if (count != 1) {
            db_error_set(error, "test executor handshake failed");
            return -1;
        }
    }
    return __real_executor_run_read(reader, statement, sink, error);
}
