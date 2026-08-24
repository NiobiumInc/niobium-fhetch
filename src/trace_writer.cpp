// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.

#include "trace_writer.h"

#include "niobium/fhetch_writer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <utility>

namespace niobium {

TraceWriter::TraceWriter() {
    // Reserve index 0 for the copy/zero-init sentinel modulus.
    modulus_table_.push_back(COPY_MODULUS_VALUE);
    modulus_index_[COPY_MODULUS_VALUE] = COPY_MODULUS_INDEX;
}

void TraceWriter::set_program_info(const std::string& name,
                                   const std::string& version,
                                   const std::string& description) {
    program_name_ = name;
    program_version_ = version;
    program_description_ = description;
}

void TraceWriter::set_source_info(const std::string& file, int line,
                                  const std::string& timestamp) {
    source_file_ = file;
    source_line_ = line;
    build_timestamp_ = timestamp;
}

void TraceWriter::start_recording() {
    std::scoped_lock lock(mutex_);
    recording_ = true;
    paused_ = false;
}

void TraceWriter::stop_recording() {
    std::scoped_lock lock(mutex_);
    recording_ = false;
    paused_ = false;
}

void TraceWriter::pause_recording() {
    std::scoped_lock lock(mutex_);
    paused_ = true;
}

void TraceWriter::resume_recording() {
    std::scoped_lock lock(mutex_);
    paused_ = false;
}

uint32_t TraceWriter::register_modulus(uint64_t modulus) {
    std::scoped_lock lock(mutex_);
    auto it = modulus_index_.find(modulus);
    if (it != modulus_index_.end())
        return it->second;
    uint32_t idx = static_cast<uint32_t>(modulus_table_.size());
    modulus_table_.push_back(modulus);
    modulus_index_[modulus] = idx;
    return idx;
}

void TraceWriter::emit(const fhetch::Instruction& instruction) {
    std::scoped_lock lock(mutex_);
    if (!recording_ || paused_) return;

    // Reject rather than truncate: a narrowed address is a different but
    // perfectly valid address, so truncating would corrupt the program instead
    // of failing it. The reader refuses the same case.
    const uint64_t widest = std::max({static_cast<uint64_t>(instruction.dest),
                                      static_cast<uint64_t>(instruction.src1),
                                      static_cast<uint64_t>(instruction.src2)});
    if (widest > fhetch::kMaxAddress) {
        const char* m = fhetch::fh_opcode_mnemonic(instruction.opcode);
        std::cerr << "[FHETCH] WARNING: dropping " << (m ? m : "instruction")
                  << ": address " << widest << " exceeds the "
                  << fhetch::kMaxAddress << " an instruction can hold"
                  << std::endl;
        return;
    }
    instructions_.push_back(instruction);
}


void TraceWriter::comment(const std::string& text) {
    std::scoped_lock lock(mutex_);
    if (recording_ && !paused_) {
        annotations_.push_back({0, text, instructions_.size()});
    }
}

void TraceWriter::normalize_modulus_table() {
    std::scoped_lock lock(mutex_);
    normalize_modulus_table_locked();
}

void TraceWriter::normalize_modulus_table_locked() {
    if (modulus_table_.size() <= 1) return;  // only sentinel — nothing to sort

    std::vector<uint64_t> regular(modulus_table_.begin() + 1, modulus_table_.end());
    std::sort(regular.begin(), regular.end());

    // Build old-index -> new-index map (sentinel stays at 0).
    std::vector<uint32_t> remap(modulus_table_.size());
    remap[COPY_MODULUS_INDEX] = COPY_MODULUS_INDEX;
    std::unordered_map<uint64_t, uint32_t> new_index;
    new_index[COPY_MODULUS_VALUE] = COPY_MODULUS_INDEX;
    for (size_t i = 0; i < regular.size(); ++i)
        new_index[regular[i]] = static_cast<uint32_t>(i + 1);
    for (size_t i = 0; i < modulus_table_.size(); ++i)
        remap[i] = new_index[modulus_table_[i]];

    // Rewrite table and index map.
    modulus_table_.resize(1);
    for (uint64_t q : regular) modulus_table_.push_back(q);
    modulus_index_ = std::move(new_index);

    // Remap each instruction's modulus index. A literal modulus is a prime,
    // not an index, so it is left alone. This used to rewrite "m=N" digits
    // inside formatted strings, which meant scanning comment text too.
    for (auto& inst : instructions_) {
        if (!inst.modulus.has_value() || inst.modulus_is_literal) continue;
        const uint64_t old_index = *inst.modulus;
        if (old_index < remap.size()) inst.modulus = remap[old_index];
    }
}

std::filesystem::path TraceWriter::write(const std::filesystem::path& directory,
                                         const std::string& program_name) {
    std::scoped_lock lock(mutex_);
    normalize_modulus_table_locked();

    std::filesystem::create_directories(directory);
    auto path = directory / (program_name + ".fhetch");

    std::ofstream out(path);
    if (!out.is_open()) {
        std::cerr << "[FHETCH] ERROR: Cannot write trace to " << path << std::endl;
        return {};
    }

    // Render through the shared writer, so this producer and every other
    // spell a program the same way. The counts describe what is actually being
    // written — which is why the instruction count is now the number of
    // instructions rather than the number of lines.
    fhetch::TraceMetadata metadata;
    metadata.program_name = program_name_;
    metadata.program_version = program_version_;
    metadata.description = program_description_;
    metadata.source_file = source_file_;
    metadata.source_line = source_line_;
    metadata.build_timestamp = build_timestamp_;
    metadata.generated_timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    fhetch::Program program;
    program.metadata = std::move(metadata);
    program.modulus_table = modulus_table_;
    program.instructions = instructions_;
    program.annotations = annotations_;

    out.close();

    // The two forms describe the same program, so both come from the one
    // writer over the same instructions.
    const auto binary_path = std::filesystem::path(path).replace_extension(".fhex");
    const bool want_text = format_ != TraceFormat::Binary;
    const bool want_binary = format_ != TraceFormat::Text;

    if (want_text && !fhetch::write_fhetch_text(path, program)) {
        std::cerr << "[FHETCH] ERROR: Cannot write trace to " << path << std::endl;
        return {};
    }
    if (want_binary && !fhetch::write_fhetch_binary(binary_path, program)) {
        std::cerr << "[FHETCH] ERROR: Cannot write trace to " << binary_path
                  << std::endl;
        return {};
    }
    if (!want_text) std::filesystem::remove(path);

    const auto& primary = want_text ? path : binary_path;
    std::cout << "[FHETCH] Trace written: " << primary
              << (format_ == TraceFormat::Both ? " (+ .fhex)" : "")
              << " (" << instructions_.size() << " instructions, "
              << modulus_table_.size() << " moduli)" << std::endl;
    return primary;
}

void TraceWriter::clear() {
    std::scoped_lock lock(mutex_);
    instructions_.clear();
    annotations_.clear();
    modulus_table_.clear();
    modulus_index_.clear();
    modulus_table_.push_back(COPY_MODULUS_VALUE);
    modulus_index_[COPY_MODULUS_VALUE] = COPY_MODULUS_INDEX;
}

}  // namespace niobium
