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
#include <unordered_map>
#include <utility>
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

    size_t inputs_materialized   = 0; ///< source addrs with real input values
    size_t outputs_tagged        = 0; ///< source addrs tagged as outputs
};

/// Input polynomial values provided by the caller for specific source-file
/// addresses in the .fhetch trace. When a referenced source address is in
/// the map, the parser creates a Polynomial::from_data() with those values
/// (instead of the default zero-initialized placeholder) and tags it as an
/// input via niobium::fhetch::tag_input.
struct DriveInputs {
    struct Element {
        std::vector<uint64_t> values;
        uint64_t modulus = 0;
    };
    /// source-file address (%N in the trace) -> polynomial values.
    std::unordered_map<uint64_t, Element> data;
};

/// Maps source-file addresses to named output probes. When the parser emits
/// an instruction whose destination address is in this map, the resulting
/// Polynomial is tagged via niobium::fhetch::tag_output with the stored
/// name (suffixed by poly_index) so Compiler::result() can reconstruct the
/// ciphertext after replay.
struct DriveOutputs {
    struct Tag {
        std::string name;     ///< output name (e.g. "result")
        size_t poly_index = 0;///< position of this tower within the ciphertext
    };
    std::unordered_map<uint64_t, Tag> map;
};

/// Parse a .fhetch file and drive the FHETCH API for each instruction.
///
/// @param path      Path to the .fhetch trace.
/// @param ring_dim  Ring dimension N used to build polynomial placeholders.
/// @param stats     Populated with counts + any diagnostics.
/// @param inputs    Optional — per-source-address input data.
/// @param outputs   Optional — per-source-address output probe tags.
/// @return true if the file was read and at least one instruction replayed.
bool parse_and_drive(const std::filesystem::path& path,
                     uint64_t ring_dim,
                     DriveStats& stats,
                     const DriveInputs& inputs = {},
                     const DriveOutputs& outputs = {});

/// Same as parse_and_drive() but reads from an in-memory string instead of a
/// file on disk. Useful in unit tests.
bool parse_and_drive_text(const std::string& text,
                          uint64_t ring_dim,
                          DriveStats& stats,
                          const DriveInputs& inputs = {},
                          const DriveOutputs& outputs = {});

}  // namespace niobium::fhetch
