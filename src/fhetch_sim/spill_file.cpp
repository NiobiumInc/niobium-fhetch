// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.

#include "spill_file.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>

namespace niobium::fhetch_sim {

SpillFile::SpillFile(std::filesystem::path dir, size_t slot_bytes)
    : dir_(std::move(dir)), slot_bytes_(slot_bytes) {}

SpillFile::~SpillFile() {
    if (fd_ >= 0) ::close(fd_);
    // The file was unlinked right after creation; nothing else to clean up.
}

bool SpillFile::ensure_open() {
    if (fd_ >= 0) return true;
    if (open_failed_) return false;

    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);  // best effort
    auto path = dir_ / ("fhetch_spill_" + std::to_string(::getpid()) + ".bin");
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd_ < 0) {
        std::cerr << "[FHETCH_SIM] cannot create spill file " << path << ": "
                  << std::strerror(errno)
                  << " (set NIOBIUM_FHETCH_SPILL_DIR to a writable local dir)"
                  << std::endl;
        open_failed_ = true;
        return false;
    }
    // Unlink immediately: the fd keeps the data alive, a crash leaks nothing.
    ::unlink(path.c_str());
    return true;
}

uint32_t SpillFile::write(const uint64_t* data) {
    if (!ensure_open()) return kNoSlot;

    uint32_t slot;
    if (!free_.empty()) {
        slot = free_.back();
        free_.pop_back();
    } else {
        slot = next_slot_++;
    }

    off_t off = static_cast<off_t>(slot) * static_cast<off_t>(slot_bytes_);
    ssize_t n = ::pwrite(fd_, data, slot_bytes_, off);
    if (n != static_cast<ssize_t>(slot_bytes_)) {
        std::cerr << "[FHETCH_SIM] spill write failed (slot " << slot << ", "
                  << std::strerror(errno) << ") — is the spill disk full?"
                  << std::endl;
        free_.push_back(slot);
        return kNoSlot;
    }
    if (live_slots() > peak_slots_) peak_slots_ = live_slots();
    return slot;
}

bool SpillFile::read(uint32_t slot, uint64_t* out) {
    if (fd_ < 0) return false;
    off_t off = static_cast<off_t>(slot) * static_cast<off_t>(slot_bytes_);
    ssize_t n = ::pread(fd_, out, slot_bytes_, off);
    if (n != static_cast<ssize_t>(slot_bytes_)) {
        std::cerr << "[FHETCH_SIM] spill read failed (slot " << slot << ", "
                  << std::strerror(errno) << ")" << std::endl;
        return false;
    }
    return true;
}

void SpillFile::release(uint32_t slot) {
    if (slot != kNoSlot) free_.push_back(slot);
}

}  // namespace niobium::fhetch_sim
