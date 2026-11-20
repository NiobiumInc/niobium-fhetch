// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// Recorder side of the FHETCH IR builder: copies the inputs and outputs
// tagged through niobium::fhetch::tag_input / tag_output (fhetch_recording.h)
// into the Compiler's captured inputs and output probes, so Compiler::replay()
// and the bundle writer see pure-FHETCH flows the same way they see the
// OpenFHE auto-facade path. The IR builder knows nothing of the Compiler.

#include "compiler_internal.h"
#include "niobium/compiler.h"
#include "niobium/fhetch_recording.h"

#include <cstdint>
#include <optional>

namespace niobium::detail {

namespace {

// One copy per recording cycle: both stop() and replay() call
// sync_fhetch_state_to_compiler(), and a second pass would append duplicate
// elements to the same captured records (find_or_create_record dedupes
// records by name, not elements) -- doubling every registry-tagged input in
// memory. The IR builder's registry generation changes whenever its
// registries are reset, which re-arms the copy.
std::optional<std::uint64_t> g_synced_generation;

}  // namespace

// Drops IR-builder recording state Compiler::reset can't otherwise reach;
// first-write-wins on the address -> modulus map would leak moduli across
// resets.
void clear_recording_registries() noexcept {
    niobium::fhetch::recording::clear_address_moduli();
    g_synced_generation.reset();
}

void sync_fhetch_state_to_compiler() {
    namespace recording = niobium::fhetch::recording;
    const auto generation = recording::registry_generation();
    if (g_synced_generation == generation) return;
    g_synced_generation = generation;

    auto& cc = niobium::compiler();
    // An input no modulus-bearing instruction used (and no bind_modulus named)
    // has no modulus to load it under, so it is left out.
    for (const auto& entry : recording::tagged_inputs()) {
        recording::for_each_residue(entry, [&](const recording::TaggedResidue& r) {
            if (!r.modulus) return;
            cc.store_input_element(entry.name, r.kind, r.starts_new_element,
                                   recording::polynomial_address(*r.poly), *r.modulus,
                                   recording::values_or_zeros(*r.poly));
        });
    }
    // An output without a known modulus is still recorded (modulus 0), so
    // downstream consumers can at least emit a template.
    for (const auto& entry : recording::tagged_outputs()) {
        recording::for_each_residue(entry, [&](const recording::TaggedResidue& r) {
            cc.store_output_probe(entry.name, r.kind, r.starts_new_element,
                                  recording::polynomial_address(*r.poly), r.modulus.value_or(0));
        });
    }
}

}  // namespace niobium::detail
