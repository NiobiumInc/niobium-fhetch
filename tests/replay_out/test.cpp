// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

/**
 * @file test.cpp
 * @brief Tests for replay_project's --out argument, replay_output_directory()
 * and replay_run_directory().
 *
 * For a non-local target, replay_project() forwards to nbcc_fhetch_replay
 * with --out=<replay_output_directory(dir)>, so the recording and run
 * directories nbcc_fhetch_replay would otherwise create in its spawned CWD
 * (nbcc_fhetch_replay_source_<project basename>/ and its sibling .run
 * directory) land inside the project directory instead. Concurrent replays of
 * distinct projects that inherit the same CWD would otherwise collide there.
 *
 * NBCC_FHETCH_REPLAY / NBCC_FHETCH_SIM point replay_project at a fake worker
 * script (not the real compiler binary) that records its argv to disk and
 * exits 0, so these tests exercise only argv construction, not an actual
 * replay.
 */

#include "niobium/compiler.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int g_checks_failed = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cout << "  FAIL " << __FILE__ << ":" << __LINE__ << "  "      \
                      << #cond << std::endl;                                   \
            ++g_checks_failed;                                                 \
        }                                                                      \
    } while (0)

std::filesystem::path scratch_root() {
    static const auto root = std::filesystem::temp_directory_path() /
                             ("replay_out_test_" + std::to_string(getpid()));
    return root;
}

std::filesystem::path scratch_dir(const std::string& name) {
    auto dir = scratch_root() / name;
    std::filesystem::create_directories(dir);
    return dir;
}

/// Write an executable fake worker script to `path`: it records its argv
/// (one arg per line) to `record_file`, then creates
/// `<--project value>/serialized_probes/dummy.ct` so replay_project() reports
/// success. Exits 0 unconditionally.
void write_fake_worker(const std::filesystem::path& path,
                       const std::filesystem::path& record_file) {
    std::ofstream f(path);
    f << "#!/usr/bin/env bash\n"
      << ": > \"" << record_file.string() << "\"\n"
      << "for a in \"$@\"; do printf '%s\\n' \"$a\" >> \"" << record_file.string()
      << "\"; done\n"
      << "proj=\"\"\n"
      << "for a in \"$@\"; do case \"$a\" in --project=*) proj=\"${a#--project=}\" ;; esac; done\n"
      << "mkdir -p \"$proj/serialized_probes\"\n"
      << ": > \"$proj/serialized_probes/dummy.ct\"\n"
      << "exit 0\n";
    f.close();
    std::filesystem::permissions(path, std::filesystem::perms::owner_all |
                                           std::filesystem::perms::group_read |
                                           std::filesystem::perms::others_read);
}

/// Read back an argv recording written by write_fake_worker's script.
std::vector<std::string> read_argv(const std::filesystem::path& record_file) {
    std::vector<std::string> args;
    std::ifstream f(record_file);
    std::string line;
    while (std::getline(f, line)) args.push_back(line);
    return args;
}

bool contains(const std::vector<std::string>& args, const std::string& arg) {
    for (const auto& a : args)
        if (a == arg) return true;
    return false;
}

bool any_starts_with(const std::vector<std::string>& args, const std::string& prefix) {
    for (const auto& a : args)
        if (a.rfind(prefix, 0) == 0) return true;
    return false;
}

bool test_out_flag_scoped_to_project_dir() {
    std::cout << "\n[test] non-local replay_project passes --out=replay_output_directory(dir)"
              << std::endl;
    const int before = g_checks_failed;

    auto dir = scratch_dir("remote_target");
    auto record_file = scratch_dir("remote_target_record") / "argv.txt";
    auto worker = scratch_dir("remote_target_bin") / "fake_nbcc_fhetch_replay";
    write_fake_worker(worker, record_file);

    setenv("NBCC_FHETCH_REPLAY", worker.c_str(), 1);
    bool ok = niobium::compiler().replay_project("FUNC_SIM", dir, "O0", false);
    unsetenv("NBCC_FHETCH_REPLAY");

    CHECK(ok);
    auto args = read_argv(record_file);
    CHECK(contains(args, "--project=" + dir.string()));
    CHECK(contains(args, "--target=FUNC_SIM"));
    CHECK(contains(args, "--out=" + niobium::replay_output_directory(dir).string()));

    return g_checks_failed == before;
}

/// A trailing directory separator on `dir` is dropped from --project, so
/// nbcc_fhetch_replay names its run directory from the project's basename,
/// as replay_run_directory() does.
bool test_project_flag_drops_trailing_separator() {
    std::cout << "\n[test] --project carries no trailing directory separator"
              << std::endl;
    const int before = g_checks_failed;

    auto dir = scratch_dir("slashed_target");
    std::filesystem::path with_slash = dir.string() + "/";
    auto record_file = scratch_dir("slashed_target_record") / "argv.txt";
    auto worker = scratch_dir("slashed_target_bin") / "fake_nbcc_fhetch_replay";
    write_fake_worker(worker, record_file);

    setenv("NBCC_FHETCH_REPLAY", worker.c_str(), 1);
    bool ok = niobium::compiler().replay_project("FUNC_SIM", with_slash, "O0", false);
    unsetenv("NBCC_FHETCH_REPLAY");

    CHECK(ok);
    auto args = read_argv(record_file);
    CHECK(contains(args, "--project=" + dir.string()));
    CHECK(contains(args, "--out=" + niobium::replay_output_directory(dir).string()));

    return g_checks_failed == before;
}

bool test_out_flag_present_alongside_hw_and_opt_level() {
    std::cout << "\n[test] --out coexists with --niobium_hw and --opt-level"
              << std::endl;
    const int before = g_checks_failed;

    auto dir = scratch_dir("hw_opt_target");
    auto record_file = scratch_dir("hw_opt_target_record") / "argv.txt";
    auto worker = scratch_dir("hw_opt_target_bin") / "fake_nbcc_fhetch_replay";
    write_fake_worker(worker, record_file);

    setenv("NBCC_FHETCH_REPLAY", worker.c_str(), 1);
    bool ok = niobium::compiler().replay_project("FUNC_SIM", dir, "O3", true);
    unsetenv("NBCC_FHETCH_REPLAY");

    CHECK(ok);
    auto args = read_argv(record_file);
    CHECK(contains(args, "--out=" + niobium::replay_output_directory(dir).string()));
    CHECK(contains(args, "--niobium_hw"));
    CHECK(contains(args, "--opt-level=O3"));

    return g_checks_failed == before;
}

bool test_out_flag_absent_for_local_target() {
    std::cout << "\n[test] target \"local\" (fhetch_sim) gets no --out"
              << std::endl;
    const int before = g_checks_failed;

    auto dir = scratch_dir("local_target");
    auto record_file = scratch_dir("local_target_record") / "argv.txt";
    auto worker = scratch_dir("local_target_bin") / "fake_fhetch_sim";
    write_fake_worker(worker, record_file);

    setenv("NBCC_FHETCH_SIM", worker.c_str(), 1);
    bool ok = niobium::compiler().replay_project("local", dir, "O0", false);
    unsetenv("NBCC_FHETCH_SIM");

    CHECK(ok);
    auto args = read_argv(record_file);
    CHECK(contains(args, "--project=" + dir.string()));
    CHECK(!any_starts_with(args, "--out="));

    return g_checks_failed == before;
}

/// Two distinct projects must derive distinct --out directories: this is
/// what keeps the compiler's own out-dir-basename-derived names (the
/// NB_TIMING_SUMMARY_DIR mirror filename, the FHETCH_SIM lift subdirectory)
/// from colliding across projects.
bool test_replay_output_directory_is_unique_per_project() {
    std::cout << "\n[test] replay_output_directory differs across projects"
              << std::endl;
    const int before = g_checks_failed;

    auto a = niobium::replay_output_directory(scratch_dir("proj_a"));
    auto b = niobium::replay_output_directory(scratch_dir("proj_b"));
    CHECK(a != b);
    CHECK(a.string().find("proj_a") != std::string::npos);
    CHECK(b.string().find("proj_b") != std::string::npos);
    // Nested inside the project, not a sibling of it.
    CHECK(a.parent_path() == scratch_dir("proj_a"));

    return g_checks_failed == before;
}

/// A trailing directory separator must not degrade the derived name to
/// "nbcc_fhetch_replay_source_" (path::filename() is "" for e.g. "/a/b/").
bool test_replay_output_directory_tolerates_trailing_slash() {
    std::cout << "\n[test] replay_output_directory tolerates a trailing slash"
              << std::endl;
    const int before = g_checks_failed;

    auto dir = scratch_dir("trailing_slash_case");
    std::filesystem::path with_slash = dir.string() + "/";

    auto plain = niobium::replay_output_directory(dir);
    auto slashed = niobium::replay_output_directory(with_slash);
    CHECK(plain.filename() == slashed.filename());
    CHECK(slashed.filename().string().rfind("nbcc_fhetch_replay_source_", 0) == 0);
    CHECK(slashed.filename() != "nbcc_fhetch_replay_source_");

    return g_checks_failed == before;
}

/// A relative project path derives the same basename-suffixed name as its
/// absolute form.
bool test_replay_output_directory_relative_path() {
    std::cout << "\n[test] replay_output_directory on a relative path"
              << std::endl;
    const int before = g_checks_failed;

    std::filesystem::path rel = "relative_project_dir";
    auto out = niobium::replay_output_directory(rel);
    CHECK(out == rel / "nbcc_fhetch_replay_source_relative_project_dir");
    CHECK(!out.is_absolute());

    return g_checks_failed == before;
}

/// A project name containing dots (a realistic workload/version-qualified
/// name) is carried through verbatim, not truncated at the first dot.
bool test_replay_output_directory_name_with_dots() {
    std::cout << "\n[test] replay_output_directory on a name with dots"
              << std::endl;
    const int before = g_checks_failed;

    auto dir = scratch_dir("workload.v1.2");
    auto out = niobium::replay_output_directory(dir);
    CHECK(out.filename() == "nbcc_fhetch_replay_source_workload.v1.2");

    return g_checks_failed == before;
}

/// The run directory is the recording's `.run` sibling, named like the
/// recording, inside the project directory.
bool test_replay_run_directory_is_the_recordings_run_dir() {
    std::cout << "\n[test] replay_run_directory is <recording>.run/<recording name>"
              << std::endl;
    const int before = g_checks_failed;

    auto dir = scratch_dir("run_dir_case");
    auto recording = niobium::replay_output_directory(dir);
    auto run = niobium::replay_run_directory(dir);
    CHECK(run == std::filesystem::path(recording.string() + ".run") /
                     "nbcc_fhetch_replay_source_run_dir_case");
    CHECK(run.parent_path().parent_path() == dir);
    CHECK(niobium::replay_run_directory(scratch_dir("run_dir_other")) != run);

    return g_checks_failed == before;
}

/// A trailing directory separator gives the same run directory.
bool test_replay_run_directory_tolerates_trailing_slash() {
    std::cout << "\n[test] replay_run_directory tolerates a trailing slash"
              << std::endl;
    const int before = g_checks_failed;

    auto dir = scratch_dir("run_dir_slash_case");
    std::filesystem::path with_slash = dir.string() + "/";
    CHECK(niobium::replay_run_directory(with_slash).filename() ==
          niobium::replay_run_directory(dir).filename());
    CHECK(niobium::replay_run_directory(with_slash).filename() ==
          "nbcc_fhetch_replay_source_run_dir_slash_case");

    return g_checks_failed == before;
}

}  // namespace

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "replay_project --out scoping" << std::endl;
    std::cout << "========================================" << std::endl;

    std::string program = "replay_out_test";
    std::string no_ring_dim_check = "--no-ring-dim-check";
    std::vector<char*> args = {program.data(), no_ring_dim_check.data(), nullptr};
    int argc = 2;
    niobium::compiler().init(argc, args.data());

    int passed = 0;
    int failed = 0;

    if (test_out_flag_scoped_to_project_dir()) passed++; else failed++;
    if (test_project_flag_drops_trailing_separator()) passed++; else failed++;
    if (test_out_flag_present_alongside_hw_and_opt_level()) passed++; else failed++;
    if (test_out_flag_absent_for_local_target()) passed++; else failed++;
    if (test_replay_output_directory_is_unique_per_project()) passed++; else failed++;
    if (test_replay_output_directory_tolerates_trailing_slash()) passed++; else failed++;
    if (test_replay_output_directory_relative_path()) passed++; else failed++;
    if (test_replay_output_directory_name_with_dots()) passed++; else failed++;
    if (test_replay_run_directory_is_the_recordings_run_dir()) passed++; else failed++;
    if (test_replay_run_directory_tolerates_trailing_slash()) passed++; else failed++;

    std::filesystem::remove_all(scratch_root());

    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << passed << " passed, " << failed << " failed"
              << std::endl;
    std::cout << "========================================" << std::endl;

    return failed > 0 ? 1 : 0;
}
