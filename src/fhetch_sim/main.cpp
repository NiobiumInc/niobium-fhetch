// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// fhetch_sim — CLI for the FHETCH instruction-set simulator.
//
// Usage: fhetch_sim <trace.fhetch> --ring-dim <N>
//    or: fhetch_sim --project <dir>   (disk-driven replay of an on-disk project)

#include "local_replay.h"
#include "niobium/fhetch_sim/simulator.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    // --project=<dir> / --project <dir>: replay an on-disk project, isolated per
    // process so concurrent local replays don't share OpenFHE static caches.
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string dir;
        if (arg.rfind("--project=", 0) == 0) {
            dir = arg.substr(std::strlen("--project="));
        } else if (arg == "--project" && i + 1 < argc) {
            dir = argv[i + 1];
        } else {
            continue;
        }
        if (dir.empty()) {
            std::cerr << "[fhetch_sim] --project requires a directory" << std::endl;
            return 1;
        }
        return niobium::run_local_replay_from_project(dir) ? 0 : 1;
    }

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <trace.fhetch> [--ring-dim N]"
                  << std::endl;
        return 1;
    }

    std::string trace_file = argv[1];
    uint64_t ring_dim = 0;

    for (int i = 2; i < argc; i++) {
        if (std::strcmp(argv[i], "--ring-dim") == 0 && i + 1 < argc) {
            ring_dim = std::stoull(argv[++i]);
        }
    }

    std::cout << "=== FHETCH Simulator ===" << std::endl;

    niobium::fhetch_sim::Simulator sim;

    if (!sim.load_trace(trace_file)) {
        std::cerr << "Failed to load trace" << std::endl;
        return 1;
    }

    if (ring_dim == 0) {
        std::cerr << "Ring dimension required: --ring-dim N" << std::endl;
        return 1;
    }
    sim.set_ring_dimension(ring_dim);

    auto result = sim.run();

    if (result.errors > 0) {
        std::cerr << "[FAIL] " << result.errors << " errors" << std::endl;
        return 1;
    }

    std::cout << "[PASS] " << result.instructions_executed << " instructions, "
              << result.elapsed_seconds << "s" << std::endl;
    return 0;
}
