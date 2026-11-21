// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

/**
 * @file binding_capture.cpp
 * @brief The recorder's binding record and output metadata capture, and
 *        writing it (binding_capture.h, binding_files.h,
 *        docs/BINDING_RECORD.md).
 */
#include "binding_capture.h"

#include "binding_files.h"
#include "niobium/binding_record.h"
#include "niobium/output_metadata.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace niobium::detail {

namespace {

using binding::Kind;
using lbcrypto::DCRTPoly;

void warn(const std::string& what) {
    std::cout << "[NIOBIUM] WARNING: binding record: " << what << std::endl;
}

// Runs a capture step on a capture that has not failed. A failure warns and
// fails the capture, which then writes no binding files: it never fails the
// recording.
template <class F>
void guarded(const char* what, F&& step) {
    auto& capture = binding_capture();
    if (capture.failed) return;
    try {
        std::forward<F>(step)(capture);
    } catch (const std::exception& e) {
        warn(std::string(what) + ": " + e.what() + "; the recording gets no binding record");
        capture.failed = true;
    }
}

// ring_dim, then Q's primes followed by P's, each with the root of unity its
// NTT uses, taken once from the program's crypto context.
void set_params(BindingCapture& capture, const lbcrypto::CryptoContext<DCRTPoly>& cc) {
    if (capture.record.ring_dim != 0) return;
    const auto rns = std::dynamic_pointer_cast<lbcrypto::CryptoParametersRNS>(cc->GetCryptoParameters());
    if (!rns) throw std::runtime_error("only RNS schemes are supported");
    binding::BindingRecord record;
    record.ring_dim = cc->GetRingDimension();
    const auto add_towers = [&](const std::shared_ptr<lbcrypto::ILDCRTParams<lbcrypto::BigInteger>>& params) {
        if (!params) return;
        for (const auto& tower : params->GetParams()) {
            record.moduli.push_back(tower->GetModulus().ConvertToInt());
            record.roots.push_back(tower->GetRootOfUnity().ConvertToInt());
        }
    };
    add_towers(cc->GetElementParams());
    add_towers(rns->GetParamsP());
    if (record.moduli.size() > UINT16_MAX) throw std::runtime_error("more than 65535 moduli");
    std::unordered_map<std::uint64_t, std::uint16_t> index;
    for (std::size_t k = 0; k < record.moduli.size(); ++k) index.emplace(record.moduli[k], static_cast<std::uint16_t>(k));
    capture.record = std::move(record);
    capture.modulus_index = std::move(index);
}

// Records `label`, of `big_polys` big polynomials, from the towers the walk
// gave addresses; replaces an object of the same label.
void add_object(BindingCapture& capture, const std::string& label, Kind kind, std::size_t big_polys,
                const ObjectTowers& towers) {
    if (big_polys > UINT16_MAX) throw std::runtime_error(label + " has more than 65535 big polynomials");
    std::vector<binding::Limb> limbs;
    limbs.reserve(towers.towers.size());
    for (const auto& tower : towers.towers) {
        const auto it = capture.modulus_index.find(tower.modulus);
        if (it == capture.modulus_index.end())
            throw std::runtime_error(label + " has modulus " + std::to_string(tower.modulus) +
                                     ", which is not among the program's moduli");
        limbs.push_back(binding::Limb{static_cast<std::uint16_t>(tower.big_poly), it->second, tower.address});
    }
    capture.record.remove_object(label);
    capture.record.add_object(label, kind, static_cast<std::uint16_t>(big_polys), limbs);
}

}  // namespace

void capture_binding_input(const std::string& name, const lbcrypto::Ciphertext<DCRTPoly>& ct,
                           const ObjectTowers& towers) {
    guarded("input", [&](BindingCapture& capture) {
        set_params(capture, ct->GetCryptoContext());
        // The client is whoever encrypted the inputs: its keys are the ones to bind.
        if (capture.key_tag.empty()) capture.key_tag = ct->GetKeyTag();
        add_object(capture, binding::make_label(Kind::Input, name), Kind::Input, ct->GetElements().size(), towers);
    });
}

void capture_binding_output(const std::string& name, const lbcrypto::Ciphertext<DCRTPoly>& ct,
                            const ObjectTowers& towers) {
    guarded("output", [&](BindingCapture& capture) {
        set_params(capture, ct->GetCryptoContext());
        const auto label = binding::make_label(Kind::Output, name);
        add_object(capture, label, Kind::Output, ct->GetElements().size(), towers);
        capture.metadata.set(binding::OutputMetadataEntry{label, ct->GetScalingFactor(),
                                                          static_cast<std::uint32_t>(ct->GetNoiseScaleDeg()),
                                                          ct->GetSlots()});
    });
}

std::optional<std::string> binding_key_tag() {
    std::optional<std::string> tag;
    guarded("keys", [&](BindingCapture& capture) {
        if (!capture.key_tag.empty()) {
            tag = capture.key_tag;
            return;
        }
        using CC = lbcrypto::CryptoContextImpl<DCRTPoly>;
        std::set<std::string> tags;
        for (const auto& entry : CC::GetAllEvalMultKeys()) tags.insert(entry.first);
        for (const auto& entry : CC::GetAllEvalAutomorphismKeys()) tags.insert(entry.first);
        if (tags.size() > 1)
            throw std::runtime_error(std::to_string(tags.size()) + " key tags and no tagged input to choose the client's");
        if (!tags.empty()) tag = *tags.begin();
    });
    return tag;
}

void capture_binding_key(const lbcrypto::CryptoContext<DCRTPoly>& cc, const std::string& label,
                         const lbcrypto::EvalKey<DCRTPoly>& key, const ObjectTowers& towers) {
    guarded("keys", [&](BindingCapture& capture) {
        set_params(capture, cc);
        const auto digits = key->GetBVector().size();
        if (key->GetAVector().size() != digits) throw std::runtime_error(label + " has unequal A and B digit counts");
        add_object(capture, label, Kind::Key, 2 * digits, towers);
    });
}

BindingFiles write_binding_files(const BindingCapture& capture, const std::filesystem::path& dir,
                                 const std::string& prog) {
    const auto record_path = dir / (prog + ".nbbr");
    const auto metadata_path = dir / (prog + ".nbom");
    const auto remove_both = [&] {
        std::error_code ec;
        std::filesystem::remove(record_path, ec);  // best effort
        std::filesystem::remove(metadata_path, ec);
    };
    remove_both();
    if (capture.failed) return {};
    BindingFiles out;
    try {
        if (!capture.record.objects.empty()) {
            capture.record.write(record_path);
            out.binding_record = record_path.filename().string();
        }
        if (!capture.metadata.entries.empty()) {
            capture.metadata.write(metadata_path);
            out.output_metadata = metadata_path.filename().string();
        }
    } catch (const std::exception& e) {
        warn(std::string("write: ") + e.what() + "; the recording gets no binding record");
        remove_both();
        return {};
    }
    return out;
}

}  // namespace niobium::detail
