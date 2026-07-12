// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// Disk-driven local FHETCH replay: run a replay purely from a project directory
// (no Compiler singleton state) so many replays can run as isolated subprocesses
// without racing OpenFHE's process-global transform caches.

#pragma once

#include "niobium/fhetch_parser.h"  // DriveInputs

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace niobium {

// Bounded-memory options for run_local_replay_from_project.
struct LocalReplayOptions {
    // 0 = unbounded (eager input load, liveness frees only). Nonzero
    // bounds resident polynomial memory: inputs/keys fault in lazily
    // from the project's .bin files (keys via a one-time flat sidecar),
    // evicted computed values spill to a per-run file, and the eviction
    // policy is exact Belady over the trace. Results are bit-identical.
    uint64_t mem_budget_mb = 0;
    std::filesystem::path spill_dir;  // empty = project dir (keep it on local disk)
    std::filesystem::path cache_dir;  // key sidecars; empty = project dir
};

// Drive the simulator over the on-disk project at `dir` and write
// fhetch_replay_outputs.json + serialized_probes/<name>.ct into it. Returns
// true on a zero-error run that produced at least one probe.
bool run_local_replay_from_project(const std::filesystem::path& dir);
bool run_local_replay_from_project(const std::filesystem::path& dir,
                                   const LocalReplayOptions& opts);

// Fill ciphertext_templates/<name>.template from fhetch_replay_outputs.json in
// `dir` and serialize to serialized_probes/<name>.ct (defined in auto_facade.cpp).
void reconstruct_probes_in(const std::filesystem::path& dir);

namespace fhetch {

// Receives one polynomial tower as it is decoded from a .bin file. The
// vector is the callee's to keep (loaders donate the buffer).
using PolySink =
    std::function<void(uint64_t addr, std::vector<uint64_t>&& values, uint64_t modulus)>;

// Load the inputs (<prog>.inputs.json + .bin/.ids) and keys
// (<prog>.{mk,rk,bp}.bin/.ids) recorded under `dir`, keyed by the FHETCH
// address recorded in each .ids file. The sink forms stream each tower as
// it is decoded (one DCRTPoly resident at a time); the DriveInputs forms
// are wrappers that collect everything into `inputs.data`.
bool load_source_inputs(const std::filesystem::path& dir, const std::string& prog,
                        const PolySink& sink);
bool load_source_keys(const std::filesystem::path& dir, const std::string& prog,
                      const PolySink& sink);
bool load_source_inputs(const std::filesystem::path& dir, const std::string& prog,
                        DriveInputs& inputs);
bool load_source_keys(const std::filesystem::path& dir, const std::string& prog,
                      DriveInputs& inputs);

// Per-file loaders (one .bin against its .ids address list), shared by
// the eager path above and the lazy DiskPolySource. All stream: one
// DCRTPoly resident at a time, each tower delivered through the sink.
std::vector<uint64_t> read_ids(const std::filesystem::path& ids_path);
bool load_input_bin(const std::filesystem::path& bin_path,
                    const std::vector<uint64_t>& ids, const PolySink& sink);
bool load_key_bin(const std::filesystem::path& bin_path,
                  const std::vector<uint64_t>& ids, const PolySink& sink);
bool load_bp_bin(const std::filesystem::path& bin_path,
                 const std::vector<uint64_t>& ids, const PolySink& sink);

}  // namespace fhetch
}  // namespace niobium
