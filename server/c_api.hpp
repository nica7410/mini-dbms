#pragma once

// The Phase 1/2 sources are compiled as C. Keep their original headers unchanged.
extern "C" {
#include "../src/common.h"
#include "../src/parser.h"
#include "../src/storage.h"
#include "../src/executor.h"
}
