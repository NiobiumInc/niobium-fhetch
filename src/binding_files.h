// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

/**
 * @file binding_files.h
 * @brief Internal, OpenFHE-free: the binding record and output metadata a
 *        recording captures (docs/BINDING_RECORD.md), and writing them.
 *
 * The Compiler owns one BindingCapture, next to its captured inputs, and
 * clears it with them; the auto-facade fills it (binding_capture.h).
 */
#pragma once

#include "niobium/binding_record.h"
#include "niobium/output_metadata.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace niobium::detail {

struct BindingCapture {
    binding::BindingRecord record;
    binding::OutputMetadata metadata;
    std::unordered_map<std::uint64_t, std::uint16_t> modulus_index;  ///< prime -> index into record.moduli
    std::string key_tag;                                             ///< the client's, from the first input
    /// A capture step failed: what was captured is incomplete, so the
    /// recording gets no binding files at all rather than wrong ones.
    bool failed = false;
};

/// The Compiler's capture (defined in compiler.cpp).
BindingCapture& binding_capture();

/// The files write_binding_files wrote, relative to its directory; empty for
/// one it did not write.
struct BindingFiles {
    std::string binding_record;   ///< <prog>.nbbr
    std::string output_metadata;  ///< <prog>.nbom
};

/// Writes `capture` into `dir` as <prog>.nbbr and <prog>.nbom, each when it
/// has content, and removes stale ones otherwise. A failed capture, or a
/// failure to write either file, writes neither: it warns and removes both.
BindingFiles write_binding_files(const BindingCapture& capture, const std::filesystem::path& dir,
                                 const std::string& prog);

}  // namespace niobium::detail
