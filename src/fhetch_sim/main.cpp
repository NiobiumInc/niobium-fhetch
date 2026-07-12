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
    niobium::LocalReplayOptions replay_opts;

    // Env fallbacks let a spawning parent (Compiler::replay_project)
    // enable budget mode without new argv plumbing.
    if (const char* v = std::getenv("NIOBIUM_FHETCH_MEM_BUDGET_MB"); v && *v)
        replay_opts.mem_budget_mb = std::strtoull(v, nullptr, 10);
    if (const char* v = std::getenv("NIOBIUM_FHETCH_SPILL_DIR"); v && *v)
        replay_opts.spill_dir = v;
    if (const char* v = std::getenv("NIOBIUM_FHETCH_CACHE_DIR"); v && *v)
        replay_opts.cache_dir = v;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--project=", 0) == 0) {
            project_dir = arg.substr(std::strlen("--project="));
        } else if (arg == "--project" && i + 1 < argc) {
            project_dir = argv[++i];
        } else if (arg == "--ring-dim" && i + 1 < argc) {
            ring_dim = std::stoull(argv[++i]);
        } else if (arg.rfind("--mem-budget-mb=", 0) == 0) {
            replay_opts.mem_budget_mb =
                std::stoull(arg.substr(std::strlen("--mem-budget-mb=")));
        } else if (arg == "--mem-budget-mb" && i + 1 < argc) {
            replay_opts.mem_budget_mb = std::stoull(argv[++i]);
        } else if (arg.rfind("--spill-dir=", 0) == 0) {
            replay_opts.spill_dir = arg.substr(std::strlen("--spill-dir="));
        } else if (arg == "--spill-dir" && i + 1 < argc) {
            replay_opts.spill_dir = argv[++i];
        } else if (arg.rfind("--cache-dir=", 0) == 0) {
            replay_opts.cache_dir = arg.substr(std::strlen("--cache-dir="));
        } else if (arg == "--cache-dir" && i + 1 < argc) {
            replay_opts.cache_dir = argv[++i];
        } else if (trace_file.empty() && !arg.empty() && arg[0] != '-') {
            trace_file = arg;
        }
    }

    // --project: replay an on-disk project, isolated per process so concurrent
    // local replays don't share OpenFHE's static caches.
    if (!project_dir.empty())
        return niobium::run_local_replay_from_project(project_dir, replay_opts) ? 0 : 1;

    if (trace_file.empty()) {
        std::cerr << "Usage: " << argv[0]
                  << " <trace.fhetch> --ring-dim N | --project <dir>\n"
                  << "  budget mode (with --project): --mem-budget-mb N\n"
                  << "  [--spill-dir DIR] [--cache-dir DIR]  (default: project dir;\n"
                  << "  env: NIOBIUM_FHETCH_MEM_BUDGET_MB / _SPILL_DIR / _CACHE_DIR)"
                  << std::endl;
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
