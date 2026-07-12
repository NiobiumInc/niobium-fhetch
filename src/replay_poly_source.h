// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// Disk-backed PolySource for budget-mode replay: knows where every
// recorded input/key polynomial lives without loading any of it.
//
//  - Per-input .bin files (one ciphertext each): faulting any address
//    stream-decodes the whole file and offers every tower to the sink.
//  - Key archives (.mk/.rk/.bp.bin — up to GBs, cereal = sequential
//    only): converted ONCE into a flat fixed-record sidecar
//    (<prog>.<kind>.polycache) so any tower afterwards is a single
//    pread. Validated against the source file's size+mtime+ring_dim;
//    written to a temp name and atomically renamed, so concurrent runs
//    race benignly.

#pragma once

#include "niobium/fhetch_sim/poly_source.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace niobium {

class DiskPolySource final : public fhetch_sim::PolySource {
public:
    // Eagerly parses <prog>.inputs.json + every .ids (small), builds or
    // validates the key sidecars (one streaming pass over each key bin on
    // first run). Loads no polynomial data. `cache_dir` empty = `dir`.
    // Returns nullptr with `error` set on failure.
    static std::shared_ptr<DiskPolySource> open(const std::filesystem::path& dir,
                                                const std::string& prog,
                                                uint64_t ring_dim,
                                                std::filesystem::path cache_dir,
                                                std::string* error);
    ~DiskPolySource() override;

    bool contains(uint64_t addr) const override;
    bool load(uint64_t addr, const fhetch_sim::PolySink& sink) override;
    size_t load_granularity(uint64_t addr) const override;

    size_t indexed_polys() const { return index_.size(); }

private:
    DiskPolySource() = default;

    struct InputFile {
        std::filesystem::path bin;
        std::vector<uint64_t> ids;  // tower addresses, in decode order
    };
    struct CacheFile {
        std::filesystem::path path;
        int fd = -1;
        uint64_t count = 0;
        uint64_t data_off = 0;  // file offset of record 0's coefficients
    };
    struct Rec {
        enum class Kind : uint8_t { Input, KeyCache } kind;
        uint32_t file = 0;    // index into inputs_ or caches_
        uint32_t record = 0;  // KeyCache: record number
        uint64_t modulus = 0; // KeyCache: from the sidecar index
    };

    bool index_inputs(const std::filesystem::path& dir, const std::string& prog,
                      std::string* error);
    bool prepare_key_cache(const std::filesystem::path& dir,
                           const std::string& prog, const std::string& kind,
                           const std::filesystem::path& cache_dir,
                           std::string* error);

    uint64_t ring_dim_ = 0;
    std::vector<InputFile> inputs_;
    std::vector<CacheFile> caches_;
    std::unordered_map<uint64_t, Rec> index_;
};

}  // namespace niobium
