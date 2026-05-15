// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// Regression test: dest==src1 for sr_automorph_eval and
// sr_rot_automorph_coeff. Both kernels read and write at *different*
// indices in the same loop, so without snapshotting src first the
// permutation corrupts in place.

#include "niobium/fhetch_sim/simulator.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

using niobium::fhetch_sim::Simulator;

namespace {

constexpr uint64_t kRingDim = 4;
constexpr uint64_t kQ = 17;  // small prime, all test values fit

void write_trace(const std::filesystem::path& p, const std::string& inst) {
    std::ofstream f(p);
    f << "modulus_count 2\n";
    f << "m[0] 0xFFFFFFFFFFFFFFFF\n";  // COPY_MODULUS sentinel
    f << "m[1] 0x" << std::hex << kQ << std::dec << "\n";
    f << "# Instructions\n" << inst << "\n";
}

// Reference automorph_eval — out-of-place ground truth.
std::vector<uint64_t> ref_automorph(const std::vector<uint64_t>& a, uint32_t k) {
    uint32_t logn = 0;
    for (uint32_t n = static_cast<uint32_t>(kRingDim); n > 1; n >>= 1) ++logn;
    uint32_t mask = (1U << logn) - 1U;
    auto rev = [](uint32_t x, uint32_t bits) {
        uint32_t r = 0;
        for (uint32_t i = 0; i < bits; ++i)
            if (x & (1U << i)) r |= 1U << (bits - 1 - i);
        return r;
    };
    std::vector<uint64_t> out(kRingDim, 0);
    uint32_t jk = k;
    for (uint32_t j = 0; j < kRingDim; ++j, jk += 2 * k) {
        out[rev(j, logn)] = a[rev((jk >> 1) & mask, logn)];
    }
    return out;
}

// Reference rot_automorph_coeff — out-of-place ground truth.
std::vector<uint64_t> ref_rot(const std::vector<uint64_t>& a, uint64_t offset) {
    std::vector<uint64_t> out(kRingDim, 0);
    for (uint64_t i = 0; i < kRingDim; ++i) {
        uint64_t src_pos = i + offset;
        out[i] = (src_pos < kRingDim) ? a[src_pos]
                                      : (kQ - a[src_pos - kRingDim]) % kQ;
    }
    return out;
}

void print_vec(const std::string& label, const std::vector<uint64_t>& v) {
    std::cout << label << ": [";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) std::cout << ", ";
        std::cout << v[i];
    }
    std::cout << "]\n";
}

int run_case(const std::string& label,
             const std::string& sr_inst,
             const std::vector<uint64_t>& input,
             const std::vector<uint64_t>& expected) {
    static int counter = 0;
    std::cout << "--- " << label << " ---\n";
    Simulator sim;
    sim.set_ring_dimension(kRingDim);
    auto tmp = std::filesystem::temp_directory_path() /
               ("alias_" + std::to_string(::getpid()) + "_" +
                std::to_string(counter++) + ".fhetch");
    write_trace(tmp, sr_inst);
    if (!sim.load_trace(tmp)) {
        std::cerr << "  load_trace failed\n";
        std::filesystem::remove(tmp);
        return 1;
    }
    sim.store_polynomial(1, input, kQ);
    auto res = sim.run();
    std::filesystem::remove(tmp);
    if (res.errors != 0) {
        std::cerr << "  simulator errors: " << res.errors << "\n";
        return 1;
    }
    auto got = sim.get_polynomial(1);
    print_vec("  input   ", input);
    print_vec("  expected", expected);
    print_vec("  got     ", got);
    if (got != expected) {
        std::cerr << "  FAIL: aliased dest=src1 corrupted the output\n";
        return 1;
    }
    std::cout << "  PASS\n";
    return 0;
}

}  // namespace

int main() {
    const std::vector<uint64_t> input = {3, 5, 7, 11};
    int fails = 0;
    fails += run_case("automorph_eval k=3",
                      "sr_automorph_eval %1, %1, k=3, m=0",
                      input, ref_automorph(input, 3));
    fails += run_case("rot_automorph_coeff offset=2",
                      "sr_rot_automorph_coeff %1, %1, offset=2, m=1",
                      input, ref_rot(input, 2));
    if (fails != 0) {
        std::cerr << "\n" << fails << " test(s) failed.\n";
        return 1;
    }
    std::cout << "\nAll alias tests passed.\n";
    return 0;
}
