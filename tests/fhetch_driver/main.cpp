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
#include "local_replay.h"
#include "niobium/compiler.h"
#include "niobium/fhetch_parser.h"

#include "ciphertext-ser.h"
#include "cryptocontext-ser.h"
#include "key/key-ser.h"
#include "scheme/ckksrns/ckksrns-ser.h"

#include <nlohmann/json.hpp>

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

// The input/key loaders (read_ids, extract_dcrt_towers, load_input_bin,
// load_key_bin, load_bp_bin, load_source_inputs, load_source_keys) live in
// src/local_replay.h so the project worker and this test share one copy.

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
        if (!niobium::fhetch::load_source_inputs(source_dir, program_name, inputs)) return 2;
        if (!niobium::fhetch::load_source_keys(source_dir, program_name, inputs))   return 2;
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
