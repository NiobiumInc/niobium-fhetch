// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

/**
 * @file fhetch_recording.h
 * @brief What a recorder reads from, and sets on, the FHETCH IR builder.
 *
 * The IR builder (fhetch_api.h) records instructions into one process-wide
 * TraceWriter and keeps the inputs and outputs tagged with tag_input /
 * tag_output in registries. It knows nothing of the recorder that drives it:
 * a recorder starts and writes the trace through trace_writer(), and copies
 * the registries into its own capture residue by residue (for_each_residue).
 *
 * Part of the recorder-free core (nbfhetch_core). A program that links the
 * core without the recorder, over the instrumented OpenFHE, must define the
 * hook that OpenFHE calls while it loads, as the recorder does:
 *
 *     extern "C" void openfhe_cporbe_with_openmp(bool) {}
 *
 * The recorder's other hooks that OpenFHE references are never called without
 * a recorder; with GNU ld, such a program links with -Wl,--allow-shlib-undefined.
 */
#pragma once

#include "niobium/compiler.h"
#include "niobium/fhetch_api.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace niobium {
class TraceWriter;
}  // namespace niobium

namespace niobium::fhetch::recording {

/// One tag_input or tag_output call: its name, and the value of whichever of
/// the four overloads it was.
struct TaggedEntry {
    std::string name;
    std::variant<Polynomial, MRP, MRPArray, SRPArray> value;
};

/// The tagged inputs and outputs, in tagging order. Cleared by reset_for_epoch().
const std::vector<TaggedEntry>& tagged_inputs();
const std::vector<TaggedEntry>& tagged_outputs();

/// One residue of a tagged entry.
struct TaggedResidue {
    CapturedKind kind;        ///< which overload tagged it
    bool starts_new_element;  ///< the first residue of an array's element
    const Polynomial* poly;   ///< never null
    /// An MRP's prime for this residue; for a single-residue polynomial, the
    /// modulus address_moduli() holds for it, if any.
    std::optional<std::uint64_t> modulus;
};

/// Calls `f` for every valid residue of `entry`, in the order a recorder
/// stores them: a polynomial itself, an MRP's residues in base order, an
/// array's element by element.
void for_each_residue(const TaggedEntry& entry, const std::function<void(const TaggedResidue&)>& f);

/// Address -> modulus of every polynomial a modulus-bearing instruction (or
/// bind_modulus) has named (the first real modulus wins over the copy
/// sentinel). Cleared by reset_for_epoch() and clear_address_moduli().
const std::unordered_map<std::uintptr_t, std::uint64_t>& address_moduli();
void clear_address_moduli();

/// Changes whenever the registries are reset (reset_for_epoch(),
/// clear_address_moduli()): a recorder that copies them once per recording
/// cycle copies again when this differs from the value it last copied at.
std::uint64_t registry_generation();

/// `p`'s values, or zeros of its ring dimension when it holds none.
std::vector<std::uint64_t> values_or_zeros(const Polynomial& p);

/// The process-wide TraceWriter every IR builder call records into.
TraceWriter& trace_writer();

/// The FHETCH address of `p`, or all bits set for an invalid polynomial.
std::uintptr_t polynomial_address(const Polynomial& p);

}  // namespace niobium::fhetch::recording
