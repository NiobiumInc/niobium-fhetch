// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// Tests for the exit status of run_local_replay_from_project (fhetch_sim
// --project): a project whose outputs have no ciphertext template has no probe
// to reconstruct and succeeds with the outputs file written; a project whose
// only template is refused fails, whatever else sits in serialized_probes/.

#include "local_replay.h"

#include "openfhe.h"
#include "ciphertext-ser.h"
#include "cryptocontext-ser.h"
#include "key/key-ser.h"
#include "scheme/ckksrns/ckksrns-ser.h"

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kRingDim = 16;
constexpr uint32_t kMultiplicativeDepth = 1;
constexpr uint32_t kScalingModSize = 50;
// The towers of the ciphertext template: two elements of three towers each.
constexpr size_t kTemplateTowers = 6;
// Exit statuses of the child process that runs a replay under a file-size limit.
constexpr int kChildSucceeded = 0;
constexpr int kChildFailed = 1;
constexpr int kChildSetupFailed = 2;
constexpr const char* kProgram = "replay";
constexpr const char* kOutputName = "out";

int g_failures = 0;

void expect(bool cond, const std::string& what) {
    if (!cond) {
        std::cerr << "FAIL: " << what << std::endl;
        ++g_failures;
    }
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    f << text;
}

// The CKKS context every case registers, PKE only. OpenFHE returns the same
// registered context for equal parameters.
lbcrypto::CryptoContext<lbcrypto::DCRTPoly> make_crypto_context() {
    lbcrypto::CCParams<lbcrypto::CryptoContextCKKSRNS> params;
    params.SetSecurityLevel(lbcrypto::HEStd_NotSet);
    params.SetRingDim(kRingDim);
    params.SetMultiplicativeDepth(kMultiplicativeDepth);
    params.SetScalingModSize(kScalingModSize);
    auto cc = lbcrypto::GenCryptoContext(params);
    cc->Enable(lbcrypto::PKE);
    return cc;
}

// A project with no live-in inputs: the zero-init `sr_mulps %i, %i, 0` writes
// each of the output's `address_count` addresses, and a real CKKS
// cryptocontext.dat. Modulus index 0 is the table's COPY_MODULUS sentinel, so a
// filled template keeps its own tower moduli.
fs::path make_project(const std::string& name, size_t address_count = 1) {
    const fs::path dir =
        fs::temp_directory_path() / ("local_replay_" + name + "_" + std::to_string(getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::string trace =
        "modulus_count 2\n"
        "m[0] 0xFFFFFFFFFFFFFFFF\n"
        "m[1] 0x11\n"
        "# Instructions\n";
    std::string addresses;
    for (size_t i = 0; i < address_count; ++i) {
        const std::string address = std::to_string(i);
        trace += "sr_mulps %" + address + ", %" + address + ", 0, m=0\n";
        addresses += (i == 0 ? "" : ", ") + address;
    }
    write_text(dir / (std::string(kProgram) + ".fhetch"), trace + "halt\n");
    write_text(dir / "fhetch_replay.json",
               "{\"crypto_context\": {\"ring_dimension\": " + std::to_string(kRingDim) +
                   "}, \"files\": {\"instructions\": \"" + kProgram + ".fhetch\"}}");
    write_text(dir / (std::string(kProgram) + ".outputs.json"),
               std::string("{\"outputs\": [{\"name\": \"") + kOutputName +
                   "\", \"ciphertext_data\": [{\"poly_index\": 0, \"elements\": [" + addresses +
                   "]}]}]}");

    const auto cc = make_crypto_context();
    const fs::path cc_path = dir / "cryptocontext.dat";
    if (!lbcrypto::Serial::SerializeToFile(cc_path.string(), cc, lbcrypto::SerType::BINARY)) {
        std::cerr << "FAIL: could not write " << cc_path << std::endl;
        ++g_failures;
    }
    return dir;
}

// No ciphertext template for any output: nothing to reconstruct, so the run
// succeeds, the outputs file is complete and serialized_probes/ stays empty.
void test_no_templates_succeeds() {
    const fs::path dir = make_project("no_templates");
    const bool ok = niobium::run_local_replay_from_project(dir);
    expect(ok, "a project with no ciphertext template succeeds");
    expect(fs::exists(dir / "fhetch_replay_outputs.json"),
           "the outputs file is written when there is nothing to reconstruct");
    const fs::path probes = dir / "serialized_probes";
    expect(!fs::exists(probes) || fs::is_empty(probes),
           "no probe is written without a template");
    fs::remove_all(dir);
}

// Writes the output's ciphertext template, a ciphertext of `kTemplateTowers`
// towers: reconstruct_probes_in fills it when the replay supplies that many
// output elements and refuses it otherwise.
void write_template(const fs::path& dir) {
    const auto cc = make_crypto_context();
    const auto keys = cc->KeyGen();
    const auto ct = cc->Encrypt(
        keys.publicKey, cc->MakeCKKSPackedPlaintext(std::vector<double>{1.0}));
    fs::create_directories(dir / "ciphertext_templates");
    const fs::path template_path =
        dir / "ciphertext_templates" / (std::string(kOutputName) + ".template");
    if (!lbcrypto::Serial::SerializeToFile(template_path.string(), ct, lbcrypto::SerType::BINARY)) {
        std::cerr << "FAIL: could not write " << template_path << std::endl;
        ++g_failures;
    }
}

// A project whose only template is refused writes no probe, so the run fails
// with the outputs file still written.
void test_refused_probe_fails() {
    const fs::path dir = make_project("refused_probe");
    write_template(dir);
    const bool ok = niobium::run_local_replay_from_project(dir);
    expect(!ok, "a project whose only template is refused fails the run");
    expect(fs::exists(dir / "fhetch_replay_outputs.json"),
           "the outputs file is still written when the probe fails");
    fs::remove_all(dir);
}

// A file left in serialized_probes/ by an earlier run is not a probe this run
// wrote: the refused template still fails the run.
void test_stale_probe_does_not_count() {
    const fs::path dir = make_project("stale_probe");
    write_template(dir);
    fs::create_directories(dir / "serialized_probes");
    write_text(dir / "serialized_probes" / "stale.ct", "left by an earlier run");
    const bool ok = niobium::run_local_replay_from_project(dir);
    expect(!ok, "a stale file in serialized_probes/ does not make a refused template succeed");
    fs::remove_all(dir);
}

// The replay supplies one element per template tower, so the template is
// filled and its probe written: the run succeeds with exactly that probe.
void test_filled_template_writes_probe() {
    const fs::path dir = make_project("filled_probe", kTemplateTowers);
    write_template(dir);
    const bool ok = niobium::run_local_replay_from_project(dir);
    expect(ok, "a project whose template the replay fills succeeds");
    const fs::path probe = dir / "serialized_probes" / (std::string(kOutputName) + ".ct");
    expect(fs::exists(probe), "the filled template's probe is written");
    expect(std::distance(fs::directory_iterator(dir / "serialized_probes"),
                         fs::directory_iterator()) == 1,
           "exactly one probe is written");
    fs::remove_all(dir);
}

// The outputs file cannot be read back when its write failed (a full disk
// truncates it). A child with a zero file-size limit makes that write fail
// without a device file; the run then fails instead of reporting that there
// was nothing to reconstruct.
void test_unreadable_outputs_file_fails() {
    const fs::path dir = make_project("unreadable_outputs");
    std::cout.flush();
    const pid_t child = fork();
    if (child == 0) {
        std::signal(SIGXFSZ, SIG_IGN);
        const rlimit no_files_grow = {0, 0};
        if (setrlimit(RLIMIT_FSIZE, &no_files_grow) != 0) _exit(kChildSetupFailed);
        _exit(niobium::run_local_replay_from_project(dir) ? kChildSucceeded : kChildFailed);
    }
    int status = 0;
    expect(child > 0 && waitpid(child, &status, 0) == child, "the child runs and is reaped");
    expect(WIFEXITED(status) && WEXITSTATUS(status) == kChildFailed,
           "a run whose outputs file cannot be read back fails");
    fs::remove_all(dir);
}

}  // namespace

int main() {
    test_no_templates_succeeds();
    test_refused_probe_fails();
    test_stale_probe_does_not_count();
    test_filled_template_writes_probe();
    test_unreadable_outputs_file_fails();

    if (g_failures) {
        std::cerr << g_failures << " local_replay test failure(s)" << std::endl;
        return 1;
    }
    std::cout << "local_replay: all checks passed" << std::endl;
    return 0;
}
