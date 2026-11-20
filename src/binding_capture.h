// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

/**
 * @file binding_capture.h
 * @brief Internal: fills the recording's binding record and output metadata
 *        (binding_files.h, docs/BINDING_RECORD.md) as the auto-facade tags
 *        inputs, probes outputs and tags keys.
 *
 * The auto-facade's own walk over an object's towers, which gives each tower
 * its FHETCH address, collects them into ObjectTowers on the way; the capture
 * records the object from those, so every address is assigned or looked up
 * once.
 *
 * Capture is a by-product of recording and never fails one: an error prints a
 * warning and marks the capture failed, and a failed capture writes no binding
 * files.
 */
#pragma once

#include "openfhe.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace niobium::detail {

/// The towers of one object, at the addresses the trace gave them, by big
/// polynomial in the object layout of docs/BINDING_RECORD.md: a ciphertext's
/// element i is big polynomial i; a key-switching key's digit j is b_j at 2j
/// and a_j at 2j + 1 (key_big_poly). A tower the trace never reads has no
/// address and is not added.
struct ObjectTowers {
    struct Tower {
        std::size_t big_poly;
        std::uint64_t modulus;
        std::uint64_t address;
    };
    std::vector<Tower> towers;

    void add(std::size_t big_poly, std::uint64_t modulus, std::uint64_t address) {
        towers.push_back(Tower{big_poly, modulus, address});
    }
};

/// Which vector of a key-switching key a digit belongs to.
enum class KeyVector : std::uint8_t { B, A };

/// The big polynomial of digit `digit` of a key-switching key's B or A vector.
constexpr std::size_t key_big_poly(KeyVector vector, std::size_t digit) {
    return (2 * digit) + (vector == KeyVector::A ? 1 : 0);
}

/// `input:<name>`. The first input's key tag selects the evaluation keys
/// binding_key_tag() names.
void capture_binding_input(const std::string& name, const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& ct,
                           const ObjectTowers& towers);

/// `output:<name>`, plus its output metadata entry (scaling factor, scale
/// degree, slots).
void capture_binding_output(const std::string& name, const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& ct,
                            const ObjectTowers& towers);

/// The key tag whose keys are the client's: the first tagged input's, or the
/// only one registered. nullopt when there is none, or when the capture failed
/// (several tags and no input to choose between them fails it).
std::optional<std::string> binding_key_tag();

/// A key of the client's tag under `label` (relin_key_label, galois_key_label).
void capture_binding_key(const lbcrypto::CryptoContext<lbcrypto::DCRTPoly>& cc, const std::string& label,
                         const lbcrypto::EvalKey<lbcrypto::DCRTPoly>& key, const ObjectTowers& towers);

}  // namespace niobium::detail
