// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// Internal trace writer — records FHETCH operations and writes .fhetch files.

#pragma once

#include "niobium/compiler.h"
#include "niobium/fhetch_ir.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace niobium {

class TraceWriter {
public:
    TraceWriter();

    // Reserved index 0: sentinel modulus used by copy/zero-init ops.
    // Matches the compiler's ModulusTable::COPY_MODULUS_VALUE convention so
    // our modulus_chain ordering aligns for replay artifact comparison.
    static constexpr uint32_t COPY_MODULUS_INDEX = 0;
    static constexpr uint64_t COPY_MODULUS_VALUE = 0xFFFFFFFFFFFFFFFFULL;

    void set_program_info(const std::string& name, const std::string& version,
                          const std::string& description);
    void set_source_info(const std::string& file, int line,
                         const std::string& timestamp);

    // Which serialization write() produces. Text by default.
    void set_trace_format(TraceFormat format) { format_ = format; }

    bool is_recording() const { return recording_; }
    void start_recording();
    void stop_recording();
    void pause_recording();
    void resume_recording();

    // Register a modulus in the table. Returns the index.
    // If the modulus is already registered, returns the existing index.
    // Thread-safe.
    uint32_t register_modulus(uint64_t modulus);

    // Record a FHETCH instruction.
    //
    // Addresses are narrowed to the IR's 32 bits here. An address too wide is
    // dropped with a warning rather than truncated into a different, valid
    // address — the reader takes the same posture. Nothing observed has ever
    // come close: the widest address in a real client trace is ~200k against a
    // 4.29-billion ceiling.
    void emit(const fhetch::Instruction& instruction);

    // Record a comment. Comments are annotations, never instructions, and
    // carry the number of instructions that precede them so the writer can put
    // them back where they were — recorded traces interleave them heavily.
    void comment(const std::string& text);

    // Write the accumulated trace. Returns the path of the primary artifact —
    // the .fhex when the format is binary, the .fhetch otherwise — which is
    // what a caller hands on to replay.
    std::filesystem::path write(const std::filesystem::path& directory,
                                const std::string& program_name);

    // Clear all recorded instructions and modulus table (for epoch reset).
    void clear();

    // Sort regular moduli ascending (sentinel stays at index 0) and remap
    // every instruction's modulus index accordingly. A field remap now: this
    // used to rewrite "m=N" digits inside formatted strings, which also meant
    // scanning comment text for something that looked like a modulus.
    // Called automatically from write(); exposed for tests.
    void normalize_modulus_table();

private:
    // The body of normalize_modulus_table(), without taking mutex_. write()
    // holds the lock across both the normalize and the file write, so it calls
    // this rather than the public wrapper — std::mutex is not recursive.
    void normalize_modulus_table_locked();

public:

    size_t instruction_count() const { return instructions_.size(); }

    /// Get the modulus table (all moduli registered during recording).
    const std::vector<uint64_t>& modulus_table() const { return modulus_table_; }

private:
    TraceFormat format_ = TraceFormat::Text;
    bool recording_ = false;
    bool paused_ = false;
    std::string program_name_;
    std::string program_version_;
    std::string program_description_;
    std::string source_file_;
    int source_line_ = 0;
    std::string build_timestamp_;
    std::vector<fhetch::Instruction> instructions_;
    std::vector<fhetch::Annotation> annotations_;

    // Modulus table: modulus value → index
    std::vector<uint64_t> modulus_table_;
    std::unordered_map<uint64_t, uint32_t> modulus_index_;

    mutable std::mutex mutex_;
};

}  // namespace niobium
