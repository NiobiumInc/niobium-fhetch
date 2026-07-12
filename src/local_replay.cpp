// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// Implementation of the disk-driven local replay declared in local_replay.h.

#include "local_replay.h"
#include "replay_poly_source.h"

#include "niobium/fhetch_parser.h"  // DriveInputs (used directly below)
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
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using DCRTPoly = lbcrypto::DCRTPoly;

// The .bin/.ids cereal loaders below are lifted verbatim from
// tests/fhetch_driver/main.cpp so the worker and that (gated) test share one
// copy of the on-disk layout, which must track the auto_facade writers.
// Exposed via local_replay.h so the lazy DiskPolySource shares them too.
namespace niobium::fhetch {

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

namespace {
// Extract per-tower (addr, values, modulus) triples from a DCRTPoly; the .ids
// iterator provides the FHETCH address for each tower in order.
void extract_dcrt_towers(const DCRTPoly& dcrt,
                         std::vector<uint64_t>::const_iterator& id_it,
                         std::vector<uint64_t>::const_iterator id_end,
                         const niobium::fhetch::PolySink& sink) {
    for (const auto& tower : dcrt.GetAllElements()) {
        if (id_it == id_end) return;
        uint64_t addr = *id_it++;
        uint64_t modulus = tower.GetModulus().ConvertToInt();
        const auto& vals = tower.GetValues();
        std::vector<uint64_t> out(vals.GetLength());
        for (size_t i = 0; i < vals.GetLength(); ++i)
            out[i] = vals[i].ConvertToInt();
        sink(addr, std::move(out), modulus);
    }
}
}  // namespace

// .input_NAME.bin: uint32 instances; uint8 payload_type; uint32 num_polys;
// DCRTPoly * num_polys.
bool load_input_bin(const fs::path& bin_path, const std::vector<uint64_t>& ids,
                    const niobium::fhetch::PolySink& sink) {
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
            extract_dcrt_towers(dcrt, id_it, ids.end(), sink);
        }
    } catch (const std::exception& e) {
        std::cerr << "[fhetch_sim] failed to read " << bin_path << ": " << e.what() << std::endl;
        return false;
    }
    return true;
}

// .mk.bin / .rk.bin: uint32 num_keys; per key uint32 av_size, DCRTPoly*av_size,
// uint32 bv_size, DCRTPoly*bv_size. The .ids flatten all keys' (A, B) towers.
bool load_key_bin(const fs::path& bin_path, const std::vector<uint64_t>& ids,
                  const niobium::fhetch::PolySink& sink) {
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
                extract_dcrt_towers(dcrt, id_it, ids.end(), sink);
            }
            uint32_t bv_size = 0;
            ar(bv_size);
            for (uint32_t i = 0; i < bv_size; ++i) {
                DCRTPoly dcrt;
                ar(dcrt);
                extract_dcrt_towers(dcrt, id_it, ids.end(), sink);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[fhetch_sim] failed to read " << bin_path << ": " << e.what() << std::endl;
        return false;
    }
    return true;
}

// .bp.bin: num_entries; per entry m_slots, m_dim1, 2x(9 uint32 params),
// 2x 2D-DCRTPoly, 2x 1D-DCRTPoly. 2D: uint32 outer; per inner uint32 inner,
// DCRTPoly... | 1D: uint32 size, DCRTPoly...
bool load_bp_bin(const fs::path& bin_path, const std::vector<uint64_t>& ids,
                 const niobium::fhetch::PolySink& sink) {
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
                        extract_dcrt_towers(dcrt, id_it, ids.end(), sink);
                    }
                }
            };
            auto read_1d = [&]() {
                uint32_t size = 0;
                ar(size);
                for (uint32_t i = 0; i < size; ++i) {
                    DCRTPoly dcrt;
                    ar(dcrt);
                    extract_dcrt_towers(dcrt, id_it, ids.end(), sink);
                }
            };
            read_2d();  // m_U0hatTPreFFT
            read_2d();  // m_U0PreFFT
            read_1d();  // m_U0Pre
            read_1d();  // m_U0hatTPre
        }
    } catch (const std::exception& e) {
        std::cerr << "[fhetch_sim] failed to read " << bin_path << ": " << e.what() << std::endl;
        return false;
    }
    return true;
}

}  // namespace niobium::fhetch

namespace niobium {
namespace fhetch {

bool load_source_inputs(const fs::path& dir, const std::string& prog, const PolySink& sink) {
    fs::path idx_path = dir / (prog + ".inputs.json");
    if (!fs::exists(idx_path)) return true;  // a trace with no live-in inputs is fine

    nlohmann::json j;
    try {
        std::ifstream in(idx_path);
        in >> j;
    } catch (const std::exception& e) {
        std::cerr << "[fhetch_sim] failed to parse " << idx_path << ": " << e.what() << std::endl;
        return false;
    }

    if (!j.contains("inputs")) return true;
    for (const auto& entry : j["inputs"]) {
        std::string bin = entry.value("bin_file", "");
        std::string ids = entry.value("ids_file", "");
        if (bin.empty() || ids.empty()) continue;
        fs::path bin_path = fs::path(bin).is_absolute() ? fs::path(bin) : dir / bin;
        fs::path ids_path = fs::path(ids).is_absolute() ? fs::path(ids) : dir / ids;
        if (!fs::exists(bin_path) || !fs::exists(ids_path)) {
            std::cerr << "[fhetch_sim] input " << entry.value("name", bin)
                      << " references a missing .bin/.ids under " << dir << std::endl;
            return false;
        }
        auto addr_ids = read_ids(ids_path);
        if (!load_input_bin(bin_path, addr_ids, sink)) return false;
    }
    return true;
}

bool load_source_keys(const fs::path& dir, const std::string& prog, const PolySink& sink) {
    // mk/rk/bp are each optional; a present file that fails to load is an error.
    auto load_with = [&](const std::string& base, auto&& reader) -> bool {
        fs::path bin = dir / (prog + "." + base + ".bin");
        fs::path ids = dir / (prog + "." + base + ".ids");
        if (!fs::exists(bin) || !fs::exists(ids)) return true;
        auto addr_ids = read_ids(ids);
        return reader(bin, addr_ids, sink);
    };
    return load_with("mk", load_key_bin) && load_with("rk", load_key_bin) &&
           load_with("bp", load_bp_bin);
}

namespace {
PolySink collect_into(DriveInputs& inputs) {
    return [&inputs](uint64_t addr, std::vector<uint64_t>&& values, uint64_t modulus) {
        inputs.data[addr] = {std::move(values), modulus};
    };
}
}  // namespace

bool load_source_inputs(const fs::path& dir, const std::string& prog, DriveInputs& inputs) {
    return load_source_inputs(dir, prog, collect_into(inputs));
}

bool load_source_keys(const fs::path& dir, const std::string& prog, DriveInputs& inputs) {
    return load_source_keys(dir, prog, collect_into(inputs));
}

}  // namespace fhetch

namespace {

using NamedOutput = std::pair<std::string, std::vector<uint64_t>>;

struct ProjectIndex {
    uint64_t ring_dim = 0;
    std::string trace_file;  // filename, relative to the project dir
    std::string prog;        // trace stem, prefixes the per-input/-output files
};

// Read ring dimension + trace filename from fhetch_replay.json.
bool read_project_index(const fs::path& dir, ProjectIndex& out) {
    std::ifstream in(dir / "fhetch_replay.json");
    if (!in.is_open()) {
        std::cerr << "[fhetch_sim] missing fhetch_replay.json in " << dir << std::endl;
        return false;
    }
    auto j = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.contains("crypto_context") || !j.contains("files")) {
        std::cerr << "[fhetch_sim] malformed fhetch_replay.json in " << dir << std::endl;
        return false;
    }
    out.ring_dim = j["crypto_context"].value("ring_dimension", uint64_t{0});
    out.trace_file = j["files"].value("instructions", std::string{});
    if (out.ring_dim == 0 || out.trace_file.empty()) {
        std::cerr << "[fhetch_sim] fhetch_replay.json lacks ring_dimension or instructions"
                  << std::endl;
        return false;
    }
    out.prog = fs::path(out.trace_file).stem().string();
    return true;
}

// Stream recorded inputs + keys into the simulator at their recorded
// addresses: one DCRTPoly is resident at a time and each tower's buffer
// is donated to the simulator, so no DriveInputs-sized copy ever exists.
// Inputs sit at their live-in addresses, so the in-process copy-lineage
// fixpoint is unnecessary here — which also means polys the trace never
// reads would only sit in memory until the end of the run; drop them.
// Must run after sim.load_trace() (needs the trace's live-in set).
bool populate_inputs(const fs::path& dir, const std::string& prog, fhetch_sim::Simulator& sim) {
    auto rbw = sim.get_read_before_write_addresses();
    std::unordered_set<uint64_t> live_in(rbw.begin(), rbw.end());

    size_t stored = 0;
    size_t dropped = 0;
    fhetch::PolySink sink = [&](uint64_t addr, std::vector<uint64_t>&& values,
                                uint64_t modulus) {
        if (live_in.count(addr) != 0U) {
            sim.store_polynomial(addr, std::move(values), modulus);
            ++stored;
        } else {
            ++dropped;
        }
    };
    if (!fhetch::load_source_inputs(dir, prog, sink)) {
        std::cerr << "[fhetch_sim] failed to load inputs for " << prog << std::endl;
        return false;
    }
    if (!fhetch::load_source_keys(dir, prog, sink)) {
        std::cerr << "[fhetch_sim] failed to load keys for " << prog << std::endl;
        return false;
    }
    std::cout << "[fhetch_sim] inputs: " << stored << " live-in polys stored, "
              << dropped << " never-read polys dropped" << std::endl;
    return true;
}

// Output probe names + addr_ids in recorded array order, from <prog>.outputs.json.
bool read_output_probes(const fs::path& dir, const std::string& prog,
                        std::vector<NamedOutput>& out) {
    std::ifstream in(dir / (prog + ".outputs.json"));
    if (!in.is_open()) {
        std::cerr << "[fhetch_sim] missing " << prog << ".outputs.json in " << dir << std::endl;
        return false;
    }
    auto j = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.contains("outputs")) {
        std::cerr << "[fhetch_sim] malformed " << prog << ".outputs.json in " << dir << std::endl;
        return false;
    }
    for (const auto& o : j["outputs"]) {
        std::string name = o.value("name", std::string{});
        if (name.empty() || !o.contains("ciphertext_data")) continue;
        std::vector<uint64_t> addr_ids;
        for (const auto& poly : o["ciphertext_data"])
            for (const auto& e : poly.value("elements", nlohmann::json::array()))
                addr_ids.push_back(e.get<uint64_t>());
        out.emplace_back(std::move(name), std::move(addr_ids));
    }
    return true;
}

// Write fhetch_replay_outputs.json exactly as Compiler::write_replay_outputs:
// the simulator's per-tower modulus (the COPY_MODULUS sentinel for ops that
// carry none, e.g. sr_automorph_eval) and computed values.
bool write_replay_outputs(const fs::path& dir, const std::vector<NamedOutput>& outputs,
                          const fhetch_sim::Simulator& sim) {
    constexpr uint64_t kCopyModulus = 0xFFFFFFFFFFFFFFFFULL;
    nlohmann::json root;
    nlohmann::json outputs_arr = nlohmann::json::array();
    for (const auto& [name, addr_ids] : outputs) {
        nlohmann::json out_entry;
        out_entry["name"] = name;
        nlohmann::json elems_arr = nlohmann::json::array();
        for (uint64_t addr : addr_ids) {
            auto values = sim.get_polynomial(addr);
            uint64_t modulus = sim.get_modulus(addr);
            nlohmann::json elem;
            elem["addr_id"] = addr;
            elem["modulus"] = (modulus == 0) ? kCopyModulus : modulus;
            elem["status"] = values.empty() ? "missing" : "computed";
            elem["values"] = values;
            elems_arr.push_back(elem);
        }
        out_entry["elements"] = elems_arr;
        outputs_arr.push_back(out_entry);
    }
    root["outputs"] = outputs_arr;
    std::ofstream out(dir / "fhetch_replay_outputs.json");
    if (!out.is_open()) {
        std::cerr << "[fhetch_sim] failed to write fhetch_replay_outputs.json in " << dir
                  << std::endl;
        return false;
    }
    out << root.dump(2) << std::endl;
    return true;
}

// Register the project CryptoContext so the reconstructed ciphertexts bind to
// the full modulus chain, matching the in-process path byte-for-byte.
bool register_crypto_context(const fs::path& dir) {
    auto cc_path = dir / "cryptocontext.dat";
    if (!fs::exists(cc_path)) {
        std::cerr << "[fhetch_sim] missing cryptocontext.dat in " << dir << std::endl;
        return false;
    }
    // `cc` is intentionally discarded: deserializing registers it in OpenFHE's
    // global CryptoContextFactory (which retains it), and that registration —
    // not the local — is what reconstruct_probes_in's template deserialize binds
    // to. Do not "clean up" this unused local.
    lbcrypto::CryptoContext<DCRTPoly> cc;
    if (!lbcrypto::Serial::DeserializeFromFile(cc_path.string(), cc, lbcrypto::SerType::BINARY)) {
        std::cerr << "[fhetch_sim] failed to load cryptocontext.dat in " << dir << std::endl;
        return false;
    }
    return true;
}

}  // namespace

bool run_local_replay_from_project(const fs::path& dir) {
    return run_local_replay_from_project(dir, LocalReplayOptions{});
}

bool run_local_replay_from_project(const fs::path& dir,
                                   const LocalReplayOptions& opts) {
    if (!fs::is_directory(dir)) {
        std::cerr << "[fhetch_sim] not a directory: " << dir << std::endl;
        return false;
    }

    ProjectIndex idx;
    if (!read_project_index(dir, idx)) return false;

    fhetch_sim::Simulator sim;
    sim.set_ring_dimension(idx.ring_dim);
    if (!sim.load_trace(dir / idx.trace_file)) {
        std::cerr << "[fhetch_sim] failed to load trace " << idx.trace_file << std::endl;
        return false;
    }

    std::vector<NamedOutput> outputs;
    if (!read_output_probes(dir, idx.prog, outputs)) return false;

    // Pin every output address so it stays recoverable for readback
    // (unbounded: never freed; budget mode: may spill, faults back).
    std::vector<uint64_t> live_out;
    for (const auto& [name, addr_ids] : outputs)
        live_out.insert(live_out.end(), addr_ids.begin(), addr_ids.end());
    sim.set_live_out_addresses(live_out);

    if (opts.mem_budget_mb > 0) {
        // Bounded: no preload — inputs/keys fault in through the lazy
        // disk source; budget mode owns freeing (no compute_liveness).
        std::string err;
        auto source = DiskPolySource::open(
            dir, idx.prog, idx.ring_dim,
            opts.cache_dir.empty() ? dir : opts.cache_dir, &err);
        if (!source) {
            std::cerr << "[fhetch_sim] " << err << std::endl;
            return false;
        }
        sim.set_poly_source(source);
        sim.set_memory_budget({opts.mem_budget_mb * 1024ULL * 1024ULL,
                               opts.spill_dir.empty() ? dir : opts.spill_dir});
    } else {
        if (!populate_inputs(dir, idx.prog, sim)) return false;
        sim.compute_liveness();
    }

    auto result = sim.run();
    if (result.errors > 0) {
        std::cerr << "[fhetch_sim] replay failed: " << result.errors << " errors" << std::endl;
        return false;
    }
    std::cout << "[fhetch_sim] replay complete: " << result.instructions_executed
              << " instructions, " << result.elapsed_seconds << "s" << std::endl;

    if (!write_replay_outputs(dir, outputs, sim)) return false;
    if (!register_crypto_context(dir)) return false;
    reconstruct_probes_in(dir);

    auto probes_dir = dir / "serialized_probes";
    if (!fs::exists(probes_dir) || fs::is_empty(probes_dir)) {
        std::cerr << "[fhetch_sim] reconstruction produced no probes in " << probes_dir
                  << std::endl;
        return false;
    }
    return true;
}

}  // namespace niobium
