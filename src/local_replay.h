// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// Disk-driven local FHETCH replay. Lets the standalone fhetch_sim binary run a
// replay purely from a project directory (no Compiler singleton state), so many
// replays can run as isolated subprocesses without racing OpenFHE's static
// transform caches.

#pragma once

#include <filesystem>
#include <string>

namespace niobium::fhetch {
struct DriveInputs;
}

namespace niobium {

// Drive the FHETCH simulator over the on-disk project at `project_dir` —
// trace, ring dimension, inputs/keys, outputs, and templates all read from
// disk — and write fhetch_replay_outputs.json + serialized_probes/<name>.ct
// into it, matching the in-process Compiler::replay() local path byte-for-byte.
// Returns true on a zero-error run that produced at least one probe.
bool run_local_replay_from_project(const std::filesystem::path& project_dir);

// Fill ciphertext_templates/<name>.template from fhetch_replay_outputs.json in
// `dir` and serialize the result to serialized_probes/<name>.ct. Disk-driven
// core shared by Compiler::reconstruct_probes() and the project worker.
void reconstruct_probes_in(const std::filesystem::path& dir);

}  // namespace niobium

namespace niobium::fhetch {

// Load the inputs (<prog>.inputs.json + .bin/.ids) and keys
// (<prog>.{mk,rk,bp}.bin/.ids) recorded under `dir` into `inputs`, keyed by the
// FHETCH address recorded in each .ids file.
bool load_source_inputs(const std::filesystem::path& dir, const std::string& prog,
                        DriveInputs& inputs);
bool load_source_keys(const std::filesystem::path& dir, const std::string& prog,
                      DriveInputs& inputs);

}  // namespace niobium::fhetch
