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
 * @brief The recorder-free core (nbfhetch_core) on its own.
 *
 * Linked against nbfhetch_core only, so it links at all only if the core needs
 * no niobium::compiler() (check_symbols.py checks the archive's symbols too).
 * Records d = (a + b) * a mod q through the IR builder the way a recorder
 * would, and checks:
 *  - what a recorder reads: the tagged inputs and outputs, the address ->
 *    modulus map, the values, and the registry generation;
 *  - the trace written through the core's TraceWriter reads back;
 *  - the simulator runs it to the right values;
 *  - reset_for_epoch() and clear_address_moduli() clear and re-arm.
 *
 * The OpenFHE build the simulator links is the instrumented one: its library
 * calls the host's openfhe_cporbe_with_openmp() hook while it loads, so a
 * process with no host definition crashes before main(). The recorder defines
 * it; this test stands in for a host with no recorder attached, as an
 * application of the core alone must. The core itself references no hook.
 */
#include "niobium/fhetch_api.h"
#include "niobium/fhetch_ir.h"
#include "niobium/fhetch_reader.h"
#include "niobium/fhetch_recording.h"
#include "niobium/fhetch_sim/simulator.h"
#include "niobium/trace_writer.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

// The one hook the instrumented OpenFHE calls while loading (see above).
extern "C" void openfhe_cporbe_with_openmp(bool /*with_openmp*/) {}

namespace fhetch = niobium::fhetch;
namespace recording = niobium::fhetch::recording;

namespace {

int g_failed = 0;
int g_passed = 0;

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  ok   " : "  FAIL ") << what << '\n';
    ++(ok ? g_passed : g_failed);
}

constexpr std::uint64_t kQ = 65537;  // a Niobium-compatible prime: q = 1 mod 2^16
constexpr std::uint64_t kN = 16;

std::vector<std::uint64_t> ramp(std::uint64_t start) {
    std::vector<std::uint64_t> v(kN);
    for (std::size_t i = 0; i < kN; ++i) v[i] = (start + (7 * i)) % kQ;
    return v;
}

}  // namespace

int main() {
    const auto dir = std::filesystem::current_path() / "fhetch_core_test.tmp";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    auto& writer = niobium::detail::trace_writer();
    fhetch::reset_for_epoch();
    writer.clear();
    writer.set_trace_format(niobium::TraceFormat::Binary);
    writer.start_recording();
    check(!niobium::detail::montgomery_mode() && !niobium::detail::bit_reversal_mode(),
          "recording modes start ordinary-form");

    const auto a_values = ramp(3);
    const auto b_values = ramp(11);
    const auto a = fhetch::Polynomial::from_data(a_values, kN);
    const auto b = fhetch::Polynomial::from_data(b_values, kN);
    fhetch::tag_input("a", a);
    fhetch::tag_input("b", b);
    const auto d = fhetch::sr_mulp(fhetch::sr_addp(a, b, kQ), a, kQ);
    fhetch::tag_output("d", d);
    writer.emit(fhetch::Instruction{fhetch::FH_HALT});
    writer.stop_recording();
    const auto trace = writer.write(dir, "core_test");

    // What a recorder reads.
    const auto& inputs = recording::tagged_inputs();
    const auto& outputs = recording::tagged_outputs();
    check(inputs.size() == 2 && inputs[0].name == "a" && inputs[1].name == "b" && outputs.size() == 1 &&
              outputs[0].name == "d",
          "the tagged inputs and output, in tagging order");
    const auto address = [](const fhetch::Polynomial& p) { return niobium::detail::polynomial_address(p); };
    const auto& moduli = recording::address_moduli();
    check(moduli.count(address(a)) == 1 && moduli.at(address(a)) == kQ && moduli.count(address(b)) == 1 &&
              moduli.at(address(d)) == kQ,
          "every polynomial an instruction used maps to its modulus");
    check(recording::values_or_zeros(a) == a_values &&
              recording::values_or_zeros(fhetch::Polynomial::zeros(kN)) == std::vector<std::uint64_t>(kN, 0),
          "values_or_zeros: the values, or zeros");

    // The trace reads back, and the simulator runs it.
    const auto read = fhetch::read_fhetch_file(trace);
    check(read.ok && read.program.instructions.size() == 3, "the trace reads back: add, mul, halt");
    niobium::fhetch_sim::Simulator sim;
    sim.set_ring_dimension(kN);
    check(sim.load_trace(trace), "the simulator loads the trace");
    sim.store_polynomial(address(a), a_values, kQ);
    sim.store_polynomial(address(b), b_values, kQ);
    const auto result = sim.run();
    std::vector<std::uint64_t> want(kN);
    for (std::size_t i = 0; i < kN; ++i) want[i] = (a_values[i] + b_values[i]) % kQ * a_values[i] % kQ;
    check(result.errors == 0 && sim.get_polynomial(address(d)) == want, "the simulator computes (a + b) * a mod q");

    // Resets clear the registries and change the generation.
    const auto g0 = recording::registry_generation();
    fhetch::reset_for_epoch();
    const auto g1 = recording::registry_generation();
    check(g1 != g0 && recording::tagged_inputs().empty() && recording::tagged_outputs().empty() &&
              recording::address_moduli().empty(),
          "reset_for_epoch clears the registries and changes the generation");
    recording::tag_input_values("live_in", fhetch::Polynomial::zeros(kN), kQ, a_values);
    check(recording::tagged_inputs().size() == 1 && recording::tagged_inputs()[0].explicit_values &&
              recording::tagged_inputs()[0].modulus == kQ && recording::tagged_inputs()[0].values == a_values,
          "tag_input_values keeps the given modulus and values");
    recording::clear_address_moduli();
    check(recording::registry_generation() != g1, "clear_address_moduli changes the generation");

    std::filesystem::remove_all(dir);
    std::cout << g_passed << " passed, " << g_failed << " failed\n";
    return g_failed != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
