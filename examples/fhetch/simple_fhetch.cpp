// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// simple_fhetch.cpp
//
// Minimal example using the FHETCH Polynomial IR API directly (no OpenFHE
// crypto context in the user code). Demonstrates:
//   1. Setting up the Niobium compiler with cache support.
//   2. Tagging named polynomial inputs and outputs.
//   3. Recording an instruction sequence: single-residue (add, mul, scalar
//      mul, negate, NTT/INTT), multi-residue (add, mul, NTT/INTT), and an
//      MRPA dot product.
//   4. Running the bundled FHETCH simulator via Compiler::replay().
//
// Tagged inputs / outputs feed through to the simulator: addresses covered
// by fhetch::tag_input get populated with the tagged polynomial's data (or
// zeros for MRP residues constructed via MRP(base, N)), and addresses covered
// by fhetch::tag_output show up under "outputs" in fhetch_replay_outputs.json.
//

#include "niobium/fhetch_api.h"
#include "niobium/compiler.h"

#include <iostream>
#include <vector>

namespace fhetch = niobium::fhetch;
using fhetch::Polynomial;
using fhetch::Scalar;
using fhetch::MRP;
using fhetch::MRS;
using fhetch::MRPArray;
using fhetch::ModuliBase;

int main(int argc, char* argv[]) {
    niobium::compiler().init(argc, argv);
    niobium::compiler().set_program_info("simple_fhetch", "1.0",
        "FHETCH Polynomial IR example — no OpenFHE");
    niobium::compiler().set_build_info(__FILE__, __LINE__, __TIMESTAMP__);

    niobium::Compiler::CacheParameters params;
    params.push_back({"example", "simple"});
    niobium::compiler().cache_parameters(params);

    // ---- Parameters ----
    // N = 2048 is a common CKKS ring dimension. The two primes are
    // ≡ 1 (mod 2N = 4096) and fit under OpenFHE's 60-bit NativeInteger limit,
    // so the simulator can actually execute NTTs on them.
    constexpr uint64_t N  = 2048;
    constexpr uint64_t q1 = 0x3FFFFE80001ULL;   // 42-bit, 2N-friendly
    constexpr uint64_t q2 = 0x40000560001ULL;   // 42-bit, 2N-friendly

    // No OpenFHE CryptoContext here, so set the ring dimension explicitly
    // for the simulator's benefit.
    niobium::compiler().set_ring_dimension(N);

    std::cout << "=== FHETCH Simple Example ===" << std::endl;
    std::cout << "Ring dimension N = " << N << std::endl;
    std::cout << "Modulus q1 = 0x" << std::hex << q1 << std::dec << std::endl;
    std::cout << "Modulus q2 = 0x" << std::hex << q2 << std::dec << std::endl;

    // ---- Prepare input data (outside the cache check) ----
    std::vector<uint64_t> data_a(N), data_b(N), data_c(N);
    for (uint64_t i = 0; i < N; ++i) {
        data_a[i] = (i + 1)       % q1;
        data_b[i] = (2 * i + 1)   % q1;
        data_c[i] = (i * i)       % q1;
    }
    auto a = Polynomial::from_data(data_a, N, fhetch::Format::Evaluation);
    auto b = Polynomial::from_data(data_b, N, fhetch::Format::Evaluation);
    auto c = Polynomial::from_data(data_c, N, fhetch::Format::Evaluation);

    ModuliBase base = {q1, q2};
    MRP x(base, N);
    MRP y(base, N);

    MRPArray ct_a(2);
    ct_a[0] = MRP(base, N);
    ct_a[1] = MRP(base, N);

    MRPArray ct_b(2);
    ct_b[0] = MRP(base, N);
    ct_b[1] = MRP(base, N);

    fhetch::tag_input("a", a);
    fhetch::tag_input("b", b);
    fhetch::tag_input("c", c);
    fhetch::tag_input("x", x);
    fhetch::tag_input("y", y);
    fhetch::tag_input("ct_a", ct_a);
    fhetch::tag_input("ct_b", ct_b);

    if (!niobium::compiler().is_cache_valid()) {
        std::cout << "\n=== RECORDING ===" << std::endl;
        niobium::compiler().start();

        // ---- Single-residue polynomial operations ----
        std::cout << "\n--- Single-residue polynomial operations ---" << std::endl;

        auto r = fhetch::sr_addp(a, b, q1);
        std::cout << "  sr_addp(a, b, q1) -> r" << std::endl;

        auto s = fhetch::sr_mulp(r, c, q1);
        std::cout << "  sr_mulp(r, c, q1) -> s" << std::endl;

        auto t = fhetch::sr_mulps(s, Scalar::from_int(42), q1);
        std::cout << "  sr_mulps(s, 42, q1) -> t" << std::endl;

        auto u = fhetch::sr_negp(t, q1);
        std::cout << "  sr_negp(t, q1) -> u" << std::endl;

        auto u_coeff = fhetch::sr_intt(u, q1);
        std::cout << "  sr_intt(u, q1) -> u_coeff" << std::endl;

        auto u_eval = fhetch::sr_ntt(u_coeff, q1);
        std::cout << "  sr_ntt(u_coeff, q1) -> u_eval" << std::endl;

        fhetch::tag_output("u_eval", u_eval);

        // ---- Multi-residue polynomial (MRP) operations ----
        std::cout << "\n--- Multi-residue polynomial operations ---" << std::endl;

        auto z = fhetch::mr_addp(x, y);
        std::cout << "  mr_addp(x, y) -> z  [" << z.num_residues() << " residues]" << std::endl;

        auto w = fhetch::mr_mulp(z, x);
        std::cout << "  mr_mulp(z, x) -> w" << std::endl;

        // Per-residue scalar arithmetic. Build an MRS once and feed it
        // through addps / subps / mulps so each gadget gets exercised
        // on the same input shape.
        MRS s_per_q = MRS::from_pairs({
            {Scalar::from_int(7),  q1},
            {Scalar::from_int(11), q2},
        });
        auto w_addps = fhetch::mr_addps(w, s_per_q);
        std::cout << "  mr_addps(w, {7,11}) -> w_addps" << std::endl;

        auto w_subps = fhetch::mr_subps(w_addps, s_per_q);
        std::cout << "  mr_subps(w_addps, {7,11}) -> w_subps" << std::endl;

        auto w_scaled = fhetch::mr_mulps(w_subps, s_per_q);
        std::cout << "  mr_mulps(w_subps, {7,11}) -> w_scaled" << std::endl;

        auto w_eval = fhetch::mr_ntt(w_scaled);
        std::cout << "  mr_ntt(w_scaled) -> w_eval" << std::endl;

        // Galois automorphism X -> X^5 in evaluation representation. k=5
        // is the standard CKKS rotation generator for power-of-2 N.
        auto w_aut = fhetch::mr_automorph_eval(w_eval, 5);
        std::cout << "  mr_automorph_eval(w_eval, 5) -> w_aut" << std::endl;

        auto w_coeff = fhetch::mr_intt(w_aut);
        std::cout << "  mr_intt(w_aut) -> w_coeff" << std::endl;
        fhetch::tag_output("w_coeff", w_coeff);

        // ---- MRP Array dot product ----
        std::cout << "\n--- MRPA dot product ---" << std::endl;

        auto dot = fhetch::mrpa_dotproduct(ct_a, ct_b);
        std::cout << "  mrpa_dotproduct(ct_a, ct_b) -> dot  ["
                  << dot.num_residues() << " residues]" << std::endl;
        fhetch::tag_output("dot_product", dot);

        niobium::compiler().stop();
    } else {
        std::cout << "\n=== REPLAY MODE (using cached instruction trace) ===" << std::endl;
    }

    // ---- Run the functional simulator ----
    niobium::compiler().replay();

    auto out_dir = niobium::compiler().get_program_directory();
    std::cout << "\n=== Done ===" << std::endl;
    std::cout << "  trace:           " << (out_dir / "simple_fhetch_example_simple.fhetch") << std::endl;
    std::cout << "  replay manifest: " << (out_dir / "fhetch_replay.json") << std::endl;
    std::cout << "  replay outputs:  " << (out_dir / "fhetch_replay_outputs.json") << std::endl;
    return 0;
}
