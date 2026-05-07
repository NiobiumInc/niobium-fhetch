// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0.
//
// fhetch_driver — test harness that reads a .fhetch trace and re-drives it
// through the FHETCH Polynomial IR API, producing a secondary trace. The
// secondary trace is replayed through the bundled simulator to verify that
// every opcode in the input trace round-trips cleanly through the library's
// recording path.
//
// Two modes:
//
//   Sanity mode  (no --source-dir):
//     fhetch_driver <trace.fhetch> [--ring-dim N]
//     Drives the trace with zero-initialized live-in inputs. Useful to
//     confirm every opcode parses and the library's recording path
//     accepts it.
//
//   Roundtrip mode (with --source-dir):
//     fhetch_driver <trace.fhetch>
//         --ring-dim N
//         --source-dir <path-to-primary-workload-dir>
//         --cc <path-to-cc.bin>
//         --output-ct NAME:PATH ...
//     Loads the primary workload's captured inputs (.input_*.bin + .ids),
//     deserializes them as DCRTPolys via OpenFHE cereal, and populates the
//     driver's input map so the secondary replay sees real values. Reads
//     the primary's outputs.json to tag output probes. After replay,
//     calls Compiler::result(cc, name, ct) for each --output-ct NAME:PATH
//     pair and serializes the reconstructed ciphertext to PATH. A decrypt
//     binary can then verify the secondary ct matches the expected value.

#include "openfhe.h"
#include "niobium/compiler.h"
#include "niobium/fhetch_parser.h"

#include "ciphertext-ser.h"
#include "cryptocontext-ser.h"
#include "key/key-ser.h"
#include "scheme/ckksrns/ckksrns-ser.h"

#include <nlohmann/json.hpp>
#include <cereal/archives/portable_binary.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace lbcrypto;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Read a .ids file written by niobium::cereal_io::write_addr_ids.
// Layout: [uint64_t count][uint64_t id_0][uint64_t id_1]...
static std::vector<uint64_t> read_ids(const fs::path& p) {
    std::vector<uint64_t> v;
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return v;
    uint64_t n = 0;
    f.read(reinterpret_cast<char*>(&n), sizeof(n));
    v.resize(n);
    if (n) f.read(reinterpret_cast<char*>(v.data()), n * sizeof(uint64_t));
    return v;
}

// Extract per-tower (addr, values, modulus) triples from a DCRTPoly.
// The .ids file provides the FHETCH address for each tower in order.
static void extract_dcrt_towers(
    const DCRTPoly& dcrt,
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

// Read a .input_NAME.bin file written by auto_facade's tag_input. Layout is:
//   uint32_t instances_count
//   uint8_t  payload_type  (1 = ciphertext)
//   uint32_t num_dcrt_polys
//   DCRTPoly * num_dcrt_polys   (cereal-serialized)
static bool load_input_bin(const fs::path& bin_path,
                           const std::vector<uint64_t>& ids,
                           niobium::fhetch::DriveInputs& inputs) {
    std::ifstream f(bin_path, std::ios::binary);
    if (!f.is_open()) return false;
    try {
        cereal::PortableBinaryInputArchive ar(f);
        uint32_t instances = 0;
        uint8_t  payload_type = 0;
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
        std::cerr << "[fhetch_driver] failed to read " << bin_path
                  << ": " << e.what() << std::endl;
        return false;
    }
    return true;
}

// Read a key .mk.bin / .rk.bin file written by auto_facade's
// serialize_eval_keys. Layout is:
//   uint32_t num_keys
//   for each key:
//     uint32_t av_size;  DCRTPoly * av_size
//     uint32_t bv_size;  DCRTPoly * bv_size
// The .ids file lists FHETCH addresses flattened across all keys' (A, B)
// DCRTPoly towers, in the same order as serialization.
static bool load_key_bin(const fs::path& bin_path,
                         const std::vector<uint64_t>& ids,
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
        std::cerr << "[fhetch_driver] failed to read " << bin_path
                  << ": " << e.what() << std::endl;
        return false;
    }
    return true;
}

// Read a .bp.bin file written by auto_facade's tag_bootstrap_precompute.
// Layout (matches the writer in src/auto_facade.cpp):
//   uint32_t num_boot_entries
//   for each entry:
//     uint32_t m_slots
//     uint32_t m_dim1
//     2 x (9 x uint32_t)   // m_paramsEnc, m_paramsDec
//     2 x 2D-DCRTPoly      // m_U0hatTPreFFT, m_U0PreFFT
//     2 x 1D-DCRTPoly      // m_U0Pre, m_U0hatTPre
// 2D layout: uint32 outer_size; per inner: uint32 inner_size, DCRTPoly...
// 1D layout: uint32 size; DCRTPoly...
static bool load_bp_bin(const fs::path& bin_path,
                        const std::vector<uint64_t>& ids,
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
                uint32_t lvlb, layersCollapse, remCollapse, numRotations,
                         b, g, numRotationsRem, bRem, gRem;
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
        std::cerr << "[fhetch_driver] failed to read " << bin_path
                  << ": " << e.what() << std::endl;
        return false;
    }
    return true;
}

// Load all inputs listed in source_dir/<program>.inputs.json, reading the
// companion .bin/.ids pairs. Populates the DriveInputs map keyed by the
// FHETCH addresses recorded in the .ids files (i.e. the same addresses that
// appear in the .fhetch trace).
static bool load_source_inputs(const fs::path& source_dir,
                               const std::string& program_name,
                               niobium::fhetch::DriveInputs& inputs) {
    fs::path idx_path = source_dir / (program_name + ".inputs.json");
    if (!fs::exists(idx_path)) return true;  // nothing to load is OK

    nlohmann::json j;
    try {
        std::ifstream in(idx_path);
        in >> j;
    } catch (...) {
        std::cerr << "[fhetch_driver] failed to parse " << idx_path << std::endl;
        return false;
    }

    if (!j.contains("inputs")) return true;
    for (const auto& entry : j["inputs"]) {
        std::string bin = entry.value("bin_file", "");
        std::string ids = entry.value("ids_file", "");
        if (bin.empty() || ids.empty()) continue;
        fs::path bin_path = fs::path(bin).is_absolute() ? fs::path(bin) : source_dir / bin;
        fs::path ids_path = fs::path(ids).is_absolute() ? fs::path(ids) : source_dir / ids;
        if (!fs::exists(bin_path) || !fs::exists(ids_path)) continue;

        auto addr_ids = read_ids(ids_path);
        if (!load_input_bin(bin_path, addr_ids, inputs)) return false;
    }
    return true;
}

// Load keys (mk.bin, rk.bin, bp.bin) if present. mk/rk use the eval-key
// layout (load_key_bin); bp uses a different precompute layout (load_bp_bin).
static bool load_source_keys(const fs::path& source_dir,
                             const std::string& program_name,
                             niobium::fhetch::DriveInputs& inputs) {
    auto load_with = [&](const std::string& base, auto&& reader) {
        fs::path bin = source_dir / (program_name + "." + base + ".bin");
        fs::path ids = source_dir / (program_name + "." + base + ".ids");
        if (!fs::exists(bin) || !fs::exists(ids)) return;
        auto addr_ids = read_ids(ids);
        reader(bin, addr_ids, inputs);
    };
    load_with("mk", load_key_bin);
    load_with("rk", load_key_bin);
    load_with("bp", load_bp_bin);
    return true;
}

// Parse source_dir/<program>.outputs.json into a DriveOutputs map.
// The JSON lists each output with its payload_type and a ciphertext_data
// array; each element has poly_index + elements=[addr_id].
static bool load_source_outputs(const fs::path& source_dir,
                                const std::string& program_name,
                                niobium::fhetch::DriveOutputs& outputs,
                                std::vector<std::string>& output_names) {
    fs::path p = source_dir / (program_name + ".outputs.json");
    if (!fs::exists(p)) return true;

    nlohmann::json j;
    try {
        std::ifstream in(p);
        in >> j;
    } catch (...) {
        std::cerr << "[fhetch_driver] failed to parse " << p << std::endl;
        return false;
    }

    if (!j.contains("outputs")) return true;
    for (const auto& out : j["outputs"]) {
        std::string name = out.value("name", "");
        if (name.empty()) continue;
        output_names.push_back(name);
        if (out.contains("ciphertext_data")) {
            for (const auto& poly : out["ciphertext_data"]) {
                size_t idx = poly.value("poly_index", size_t{0});
                if (!poly.contains("elements")) continue;
                for (const auto& e : poly["elements"]) {
                    outputs.map[e.get<uint64_t>()] = { name, idx };
                }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr
            << "Usage: " << argv[0] << " <trace.fhetch>\n"
            << "           [--ring-dim N]\n"
            << "           [--source-dir DIR]   # primary workload dir\n"
            << "           [--cc FILE]          # crypto context (.bin)\n"
            << "           [--output-ct NAME:PATH]  # serialize result CT\n"
            << std::endl;
        return 1;
    }

    fs::path trace_path = argv[1];
    uint64_t ring_dim = 2048;
    fs::path source_dir;
    fs::path cc_path;
    std::vector<std::pair<std::string, fs::path>> output_ct_targets;

    std::vector<char*> forwarded_argv;
    forwarded_argv.push_back(argv[0]);

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--ring-dim" && i + 1 < argc) {
            ring_dim = std::stoull(argv[++i]);
        } else if (arg == "--source-dir" && i + 1 < argc) {
            source_dir = argv[++i];
        } else if (arg == "--cc" && i + 1 < argc) {
            cc_path = argv[++i];
        } else if (arg == "--output-ct" && i + 1 < argc) {
            std::string spec = argv[++i];
            size_t colon = spec.find(':');
            if (colon == std::string::npos) {
                std::cerr << "[fhetch_driver] --output-ct expects NAME:PATH" << std::endl;
                return 1;
            }
            output_ct_targets.emplace_back(spec.substr(0, colon),
                                           spec.substr(colon + 1));
        } else {
            forwarded_argv.push_back(argv[i]);
        }
    }

    if (!fs::exists(trace_path)) {
        std::cerr << "[fhetch_driver] no such file: " << trace_path << std::endl;
        return 1;
    }

    std::cout << "[fhetch_driver] trace:      " << trace_path << std::endl;
    std::cout << "[fhetch_driver] ring_dim:   " << ring_dim << std::endl;
    if (!source_dir.empty())
        std::cout << "[fhetch_driver] source:     " << source_dir << std::endl;
    if (!cc_path.empty())
        std::cout << "[fhetch_driver] cc:         " << cc_path << std::endl;

    // ---- Load primary-workload metadata (if --source-dir given) ----
    niobium::fhetch::DriveInputs inputs;
    niobium::fhetch::DriveOutputs outputs;
    std::vector<std::string> output_names;

    if (!source_dir.empty()) {
        // Program name is the trace's stem.
        std::string program_name = trace_path.stem().string();
        if (!load_source_inputs(source_dir, program_name, inputs)) return 2;
        if (!load_source_keys(source_dir, program_name, inputs))   return 2;
        if (!load_source_outputs(source_dir, program_name, outputs, output_names))
            return 2;
        std::cout << "[fhetch_driver] loaded " << inputs.data.size()
                  << " input addrs, " << outputs.map.size()
                  << " output addrs, " << output_names.size()
                  << " named outputs" << std::endl;
    }

    // ---- Compiler session ----
    int fwd_argc = static_cast<int>(forwarded_argv.size());
    char** fwd_argv = forwarded_argv.data();
    niobium::compiler().init(fwd_argc, fwd_argv);
    niobium::compiler().set_program_info(
        "fhetch_driver", "1.0",
        "Re-drive a .fhetch trace through the FHETCH API");
    niobium::compiler().set_build_info(__FILE__, __LINE__, __TIMESTAMP__);

    niobium::Compiler::CacheParameters cache_params;
    cache_params.push_back({"source", trace_path.stem().string()});
    niobium::compiler().cache_parameters(cache_params);
    niobium::compiler().set_ring_dimension(ring_dim);

    // ---- Load CryptoContext (needed to reconstruct ciphertexts later) ----
    CryptoContext<DCRTPoly> cc;
    if (!cc_path.empty()) {
        if (!Serial::DeserializeFromFile(cc_path.string(), cc, SerType::BINARY)) {
            std::cerr << "[fhetch_driver] failed to load CC from " << cc_path << std::endl;
            return 2;
        }
        std::cout << "[fhetch_driver] loaded crypto context (ring dim "
                  << cc->GetRingDimension() << ")" << std::endl;
    }

    // ---- Parse + drive ----
    niobium::compiler().start();

    niobium::fhetch::DriveStats stats;
    bool ok = niobium::fhetch::parse_and_drive(
        trace_path, ring_dim, stats, inputs, outputs);

    niobium::compiler().stop();

    std::cout << "[fhetch_driver] parsed:             " << stats.instructions_parsed << std::endl;
    std::cout << "[fhetch_driver] replayed:           " << stats.instructions_replayed << std::endl;
    std::cout << "[fhetch_driver] moduli:             " << stats.modulus_table.size() << std::endl;
    std::cout << "[fhetch_driver] inputs materialized:" << stats.inputs_materialized << std::endl;
    std::cout << "[fhetch_driver] outputs tagged:     " << stats.outputs_tagged << std::endl;
    if (stats.unknown_opcodes)
        std::cout << "[fhetch_driver] unknown opcodes:    " << stats.unknown_opcodes << std::endl;
    if (stats.skipped_lines)
        std::cout << "[fhetch_driver] skipped lines:      " << stats.skipped_lines << std::endl;
    for (const auto& e : stats.errors)
        std::cerr << "[fhetch_driver] " << e << std::endl;

    if (!ok) {
        std::cerr << "[fhetch_driver] no instructions replayed" << std::endl;
        return 3;
    }

    // ---- Copy ciphertext templates so Compiler::reconstruct_probes() can
    //      fill them with simulator-computed values during replay ----
    if (!source_dir.empty()) {
        fs::path src_tpl = source_dir / "ciphertext_templates";
        fs::path dst_tpl = niobium::compiler().get_program_directory() / "ciphertext_templates";
        if (fs::exists(src_tpl)) {
            fs::create_directories(dst_tpl);
            for (const auto& entry : fs::directory_iterator(src_tpl)) {
                fs::copy_file(entry.path(), dst_tpl / entry.path().filename(),
                              fs::copy_options::overwrite_existing);
            }
        }
    }

    // ---- Secondary simulator replay ----
    std::cout << "[fhetch_driver] running secondary replay..." << std::endl;
    if (!niobium::compiler().replay()) {
        std::cerr << "[fhetch_driver] replay failed" << std::endl;
        return 4;
    }

    // ---- Reconstruct + serialize requested output ciphertexts ----
    if (!output_ct_targets.empty()) {
        if (!cc) {
            std::cerr << "[fhetch_driver] --output-ct requires --cc" << std::endl;
            return 5;
        }
        for (const auto& [name, path] : output_ct_targets) {
            Ciphertext<DCRTPoly> ct;
            if (!niobium::compiler().result(cc, name, ct)) {
                std::cerr << "[fhetch_driver] could not reconstruct output '"
                          << name << "'" << std::endl;
                return 6;
            }
            if (!Serial::SerializeToFile(path.string(), ct, SerType::BINARY)) {
                std::cerr << "[fhetch_driver] failed to serialize "
                          << path << std::endl;
                return 7;
            }
            std::cout << "[fhetch_driver] wrote " << path << std::endl;
        }
    }

    std::cout << "[fhetch_driver] done." << std::endl;
    return 0;
}
