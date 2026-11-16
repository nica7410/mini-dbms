#pragma once

// The Phase 1/2 sources are compiled as C. Phase 3 only adds the TableReader /
// executor_run_read functions; existing C APIs and behavior are unchanged.
extern "C" {
#include "../src/common.h"
#include "../src/parser.h"
#include "../src/storage.h"
#include "../src/executor.h"
}
