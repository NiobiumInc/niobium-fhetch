// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// Abstract backing store for lazily loaded polynomials. The simulator's
// budget mode faults input/key data in through this interface on first
// read instead of preloading it; implementations decode from disk (see
// src/replay_poly_source.h). The simulator core stays serialization-
// agnostic: cereal/OpenFHE decoding lives entirely in the implementation.

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace niobium::fhetch_sim {

/// One polynomial tower delivered by a source.
struct LoadedPoly {
    uint64_t addr = 0;
    uint64_t modulus = 0;
    std::vector<uint64_t> values;
};

/// Receives polys decoded by a source. A single load() may deliver
/// several polys (whole-file decodes hand over siblings as a side
/// effect); the sink returns true if it kept the poly.
using PolySink = std::function<bool(LoadedPoly&&)>;

class PolySource {
public:
    virtual ~PolySource() = default;

    /// True if this source can produce data for `addr`.
    virtual bool contains(uint64_t addr) const = 0;

    /// Deliver `addr` (and possibly siblings) through `sink`.
    /// @return true if `addr` itself was delivered.
    virtual bool load(uint64_t addr, const PolySink& sink) = 0;

    /// Number of polys decoded together with `addr` in one load()
    /// (file granularity). Lets the caller make room up front.
    virtual size_t load_granularity(uint64_t addr) const = 0;
};

}  // namespace niobium::fhetch_sim
