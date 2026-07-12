// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// Backing store for evicted computed polynomials. One per-run file of
// fixed-size slots (ring_dim * 8 bytes each) with a free list, so a
// slot read/write is a single pread/pwrite. Created lazily on first
// spill; unlinked immediately after open on POSIX so a crash cannot
// leak it. Coefficients only — modulus and flags stay in the caller's
// in-memory descriptor.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace niobium::fhetch_sim {

class SpillFile {
public:
    static constexpr uint32_t kNoSlot = UINT32_MAX;

    // `dir` is where the spill file is created (callers pass the project
    // dir or an override; large runs should keep it on local disk).
    // `slot_bytes` is the fixed record size (ring_dim * sizeof(uint64_t)).
    SpillFile(std::filesystem::path dir, size_t slot_bytes);
    ~SpillFile();

    SpillFile(const SpillFile&) = delete;
    SpillFile& operator=(const SpillFile&) = delete;

    // Write one slot's worth of data; reuses a freed slot else appends.
    // Returns kNoSlot (with a message on first failure) if the write
    // failed — callers treat that as fatal.
    uint32_t write(const uint64_t* data);

    // Read a previously written slot into `out` (slot_bytes/8 elements).
    // Returns false on I/O error.
    bool read(uint32_t slot, uint64_t* out);

    // Return a slot to the free list.
    void release(uint32_t slot);

    size_t live_slots() const { return next_slot_ - free_.size(); }
    uint64_t peak_bytes() const { return peak_slots_ * static_cast<uint64_t>(slot_bytes_); }

private:
    bool ensure_open();

    std::filesystem::path dir_;
    size_t slot_bytes_;
    int fd_ = -1;
    bool open_failed_ = false;
    uint32_t next_slot_ = 0;   // slots ever allocated (file grows to this)
    uint64_t peak_slots_ = 0;  // max simultaneously-live slots
    std::vector<uint32_t> free_;
};

}  // namespace niobium::fhetch_sim
