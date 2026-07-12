// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// Tests for the simulator's liveness-driven free schedule
// (Simulator::compute_liveness): frees at each address's last read AND
// at dead writes (a write nothing reads afterwards), with live-out
// addresses pinned. Near-SSA traces retain every dead result buffer
// without the dead-write pass, so this is load-bearing for memory use.

#include "niobium/fhetch_sim/simulator.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

using niobium::fhetch_sim::Simulator;

namespace {

constexpr uint64_t kRingDim = 4;
constexpr uint64_t kQ = 17;

int g_failures = 0;

void expect(bool cond, const std::string& what) {
    if (!cond) {
        std::cerr << "FAIL: " << what << std::endl;
        ++g_failures;
    }
}

bool freed_at(const std::vector<std::vector<uint64_t>>& sched, size_t inst,
              uint64_t addr) {
    if (inst >= sched.size()) return false;
    const auto& v = sched[inst];
    return std::find(v.begin(), v.end(), addr) != v.end();
}

bool freed_anywhere(const std::vector<std::vector<uint64_t>>& sched, uint64_t addr) {
    for (const auto& v : sched)
        if (std::find(v.begin(), v.end(), addr) != v.end()) return true;
    return false;
}

}  // namespace

int main() {
    // i0: %3 = %1 + %2      last read of %2; %3 read later (not dead)
    // i1: %4 = %3 * %1      last read of %3 and of %1; %4 read later
    // i2: %1 = %4 * 5       rewrite of %1 after its last read; never read again
    // i3: %5 = %4 + %4      %5 never read (dead store)
    // i4: %6 = %6 * 0       zero-init idiom (write-only); %6 read at i5
    // i5: %7 = %6 + %4      %7 is live-out; last read of %6 and %4
    // i6: %8 = %8 * 0       zero-init idiom, never read (dead store)
    const std::string trace =
        "modulus_count 2\n"
        "m[0] 0xFFFFFFFFFFFFFFFF\n"
        "m[1] 0x11\n"
        "# Instructions\n"
        "sr_addp %3, %1, %2, m=1\n"
        "sr_mulp %4, %3, %1, m=1\n"
        "sr_mulps %1, %4, 5, m=1\n"
        "sr_addp %5, %4, %4, m=1\n"
        "sr_mulps %6, %6, 0, m=1\n"
        "sr_addp %7, %6, %4, m=1\n"
        "sr_mulps %8, %8, 0, m=1\n"
        "halt\n";

    auto trace_path = std::filesystem::temp_directory_path() /
                      ("liveness_test_" + std::to_string(getpid()) + ".fhetch");
    {
        std::ofstream f(trace_path);
        f << trace;
    }

    Simulator sim;
    sim.set_ring_dimension(kRingDim);
    if (!sim.load_trace(trace_path)) {
        std::cerr << "FAIL: could not load trace" << std::endl;
        return 1;
    }

    // Only %1 and %2 are genuine live-ins: the zero-init writes to %6/%8
    // must NOT count as reads of their (aliased) source operand.
    auto rbw = sim.get_read_before_write_addresses();
    std::sort(rbw.begin(), rbw.end());
    expect(rbw == std::vector<uint64_t>({1, 2}),
           "read-before-write set is exactly {1, 2}");

    sim.set_live_out_addresses({7});
    sim.compute_liveness();
    const auto& sched = sim.get_free_after_for_test();
    expect(sched.size() == 8, "schedule sized to instruction count (incl. halt)");

    expect(freed_at(sched, 0, 2), "%2 freed at its last read (i0)");
    expect(!freed_at(sched, 0, 1), "%1 not freed at i0 (read again at i1)");
    expect(!freed_at(sched, 0, 3), "%3 not freed at its defining write (read at i1)");
    expect(freed_at(sched, 1, 3), "%3 freed at its last read (i1)");
    expect(freed_at(sched, 1, 1), "%1 freed at its last read (i1)");
    expect(freed_at(sched, 2, 1), "%1 freed again at its dead rewrite (i2)");
    expect(freed_at(sched, 3, 5), "%5 freed at its never-read write (i3)");
    expect(sched[4].empty(), "zero-init %6 not freed at i4 (read at i5)");
    expect(freed_at(sched, 5, 6), "%6 freed at its last read (i5)");
    expect(freed_at(sched, 5, 4), "%4 freed at its last read (i5)");
    expect(freed_at(sched, 6, 8), "%8 freed at its never-read zero-init (i6)");
    expect(!freed_anywhere(sched, 7), "live-out %7 never freed");

    // Full-run integration: after run(), only the live-out address remains.
    sim.store_polynomial(1, {1, 2, 3, 4}, kQ);
    sim.store_polynomial(2, {5, 6, 7, 8}, kQ);
    auto result = sim.run();
    expect(result.errors == 0, "run completes without errors");
    expect(sim.is_initialized(7), "live-out %7 resident after run");
    expect(sim.get_polynomial(7).size() == kRingDim, "live-out %7 has full data");
    for (uint64_t a : {1ULL, 2ULL, 3ULL, 4ULL, 5ULL, 6ULL, 8ULL})
        expect(!sim.is_initialized(a),
               "%" + std::to_string(a) + " freed by end of run");

    std::filesystem::remove(trace_path);

    if (g_failures) {
        std::cerr << g_failures << " liveness test failure(s)" << std::endl;
        return 1;
    }
    std::cout << "simulator_liveness: all checks passed" << std::endl;
    return 0;
}
