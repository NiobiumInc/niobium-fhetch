// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

/**
 * @file test.cpp
 * @brief The recorder writes the binding record and output metadata
 *        (docs/BINDING_RECORD.md) next to the trace.
 *
 * Records a toy CKKS program (n = 2^11): y = rotate(x * x, 1), with the
 * relinearization key and one rotation key tagged. Checks that
 *  - fhetch_replay.json names <prog>.nbbr and <prog>.nbom, and no interface;
 *  - the record's ring, moduli (Q then P) and roots are the context's;
 *  - input:x, key:relin, key:galois:<g> and output:y hold exactly the limbs
 *    the recorder addressed, in the Object layout's order (a key's big
 *    polynomial 2j is b_j, 2j + 1 is a_j);
 *  - every input limb is read by the trace and every output limb written;
 *  - the output metadata carries y's scaling factor, scale degree and slots.
 */
#include "compiler_internal.h"
#include "niobium/binding_record.h"
#include "niobium/compiler.h"
#include "niobium/fhetch_ir.h"
#include "niobium/fhetch_reader.h"
#include "niobium/output_metadata.h"

#include "openfhe.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace binding = niobium::binding;
using lbcrypto::DCRTPoly;

namespace {

int g_failed = 0;
int g_passed = 0;

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  ok   " : "  FAIL ") << what << '\n';
    ++(ok ? g_passed : g_failed);
}

constexpr std::uint64_t kNoAddress = ~std::uint64_t{0};

lbcrypto::CryptoContext<DCRTPoly> make_context(std::uint32_t first_mod_size = 60) {
    lbcrypto::CCParams<lbcrypto::CryptoContextCKKSRNS> p;
    p.SetSecurityLevel(lbcrypto::HEStd_NotSet);
    p.SetRingDim(1U << 11U);
    p.SetMultiplicativeDepth(2);
    p.SetScalingModSize(50);
    p.SetFirstModSize(first_mod_size);
    p.SetBatchSize(8);
    auto cc = lbcrypto::GenCryptoContext(p);
    cc->Enable(lbcrypto::PKE);
    cc->Enable(lbcrypto::KEYSWITCH);
    cc->Enable(lbcrypto::LEVELEDSHE);
    return cc;
}

// The limbs the recorder addressed for `polys`, under the record's moduli,
// split into runs: what the record must hold for that object.
std::vector<binding::Run> expected_runs(const binding::BindingRecord& rec, const std::vector<const DCRTPoly*>& polys) {
    std::unordered_map<std::uint64_t, std::uint16_t> index;
    for (std::size_t k = 0; k < rec.moduli.size(); ++k) index[rec.moduli[k]] = static_cast<std::uint16_t>(k);
    std::vector<binding::Limb> limbs;
    for (std::size_t i = 0; i < polys.size(); ++i)
        for (const auto& tower : polys[i]->GetAllElements()) {
            const auto address = niobium::detail::lookup_fhetch_address(tower.GetId());
            if (address == kNoAddress) continue;
            limbs.push_back(binding::Limb{static_cast<std::uint16_t>(i), index.at(tower.GetModulus().ConvertToInt()),
                                          address});
        }
    std::sort(limbs.begin(), limbs.end(), [](const binding::Limb& a, const binding::Limb& b) {
        return std::make_pair(a.big_poly, a.modulus_index) < std::make_pair(b.big_poly, b.modulus_index);
    });
    return binding::make_runs(limbs);
}

std::vector<const DCRTPoly*> elements(const lbcrypto::Ciphertext<DCRTPoly>& ct) {
    std::vector<const DCRTPoly*> out;
    for (const auto& e : ct->GetElements()) out.push_back(&e);
    return out;
}

// Row by row, as the Object layout says: b_0, a_0, b_1, a_1, ...
std::vector<const DCRTPoly*> rows(const lbcrypto::EvalKey<DCRTPoly>& key) {
    std::vector<const DCRTPoly*> out;
    for (std::size_t j = 0; j < key->GetBVector().size(); ++j) {
        out.push_back(&key->GetBVector()[j]);
        out.push_back(&key->GetAVector()[j]);
    }
    return out;
}

void check_object(const binding::BindingRecord& rec, const std::string& label, binding::Kind kind,
                  const std::vector<const DCRTPoly*>& polys) {
    const auto* obj = rec.find(label);
    const auto want = expected_runs(rec, polys);
    check(obj != nullptr && obj->kind == kind && obj->poly_count == polys.size() && obj->runs == want && !want.empty(),
          label + ": " + std::to_string(polys.size()) + " big polynomials, " + std::to_string(want.size()) +
              " runs, in the Object layout's order");
}

std::set<std::uint64_t> addresses(const binding::Object& obj) {
    std::set<std::uint64_t> out;
    for (const auto& r : obj.runs)
        for (std::uint16_t i = 0; i < r.limb_count; ++i) out.insert(r.first_address + i);
    return out;
}

}  // namespace

int main() {
    const auto dir = std::filesystem::current_path() / "binding_capture_test.tmp";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    std::string arg0 = "binding_capture_test";
    std::string arg1 = "--no-ring-dim-check";
    char* argv[] = {arg0.data(), arg1.data(), nullptr};
    int argc = 2;
    auto& compiler = niobium::compiler();
    compiler.init(argc, argv);
    compiler.set_program_info("binding_capture", "1.0", "binding record capture");
    compiler.set_program_directory(dir);

    auto cc = make_context();
    auto kp = cc->KeyGen();
    cc->EvalMultKeyGen(kp.secretKey);
    cc->EvalRotateKeyGen(kp.secretKey, {1});
    const std::vector<double> xs{0.25, 0.5, 0.75, 1.0, 2.0, 3.0, 4.0, 5.0};
    auto x = cc->Encrypt(kp.publicKey, cc->MakeCKKSPackedPlaintext(xs));

    compiler.capture_crypto_context(cc);
    compiler.tag_input("x", x);
    compiler.tag_keys(cc);
    compiler.start();
    auto y = cc->EvalRotate(cc->EvalMult(x, x), 1);
    compiler.probe("y", y);
    compiler.stop();

    const auto prog_dir = compiler.get_program_directory();
    const auto prog = compiler.program_name();

    // fhetch_replay.json names both files, and the old interface is gone.
    nlohmann::json replay;
    std::ifstream(prog_dir / "fhetch_replay.json") >> replay;
    const auto& files = replay.at("files");
    check(files.value("binding_record", "") == prog + ".nbbr" && files.value("output_metadata", "") == prog + ".nbom" &&
              !files.contains("interface"),
          "fhetch_replay.json names " + prog + ".nbbr and " + prog + ".nbom, and no interface");

    const auto rec = binding::BindingRecord::read(prog_dir / (prog + ".nbbr"));
    check(rec.objects.size() == 4, "4 objects: input, relin key, one rotation key, output");

    // Ring, moduli (Q then P) and roots: the context's.
    {
        const auto rns = std::dynamic_pointer_cast<lbcrypto::CryptoParametersRNS>(cc->GetCryptoParameters());
        std::vector<std::uint64_t> moduli;
        std::vector<std::uint64_t> roots;
        for (const auto& params : {cc->GetElementParams(), rns->GetParamsP()})
            for (const auto& t : params->GetParams()) {
                moduli.push_back(t->GetModulus().ConvertToInt());
                roots.push_back(t->GetRootOfUnity().ConvertToInt());
            }
        check(rec.ring_dim == cc->GetRingDimension() && rec.moduli == moduli && rec.roots == roots &&
                  moduli.size() > cc->GetElementParams()->GetParams().size(),
              "ring_dim, moduli (Q then P) and roots are the context's");
    }

    // Each object holds exactly the limbs the recorder addressed, in layout order.
    check_object(rec, "input:x", binding::Kind::Input, elements(x));
    check_object(rec, "output:y", binding::Kind::Output, elements(y));
    const auto tag = x->GetKeyTag();
    const auto& relin = lbcrypto::CryptoContextImpl<DCRTPoly>::GetAllEvalMultKeys().at(tag);
    check(relin.size() == 1, "one EvalMult key");
    check_object(rec, "key:relin", binding::Kind::Key, rows(relin.front()));
    const auto& autos = *lbcrypto::CryptoContextImpl<DCRTPoly>::GetAllEvalAutomorphismKeys().at(tag);
    check(autos.size() == 1, "one automorphism key");
    for (const auto& [g, key] : autos)
        check_object(rec, "key:galois:" + std::to_string(g), binding::Kind::Key, rows(key));

    // Against the trace: the program reads every input limb and writes every output limb.
    {
        const auto trace =
            niobium::fhetch::read_fhetch_file(prog_dir / replay.at("program_name").get<std::string>());
        check(trace.ok, "the trace reads back");
        std::set<std::uint64_t> read;
        std::set<std::uint64_t> written;
        for (const auto& inst : trace.program.instructions) {
            read.insert(inst.src1);
            read.insert(inst.src2);
            written.insert(inst.dest);
        }
        const auto in = addresses(*rec.find("input:x"));
        const auto out = addresses(*rec.find("output:y"));
        check(std::includes(read.begin(), read.end(), in.begin(), in.end()),
              "the trace reads all " + std::to_string(in.size()) + " input limbs");
        check(std::includes(written.begin(), written.end(), out.begin(), out.end()),
              "the trace writes all " + std::to_string(out.size()) + " output limbs");
    }

    // Output metadata.
    {
        const auto meta = binding::OutputMetadata::read(prog_dir / (prog + ".nbom"));
        const auto* e = meta.find("output:y");
        check(meta.entries.size() == 1 && e != nullptr && e->scaling_factor == y->GetScalingFactor() &&
                  e->scale_degree == y->GetNoiseScaleDeg() && e->slots == y->GetSlots(),
              "output metadata: y's scaling factor, scale degree and slots");
    }

    // reset() clears the capture: a recording with nothing tagged writes neither file.
    compiler.reset();
    compiler.set_program_info("binding_capture_empty", "1.0", "nothing tagged");
    compiler.set_program_directory(dir / "empty");
    compiler.start();
    compiler.stop();
    const auto empty_dir = compiler.get_program_directory();
    const auto empty = compiler.program_name();
    check(!std::filesystem::exists(empty_dir / (empty + ".nbbr")) && !std::filesystem::exists(empty_dir / (empty + ".nbom")),
          "after reset, a recording with nothing tagged writes neither file");

    // A capture that fails (here an input under moduli the program does not
    // have) writes neither file, and removes an earlier recording's.
    compiler.reset();
    compiler.set_program_info("binding_capture_failed", "1.0", "an input under other moduli");
    compiler.set_program_directory(dir / "failed");
    const auto failed_dir = compiler.get_program_directory();
    const auto failed = compiler.program_name();
    std::filesystem::create_directories(failed_dir);
    std::ofstream(failed_dir / (failed + ".nbbr")) << "an earlier recording's";
    auto other_cc = make_context(59);
    auto other_kp = other_cc->KeyGen();
    auto z = other_cc->Encrypt(other_kp.publicKey, other_cc->MakeCKKSPackedPlaintext(xs));
    compiler.tag_input("x", x);
    compiler.tag_input("z", z);
    compiler.start();
    compiler.probe("y", y);
    compiler.stop();
    check(!std::filesystem::exists(failed_dir / (failed + ".nbbr")) &&
              !std::filesystem::exists(failed_dir / (failed + ".nbom")),
          "a failed capture writes neither file and removes stale ones");

    std::filesystem::remove_all(dir);
    std::cout << g_passed << " passed, " << g_failed << " failed\n";
    return g_failed != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
