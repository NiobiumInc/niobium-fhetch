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
// Usage:
//   fhetch_driver <trace.fhetch> [--ring-dim N] [compiler-flags...]

#include "niobium/compiler.h"
#include "niobium/fhetch_parser.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
    // ---- Parse CLI ----
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <trace.fhetch> [--ring-dim N] [compiler-flags...]"
                  << std::endl;
        return 1;
    }

    fs::path trace_path = argv[1];
    uint64_t ring_dim = 2048;  // default; overridable via --ring-dim N

    // Sift our own flags out of argv before passing the rest to compiler().init().
    std::vector<char*> forwarded_argv;
    forwarded_argv.push_back(argv[0]);
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--ring-dim" && i + 1 < argc) {
            ring_dim = std::stoull(argv[++i]);
        } else {
            forwarded_argv.push_back(argv[i]);
        }
    }
    int fwd_argc = static_cast<int>(forwarded_argv.size());
    char** fwd_argv = forwarded_argv.data();

    if (!fs::exists(trace_path)) {
        std::cerr << "[fhetch_driver] no such file: " << trace_path << std::endl;
        return 1;
    }

    std::cout << "[fhetch_driver] trace:    " << trace_path << std::endl;
    std::cout << "[fhetch_driver] ring_dim: " << ring_dim << std::endl;

    // ---- Compiler session setup ----
    niobium::compiler().init(fwd_argc, fwd_argv);
    niobium::compiler().set_program_info(
        "fhetch_driver", "1.0",
        "Re-drive a .fhetch trace through the FHETCH API");
    niobium::compiler().set_build_info(__FILE__, __LINE__, __TIMESTAMP__);

    // Separate each secondary run by the input trace's stem so cache
    // directories don't collide when driving many traces.
    niobium::Compiler::CacheParameters cache_params;
    cache_params.push_back({"source", trace_path.stem().string()});
    niobium::compiler().cache_parameters(cache_params);

    // No OpenFHE CryptoContext here — set the ring dim manually so replay()
    // has enough to construct NativeInteger/NativeVector lanes.
    niobium::compiler().set_ring_dimension(ring_dim);

    // ---- Parse + drive ----
    niobium::compiler().start();

    niobium::fhetch::DriveStats stats;
    bool ok = niobium::fhetch::parse_and_drive(trace_path, ring_dim, stats);

    niobium::compiler().stop();

    std::cout << "[fhetch_driver] parsed:   " << stats.instructions_parsed << std::endl;
    std::cout << "[fhetch_driver] replayed: " << stats.instructions_replayed << std::endl;
    std::cout << "[fhetch_driver] moduli:   " << stats.modulus_table.size() << std::endl;
    if (stats.unknown_opcodes)
        std::cout << "[fhetch_driver] unknown opcodes: " << stats.unknown_opcodes << std::endl;
    if (stats.skipped_lines)
        std::cout << "[fhetch_driver] skipped lines:   " << stats.skipped_lines << std::endl;
    for (const auto& e : stats.errors) {
        std::cerr << "[fhetch_driver] " << e << std::endl;
    }

    if (!ok) {
        std::cerr << "[fhetch_driver] no instructions replayed" << std::endl;
        return 2;
    }

    // ---- Secondary replay through the simulator ----
    std::cout << "[fhetch_driver] running secondary replay..." << std::endl;
    if (!niobium::compiler().replay()) {
        std::cerr << "[fhetch_driver] replay failed" << std::endl;
        return 3;
    }

    std::cout << "[fhetch_driver] done." << std::endl;
    return 0;
}
