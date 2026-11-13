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
#include "niobium/fhetch_api.h"
#include "niobium/fhetch_recording.h"

#include <cstddef>
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
    using namespace niobium::fhetch;
    using recording::TaggedEntry;
    const auto generation = recording::registry_generation();
    if (g_synced_generation == generation) return;
    g_synced_generation = generation;

    auto& cc = niobium::compiler();
    const auto& mod_map = recording::address_moduli();

    // ---- Inputs ----
    // A trace driver's live-ins come first, as given: the driver used to store
    // them straight into the Compiler as it met them, ahead of this copy.
    for (const auto& e : recording::tagged_inputs()) {
        if (!e.explicit_values) continue;
        cc.store_input_element(e.name, niobium::CapturedKind::SRP,
                               /*starts_new_element=*/false,
                               niobium::detail::polynomial_address(e.poly), e.modulus, e.values);
    }
    for (const auto& e : recording::tagged_inputs()) {
        if (e.explicit_values) continue;
        switch (e.kind) {
        case TaggedEntry::POLY: {
            if (!e.poly.is_valid()) break;
            const auto address = niobium::detail::polynomial_address(e.poly);
            auto it = mod_map.find(address);
            if (it == mod_map.end()) break;  // never used in a modulus-bearing op
            cc.store_input_element(e.name, niobium::CapturedKind::SRP,
                                   /*starts_new_element=*/false,
                                   address, it->second,
                                   recording::values_or_zeros(e.poly));
            break;
        }
        case TaggedEntry::MRP_KIND:
            if (e.mrp.is_valid()) {
                for (auto q : e.mrp.base()) {
                    const auto& res = e.mrp[q];
                    if (!res.is_valid()) continue;
                    cc.store_input_element(e.name, niobium::CapturedKind::MRP,
                                           /*starts_new_element=*/false,
                                           niobium::detail::polynomial_address(res), q,
                                           recording::values_or_zeros(res));
                }
            }
            break;
        case TaggedEntry::MRPA:
            if (e.mrpa.is_valid()) {
                for (size_t i = 0; i < e.mrpa.length(); ++i) {
                    const auto& m = e.mrpa[i];
                    bool first = true;
                    for (auto q : m.base()) {
                        const auto& res = m[q];
                        if (!res.is_valid()) continue;
                        cc.store_input_element(e.name, niobium::CapturedKind::MRPArray,
                                               /*starts_new_element=*/first,
                                               niobium::detail::polynomial_address(res), q,
                                               recording::values_or_zeros(res));
                        first = false;
                    }
                }
            }
            break;
        case TaggedEntry::SRPA:
            if (e.srpa.is_valid()) {
                for (size_t i = 0; i < e.srpa.length(); ++i) {
                    const auto& p = e.srpa[i];
                    if (!p.is_valid()) continue;
                    const auto address = niobium::detail::polynomial_address(p);
                    auto it = mod_map.find(address);
                    if (it == mod_map.end()) continue;
                    cc.store_input_element(e.name, niobium::CapturedKind::SRPArray,
                                           /*starts_new_element=*/true,
                                           address, it->second,
                                           recording::values_or_zeros(p));
                }
            }
            break;
        }
    }

    // ---- Outputs ----
    // Outputs without a known modulus still get recorded (modulus=0) so
    // downstream consumers can at least emit a template.
    auto modulus_for = [&](const Polynomial& p) -> uint64_t {
        auto it = mod_map.find(niobium::detail::polynomial_address(p));
        return (it == mod_map.end()) ? 0 : it->second;
    };

    for (const auto& e : recording::tagged_outputs()) {
        switch (e.kind) {
        case TaggedEntry::POLY:
            if (e.poly.is_valid()) {
                cc.store_output_probe(e.name, niobium::CapturedKind::SRP,
                                      /*starts_new_element=*/false,
                                      niobium::detail::polynomial_address(e.poly), modulus_for(e.poly));
            }
            break;
        case TaggedEntry::MRP_KIND:
            if (e.mrp.is_valid()) {
                for (auto q : e.mrp.base()) {
                    cc.store_output_probe(e.name, niobium::CapturedKind::MRP,
                                          /*starts_new_element=*/false,
                                          niobium::detail::polynomial_address(e.mrp[q]), q);
                }
            }
            break;
        case TaggedEntry::MRPA:
            if (e.mrpa.is_valid()) {
                for (size_t i = 0; i < e.mrpa.length(); ++i) {
                    const auto& m = e.mrpa[i];
                    bool first = true;
                    for (auto q : m.base()) {
                        cc.store_output_probe(e.name, niobium::CapturedKind::MRPArray,
                                              /*starts_new_element=*/first,
                                              niobium::detail::polynomial_address(m[q]), q);
                        first = false;
                    }
                }
            }
            break;
        case TaggedEntry::SRPA:
            if (e.srpa.is_valid()) {
                for (size_t i = 0; i < e.srpa.length(); ++i) {
                    const auto& p = e.srpa[i];
                    if (!p.is_valid()) continue;
                    cc.store_output_probe(e.name, niobium::CapturedKind::SRPArray,
                                          /*starts_new_element=*/true,
                                          niobium::detail::polynomial_address(p), modulus_for(p));
                }
            }
            break;
        }
    }
}

}  // namespace niobium::detail
