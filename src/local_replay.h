// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// Disk-driven local FHETCH replay: run a replay purely from a project directory
// (no Compiler singleton state) so many replays can run as isolated subprocesses
// without racing OpenFHE's process-global transform caches.

#pragma once

#include "niobium/fhetch_parser.h"  // DriveInputs

#include <filesystem>
#include <string>

namespace niobium {

// Drive the simulator over the on-disk project at `dir` and write
// fhetch_replay_outputs.json + serialized_probes/<name>.ct into it. Returns
// true on a zero-error run that produced at least one probe.
bool run_local_replay_from_project(const std::filesystem::path& dir);

// Fill ciphertext_templates/<name>.template from fhetch_replay_outputs.json in
// `dir` and serialize to serialized_probes/<name>.ct (defined in auto_facade.cpp).
void reconstruct_probes_in(const std::filesystem::path& dir);

namespace fhetch {

// Load the inputs (<prog>.inputs.json + .bin/.ids) and keys
// (<prog>.{mk,rk,bp}.bin/.ids) recorded under `dir` into `inputs`, keyed by the
// FHETCH address recorded in each .ids file.
bool load_source_inputs(const std::filesystem::path& dir, const std::string& prog,
                        DriveInputs& inputs);
bool load_source_keys(const std::filesystem::path& dir, const std::string& prog,
                      DriveInputs& inputs);

}  // namespace fhetch
}  // namespace niobium
