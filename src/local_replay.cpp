// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// run_local_replay_from_project — disk-driven local FHETCH replay. Reads a
// project directory (trace, inputs/keys, outputs, templates) with no Compiler
// singleton state, drives the simulator, and writes the same artifacts the
// in-process Compiler::replay() local path does, so spawned workers can replay
// concurrently without sharing OpenFHE's process-global transform caches.

#include "local_replay.h"

#include "niobium/fhetch_parser.h"
#include "niobium/fhetch_sim/simulator.h"

#include "openfhe.h"
#include "ciphertext-ser.h"
#include "cryptocontext-ser.h"
#include "key/key-ser.h"
#include "scheme/ckksrns/ckksrns-ser.h"

#include <cereal/archives/portable_binary.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using DCRTPoly = lbcrypto::DCRTPoly;

// ===========================================================================
// Input/key loaders. Lifted verbatim from tests/fhetch_driver/main.cpp so the
// worker and the (gated) fhetch_driver test share one copy of the OpenFHE-cereal
// .bin/.ids layout that must track the auto_facade writers.
// ===========================================================================

namespace {

// .ids layout: [uint64_t count][uint64_t id_0]...
std::vector<uint64_t> read_ids(const fs::path& p) {
    std::vector<uint64_t> v;
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return v;
    uint64_t n = 0;
    f.read(reinterpret_cast<char*>(&n), sizeof(n));
    v.resize(n);
    if (n) f.read(reinterpret_cast<char*>(v.data()), n * sizeof(uint64_t));
    return v;
}

// Extract per-tower (addr, values, modulus) triples from a DCRTPoly; the .ids
// iterator provides the FHETCH address for each tower in order.
void extract_dcrt_towers(const DCRTPoly& dcrt,
                         std::vector<uint64_t>::const_iterator& id_it,
                         std::vector<uint64_t>::const_iterator id_end,
                         niobium::fhetch::DriveInputs& inputs) {
    for (const auto& tower : dcrt.GetAllElements()) {
        if (id_it == id_end) return;
        uint64_t addr = *id_it++;
        uint64_t modulus = tower.GetModulus().ConvertToInt();
        const auto& vals = tower.GetValues();
        std::vector<uint64_t> out(vals.GetLength());
        for (size_t i = 0; i < vals.GetLength(); ++i)
            out[i] = vals[i].ConvertToInt();
        inputs.data[addr] = {std::move(out), modulus};
    }
}

// .input_NAME.bin layout (auto_facade tag_input):
//   uint32 instances; uint8 payload_type; uint32 num_polys; DCRTPoly * num_polys
bool load_input_bin(const fs::path& bin_path, const std::vector<uint64_t>& ids,
                    niobium::fhetch::DriveInputs& inputs) {
    std::ifstream f(bin_path, std::ios::binary);
    if (!f.is_open()) return false;
    try {
        cereal::PortableBinaryInputArchive ar(f);
        uint32_t instances = 0;
        uint8_t payload_type = 0;
        uint32_t num_polys = 0;
        ar(instances);
        ar(payload_type);
        ar(num_polys);

        auto id_it = ids.begin();
        for (uint32_t i = 0; i < num_polys; ++i) {
            DCRTPoly dcrt;
            ar(dcrt);
            extract_dcrt_towers(dcrt, id_it, ids.end(), inputs);
        }
    } catch (const std::exception& e) {
        std::cerr << "[fhetch_sim] failed to read " << bin_path << ": " << e.what() << std::endl;
        return false;
    }
    return true;
}

// .mk.bin / .rk.bin layout (auto_facade serialize_eval_keys):
//   uint32 num_keys; per key: uint32 av_size, DCRTPoly*av_size, uint32 bv_size,
//   DCRTPoly*bv_size. The .ids list flattens all keys' (A, B) towers in order.
bool load_key_bin(const fs::path& bin_path, const std::vector<uint64_t>& ids,
                  niobium::fhetch::DriveInputs& inputs) {
    std::ifstream f(bin_path, std::ios::binary);
    if (!f.is_open()) return false;
    try {
        cereal::PortableBinaryInputArchive ar(f);
        uint32_t num_keys = 0;
        ar(num_keys);
        auto id_it = ids.begin();
        for (uint32_t k = 0; k < num_keys; ++k) {
            uint32_t av_size = 0;
            ar(av_size);
            for (uint32_t i = 0; i < av_size; ++i) {
                DCRTPoly dcrt;
                ar(dcrt);
                extract_dcrt_towers(dcrt, id_it, ids.end(), inputs);
            }
            uint32_t bv_size = 0;
            ar(bv_size);
            for (uint32_t i = 0; i < bv_size; ++i) {
                DCRTPoly dcrt;
                ar(dcrt);
                extract_dcrt_towers(dcrt, id_it, ids.end(), inputs);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[fhetch_sim] failed to read " << bin_path << ": " << e.what() << std::endl;
        return false;
    }
    return true;
}

// .bp.bin layout (auto_facade tag_bootstrap_precompute): num_entries; per entry
// m_slots, m_dim1, 2x(9 uint32 params), 2x 2D-DCRTPoly, 2x 1D-DCRTPoly.
// 2D: uint32 outer; per inner: uint32 inner, DCRTPoly... | 1D: uint32 size, DCRTPoly...
bool load_bp_bin(const fs::path& bin_path, const std::vector<uint64_t>& ids,
                 niobium::fhetch::DriveInputs& inputs) {
    std::ifstream f(bin_path, std::ios::binary);
    if (!f.is_open()) return false;
    try {
        cereal::PortableBinaryInputArchive ar(f);
        uint32_t num_entries = 0;
        ar(num_entries);
        auto id_it = ids.begin();
        for (uint32_t e = 0; e < num_entries; ++e) {
            uint32_t m_slots = 0, m_dim1 = 0;
            ar(m_slots);
            ar(m_dim1);
            for (int p = 0; p < 2; ++p) {
                uint32_t lvlb = 0, layersCollapse = 0, remCollapse = 0, numRotations = 0,
                         b = 0, g = 0, numRotationsRem = 0, bRem = 0, gRem = 0;
                ar(lvlb, layersCollapse, remCollapse, numRotations,
                   b, g, numRotationsRem, bRem, gRem);
            }
            auto read_2d = [&]() {
                uint32_t outer_size = 0;
                ar(outer_size);
                for (uint32_t i = 0; i < outer_size; ++i) {
                    uint32_t inner_size = 0;
                    ar(inner_size);
                    for (uint32_t j = 0; j < inner_size; ++j) {
                        DCRTPoly dcrt;
                        ar(dcrt);
                        extract_dcrt_towers(dcrt, id_it, ids.end(), inputs);
                    }
                }
            };
            auto read_1d = [&]() {
                uint32_t size = 0;
                ar(size);
                for (uint32_t i = 0; i < size; ++i) {
                    DCRTPoly dcrt;
                    ar(dcrt);
                    extract_dcrt_towers(dcrt, id_it, ids.end(), inputs);
                }
            };
            read_2d();   // m_U0hatTPreFFT
            read_2d();   // m_U0PreFFT
            read_1d();   // m_U0Pre
            read_1d();   // m_U0hatTPre
        }
    } catch (const std::exception& e) {
        std::cerr << "[fhetch_sim] failed to read " << bin_path << ": " << e.what() << std::endl;
        return false;
    }
    return true;
}

}  // namespace

namespace niobium::fhetch {

bool load_source_inputs(const fs::path& dir, const std::string& prog, DriveInputs& inputs) {
    fs::path idx_path = dir / (prog + ".inputs.json");
    if (!fs::exists(idx_path)) return true;  // nothing to load is OK

    nlohmann::json j;
    try {
        std::ifstream in(idx_path);
        in >> j;
    } catch (...) {
        std::cerr << "[fhetch_sim] failed to parse " << idx_path << std::endl;
        return false;
    }

    if (!j.contains("inputs")) return true;
    for (const auto& entry : j["inputs"]) {
        std::string bin = entry.value("bin_file", "");
        std::string ids = entry.value("ids_file", "");
        if (bin.empty() || ids.empty()) continue;
        fs::path bin_path = fs::path(bin).is_absolute() ? fs::path(bin) : dir / bin;
        fs::path ids_path = fs::path(ids).is_absolute() ? fs::path(ids) : dir / ids;
        if (!fs::exists(bin_path) || !fs::exists(ids_path)) continue;

        auto addr_ids = read_ids(ids_path);
        if (!load_input_bin(bin_path, addr_ids, inputs)) return false;
    }
    return true;
}

bool load_source_keys(const fs::path& dir, const std::string& prog, DriveInputs& inputs) {
    auto load_with = [&](const std::string& base, auto&& reader) {
        fs::path bin = dir / (prog + "." + base + ".bin");
        fs::path ids = dir / (prog + "." + base + ".ids");
        if (!fs::exists(bin) || !fs::exists(ids)) return;
        auto addr_ids = read_ids(ids);
        reader(bin, addr_ids, inputs);
    };
    load_with("mk", load_key_bin);
    load_with("rk", load_key_bin);
    load_with("bp", load_bp_bin);
    return true;
}

}  // namespace niobium::fhetch

namespace niobium {

bool run_local_replay_from_project(const fs::path& project_dir) {
    using json = nlohmann::json;

    if (!fs::is_directory(project_dir)) {
        std::cerr << "[fhetch_sim] not a directory: " << project_dir << std::endl;
        return false;
    }

    // ---- Project index: ring dimension + trace filename ----
    std::ifstream index_in(project_dir / "fhetch_replay.json");
    if (!index_in.is_open()) {
        std::cerr << "[fhetch_sim] missing fhetch_replay.json in " << project_dir << std::endl;
        return false;
    }
    json index = json::parse(index_in, nullptr, false);
    if (index.is_discarded() || !index.contains("crypto_context") || !index.contains("files")) {
        std::cerr << "[fhetch_sim] malformed fhetch_replay.json" << std::endl;
        return false;
    }
    uint64_t ring_dim = index["crypto_context"].value("ring_dimension", uint64_t{0});
    std::string trace_file = index["files"].value("instructions", std::string{});
    if (ring_dim == 0 || trace_file.empty()) {
        std::cerr << "[fhetch_sim] fhetch_replay.json lacks ring_dimension or instructions"
                  << std::endl;
        return false;
    }
    std::string prog = fs::path(trace_file).stem().string();

    // ---- Build + load the simulator ----
    fhetch_sim::Simulator sim;
    sim.set_ring_dimension(ring_dim);
    if (!sim.load_trace(project_dir / trace_file)) {
        std::cerr << "[fhetch_sim] failed to load trace " << trace_file << std::endl;
        return false;
    }

    // ---- Populate inputs + keys. Each input is recorded at its live-in
    //      address (.ids), so the in-process copy-lineage fixpoint is moot. ----
    fhetch::DriveInputs inputs;
    if (!fhetch::load_source_inputs(project_dir, prog, inputs) ||
        !fhetch::load_source_keys(project_dir, prog, inputs)) {
        return false;
    }
    for (const auto& [addr, elem] : inputs.data)
        sim.store_polynomial(addr, elem.values, elem.modulus);

    // ---- Output probe names + addr_ids, in the recorded array order ----
    std::vector<std::pair<std::string, std::vector<uint64_t>>> outputs;
    {
        std::ifstream out_in(project_dir / (prog + ".outputs.json"));
        if (out_in.is_open()) {
            json oj = json::parse(out_in, nullptr, false);
            if (!oj.is_discarded() && oj.contains("outputs")) {
                for (const auto& out : oj["outputs"]) {
                    std::string name = out.value("name", std::string{});
                    if (name.empty() || !out.contains("ciphertext_data")) continue;
                    std::vector<uint64_t> addr_ids;
                    for (const auto& poly : out["ciphertext_data"]) {
                        if (!poly.contains("elements")) continue;
                        for (const auto& elem : poly["elements"])
                            addr_ids.push_back(elem.get<uint64_t>());
                    }
                    outputs.emplace_back(std::move(name), std::move(addr_ids));
                }
            }
        }
    }

    std::vector<uint64_t> live_out;
    for (const auto& [name, addr_ids] : outputs)
        live_out.insert(live_out.end(), addr_ids.begin(), addr_ids.end());
    sim.set_live_out_addresses(live_out);
    sim.compute_liveness();

    auto result = sim.run();
    if (result.errors > 0) {
        std::cerr << "[fhetch_sim] replay failed: " << result.errors << " errors" << std::endl;
        return false;
    }
    std::cout << "[fhetch_sim] replay complete: " << result.instructions_executed
              << " instructions, " << result.elapsed_seconds << "s" << std::endl;

    // ---- fhetch_replay_outputs.json, matching Compiler::write_replay_outputs:
    //      use the simulator's per-tower modulus, COPY_MODULUS for ops that
    //      carry none (e.g. sr_automorph_eval, which the sim stores as 0). ----
    constexpr uint64_t COPY_MODULUS = 0xFFFFFFFFFFFFFFFFULL;
    json root;
    json outputs_arr = json::array();
    for (const auto& [name, addr_ids] : outputs) {
        json out_entry;
        out_entry["name"] = name;
        json elems_arr = json::array();
        for (uint64_t addr : addr_ids) {
            auto values = sim.get_polynomial(addr);
            uint64_t modulus = sim.get_modulus(addr);
            json elem;
            elem["addr_id"] = addr;
            elem["modulus"] = (modulus == 0) ? COPY_MODULUS : modulus;
            elem["status"] = values.empty() ? "missing" : "computed";
            elem["values"] = values;
            elems_arr.push_back(elem);
        }
        out_entry["elements"] = elems_arr;
        outputs_arr.push_back(out_entry);
    }
    root["outputs"] = outputs_arr;
    {
        std::ofstream out(project_dir / "fhetch_replay_outputs.json");
        if (!out.is_open()) {
            std::cerr << "[fhetch_sim] failed to write fhetch_replay_outputs.json" << std::endl;
            return false;
        }
        out << root.dump(2) << std::endl;
    }

    // ---- Register the project's CryptoContext so the templates reconstructed
    //      below bind to the full modulus chain, matching the in-process path
    //      (whose CC is already registered) byte-for-byte. ----
    {
        lbcrypto::CryptoContext<DCRTPoly> cc;
        auto cc_path = project_dir / "cryptocontext.dat";
        if (fs::exists(cc_path) &&
            !lbcrypto::Serial::DeserializeFromFile(cc_path.string(), cc, lbcrypto::SerType::BINARY)) {
            std::cerr << "[fhetch_sim] failed to load cryptocontext.dat" << std::endl;
            return false;
        }
    }

    // ---- Reconstruct serialized_probes/<name>.ct from the templates ----
    reconstruct_probes_in(project_dir);

    fs::path probes_dir = project_dir / "serialized_probes";
    if (!fs::exists(probes_dir) || fs::is_empty(probes_dir)) {
        std::cerr << "[fhetch_sim] no probes produced" << std::endl;
        return false;
    }
    return true;
}

}  // namespace niobium
