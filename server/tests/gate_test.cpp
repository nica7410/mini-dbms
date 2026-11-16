#include "test_access.hpp"
#include "concurrency_support.hpp"
#include "benchmarks/fair_gate.hpp"

#include <cstdint>
#include <iostream>
#include <shared_mutex>
#include <string>

namespace minidb::detail {
struct FairGateTestAccess {
    // Registered requests that have taken a ticket but are not yet admitted.
    static std::uint64_t waiting(FairGate& gate) {
        std::lock_guard<std::mutex> lock(gate.mutex_);
        return gate.next_ticket_ - gate.serving_;
    }
    static std::uint64_t readers(FairGate& gate) {
        std::lock_guard<std::mutex> lock(gate.mutex_);
        return gate.readers_;
    }
};
} // namespace minidb::detail

using minidb::detail::FairGate;
using minidb::detail::FairGateTestAccess;
using minidb::detail::WriterPreferredGate;
using minidb::detail::GateTestAccess;
using namespace minidb::test;

void writer_before_later_reader() {
    WriterPreferredGate gate;
    Event writer_entered, release_writer, reader_entered;
    std::shared_lock<WriterPreferredGate> reader_a(gate);
    Threads threads([&] {
        if (reader_a.owns_lock()) reader_a.unlock();
        release_writer.set();
    });
    threads.start([&] {
        std::unique_lock<WriterPreferredGate> lock(gate);
        writer_entered.set();
        release_writer.wait();
    });
    await([&] { return GateTestAccess::snapshot(gate).waiting_writers == 1; });
    // A is still reading, so the observed writer cannot leave its wait state.
    const bool bypassed = gate.try_lock_shared();
    if (bypassed) gate.unlock_shared();
    require(!bypassed, "new reader bypassed a registered waiting writer");
    threads.start([&] {
        std::shared_lock<WriterPreferredGate> lock(gate);
        reader_entered.set();
    });
    reader_a.unlock();
    writer_entered.wait();
    require(!reader_entered.is_set(), "reader entered before the writer released its lock");
    const bool during_write = gate.try_lock_shared();
    if (during_write) gate.unlock_shared();
    require(!during_write, "reader entered during a write");
    release_writer.set();
    reader_entered.wait();
    threads.finish();
}

void writer_waits_for_all_readers() {
    WriterPreferredGate gate;
    Event second_entered, release_second, second_finished, writer_entered;
    std::shared_lock<WriterPreferredGate> first(gate);
    Threads threads([&] {
        if (first.owns_lock()) first.unlock();
        release_second.set();
    });
    threads.start([&] {
        {
            std::shared_lock<WriterPreferredGate> second(gate);
            second_entered.set();
            release_second.wait();
        }
        second_finished.set();
    });
    second_entered.wait();
    require(GateTestAccess::snapshot(gate).readers == 2, "readers did not share the gate");
    threads.start([&] {
        std::unique_lock<WriterPreferredGate> lock(gate);
        writer_entered.set();
    });
    await([&] { return GateTestAccess::snapshot(gate).waiting_writers == 1; });
    release_second.set();
    second_finished.wait();
    require(!writer_entered.is_set() && GateTestAccess::snapshot(gate).readers == 1,
            "writer ignored the remaining reader");
    first.unlock();
    writer_entered.wait();
    threads.finish();
}

// Benchmark candidate: a reader that arrives after a waiting writer stays behind it.
void fair_reader_does_not_pass_waiting_writer() {
    FairGate gate;
    Event writer_entered, release_writer, reader_entered;
    std::shared_lock<FairGate> reader_a(gate);
    Threads threads([&] {
        if (reader_a.owns_lock()) reader_a.unlock();
        release_writer.set();
    });
    threads.start([&] {
        std::unique_lock<FairGate> lock(gate);
        writer_entered.set();
        release_writer.wait();
    });
    await([&] { return FairGateTestAccess::waiting(gate) == 1; });
    const bool bypassed = gate.try_lock_shared();
    if (bypassed) gate.unlock_shared();
    require(!bypassed, "fair: new reader bypassed a registered waiting writer");
    threads.start([&] {
        std::shared_lock<FairGate> lock(gate);
        reader_entered.set();
    });
    await([&] { return FairGateTestAccess::waiting(gate) == 2; });
    reader_a.unlock();
    writer_entered.wait();
    require(!reader_entered.is_set(), "fair: later reader entered before the writer");
    release_writer.set();
    reader_entered.wait();
    threads.finish();
}

// Benchmark candidate: waiting readers are admitted together and a later writer
// cannot pass them (the writer-preferred gate would admit that writer first).
void fair_writer_does_not_pass_waiting_readers() {
    FairGate gate;
    Event r1_entered, r2_entered, release_readers, w2_entered;
    std::unique_lock<FairGate> w1(gate);
    Threads threads([&] {
        if (w1.owns_lock()) w1.unlock();
        release_readers.set();
    });
    auto reader = [&](Event& entered) {
        std::shared_lock<FairGate> lock(gate);
        entered.set();
        release_readers.wait();
    };
    threads.start([&] { reader(r1_entered); });
    await([&] { return FairGateTestAccess::waiting(gate) == 1; });
    threads.start([&] { reader(r2_entered); });
    await([&] { return FairGateTestAccess::waiting(gate) == 2; });
    threads.start([&] {
        std::unique_lock<FairGate> lock(gate);
        w2_entered.set();
    });
    await([&] { return FairGateTestAccess::waiting(gate) == 3; });
    w1.unlock();
    r1_entered.wait();
    r2_entered.wait();
    require(FairGateTestAccess::readers(gate) == 2, "fair: consecutive readers were not admitted together");
    require(!w2_entered.is_set(), "fair: later writer passed waiting readers");
    release_readers.set();
    w2_entered.wait();
    threads.finish();
}

int main() {
    try {
        writer_before_later_reader();
        writer_waits_for_all_readers();
        std::cout << "Gate: 2 deterministic scenarios passed\n";
        fair_reader_does_not_pass_waiting_writer();
        fair_writer_does_not_pass_waiting_readers();
        std::cout << "FairGate (benchmark candidate): 2 deterministic scenarios passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
