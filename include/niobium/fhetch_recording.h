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
 * the registries into its own capture (resolving each single-residue input's
 * modulus with address_moduli()).
 *
 * Part of the recorder-free core (nbfhetch_core).
 */
#pragma once

#include "niobium/fhetch_api.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace niobium {
class TraceWriter;
}  // namespace niobium

namespace niobium::fhetch::recording {

/// One tag_input or tag_output call: which of the four overloads, and its value.
struct TaggedEntry {
    std::string name;
    enum Kind : std::uint8_t { POLY, MRP_KIND, MRPA, SRPA } kind = POLY;
    Polynomial poly;
    MRP mrp;
    MRPArray mrpa;
    SRPArray srpa;
    /// Set for a trace driver's live-in (tag_input_values): the input's modulus
    /// and values are known, so a recorder stores them as given, ahead of the
    /// registry's other inputs, instead of resolving them.
    bool explicit_values = false;
    std::uint64_t modulus = 0;
    std::vector<std::uint64_t> values;
};

/// The tagged inputs and outputs, in tagging order. Cleared by reset_for_epoch().
const std::vector<TaggedEntry>& tagged_inputs();
const std::vector<TaggedEntry>& tagged_outputs();

/// Tags single-residue input `p` whose modulus and values are already known.
void tag_input_values(const std::string& name, const Polynomial& p, std::uint64_t modulus,
                      std::vector<std::uint64_t> values);

/// Address -> modulus of every polynomial a modulus-bearing instruction has
/// used (the first real modulus wins over the copy sentinel). Cleared by
/// reset_for_epoch() and clear_address_moduli().
const std::unordered_map<std::uintptr_t, std::uint64_t>& address_moduli();
void clear_address_moduli();

/// Changes whenever the registries are reset (reset_for_epoch(),
/// clear_address_moduli()): a recorder that copies them once per recording
/// cycle copies again when this differs from the value it last copied at.
std::uint64_t registry_generation();

/// `p`'s values, or zeros of its ring dimension when it holds none.
std::vector<std::uint64_t> values_or_zeros(const Polynomial& p);

}  // namespace niobium::fhetch::recording

namespace niobium::detail {

/// The process-wide TraceWriter every IR builder call records into.
TraceWriter& trace_writer();

/// Data-format toggles a recorder sets (its --montgomery / --bit_reversal /
/// --niobium_hw flags); the IR builder reads them to choose Montgomery-form
/// immediates and the hardware switchmodulus lowering.
struct RecordingModes {
    bool montgomery = false;
    bool bit_reversal = false;
};
RecordingModes& recording_modes();
inline bool montgomery_mode() { return recording_modes().montgomery; }
inline bool bit_reversal_mode() { return recording_modes().bit_reversal; }

/// The FHETCH address of `p`, or all bits set for an invalid polynomial.
std::uintptr_t polynomial_address(const niobium::fhetch::Polynomial& p);

}  // namespace niobium::detail
