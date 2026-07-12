// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.

#include "use_index.h"

namespace niobium::fhetch_sim {

void UseIndex::build(const ParsedTrace& trace) {
    uses_.clear();
    spans_.clear();

    const auto& insts = trace.instructions;

    // Pass 1: count uses per address.
    std::unordered_map<uint64_t, uint32_t> counts;
    auto count_one = [&counts](uint64_t a) {
        if (a != 0) ++counts[a];
    };
    for (const auto& inst : insts) {
        auto u = classify_uses(inst);
        for (int j = 0; j < u.n_reads; ++j) count_one(u.reads[j]);
        if (u.writes) count_one(u.write);
    }

    // Prefix-sum into spans over one flat array.
    uint32_t total = 0;
    spans_.reserve(counts.size());
    for (const auto& [addr, n] : counts) {
        spans_[addr] = {total, total + n, total};
        total += n;
    }
    uses_.resize(total);

    // Pass 2: fill. Instruction order is increasing, so each span ends up
    // sorted; a read and a write of the same addr in one instruction
    // produce two identical entries, which the cursor skips together.
    std::unordered_map<uint64_t, uint32_t> fill;
    fill.reserve(counts.size());
    auto fill_one = [this, &fill](uint64_t a, uint32_t i) {
        if (a == 0) return;
        auto [it, inserted] = fill.try_emplace(a, spans_[a].begin);
        uses_[it->second++] = i;
    };
    for (uint32_t i = 0; i < insts.size(); ++i) {
        auto u = classify_uses(insts[i]);
        for (int j = 0; j < u.n_reads; ++j) fill_one(u.reads[j], i);
        if (u.writes) fill_one(u.write, i);
    }
}

uint32_t UseIndex::next_use_after(uint64_t addr, int64_t pos) {
    auto it = spans_.find(addr);
    if (it == spans_.end()) return kNever;
    auto& s = it->second;
    while (s.cursor < s.end &&
           static_cast<int64_t>(uses_[s.cursor]) <= pos)
        ++s.cursor;
    return (s.cursor < s.end) ? uses_[s.cursor] : kNever;
}

}  // namespace niobium::fhetch_sim
