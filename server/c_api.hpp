#pragma once

// The Phase 1/2 sources are compiled as C. Phase 3 adds the TableReader /
// executor_run_read functions and multi-row INSERT (Statement.rows/row_count);
// single-row statements behave as before.
extern "C" {
#include "../src/common.h"
#include "../src/parser.h"
#include "../src/storage.h"
#include "../src/executor.h"
}
