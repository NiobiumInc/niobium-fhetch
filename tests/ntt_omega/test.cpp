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
 * @brief Tests for the simulator honouring a recorded NTT root.
 *
 * The FHETCH IR's `omega` field names the primitive 2N-th root of unity psi
 * a negacyclic transform uses. The simulator now honours a present root
 * unconditionally instead of reading 0 as absent, keeps its own twiddle
 * tables keyed by (modulus, recorded root) instead of going through
 * OpenFHE's process-wide, modulus-keyed cache, and drops those tables when
 * the ring dimension changes.
 *
 * Every recorded root here is psi' = g^((q-1)/2N), g the smallest quadratic
 * non-residue mod q: a valid root that is not the minimal one a consumer
 * derives when none is recorded, so a consumer that ignores the recorded
 * root computes a different transform. Traces are hand-written .fhetch
 * text, since the recording API cannot yet set omega.
 *
 * OpenFHE caches twiddle tables per modulus, process-wide, and ignores the
 * root it is handed once a table for that modulus exists. Every test that
 * reads or relies on that cache resets it first.
 */

#include "niobium/fhetch_sim/simulator.h"

#include "math/math-hal.h"
#include "math/nbtheory.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

using lbcrypto::NativeInteger;
using lbcrypto::NativeVector;
using niobium::fhetch_sim::Simulator;

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

constexpr uint64_t kRingDim = 1024;

// 42-bit primes with q = 1 (mod 2N).
constexpr uint64_t kQ1 = 0x3FFFFE80001ULL;
constexpr uint64_t kQ2 = 0x40000560001ULL;
constexpr uint64_t kQ3 = 0x40000003001ULL;

constexpr uint64_t kSeedA = 1;
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

}  // namespace

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Caller-chosen NTT roots" << std::endl;
    std::cout << "========================================" << std::endl;

    int passed = 0;
    int failed = 0;

    if (test_neutral_root_is_not_minimal()) passed++; else failed++;
    if (test_simulator_rejects_invalid_omega()) passed++; else failed++;
    if (test_ring_dimension_change_rebuilds_tables()) passed++; else failed++;
    if (test_simulator_leaves_openfhe_tables_alone()) passed++; else failed++;

    std::filesystem::remove_all(scratch_root());

    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << passed << " passed, " << failed << " failed"
              << std::endl;
    std::cout << "========================================" << std::endl;

    return failed > 0 ? 1 : 0;
}
