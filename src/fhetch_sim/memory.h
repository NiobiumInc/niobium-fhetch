// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// Simulator memory model. Ops write directly into an address's
// existing buffer via reserve_dest/commit_dest; non-SSA repeat writes
// to the same address reuse the buffer in place.

#pragma once

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace niobium::fhetch_sim {

struct MemoryElement {
    std::vector<uint64_t> values;
    uint64_t modulus = 0;
    bool initialized = false;
};

class Memory {
public:
    void set_ring_dim(size_t n) { ring_dim_ = n; }

    // Mutable buffer for `address`, sized to the simulator's ring
    // dimension. In-place hot path when the slot already holds an
    // appropriately-sized buffer. Caller writes every element and
    // then commit_dest().
    std::vector<uint64_t>& reserve_dest(uint64_t address) {
        auto& elem = mem_[address];
        if (elem.values.size() == ring_dim_) {
            return elem.values;
        }
        elem.values.resize(ring_dim_);
        return elem.values;
    }

    void commit_dest(uint64_t address, uint64_t modulus) {
        auto& elem = mem_[address];
        elem.modulus = modulus;
        elem.initialized = true;
    }

    // No rvalue overload by design: the slot's buffer is retained
    // across writes, so the kernel copies into the existing storage.
    void set(uint64_t address, const std::vector<uint64_t>& values, uint64_t modulus) {
        auto& out = reserve_dest(address);
        if (&out != &values) {
            std::copy(values.begin(), values.end(), out.begin());
        }
        commit_dest(address, modulus);
    }

    void erase(uint64_t address) {
        mem_.erase(address);
    }

    const MemoryElement& get(uint64_t address) const {
        static const MemoryElement empty;
        auto it = mem_.find(address);
        return (it != mem_.end()) ? it->second : empty;
    }

    bool is_initialized(uint64_t address) const {
        auto it = mem_.find(address);
        return it != mem_.end() && it->second.initialized;
    }

    size_t size() const { return mem_.size(); }

private:
    std::unordered_map<uint64_t, MemoryElement> mem_;
    size_t ring_dim_ = 0;
};

}  // namespace niobium::fhetch_sim
