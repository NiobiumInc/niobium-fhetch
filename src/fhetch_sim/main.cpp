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
    std::string trace_file;
    std::string project_dir;
    uint64_t ring_dim = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--project=", 0) == 0) {
            project_dir = arg.substr(std::strlen("--project="));
        } else if (arg == "--project" && i + 1 < argc) {
            project_dir = argv[++i];
        } else if (arg == "--ring-dim" && i + 1 < argc) {
            ring_dim = std::stoull(argv[++i]);
        } else if (trace_file.empty() && !arg.empty() && arg[0] != '-') {
            trace_file = arg;
        }
    }

    // --project: replay an on-disk project, isolated per process so concurrent
    // local replays don't share OpenFHE's static caches.
    if (!project_dir.empty())
        return niobium::run_local_replay_from_project(project_dir) ? 0 : 1;

    if (trace_file.empty()) {
        std::cerr << "Usage: " << argv[0]
                  << " <trace.fhetch> --ring-dim N | --project <dir>" << std::endl;
        return 1;
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
