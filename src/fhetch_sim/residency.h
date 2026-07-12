// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// Budget-bounded residency for simulator memory. Sits beside Memory
// (which stays the dumb resident container) and is driven from the
// execute loop at instruction boundaries only — never mid-kernel, so
// kernels can keep holding references into the map:
//
//   begin_instruction: pin this instruction's operands, fault reads in
//     (spill slot first, then the PolySource), reserve the dest
//     write-only, then evict down to budget (Belady: farthest next use
//     first, from the exact UseIndex).
//   end_instruction: advance next-use cursors, drop dead polys, mark
//     the dest dirty (its spill slot / source copy are now stale).
//
// Eviction decision: dead → free; clean (valid spill slot, or an input
// never overwritten) → drop; dirty → spill then drop. Live-out polys
// may be spilled — get_polynomial faults them back after the run.

#pragma once

#include "memory.h"
#include "spill_file.h"
#include "use_index.h"

#include "niobium/fhetch_sim/poly_source.h"
#include "niobium/fhetch_sim/simulator.h"  // MemoryStats

#include <cstdint>
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace niobium::fhetch_sim {

class ResidencyManager {
public:
    ResidencyManager(Memory& memory, uint64_t ring_dim, uint64_t budget_bytes,
                     const std::filesystem::path& spill_dir,
                     std::shared_ptr<PolySource> source,
                     std::unordered_set<uint64_t> live_out);

    // Smallest budget that can hold one instruction's operands plus
    // kernel scratch; enforced by the Simulator before running.
    static constexpr uint64_t kMinBudgetPolys = 8;
    uint64_t budget_polys() const { return budget_polys_; }

    void set_use_index(UseIndex&& idx) { use_index_ = std::move(idx); }

    // --- instruction-boundary protocol (execute loop) ---
    void begin_instruction(uint32_t i, const InstUses& u);
    void end_instruction(uint32_t i, const InstUses& u);

    // --- population (store_polynomial) and post-run readback ---
    // Called right after the simulator stored `addr` into Memory.
    void notify_stored(uint64_t addr);
    // Make `addr` resident again for readback (get_polynomial).
    // Returns false if it is neither resident nor recoverable.
    bool fault_for_read(uint64_t addr);

    const MemoryStats& stats() const { return stats_; }
    void note_rss(long rss_mb) {
        if (rss_mb > stats_.peak_rss_mb) stats_.peak_rss_mb = rss_mb;
    }

private:
    struct PolyDesc {
        uint32_t next_use = 0;  // last value handed to the heap
        uint32_t spill_slot = SpillFile::kNoSlot;
        uint64_t modulus = 0;   // for restoring evicted polys
        bool resident = false;
        bool dirty = false;         // no valid disk copy (spill slot aside)
        bool source_valid = false;  // reloadable from source_
        bool live_out = false;
    };

    PolyDesc& desc(uint64_t addr);
    bool is_pinned(uint64_t addr) const;
    void ensure_resident(uint64_t addr, int64_t pos);
    void make_room_for(size_t incoming_polys);
    void evict_to(uint64_t target_polys);
    void evict_one(uint64_t addr, PolyDesc& d);
    void free_poly(uint64_t addr, PolyDesc& d);
    void push_heap(uint64_t addr, uint32_t next_use);

    Memory& memory_;
    uint64_t ring_dim_;
    uint64_t budget_polys_;
    UseIndex use_index_;
    SpillFile spill_;
    std::shared_ptr<PolySource> source_;
    std::unordered_map<uint64_t, PolyDesc> descs_;
    // Max-heap of (next_use, addr); entries are lazily invalidated by
    // comparing against desc.next_use / residency when popped.
    std::priority_queue<std::pair<uint32_t, uint64_t>> victims_;
    uint64_t resident_polys_ = 0;
    uint64_t pinned_[3] = {0, 0, 0};
    int n_pinned_ = 0;
    bool warned_unenforceable_ = false;
    MemoryStats stats_;
};

}  // namespace niobium::fhetch_sim
