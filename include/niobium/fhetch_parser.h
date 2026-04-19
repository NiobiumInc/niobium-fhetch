// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// .fhetch trace parser/driver.
//
// Reads a .fhetch text trace and re-drives the FHETCH Polynomial IR API
// (sr_addp, sr_ntt, ...) instruction-by-instruction. Each line in the input
// trace becomes a call to the corresponding niobium::fhetch:: free function,
// which in turn emits a secondary trace through the normal recording path.
//
// Intended use: a test harness that exercises the FHETCH API from a prior
// trace recording, without re-running the original OpenFHE workload.
//
// The caller is responsible for bracketing the parse/drive step with the
// usual Compiler session lifecycle:
//
//     niobium::compiler().init(argc, argv);
//     niobium::compiler().set_program_info(...);
//     niobium::compiler().cache_parameters(...);
//     niobium::compiler().set_ring_dimension(ring_dim);
//     niobium::compiler().start();
//
//     niobium::fhetch::DriveStats stats;
//     niobium::fhetch::parse_and_drive(path, ring_dim, stats);
//
//     niobium::compiler().stop();
//     niobium::compiler().replay();
//
// Addresses referenced in the input .fhetch file are mapped to Polynomial
// objects on first use. Any address that is read before it is written gets
// a zero-initialized Polynomial that is tagged as an input via
// niobium::fhetch::tag_input(), so Compiler::replay() can populate the
// simulator memory at those live-in addresses.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace niobium::fhetch {

/// Statistics / diagnostics returned by parse_and_drive().
struct DriveStats {
    size_t instructions_parsed   = 0; ///< total instruction lines seen
    size_t instructions_replayed = 0; ///< lines that successfully drove the API
    size_t unknown_opcodes       = 0; ///< opcode strings the parser doesn't handle
    size_t skipped_lines         = 0; ///< malformed lines (args missing, etc.)
    std::vector<uint64_t> modulus_table;
    std::vector<std::string> errors;  ///< human-readable diagnostics
};

/// Parse a .fhetch file and drive the FHETCH API for each instruction.
///
/// @param path      Path to the .fhetch trace.
/// @param ring_dim  Ring dimension N used to build polynomial placeholders.
/// @param stats     Populated with counts + any diagnostics.
/// @return true if the file was read and at least one instruction replayed.
bool parse_and_drive(const std::filesystem::path& path,
                     uint64_t ring_dim,
                     DriveStats& stats);

/// Same as parse_and_drive() but reads from an in-memory string instead of a
/// file on disk. Useful in unit tests.
bool parse_and_drive_text(const std::string& text,
                          uint64_t ring_dim,
                          DriveStats& stats);

}  // namespace niobium::fhetch
