// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// Exact next-use index over a parsed trace. The whole trace is known
// before execution, so eviction can be optimal (Belady: evict the
// resident poly whose next use is farthest away) instead of heuristic.
// One pass with classify_uses builds, per address, the ordered list of
// instruction indices that use it (reads AND writes both count); a
// monotonic per-address cursor then answers "next use after i" in
// amortized O(1) during execution.

#pragma once

#include "instruction.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace niobium::fhetch_sim {

class UseIndex {
public:
    static constexpr uint32_t kNever = UINT32_MAX;

    void build(const ParsedTrace& trace);

    // First use of `addr` strictly after instruction `pos` (pass -1 for
    // "before the first instruction"). Advances the address's cursor —
    // callers must query in non-decreasing `pos` order per address.
    uint32_t next_use_after(uint64_t addr, int64_t pos);

    bool known(uint64_t addr) const { return spans_.count(addr) != 0U; }

private:
    struct Span {
        uint32_t begin = 0;   // into uses_
        uint32_t end = 0;
        uint32_t cursor = 0;  // next candidate ≥ begin
    };
    std::vector<uint32_t> uses_;  // instruction indices, grouped by addr
    std::unordered_map<uint64_t, Span> spans_;
};

}  // namespace niobium::fhetch_sim
