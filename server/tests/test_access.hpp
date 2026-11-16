#pragma once

#include "engine.hpp"
#include "writer_preferred_gate.hpp"

namespace minidb {
struct GateSnapshot {
    std::size_t readers;
    std::size_t waiting_writers;
    bool writer;
};

namespace detail {
struct GateTestAccess {
    static GateSnapshot snapshot(WriterPreferredGate& gate) {
        std::lock_guard<std::mutex> lock(gate.mutex_);
        return {gate.readers_, gate.waiting_writers_, gate.writer_};
    }
};
} // namespace detail

// Defined only in the separate test engine library, never in the normal engine.
struct EngineTestAccess {
    static GateSnapshot snapshot(Engine& engine);
};
} // namespace minidb
