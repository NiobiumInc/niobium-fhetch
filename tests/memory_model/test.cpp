// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// Budget-mode tests:
//   1. SpillFile slot round-trip / reuse / peak accounting.
//   2. UseIndex exact next-use semantics.
//   3. The keystone: a synthetic trace run unbounded vs. under a budget
//      small enough to force eviction, spill, and re-faulting — every
//      probed polynomial must be bit-identical, and the eviction/fault
//      counters must show the machinery actually engaged.

#include "niobium/fhetch_sim/poly_source.h"
#include "niobium/fhetch_sim/simulator.h"

#include "spill_file.h"
#include "use_index.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace niobium::fhetch_sim;

namespace {

constexpr uint64_t kRingDim = 4;
constexpr uint64_t kQ = 17;
constexpr int kInputs = 12;      // source-backed addrs %1..%12
constexpr int kComputed = 40;    // computed addrs %100..%139

int g_failures = 0;

void expect(bool cond, const std::string& what) {
    if (!cond) {
        std::cerr << "FAIL: " << what << std::endl;
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------
// 1. SpillFile
// ---------------------------------------------------------------------------

void test_spill_file() {
    SpillFile spill(std::filesystem::temp_directory_path(),
                    kRingDim * sizeof(uint64_t));
    uint64_t a[kRingDim] = {1, 2, 3, 4};
    uint64_t b[kRingDim] = {5, 6, 7, 8};
    uint32_t sa = spill.write(a);
    uint32_t sb = spill.write(b);
    expect(sa != SpillFile::kNoSlot && sb != SpillFile::kNoSlot && sa != sb,
           "spill: two distinct slots");
    uint64_t out[kRingDim] = {0};
    expect(spill.read(sa, out) && out[0] == 1 && out[3] == 4, "spill: read a");
    expect(spill.read(sb, out) && out[0] == 5 && out[3] == 8, "spill: read b");
    expect(spill.live_slots() == 2, "spill: two live slots");
    spill.release(sa);
    expect(spill.live_slots() == 1, "spill: release drops live count");
    uint32_t sc = spill.write(b);
    expect(sc == sa, "spill: freed slot is reused");
    expect(spill.peak_bytes() == 2 * kRingDim * sizeof(uint64_t),
           "spill: peak is two slots");
}

// ---------------------------------------------------------------------------
// 2. UseIndex
// ---------------------------------------------------------------------------

void test_use_index() {
    std::string trace =
        "modulus_count 2\nm[0] 0xFFFFFFFFFFFFFFFF\nm[1] 0x11\n"
        "sr_addp %3, %1, %2, m=1\n"   // i0: uses 1,2,3
        "sr_mulp %4, %3, %1, m=1\n"   // i1: uses 3,1,4
        "sr_addp %5, %4, %4, m=1\n"   // i2: uses 4,5
        "halt\n";
    auto parsed = parse_trace(trace);
    UseIndex idx;
    idx.build(parsed);

    expect(idx.next_use_after(1, -1) == 0, "useindex: %1 first used at i0");
    expect(idx.next_use_after(1, 0) == 1, "useindex: %1 next at i1");
    expect(idx.next_use_after(1, 1) == UseIndex::kNever, "useindex: %1 dead after i1");
    expect(idx.next_use_after(3, -1) == 0, "useindex: %3 write counts as a use");
    expect(idx.next_use_after(3, 0) == 1, "useindex: %3 read at i1");
    expect(idx.next_use_after(5, 1) == 2, "useindex: %5 write at i2");
    expect(idx.next_use_after(99, -1) == UseIndex::kNever, "useindex: unknown addr");
}

// ---------------------------------------------------------------------------
// 3. Budget equivalence
// ---------------------------------------------------------------------------

// Deterministic per-addr input values.
std::vector<uint64_t> input_values(uint64_t addr) {
    std::vector<uint64_t> v(kRingDim);
    for (uint64_t i = 0; i < kRingDim; ++i) v[i] = (addr * 7 + i * 3 + 1) % kQ;
    return v;
}

class FakeSource final : public PolySource {
public:
    bool contains(uint64_t addr) const override {
        return addr >= 1 && addr <= kInputs;
    }
    bool load(uint64_t addr, const PolySink& sink) override {
        if (!contains(addr)) return false;
        ++loads;
        // Deliver the requested addr plus one sibling, mimicking
        // whole-file decodes (sink may decline the sibling).
        LoadedPoly p{addr, kQ, input_values(addr)};
        bool ok = sink(std::move(p));
        uint64_t sib = (addr % kInputs) + 1;
        if (sib != addr) {
            LoadedPoly s{sib, kQ, input_values(sib)};
            sink(std::move(s));
        }
        return ok;
    }
    size_t load_granularity(uint64_t) const override { return 2; }
    size_t loads = 0;
};

// A trace with long-range reuse: computed values are consumed again far
// from their definition, so a small budget must spill and re-fault them.
std::string make_budget_trace(std::vector<uint64_t>& live_out) {
    std::ostringstream t;
    t << "modulus_count 2\nm[0] 0xFFFFFFFFFFFFFFFF\nm[1] 0x11\n";
    for (int i = 0; i < kComputed; ++i) {
        uint64_t dest = 100 + i;
        // src1: a computed value from 8 instructions ago (long-range) or an input.
        std::string src1 = (i >= 8) ? "%" + std::to_string(100 + i - 8)
                                    : "%" + std::to_string((i % kInputs) + 1);
        // src2: inputs revisited throughout the run.
        std::string src2 = "%" + std::to_string(((i * 5) % kInputs) + 1);
        t << "sr_addp %" << dest << ", " << src1 << ", " << src2 << ", m=1\n";
    }
    // Consume the very first computed value at the very end.
    t << "sr_mulp %500, %100, %" << (100 + kComputed - 1) << ", m=1\n";
    t << "halt\n";
    live_out = {500, 100 + kComputed - 1};
    return t.str();
}

void run_sim(const std::filesystem::path& trace_path,
             const std::vector<uint64_t>& live_out, uint64_t budget_bytes,
             std::shared_ptr<PolySource> source, bool preload,
             Simulator& sim) {
    sim.set_ring_dimension(kRingDim);
    expect(sim.load_trace(trace_path), "budget: trace loads");
    sim.set_live_out_addresses(live_out);
    if (source) sim.set_poly_source(std::move(source));
    if (budget_bytes != 0) sim.set_memory_budget({budget_bytes, std::filesystem::temp_directory_path()});
    if (preload)
        for (uint64_t a = 1; a <= kInputs; ++a)
            sim.store_polynomial(a, input_values(a), kQ);
    auto r = sim.run();
    expect(r.errors == 0, "budget: run has zero errors");
}

// 8 polys * ring_dim * 8 bytes — the manager's floor (kMinBudgetPolys).
uint64_t min_budget_bytes() {
    return 8 * kRingDim * sizeof(uint64_t);
}

void test_budget_equivalence() {
    std::vector<uint64_t> live_out;
    std::string trace = make_budget_trace(live_out);
    auto trace_path = std::filesystem::temp_directory_path() /
                      ("budget_test_" + std::to_string(getpid()) + ".fhetch");
    { std::ofstream f(trace_path); f << trace; }

    // Reference: unbounded, inputs preloaded, no freeing at all.
    Simulator ref;
    run_sim(trace_path, live_out, 0, nullptr, /*preload=*/true, ref);

    // Bounded: minimum budget (8 polys), inputs faulted from the source.
    auto source = std::make_shared<FakeSource>();
    Simulator diet;
    run_sim(trace_path, live_out, min_budget_bytes(), source,
            /*preload=*/false, diet);

    // Every probed output must be bit-identical.
    for (uint64_t a : live_out) {
        auto expect_v = ref.get_polynomial(a);
        auto got = diet.get_polynomial(a);
        expect(!expect_v.empty(), "budget: ref has %" + std::to_string(a));
        expect(expect_v == got,
               "budget: %" + std::to_string(a) + " identical under budget");
        expect(ref.get_modulus(a) == diet.get_modulus(a),
               "budget: modulus identical for %" + std::to_string(a));
    }

    auto ms = diet.memory_stats();
    expect(ms.peak_resident_polys <= 8 + 3,
           "budget: peak resident within budget + pin headroom");
    expect(ms.faults_source > 0, "budget: inputs faulted from source");
    expect(ms.faults_spill > 0, "budget: spilled values faulted back");
    expect(ms.evictions_spilled > 0, "budget: dirty evictions spilled");
    expect(ms.evictions_clean > 0, "budget: clean evictions dropped");

    std::filesystem::remove(trace_path);
}

}  // namespace

int main() {
    test_spill_file();
    test_use_index();
    test_budget_equivalence();

    if (g_failures) {
        std::cerr << g_failures << " memory_model test failure(s)" << std::endl;
        return 1;
    }
    std::cout << "memory_model: all checks passed" << std::endl;
    return 0;
}
