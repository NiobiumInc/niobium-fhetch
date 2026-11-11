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
 * @brief The recorder's binding record and output metadata capture (see
 *        binding_capture.h and docs/BINDING_RECORD.md).
 */
#include "binding_capture.h"

#include "compiler_internal.h"
#include "niobium/binding_record.h"
#include "niobium/output_metadata.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
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

using lbcrypto::DCRTPoly;
using binding::Kind;

/// What lookup_fhetch_address returns for a tower the trace never reads.
constexpr std::uint64_t kNoAddress = ~std::uint64_t{0};

struct Capture {
    binding::BindingRecord record;
    binding::OutputMetadata metadata;
    std::unordered_map<std::uint64_t, std::uint16_t> modulus_index;  ///< prime -> index into record.moduli
    std::string key_tag;                                             ///< the client's, from the first input
};

Capture& capture() {
    static Capture c;
    return c;
}

void warn(const char* what, const std::exception& e) {
    std::cout << "[NIOBIUM] WARNING: binding record: " << what << ": " << e.what() << std::endl;
}

// Runs a capture step; a failure only warns, so it never fails the recording.
template <class F>
void guarded(const char* what, F&& step) {
    try {
        std::forward<F>(step)();
    } catch (const std::exception& e) {
        warn(what, e);
    }
}

// ring_dim, then Q's primes followed by P's, each with the root of unity its
// NTT uses, taken once from the program's crypto context.
void set_params(const lbcrypto::CryptoContext<DCRTPoly>& cc) {
    auto& c = capture();
    if (c.record.ring_dim != 0) return;
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
    for (std::size_t k = 0; k < record.moduli.size(); ++k)
        index.emplace(record.moduli[k], static_cast<std::uint16_t>(k));
    c.record = std::move(record);
    c.modulus_index = std::move(index);
}

// --- The walk -------------------------------------------------------------------
//
// Which big polynomial of an object is number 0, 1, ...: this MUST match the
// Object layout section of docs/BINDING_RECORD.md, which every producer of an
// upload follows. Temporary: it moves, with the recorder, to the OpenFHE
// adapter's trace library.

using BigPolys = std::vector<const DCRTPoly*>;

/// A ciphertext c_0 ... c_k: big polynomial i is c_i.
BigPolys big_polys(const lbcrypto::Ciphertext<DCRTPoly>& ct) {
    BigPolys out;
    for (const auto& element : ct->GetElements()) out.push_back(&element);
    return out;
}

/// A key-switching key of d digits, row by row: 2j is b_j, 2j + 1 is a_j.
BigPolys big_polys(const lbcrypto::EvalKey<DCRTPoly>& key) {
    const auto& b = key->GetBVector();
    const auto& a = key->GetAVector();
    if (a.size() != b.size()) throw std::runtime_error("a key with unequal A and B digit counts");
    BigPolys out;
    for (std::size_t j = 0; j < b.size(); ++j) {
        out.push_back(&b[j]);
        out.push_back(&a[j]);
    }
    return out;
}

// -----------------------------------------------------------------------------

// Records `label` from its big polynomials: every tower the trace reads is a
// limb at its address; a tower it never reads lies in no run. Replaces an
// object of the same label.
void add_object(const std::string& label, Kind kind, const BigPolys& polys) {
    auto& c = capture();
    if (polys.size() > UINT16_MAX) throw std::runtime_error(label + " has more than 65535 big polynomials");
    std::vector<binding::Limb> limbs;
    for (std::size_t i = 0; i < polys.size(); ++i) {
        for (const auto& tower : polys[i]->GetAllElements()) {
            const std::uint64_t address = lookup_fhetch_address(tower.GetId());
            if (address == kNoAddress) continue;
            const std::uint64_t q = tower.GetModulus().ConvertToInt();
            const auto it = c.modulus_index.find(q);
            if (it == c.modulus_index.end())
                throw std::runtime_error(label + " has modulus " + std::to_string(q) +
                                         ", which is not among the program's moduli");
            limbs.push_back(binding::Limb{static_cast<std::uint16_t>(i), it->second, address});
        }
    }
    c.record.remove_object(label);
    c.record.add_object(label, kind, static_cast<std::uint16_t>(polys.size()), limbs);
}

// The client's key tag: the first tagged input's, or the only one registered.
std::string client_key_tag() {
    if (!capture().key_tag.empty()) return capture().key_tag;
    using CC = lbcrypto::CryptoContextImpl<DCRTPoly>;
    std::set<std::string> tags;
    for (const auto& entry : CC::GetAllEvalMultKeys()) tags.insert(entry.first);
    for (const auto& entry : CC::GetAllEvalAutomorphismKeys()) tags.insert(entry.first);
    if (tags.size() > 1)
        throw std::runtime_error(std::to_string(tags.size()) + " key tags and no tagged input to choose the client's");
    return tags.empty() ? std::string() : *tags.begin();
}

}  // namespace

void capture_binding_input(const std::string& name, const lbcrypto::Ciphertext<DCRTPoly>& ct) {
    guarded("input", [&] {
        set_params(ct->GetCryptoContext());
        // The client is whoever encrypted the inputs: its keys are the ones to bind.
        if (capture().key_tag.empty()) capture().key_tag = ct->GetKeyTag();
        add_object(binding::make_label(Kind::Input, name), Kind::Input, big_polys(ct));
    });
}

void capture_binding_output(const std::string& name, const lbcrypto::Ciphertext<DCRTPoly>& ct) {
    guarded("output", [&] {
        set_params(ct->GetCryptoContext());
        const auto label = binding::make_label(Kind::Output, name);
        add_object(label, Kind::Output, big_polys(ct));
        capture().metadata.set(binding::OutputMetadataEntry{label, ct->GetScalingFactor(),
                                                            static_cast<std::uint32_t>(ct->GetNoiseScaleDeg()),
                                                            ct->GetSlots()});
    });
}

void capture_binding_keys(const lbcrypto::CryptoContext<DCRTPoly>& cc) {
    guarded("keys", [&] {
        set_params(cc);
        const std::string tag = client_key_tag();
        if (tag.empty()) return;
        using CC = lbcrypto::CryptoContextImpl<DCRTPoly>;
        const auto& mult = CC::GetAllEvalMultKeys();
        if (const auto it = mult.find(tag); it != mult.end() && !it->second.empty()) {
            if (it->second.size() > 1)
                warn("keys", std::runtime_error("more than one EvalMult key under the client's tag; key:relin left out"));
            else
                guarded("key:relin",
                        [&] { add_object(binding::make_label(Kind::Key, "relin"), Kind::Key, big_polys(it->second.front())); });
        }
        const auto& autos = CC::GetAllEvalAutomorphismKeys();
        if (const auto it = autos.find(tag); it != autos.end() && it->second)
            for (const auto& [galois, key] : *it->second)
                guarded("key:galois", [&, g = galois, k = key] {
                    add_object(binding::make_label(Kind::Key, "galois:" + std::to_string(g)), Kind::Key, big_polys(k));
                });
    });
}

BindingFiles write_binding_files(const std::filesystem::path& dir, const std::string& prog) {
    BindingFiles out;
    const auto& c = capture();
    // Writes `doc` as <prog><ext> when it has content; removes a stale file otherwise.
    const auto write = [&](const char* what, bool has_content, const std::string& name, const auto& doc) {
        std::string written;
        guarded(what, [&] {
            const auto path = dir / name;
            if (has_content) {
                doc.write(path);
                written = name;
            } else {
                std::error_code ec;
                std::filesystem::remove(path, ec);  // best effort
            }
        });
        return written;
    };
    out.binding_record = write("write", !c.record.objects.empty(), prog + ".nbbr", c.record);
    out.output_metadata = write("write output metadata", !c.metadata.entries.empty(), prog + ".nbom", c.metadata);
    return out;
}

void clear_binding_capture() { capture() = Capture{}; }

}  // namespace niobium::detail
