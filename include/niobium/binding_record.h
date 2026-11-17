// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

/**
 * @file binding_record.h
 * @brief The binding record (.nbbr): which trace addresses hold each labelled
 *        object's limbs. See docs/BINDING_RECORD.md.
 *
 * Tracing side: add every object's limbs, then write the record.
 *
 *     niobium::binding::BindingRecord rec;
 *     rec.ring_dim = n; rec.moduli = q; rec.roots = psi;
 *     rec.add_object(make_label(Kind::Input, "x"), Kind::Input, 2, limbs);
 *     rec.write("program.nbbr");
 *
 * Server side: BindingRecord::read("program.nbbr"), then bind an upload with
 * niobium/binding.h.
 *
 * C++17 and dependency-free: the nbfhetch_formats library, which the recorder
 * links. Bind lives in nbfhetch_binding (C++20, needs nbbf).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace niobium::binding {

// -----------------------------------------------------------------------------
// Labels
// -----------------------------------------------------------------------------

/// An object's kind; the values are the file's `kind` codes.
enum class Kind : std::uint8_t { Input = 0, Output = 1, Key = 2, Constant = 3 };

inline constexpr std::uint8_t kMaxKindCode = 3;

/// The label index's length limit: a label is 1 to 255 bytes.
inline constexpr std::size_t kMaxLabelLen = 255;

/// "input", "output", "key" or "constant".
std::string_view kind_name(Kind kind) noexcept;

/// The label prefix of `kind`: "input:", "output:", "key:" or "const:".
std::string_view kind_prefix(Kind kind) noexcept;

/// The kind whose prefix `label` starts with, if any.
std::optional<Kind> kind_of_label(std::string_view label) noexcept;

/// True when `label` is 1 to 255 bytes of [A-Za-z0-9_.:-], the label index's
/// characters and length limits (any kind).
bool is_label_text(std::string_view label) noexcept;

/// Why `label` is not a well-formed label of `kind` (docs/BINDING_RECORD.md,
/// Labels), or an empty string when it is.
std::string label_problem(std::string_view label, Kind kind);

inline bool is_valid_label(std::string_view label, Kind kind) { return label_problem(label, kind).empty(); }

/// `<prefix><name>`. Throws RecordError (InvalidArgument) when the result is
/// not a well-formed label of `kind`.
std::string make_label(Kind kind, std::string_view name);

/// The label of the relinearization key for the s^degree component:
/// `key:relin` for 2, `key:relin:<degree>` above. Throws RecordError
/// (InvalidArgument) below 2.
std::string relin_key_label(std::uint32_t degree);

// -----------------------------------------------------------------------------
// Errors
// -----------------------------------------------------------------------------

/// What a binding record (or output metadata, output_metadata.h) failed.
/// Binding record validation codes name their rule number.
enum class RecordErrc : std::uint8_t {
    // Rule 1
    BadMagic = 1,
    BigEndianWriter,  ///< sentinel is byte-swapped
    BadSentinel,
    UnsupportedVersion,
    SizeMismatch,  ///< the file is not exactly as long as its layout
    // Rule 2
    DuplicateModulus,
    BadRoot,
    // Rule 3
    BadKind,
    BadLabel,
    BadPolyCount,  ///< poly_count == 0
    BadRunRange,   ///< first_run + run_count > num_runs
    // Rule 4
    DuplicateLabel,
    OverlappingRuns,  ///< two objects' run ranges overlap
    OrphanRun,        ///< a run no object's range covers
    // Rule 5
    BadRun,
    // Rule 6
    DuplicateLimb,
    DuplicateAddress,
    // Writer
    InvalidArgument,
    Io,
};

class RecordError : public std::runtime_error {
public:
    RecordError(RecordErrc code, const std::string& message) : std::runtime_error(message), code_(code) {}
    RecordErrc code() const noexcept { return code_; }

private:
    RecordErrc code_;
};

// -----------------------------------------------------------------------------
// The record
// -----------------------------------------------------------------------------

/// Limbs first_modulus .. first_modulus + limb_count - 1 of big polynomial
/// big_poly live at first_address .. first_address + limb_count - 1.
struct Run {
    std::uint16_t big_poly = 0;
    std::uint16_t first_modulus = 0;  ///< index into BindingRecord::moduli
    std::uint16_t limb_count = 0;
    std::uint64_t first_address = 0;
};

/// One limb as the tracer sees it.
struct Limb {
    std::uint16_t big_poly = 0;
    std::uint16_t modulus_index = 0;  ///< index into BindingRecord::moduli
    std::uint64_t address = 0;
};

/// An object: which big polynomial is number 0, 1, ... follows the Object
/// layout section of the specification; the record treats it as opaque.
struct Object {
    std::string label;
    Kind kind = Kind::Input;
    std::uint16_t poly_count = 0;
    std::vector<Run> runs;  ///< sorted by (big_poly, first_modulus) when built by add_object
};

bool operator==(const Run& a, const Run& b) noexcept;
bool operator==(const Limb& a, const Limb& b) noexcept;
bool operator==(const Object& a, const Object& b) noexcept;
inline bool operator!=(const Run& a, const Run& b) noexcept { return !(a == b); }
inline bool operator!=(const Limb& a, const Limb& b) noexcept { return !(a == b); }
inline bool operator!=(const Object& a, const Object& b) noexcept { return !(a == b); }

struct BindingRecord {
    static constexpr std::uint16_t kVersion = 1;

    std::uint32_t ring_dim = 0;
    std::vector<std::uint64_t> moduli;  ///< every prime the program uses
    std::vector<std::uint64_t> roots;   ///< roots[k] belongs to moduli[k]
    std::vector<Object> objects;

    /// Parses and runs every Validation check. Throws RecordError.
    static BindingRecord parse(const std::byte* data, std::size_t size);
    static BindingRecord parse(const std::vector<std::byte>& bytes) { return parse(bytes.data(), bytes.size()); }
    static BindingRecord read(const std::filesystem::path& path);

    /// Adds an object built from its limbs, in any order: they are sorted by
    /// (big_poly, modulus_index) and split into runs as the specification says.
    /// Throws RecordError (InvalidArgument) on a malformed label, a limb outside
    /// the object or the moduli, or a limb given twice; the record's other rules
    /// are checked by validate().
    void add_object(std::string label, Kind kind, std::uint16_t poly_count, const std::vector<Limb>& limbs);

    /// Removes the object with this label; true when there was one.
    bool remove_object(std::string_view label);

    /// The checks of rules 2 to 6 that apply to the in-memory form. Throws RecordError.
    void validate() const;

    /// The file's bytes, after validate().
    std::vector<std::byte> serialize() const;

    /// Writes the file atomically (a temp file beside `path`, fsynced, then
    /// renamed over it), after validate().
    void write(const std::filesystem::path& path) const;

    /// The object with this label, or nullptr.
    const Object* find(std::string_view label) const noexcept;
};

bool operator==(const BindingRecord& a, const BindingRecord& b) noexcept;
inline bool operator!=(const BindingRecord& a, const BindingRecord& b) noexcept { return !(a == b); }

/// Splits limbs, already sorted by (big_poly, modulus_index), into runs: a new
/// run starts when the big polynomial changes, the modulus index or the address
/// is not the previous one plus 1, or the run holds 65535 limbs.
std::vector<Run> make_runs(const std::vector<Limb>& sorted_limbs);

/// The `<program>.nbbr` that sits next to a trace.
std::filesystem::path record_path(const std::filesystem::path& trace);

}  // namespace niobium::binding
