// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

/**
 * @file binding.h
 * @brief Bind, which joins an NBBF upload and its label index with a binding
 *        record, and its inverse, which writes a run's outputs back as an NBBF
 *        result. See docs/BINDING_RECORD.md, Bind and Result.
 *
 *     const auto rec = BindingRecord::read("program.nbbr");
 *     const auto res = bind_upload(rec, Kind::Key, nbbf_file.view(), label_index);
 *     // res.bindings: (trace address, NBBF block) pairs, sorted by address
 *     // ... run the trace ...
 *     write_result(rec, OutputMetadata::read("program.nbom"), residues_at, out_dir);
 *
 * C++20 and needs nbbf: the opt-in nbfhetch_binding library. The record itself
 * (niobium/binding_record.h) is C++17 and dependency-free.
 */
#pragma once

#include "niobium/binding_record.h"
#include "niobium/output_metadata.h"

#include <nbbf/label_index.hpp>
#include <nbbf/nbbf.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace niobium::binding {

/// What a bind failed (docs/BINDING_RECORD.md, Bind).
enum class BindErrc : std::uint8_t {
    OutputKind = 1,      ///< kind output is never bound
    LabelIndexMismatch,  ///< the label index does not belong to the NBBF file
    RingDimMismatch,
    RootMismatch,
    MissingLabel,  ///< an object of the kind has no label index entry
    PolyCountMismatch,
    MissingLimb,
    BadRepresentation,  ///< a block is not evaluation domain, bit-reversed order
    UnexpectedLabel,    ///< an input upload names a label the record lacks
    WrongPrefix,        ///< an input upload names a label not prefixed "input:"
    MissingOutputMetadata,  ///< write_result: an output has no output metadata entry
    ResidueCountMismatch,   ///< write_result: a limb's residues are not ring_dim many
};

/// A bind failure. label() is the label it concerns (empty for a failure of
/// the upload as a whole).
class BindError : public std::runtime_error {
public:
    BindError(BindErrc code, std::string label, const std::string& message)
        : std::runtime_error(message), code_(code), label_(std::move(label)) {}
    BindErrc code() const noexcept { return code_; }
    const std::string& label() const noexcept { return label_; }

private:
    BindErrc code_;
    std::string label_;
};

/// One trace address and the NBBF block that fills it.
struct Binding {
    std::uint64_t address = 0;
    std::size_t block = 0;  ///< NBBF block-table index

    bool operator==(const Binding&) const = default;
};

/// Blocks of a bound label that no run reads (a ciphertext at a higher level
/// than the program reads, say).
struct UnusedLimbs {
    std::string label;
    std::vector<std::size_t> blocks;  ///< NBBF block-table indices, ascending

    bool operator==(const UnusedLimbs&) const = default;
};

struct BindResult {
    std::vector<Binding> bindings;            ///< sorted by address
    std::vector<std::string> ignored_labels;  ///< key and constant labels naming no object of the kind
    std::vector<UnusedLimbs> unused_limbs;    ///< per bound label, in record order
};

/// Binds one upload of `kind` (not Output): checks the label index against the
/// NBBF file, then every rule of docs/BINDING_RECORD.md, Bind. Throws BindError.
/// Named so that argument-dependent lookup never prefers std::bind.
BindResult bind_upload(const BindingRecord& record, Kind kind, const nbbf::FileView& file,
                       const nbbf::LabelIndex& index);

/// The same, for a caller that holds only the NBBF header region (its bytes
/// [0, header_len)) and its Layout, such as a StreamReader's.
BindResult bind_upload(const BindingRecord& record, Kind kind, std::span<const std::byte> header_region,
                       const nbbf::Layout& layout, const nbbf::LabelIndex& index);

/// One limb of an output: read `address` back and write it as the block of
/// big polynomial big_poly under modulus_index (of the record's moduli).
struct OutputLimb {
    std::uint16_t big_poly = 0;
    std::uint16_t modulus_index = 0;
    std::uint64_t modulus = 0;
    std::uint64_t address = 0;

    bool operator==(const OutputLimb&) const = default;
};

struct OutputObject {
    std::string label;
    std::uint16_t poly_count = 0;
    std::vector<OutputLimb> limbs;  ///< sorted by (big_poly, modulus_index)
};

/// Every output object of the record, in record order, with its limbs. A result
/// writer reads each address back and writes NBBF blocks under the record's
/// moduli and roots, plus a label index naming each object.
std::vector<OutputObject> output_limbs(const BindingRecord& record);

/// The residues a run computed at trace `address`, whose modulus is `modulus`:
/// ring_dim values in [0, modulus), evaluation domain, bit-reversed order.
/// Throws if it has none.
using ResidueSource = std::function<std::vector<std::uint64_t>(std::uint64_t address, std::uint64_t modulus)>;

/// Writes every output of the record into `dir` as result.nbbf, result.nbli
/// and result.nbom (docs/BINDING_RECORD.md, Result), creating `dir`. The
/// NBBF declares only the primes the outputs use. Throws BindError
/// (MissingOutputMetadata, ResidueCountMismatch), nbbf::Exception (a residue
/// out of range, a failed write) and whatever `residues` throws.
void write_result(const BindingRecord& record, const OutputMetadata& metadata, const ResidueSource& residues,
                  const std::filesystem::path& dir);

}  // namespace niobium::binding
