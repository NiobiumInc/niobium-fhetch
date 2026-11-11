// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

/**
 * @file output_metadata.h
 * @brief Output metadata (.nbom): what a third party needs, besides the
 *        residues, to rebuild each output as a CKKS ciphertext. See
 *        docs/BINDING_RECORD.md, Output metadata.
 *
 *     niobium::binding::OutputMetadata meta;
 *     meta.set({"output:y", 0x1p40, 1, 8});  // label, scaling factor, scale degree, slots
 *     meta.write(nbom_path("program.fhex"));                 // program.nbom
 *     const auto back = OutputMetadata::read("result.nbom");
 *
 * Level, domain and order are not stored: the result's NBBF blocks carry them.
 * Errors are RecordError, with messages prefixed "output metadata: ".
 *
 * C++17 and dependency-free: part of nbfhetch_formats.
 */
#pragma once

#include "niobium/binding_record.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace niobium::binding {

/// One output.
struct OutputMetadataEntry {
    std::string label;          ///< a valid `output:` label
    double scaling_factor = 0;  ///< the scale the message is multiplied by
    std::uint32_t scale_degree = 1;  ///< factors of the base scale it carries
    std::uint32_t slots = 0;         ///< CKKS slots encoded, at most n/2

};

struct OutputMetadata {
    static constexpr std::uint16_t kVersion = 1;

    std::vector<OutputMetadataEntry> entries;

    /// Parses and runs every check of the specification: magic, sentinel
    /// (byte-swapped: BigEndianWriter), version, exact size, every label within
    /// the pool, a valid `output:` label, and unique. Throws RecordError.
    static OutputMetadata parse(const std::byte* data, std::size_t size);
    static OutputMetadata parse(const std::vector<std::byte>& bytes) { return parse(bytes.data(), bytes.size()); }
    static OutputMetadata read(const std::filesystem::path& path);

    /// Every label a valid `output:` label, unique; the counts fit their fields.
    /// Throws RecordError (BadLabel, DuplicateLabel, InvalidArgument).
    void validate() const;

    /// The file's bytes, after validate().
    std::vector<std::byte> serialize() const;

    /// Writes the file atomically, as BindingRecord::write does, after validate().
    void write(const std::filesystem::path& path) const;

    /// The entry with this label, or nullptr.
    const OutputMetadataEntry* find(std::string_view label) const noexcept;

    /// Adds `entry`, replacing an entry of the same label.
    void set(OutputMetadataEntry entry);
};

bool operator==(const OutputMetadataEntry& a, const OutputMetadataEntry& b) noexcept;
bool operator==(const OutputMetadata& a, const OutputMetadata& b) noexcept;
inline bool operator!=(const OutputMetadataEntry& a, const OutputMetadataEntry& b) noexcept { return !(a == b); }
inline bool operator!=(const OutputMetadata& a, const OutputMetadata& b) noexcept { return !(a == b); }

/// The `<program>.nbom` that sits next to a trace.
std::filesystem::path nbom_path(const std::filesystem::path& trace);

}  // namespace niobium::binding
