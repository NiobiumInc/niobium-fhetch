// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// FHETCH Simulator — instruction-set simulator for .fhetch trace files.
//
// Reads a FHETCH instruction trace and executes each operation using
// OpenFHE modular arithmetic, producing computed polynomial values
// in memory. This is the functional equivalent of the niobium-compiler's
// FHE_SIM replay target, but operating on the FHETCH text format
// instead of the internal .seq format.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace niobium::fhetch_sim {

class PolySource;

/// Result of running the simulator.
struct SimResult {
    size_t instructions_executed = 0;
    size_t errors = 0;
    double elapsed_seconds = 0.0;
};

/// Bounded-residency configuration (see Simulator::set_memory_budget).
struct MemoryBudgetOptions {
    uint64_t budget_bytes = 0;        ///< 0 = unbounded (legacy behavior).
    std::filesystem::path spill_dir;  ///< For evicted computed polys; empty = cwd.
};

/// Counters reported by Simulator::memory_stats() in budget mode.
struct MemoryStats {
    uint64_t faults_source = 0;      ///< polys loaded from the PolySource
    uint64_t faults_spill = 0;       ///< polys restored from the spill file
    uint64_t evictions_clean = 0;    ///< dropped without writing (reloadable)
    uint64_t evictions_spilled = 0;  ///< written to the spill file first
    uint64_t peak_resident_polys = 0;
    uint64_t peak_spill_bytes = 0;
    long peak_rss_mb = 0;
};

/// FHETCH instruction-set simulator.
///
/// Usage:
///   Simulator sim;
///   sim.set_ring_dimension(4096);
///   sim.load_trace("program.fhetch");
///   sim.load_input("a", {1, 2, 3, ...}, modulus);
///   auto result = sim.run();
///   auto values = sim.get_polynomial(address);
///
class Simulator {
public:
    Simulator();
    ~Simulator();

    Simulator(const Simulator&) = delete;
    Simulator& operator=(const Simulator&) = delete;

    /// Set the ring dimension (N) for all polynomials.
    void set_ring_dimension(uint64_t N);

    /// Load a .fhetch trace file.
    /// @return true if parsing succeeded.
    bool load_trace(const std::filesystem::path& trace_file);

    /// Store a polynomial in simulator memory at the given address.
    /// Used to populate inputs before running. The rvalue overload
    /// donates the buffer instead of copying.
    void store_polynomial(uint64_t address, const std::vector<uint64_t>& values,
                          uint64_t modulus);
    void store_polynomial(uint64_t address, std::vector<uint64_t>&& values,
                          uint64_t modulus);

    /// Run the loaded trace.
    SimResult run();

    /// Retrieve a polynomial from simulator memory after execution.
    /// @return empty vector if address is uninitialized.
    std::vector<uint64_t> get_polynomial(uint64_t address) const;

    /// Get the modulus associated with a memory address.
    uint64_t get_modulus(uint64_t address) const;

    /// Check if a memory address is initialized.
    bool is_initialized(uint64_t address) const;

    /// Get the set of addresses that are read before being written in the trace.
    /// These are the "live-in" addresses that need input data.
    std::vector<uint64_t> get_read_before_write_addresses() const;

    /// Scan the trace for instructions that initialize an address with a
    /// known constant value of zero (e.g. `sr_mulps %x, %x, 0, m=...`)
    /// BEFORE it appears as a read anywhere, and materialize the zero
    /// vector in simulator memory at that address. Called by replay()
    /// as part of the load-time population so the data-parent chain can
    /// propagate from zero-initialized sources.

    /// Mark addresses that must NOT be freed during execution. The compiler's
    /// replay() pass calls this with every probe/output address before run(),
    /// so write_replay_outputs() can still read them after execution finishes.
    void set_live_out_addresses(const std::vector<uint64_t>& addrs);

    /// Liveness-driven free schedule: a memory.erase() is scheduled at
    /// each address's last read, and at every write whose value is never
    /// read afterwards (dead stores and rewrites after the last read —
    /// near-SSA traces retain hundreds of thousands of such buffers
    /// otherwise). Must be called after load_trace() and (optionally)
    /// set_live_out_addresses(); live-out addresses are never freed.
    /// `NIOBIUM_DISABLE_SIM_FREES` skips the pass entirely.
    void compute_liveness();

    /// Test-only accessor: returns the per-instruction free schedule built
    /// by compute_liveness(). Each inner vector holds the addresses freed
    /// after the instruction at that index. Used by test_simulator_liveness;
    /// production callers should not rely on this.
    const std::vector<std::vector<uint64_t>>& get_free_after_for_test() const;

    /// Bound resident polynomial memory to `opts.budget_bytes`: reads are
    /// faulted in on demand (spill file first, then the PolySource), the
    /// eviction victim is always the resident poly with the farthest next
    /// use (exact — the whole trace is known), and evicted computed values
    /// spill to a per-run file in `opts.spill_dir`. Replaces
    /// compute_liveness() — dead polys are freed at the same instructions
    /// by the next-use index. Call AFTER load_trace() and
    /// set_live_out_addresses(), BEFORE store_polynomial()/run().
    /// Results are bit-identical to an unbounded run.
    void set_memory_budget(const MemoryBudgetOptions& opts);

    /// Backing store for lazily loaded inputs/keys in budget mode.
    /// Call before set_memory_budget().
    void set_poly_source(std::shared_ptr<PolySource> source);

    /// Budget-mode counters (zeros when unbounded).
    MemoryStats memory_stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace niobium::fhetch_sim
