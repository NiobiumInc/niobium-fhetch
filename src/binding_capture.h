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
 * @brief Internal: captures the binding record and output metadata of a
 *        recording (docs/BINDING_RECORD.md) as the auto-facade tags inputs,
 *        probes outputs and tags keys. Writing them, and clearing on reset,
 *        are declared OpenFHE-free in compiler_internal.h.
 *
 * Capture is a by-product of recording and never fails one: every error
 * prints a warning and leaves that object out.
 */
#pragma once

#include "openfhe.h"

#include <string>

namespace niobium::detail {

/// `input:<name>`: big polynomial i is element i of the ciphertext. The first
/// input's key tag selects the evaluation keys capture_binding_keys records.
void capture_binding_input(const std::string& name, const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& ct);

/// `output:<name>`, plus its output metadata entry (scaling factor, scale
/// degree, slots).
void capture_binding_output(const std::string& name, const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& ct);

/// `key:relin` and `key:galois:<g>` of the client's key tag: the first tagged
/// input's, or the only one registered.
void capture_binding_keys(const lbcrypto::CryptoContext<lbcrypto::DCRTPoly>& cc);

}  // namespace niobium::detail
