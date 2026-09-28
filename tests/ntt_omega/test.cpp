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
 * @brief Tests for caller-chosen NTT roots.
 *
 * sr_ntt_with_root, sr_intt_with_root, mr_ntt_with_roots and
 * mr_intt_with_roots record the primitive 2N-th root of unity psi their
 * transform uses. Every recorded root here is
 * psi' = g^((q-1)/2N), g the smallest quadratic non-residue mod q: a valid
 * root that is not the minimal one a consumer picks when none is recorded, so
 * a consumer that ignores the recorded root computes a different transform.
 *
 * Checked: the API records and validates the root, both serializations carry
 * it, the fhetch parser re-records it or fails at its line, and the
 * simulator's transforms match the definition for the same root, as
 * OpenFHE's do.
 *
 * OpenFHE caches twiddle tables per modulus, process-wide, and ignores the
 * root it is handed once a table for that modulus exists. Every test that
 * reads or relies on that cache resets it first.
 */

#include "compiler_internal.h"
#include "niobium/compiler.h"
#include "niobium/fhetch_api.h"
#include "niobium/fhetch_ir.h"
#include "niobium/fhetch_parser.h"
#include "niobium/fhetch_reader.h"
#include "niobium/fhetch_sim/simulator.h"

#include "math/math-hal.h"
#include "math/nbtheory.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace fhetch = niobium::fhetch;
using fhetch::Instruction;
using fhetch::MRP;
using fhetch::Polynomial;
using fhetch::Program;
using lbcrypto::NativeInteger;
using lbcrypto::NativeVector;
using niobium::fhetch_sim::Simulator;

namespace {

// The rooted transforms have names of their own, so each rootless one stays
// a single function: an `auto` template argument, or taking its address, as
// niobium-haze does, deduces only from a single function, never an overload
// set.
template <auto Fn>
constexpr bool names_one_function = true;
static_assert(names_one_function<&fhetch::sr_ntt> && names_one_function<&fhetch::sr_intt> &&
              names_one_function<&fhetch::mr_ntt> && names_one_function<&fhetch::mr_intt>);

int g_checks_failed = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cout << "  FAIL " << __FILE__ << ":" << __LINE__ << "  "      \
                      << #cond << std::endl;                                   \
            ++g_checks_failed;                                                 \
        }                                                                      \
    } while (0)

constexpr uint64_t kRingDim = 1024;

// 42-bit primes with q = 1 (mod 2N).
constexpr uint64_t kQ1 = 0x3FFFFE80001ULL;
constexpr uint64_t kQ2 = 0x40000560001ULL;
constexpr uint64_t kQ3 = 0x40000003001ULL;

constexpr uint64_t kSeedA = 1;
constexpr uint64_t kSeedX1 = 2;
constexpr uint64_t kSeedX2 = 3;
constexpr uint64_t kSeedReference = 4;

uint64_t mul_mod(uint64_t a, uint64_t b, uint64_t q) {
    return static_cast<uint64_t>(static_cast<__uint128_t>(a) * b % q);
}

uint64_t pow_mod(uint64_t base, uint64_t exp, uint64_t q) {
    uint64_t r = 1;
    base %= q;
    for (; exp > 0; exp >>= 1U) {
        if ((exp & 1U) != 0U) r = mul_mod(r, base, q);
        base = mul_mod(base, base, q);
    }
    return r;
}

/// (smallest quadratic non-residue mod q)^((q-1)/2N). Its N-th power is
/// g^((q-1)/2) = -1 by Euler's criterion, so it is a primitive 2N-th root.
uint64_t neutral_root(uint64_t q) {
    uint64_t g = 2;
    while (pow_mod(g, (q - 1) / 2, q) != q - 1) ++g;
    return pow_mod(g, (q - 1) / (2 * kRingDim), q);
}

/// The root a consumer uses when none is recorded, for ring dimension n.
uint64_t minimal_root(uint64_t q, uint64_t n) {
    return lbcrypto::RootOfUnity<NativeInteger>(2 * n, NativeInteger(q)).ConvertToInt();
}

uint64_t reverse_bits(uint64_t x, unsigned bits) {
    uint64_t r = 0;
    for (unsigned i = 0; i < bits; ++i) r |= ((x >> i) & 1U) << (bits - 1 - i);
    return r;
}

/// The negacyclic NTT from its definition, in OpenFHE's bit-reversed slot
/// order: slot j holds a(psi^(2*brv(j) + 1)) mod q, with N = a.size() and brv
/// reversing log2(N) bits. O(N^2), and shares no code with any transform
/// under test.
std::vector<uint64_t> ntt_by_definition(const std::vector<uint64_t>& a,
                                        uint64_t q, uint64_t psi) {
    const uint64_t n = a.size();
    unsigned log_n = 0;
    while ((uint64_t{1} << log_n) < n) ++log_n;
    std::vector<uint64_t> out(n);
    for (uint64_t j = 0; j < n; ++j) {
        const uint64_t point = pow_mod(psi, (2 * reverse_bits(j, log_n)) + 1, q);
        uint64_t acc = 0;
        uint64_t x = 1;
        for (uint64_t c : a) {
            acc = (acc + mul_mod(c, x, q)) % q;
            x = mul_mod(x, point, q);
        }
        out[j] = acc;
    }
    return out;
}

NativeVector to_native(const std::vector<uint64_t>& v, uint64_t q) {
    NativeVector n(static_cast<uint32_t>(v.size()), NativeInteger(q));
    for (size_t i = 0; i < v.size(); ++i) n[i] = NativeInteger(v[i]);
    return n;
}

std::vector<uint64_t> from_native(const NativeVector& n) {
    std::vector<uint64_t> v(n.GetLength());
    for (size_t i = 0; i < v.size(); ++i) v[i] = n[i].ConvertToInt();
    return v;
}

/// Empty OpenFHE's process-wide, per-modulus twiddle cache, so the next
/// transform mod q builds its table from the root it is handed.
void reset_openfhe_ntt_tables() {
    ChineseRemainderTransformFTT<NativeVector>().Reset();
}

std::vector<uint64_t> openfhe_ntt(const std::vector<uint64_t>& a, uint64_t q,
                                  uint64_t psi) {
    NativeVector out(kRingDim, NativeInteger(q));
    ChineseRemainderTransformFTT<NativeVector>().ForwardTransformToBitReverse(
        to_native(a, q), NativeInteger(psi), 2 * kRingDim, &out);
    return from_native(out);
}

std::vector<uint64_t> openfhe_intt(const std::vector<uint64_t>& a, uint64_t q,
                                   uint64_t psi) {
    NativeVector out(kRingDim, NativeInteger(q));
    ChineseRemainderTransformFTT<NativeVector>().InverseTransformFromBitReverse(
        to_native(a, q), NativeInteger(psi), 2 * kRingDim, &out);
    return from_native(out);
}

/// n deterministic coefficients in [0, q).
std::vector<uint64_t> sample(uint64_t q, uint64_t seed, uint64_t n) {
    std::vector<uint64_t> v(n);
    uint64_t s = seed;
    for (auto& c : v) {
        s = (s * 6364136223846793005ULL) + 1442695040888963407ULL;
        c = (s >> 11U) % q;
    }
    return v;
}

uint64_t address(const Polynomial& p) {
    return niobium::detail::polynomial_address(p);
}

std::filesystem::path scratch_root() {
    static const auto root = std::filesystem::temp_directory_path() /
                             ("ntt_omega_test_" + std::to_string(getpid()));
    return root;
}

std::filesystem::path scratch_dir(const std::string& name) {
    auto dir = scratch_root() / name;
    std::filesystem::create_directories(dir);
    return dir;
}

/// Start a recording session whose artifacts land in their own directory.
void begin_session(const std::string& name) {
    niobium::compiler().set_program_info(name, "1.0", "caller-chosen NTT roots");
    niobium::compiler().set_program_directory(scratch_dir(name));
    niobium::compiler().start();
}

/// Stop the session; return its trace path without an extension.
std::filesystem::path end_session() {
    niobium::compiler().stop();
    return niobium::compiler().get_program_directory() /
           niobium::compiler().program_name();
}

Program read_program(const std::filesystem::path& path) {
    auto r = fhetch::read_fhetch_file(path);
    CHECK(r.ok);
    return r.program;
}

const Instruction* written_to(const Program& p, uint64_t dest) {
    for (const auto& inst : p.instructions)
        if (inst.dest == dest) return &inst;
    return nullptr;
}

/// dest's instruction is `opcode` mod `q`, carrying exactly `omega`.
bool records(const Program& p, uint64_t dest, fhetch::FhOpcode opcode,
             uint64_t q, std::optional<uint64_t> omega) {
    const Instruction* inst = written_to(p, dest);
    return inst != nullptr && inst->opcode == opcode && inst->modulus.has_value() &&
           p.modulus_table.at(*inst->modulus) == q && inst->omega == omega;
}

std::filesystem::path write_trace(const std::string& name, const std::string& text) {
    auto path = scratch_dir(name) / (name + ".fhetch");
    std::ofstream(path) << text;
    return path;
}

std::string one_modulus_table(uint64_t q) {
    return "modulus_count 2\n"
           "m[0] 0xFFFFFFFFFFFFFFFF\n"
           "m[1] " + std::to_string(q) + "\n";
}

// ---------------------------------------------------------------------------

/// Everything record_transforms() recorded, for the tests that inspect and
/// replay it.
struct Recording {
    std::filesystem::path trace;  ///< without extension; .fhetch and .fhex exist
    std::vector<uint64_t> a, x1, x2;
    uint64_t psi1 = 0, psi2 = 0;
    uint64_t a_addr = 0, x1_addr = 0, x2_addr = 0;
    uint64_t ntt_default = 0, ntt = 0, back = 0, intt = 0;
    uint64_t mr_ntt1 = 0, mr_ntt2 = 0, mr_back1 = 0, mr_back2 = 0;
};

bool test_neutral_root_is_not_minimal() {
    std::cout << "\n[test] (smallest QNR)^((q-1)/2N) is a root, not the minimal one"
              << std::endl;
    const int before = g_checks_failed;
    reset_openfhe_ntt_tables();

    for (uint64_t q : {kQ1, kQ2, kQ3}) {
        const uint64_t psi = neutral_root(q);
        const uint64_t minimal = minimal_root(q, kRingDim);
        std::cout << "  q=" << q << "  psi'=" << psi << "  minimal=" << minimal
                  << std::endl;
        CHECK(pow_mod(psi, kRingDim, q) == q - 1);
        CHECK(psi != minimal);
    }

    // The oracle's slot order is OpenFHE's, and OpenFHE's reference really
    // used psi': its cache held no table for either prime.
    for (uint64_t q : {kQ1, kQ2}) {
        const auto v = sample(q, kSeedReference, kRingDim);
        const auto reference = openfhe_ntt(v, q, neutral_root(q));
        CHECK(reference == ntt_by_definition(v, q, neutral_root(q)));
        CHECK(reference != ntt_by_definition(v, q, minimal_root(q, kRingDim)));
        CHECK(openfhe_intt(reference, q, neutral_root(q)) == v);
    }
    return g_checks_failed == before;
}

Recording record_transforms() {
    Recording r;
    r.psi1 = neutral_root(kQ1);
    r.psi2 = neutral_root(kQ2);
    r.a = sample(kQ1, kSeedA, kRingDim);
    r.x1 = sample(kQ1, kSeedX1, kRingDim);
    r.x2 = sample(kQ2, kSeedX2, kRingDim);

    niobium::detail::trace_writer().set_trace_format(niobium::TraceFormat::Both);
    begin_session("transforms");
    const auto a = Polynomial::from_data(r.a, kRingDim, fhetch::Format::Coefficient);
    const auto x = MRP::from_pairs(
        {{Polynomial::from_data(r.x1, kRingDim, fhetch::Format::Coefficient), kQ1},
         {Polynomial::from_data(r.x2, kRingDim, fhetch::Format::Coefficient), kQ2}});

    const auto ntt_default = fhetch::sr_ntt(a, kQ1);
    const auto ntt = fhetch::sr_ntt_with_root(a, kQ1, r.psi1);
    const auto back = fhetch::sr_intt_with_root(ntt, kQ1, r.psi1);
    const auto intt = fhetch::sr_intt_with_root(a, kQ1, r.psi1);
    const auto mr_ntt = fhetch::mr_ntt_with_roots(x, {r.psi1, r.psi2});
    const auto mr_back = fhetch::mr_intt_with_roots(mr_ntt, {r.psi1, r.psi2});
    r.trace = end_session();
    niobium::detail::trace_writer().set_trace_format(niobium::TraceFormat::Text);

    r.a_addr = address(a);
    r.x1_addr = address(x[kQ1]);
    r.x2_addr = address(x[kQ2]);
    r.ntt_default = address(ntt_default);
    r.ntt = address(ntt);
    r.back = address(back);
    r.intt = address(intt);
    r.mr_ntt1 = address(mr_ntt[kQ1]);
    r.mr_ntt2 = address(mr_ntt[kQ2]);
    r.mr_back1 = address(mr_back[kQ1]);
    r.mr_back2 = address(mr_back[kQ2]);
    return r;
}

bool test_trace_carries_roots(const Recording& r) {
    std::cout << "\n[test] recorded roots survive both serializations" << std::endl;
    const int before = g_checks_failed;

    const Program text = read_program(r.trace.string() + ".fhetch");
    const Program binary = read_program(r.trace.string() + ".fhex");
    CHECK(text.instructions == binary.instructions);
    CHECK(text.modulus_table == binary.modulus_table);

    for (const Program* p : {&text, &binary}) {
        CHECK(records(*p, r.ntt_default, fhetch::FH_SR_NTT, kQ1, std::nullopt));
        CHECK(records(*p, r.ntt, fhetch::FH_SR_NTT, kQ1, r.psi1));
        CHECK(records(*p, r.back, fhetch::FH_SR_INTT, kQ1, r.psi1));
        CHECK(records(*p, r.intt, fhetch::FH_SR_INTT, kQ1, r.psi1));
        // One root per limb, each beside its own prime.
        CHECK(records(*p, r.mr_ntt1, fhetch::FH_SR_NTT, kQ1, r.psi1));
        CHECK(records(*p, r.mr_ntt2, fhetch::FH_SR_NTT, kQ2, r.psi2));
        CHECK(records(*p, r.mr_back1, fhetch::FH_SR_INTT, kQ1, r.psi1));
        CHECK(records(*p, r.mr_back2, fhetch::FH_SR_INTT, kQ2, r.psi2));
    }
    return g_checks_failed == before;
}

bool test_simulator_uses_recorded_roots(const Recording& r) {
    std::cout << "\n[test] the simulator transforms with the recorded root"
              << std::endl;
    const int before = g_checks_failed;

    // Hold psi1's table for kQ1 in OpenFHE's cache, so a simulator that went
    // through it would transform the rootless NTT with psi1 too.
    reset_openfhe_ntt_tables();
    openfhe_ntt(r.a, kQ1, r.psi1);

    Simulator sim;
    sim.set_ring_dimension(kRingDim);
    CHECK(sim.load_trace(r.trace.string() + ".fhetch"));
    sim.store_polynomial(r.a_addr, r.a, kQ1);
    sim.store_polynomial(r.x1_addr, r.x1, kQ1);
    sim.store_polynomial(r.x2_addr, r.x2, kQ2);
    CHECK(sim.run().errors == 0);

    CHECK(sim.get_polynomial(r.ntt) == ntt_by_definition(r.a, kQ1, r.psi1));
    // The INTT inverts the definition's NTT for the same forward root.
    CHECK(ntt_by_definition(sim.get_polynomial(r.intt), kQ1, r.psi1) == r.a);
    CHECK(sim.get_polynomial(r.back) == r.a);
    CHECK(sim.get_polynomial(r.ntt_default) ==
          ntt_by_definition(r.a, kQ1, minimal_root(kQ1, kRingDim)));

    CHECK(sim.get_polynomial(r.mr_ntt1) == ntt_by_definition(r.x1, kQ1, r.psi1));
    CHECK(sim.get_polynomial(r.mr_ntt2) == ntt_by_definition(r.x2, kQ2, r.psi2));
    CHECK(sim.get_polynomial(r.mr_back1) == r.x1);
    CHECK(sim.get_polynomial(r.mr_back2) == r.x2);
    return g_checks_failed == before;
}

/// `record` throws std::invalid_argument and leaves the trace untouched.
void check_rejected(const std::string& what, const std::function<void()>& record) {
    const size_t recorded = niobium::detail::trace_writer().instruction_count();
    bool rejected = false;
    try {
        record();
    } catch (const std::invalid_argument& e) {
        rejected = true;
        std::cout << "  " << what << ": " << e.what() << std::endl;
    }
    if (!rejected) std::cout << "  " << what << ": not rejected" << std::endl;
    CHECK(rejected);
    CHECK(niobium::detail::trace_writer().instruction_count() == recorded);
}

bool test_invalid_roots_are_rejected() {
    std::cout << "\n[test] the API rejects anything but a primitive 2N-th root"
              << std::endl;
    const int before = g_checks_failed;

    const uint64_t psi1 = neutral_root(kQ1);
    const uint64_t psi2 = neutral_root(kQ2);
    begin_session("invalid");
    const auto a = Polynomial::from_data(sample(kQ1, kSeedA, kRingDim), kRingDim,
                                         fhetch::Format::Coefficient);
    const auto x = MRP::from_pairs(
        {{Polynomial::from_data(sample(kQ1, kSeedX1, kRingDim), kRingDim), kQ1},
         {Polynomial::from_data(sample(kQ2, kSeedX2, kRingDim), kRingDim), kQ2}});

    check_rejected("sr_ntt_with_root omega 0", [&] { fhetch::sr_ntt_with_root(a, kQ1, 0); });
    check_rejected("sr_intt_with_root omega 0", [&] { fhetch::sr_intt_with_root(a, kQ1, 0); });
    // psi^2 has order N, so its N-th power is 1 rather than -1.
    check_rejected("sr_ntt_with_root primitive N-th root",
                   [&] { fhetch::sr_ntt_with_root(a, kQ1, mul_mod(psi1, psi1, kQ1)); });
    check_rejected("sr_intt_with_root another prime's root",
                   [&] { fhetch::sr_intt_with_root(a, kQ1, psi2); });
    // psi1 + q is psi1 mod q, but a root has one spelling: [1, q).
    check_rejected("sr_ntt_with_root root plus q",
                   [&] { fhetch::sr_ntt_with_root(a, kQ1, psi1 + kQ1); });
    check_rejected("mr_ntt_with_roots too few roots",
                   [&] { fhetch::mr_ntt_with_roots(x, {psi1}); });
    check_rejected("mr_intt_with_roots too many roots",
                   [&] { fhetch::mr_intt_with_roots(x, {psi1, psi2, psi1}); });
    check_rejected("mr_ntt_with_roots roots on the wrong limbs",
                   [&] { fhetch::mr_ntt_with_roots(x, {psi2, psi1}); });
    // The first limb is valid: nothing may be recorded before the second fails.
    check_rejected("mr_intt_with_roots second root 0",
                   [&] { fhetch::mr_intt_with_roots(x, {psi1, 0}); });
    end_session();
    return g_checks_failed == before;
}

bool test_simulator_rejects_invalid_omega() {
    std::cout << "\n[test] the simulator reports an invalid omega instead of deriving a root"
              << std::endl;
    const int before = g_checks_failed;

    const auto path = write_trace("invalid_omega",
                                  one_modulus_table(kQ1) +
                                      "sr_ntt %1, %0, m=1, omega=0\n"
                                      "sr_intt %2, %0, m=1, omega=0\n"
                                      "sr_ntt %3, %0, m=1, omega=" +
                                      std::to_string(neutral_root(kQ1) + kQ1) + "\nhalt\n");
    Simulator sim;
    sim.set_ring_dimension(kRingDim);
    CHECK(sim.load_trace(path));
    sim.store_polynomial(0, sample(kQ1, kSeedA, kRingDim), kQ1);
    CHECK(sim.run().errors == 3);
    return g_checks_failed == before;
}

bool test_ring_dimension_change_rebuilds_tables() {
    std::cout << "\n[test] set_ring_dimension drops tables built for the old N"
              << std::endl;
    const int before = g_checks_failed;

    constexpr uint64_t kLargerRingDim = 2 * kRingDim;
    const auto path = write_trace("resize", one_modulus_table(kQ1) +
                                                "sr_ntt %1, %0, m=1\n"
                                                "sr_ntt %2, %0, m=1, omega=" +
                                                std::to_string(neutral_root(kQ1)) +
                                                "\nhalt\n");
    Simulator sim;
    sim.set_ring_dimension(kRingDim);
    CHECK(sim.load_trace(path));
    sim.store_polynomial(0, sample(kQ1, kSeedA, kRingDim), kQ1);
    CHECK(sim.run().errors == 0);

    // Both instructions now have tables for N = 1024. At 2048 the rootless
    // one needs the minimal root for 2048, and psi' (order 2048) is no root.
    sim.set_ring_dimension(kLargerRingDim);
    const auto v = sample(kQ1, kSeedA, kLargerRingDim);
    sim.store_polynomial(0, v, kQ1);
    CHECK(sim.run().errors == 1);
    CHECK(sim.get_polynomial(1) ==
          ntt_by_definition(v, kQ1, minimal_root(kQ1, kLargerRingDim)));
    return g_checks_failed == before;
}

bool test_simulator_leaves_openfhe_tables_alone() {
    std::cout << "\n[test] a recorded root does not leak into OpenFHE's tables"
              << std::endl;
    const int before = g_checks_failed;
    reset_openfhe_ntt_tables();

    const uint64_t psi3 = neutral_root(kQ3);
    const auto v = sample(kQ3, kSeedA, kRingDim);
    const auto path = write_trace("isolation", one_modulus_table(kQ3) +
                                                   "sr_ntt %1, %0, m=1, omega=" +
                                                   std::to_string(psi3) + "\nhalt\n");
    const auto replay = [&] {
        Simulator sim;
        sim.set_ring_dimension(kRingDim);
        CHECK(sim.load_trace(path));
        sim.store_polynomial(0, v, kQ3);
        CHECK(sim.run().errors == 0);
        return sim.get_polynomial(1);
    };

    CHECK(replay() == ntt_by_definition(v, kQ3, psi3));
    // A host transforming mod kQ3 afterwards gets the root it asked for...
    CHECK(openfhe_ntt(v, kQ3, minimal_root(kQ3, kRingDim)) ==
          ntt_by_definition(v, kQ3, minimal_root(kQ3, kRingDim)));
    // ...and the simulator still gets its own once OpenFHE holds the other.
    CHECK(replay() == ntt_by_definition(v, kQ3, psi3));
    return g_checks_failed == before;
}

bool test_driver_rerecords_roots() {
    std::cout << "\n[test] re-driving a trace re-records its roots" << std::endl;
    const int before = g_checks_failed;

    const std::string psi1 = std::to_string(neutral_root(kQ1));
    begin_session("driver");
    fhetch::DriveStats stats;
    CHECK(fhetch::parse_and_drive_text(one_modulus_table(kQ1) +
                                           "sr_ntt %1, %0, m=1, omega=" + psi1 + "\n"
                                           "sr_intt %2, %1, m=1, omega=" + psi1 + "\n"
                                           "sr_ntt %3, %0, m=1\n",
                                       kRingDim, stats));
    const Program p = read_program(end_session().string() + ".fhetch");

    std::vector<std::optional<uint64_t>> roots;
    for (const auto& inst : p.instructions)
        if (inst.opcode == fhetch::FH_SR_NTT || inst.opcode == fhetch::FH_SR_INTT)
            roots.push_back(inst.omega);
    CHECK((roots == std::vector<std::optional<uint64_t>>{
               neutral_root(kQ1), neutral_root(kQ1), std::nullopt}));
    return g_checks_failed == before;
}

bool test_driver_rejects_invalid_roots() {
    std::cout << "\n[test] re-driving an invalid root fails, naming its line"
              << std::endl;
    const int before = g_checks_failed;

    begin_session("driver_invalid");
    const size_t recorded = niobium::detail::trace_writer().instruction_count();
    fhetch::DriveStats stats;
    std::string message;
    try {
        fhetch::parse_and_drive_text(one_modulus_table(kQ1) +
                                         "sr_ntt %1, %0, m=1, omega=" +
                                         std::to_string(neutral_root(kQ1)) + "\n"
                                         "sr_ntt %2, %0, m=1, omega=0\n"
                                         "sr_ntt %3, %0, m=1\n",
                                     kRingDim, stats);
    } catch (const std::invalid_argument& e) {
        message = e.what();
    }
    std::cout << "  " << (message.empty() ? "not rejected" : message) << std::endl;
    CHECK(message.rfind("line 5: sr_ntt_with_root: omega 0 ", 0) == 0);
    // Line 4 is already in the session; line 6 was never reached.
    CHECK(niobium::detail::trace_writer().instruction_count() == recorded + 1);
    end_session();
    return g_checks_failed == before;
}

}  // namespace

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Caller-chosen NTT roots" << std::endl;
    std::cout << "========================================" << std::endl;

    std::string program = "ntt_omega_test";
    std::string no_ring_dim_check = "--no-ring-dim-check";
    std::vector<char*> args = {program.data(), no_ring_dim_check.data(), nullptr};
    int argc = 2;
    niobium::compiler().init(argc, args.data());
    niobium::compiler().set_ring_dimension(kRingDim);

    int passed = 0;
    int failed = 0;

    if (test_neutral_root_is_not_minimal()) passed++; else failed++;
    const Recording recording = record_transforms();
    if (test_trace_carries_roots(recording)) passed++; else failed++;
    if (test_simulator_uses_recorded_roots(recording)) passed++; else failed++;
    if (test_invalid_roots_are_rejected()) passed++; else failed++;
    if (test_simulator_rejects_invalid_omega()) passed++; else failed++;
    if (test_ring_dimension_change_rebuilds_tables()) passed++; else failed++;
    if (test_simulator_leaves_openfhe_tables_alone()) passed++; else failed++;
    if (test_driver_rerecords_roots()) passed++; else failed++;
    if (test_driver_rejects_invalid_roots()) passed++; else failed++;

    std::filesystem::remove_all(scratch_root());

    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << passed << " passed, " << failed << " failed"
              << std::endl;
    std::cout << "========================================" << std::endl;

    return failed > 0 ? 1 : 0;
}
