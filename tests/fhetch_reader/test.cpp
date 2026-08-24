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
 * @brief Tests for the `.fhetch` text reader.
 *
 * Covers every operand shape, the header block, the modulus index-vs-literal
 * rule, and the cases where the two readers this replaces disagreed with each
 * other: hex named values, and whether `omega` survives.
 */

#include "niobium/fhetch_reader.h"
#include "niobium/fhetch_writer.h"

#include <filesystem>

#include <iostream>
#include <string>

using namespace niobium::fhetch;

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

/// A two-entry table so `m=0`/`m=1` are indices and anything larger is a
/// literal, which is what the magnitude rule keys on.
const char* kTable =
    "modulus_count 2\n"
    "m[0] 0xFFFFFFFFFFFFFFFF\n"
    "m[1] 0x11\n";

std::string with_table(const std::string& body) { return std::string(kTable) + body; }

// ---------------------------------------------------------------------------

bool test_every_operand_shape() {
    std::cout << "\n[test] every operand shape" << std::endl;
    const int before = g_checks_failed;

    const auto r = read_fhetch_text(with_table(
        "sr_addp %3, %1, %2, m=1\n"
        "sr_mulps %4, %3, 12345, m=1\n"
        "sr_addp_ni %5, %1, %2\n"
        "sr_mulps_ni %6, %5, 7.000000\n"
        "sr_negp %7, %6, m=1\n"
        "sr_ft %8, %7\n"
        "sr_ntt %9, %8, m=1, omega=320656143\n"
        "sr_automorph_eval %10, %9, m=1, mask=2047, logn=11, k=65\n"
        "sr_automorph_coeff %11, %10, k=3, m=1\n"
        "sr_rot_automorph_coeff %12, %11, offset=2, m=1\n"
        "halt\n"));

    CHECK(r.ok);
    for (const auto& e : r.errors) std::cout << "    unexpected error: " << e.message << std::endl;
    for (const auto& w : r.warnings) std::cout << "    unexpected warning: " << w.message << std::endl;
    CHECK(r.warnings.empty());
    CHECK(r.program.instructions.size() == 11);
    if (r.program.instructions.size() != 11) return false;
    const auto& in = r.program.instructions;

    CHECK(in[0].opcode == FH_SR_ADDP);
    CHECK(in[0].dest == 3 && in[0].src1 == 1 && in[0].src2 == 2);
    CHECK(in[0].modulus.value_or(99) == 1 && !in[0].modulus_is_literal);
    CHECK(!in[0].immediate.has_value());

    CHECK(in[1].opcode == FH_SR_MULPS);
    CHECK(in[1].immediate.value_or(0) == 12345);
    // src2 is untouched on the immediate forms, not set to a stray value.
    CHECK(in[1].src2 == 0);

    // The _ni forms carry no modulus at all — a distinction, not an omission.
    CHECK(in[2].opcode == FH_SR_ADDP_NI);
    CHECK(!in[2].modulus.has_value());
    // The _ni forms carry a NonInteger scalar, so the immediate is floating
    // point and lands in its own field. Parsing it as an integer is what made
    // the previous reader reject every `_ni` scalar instruction outright.
    CHECK(in[3].opcode == FH_SR_MULPS_NI);
    CHECK(in[3].fp_immediate.value_or(0.0) == 7.0);
    CHECK(!in[3].immediate.has_value());
    CHECK(!in[3].modulus.has_value());

    CHECK(in[4].opcode == FH_SR_NEGP && in[4].modulus.has_value());
    CHECK(in[5].opcode == FH_SR_FT && !in[5].modulus.has_value());

    CHECK(in[6].opcode == FH_SR_NTT);
    CHECK(in[6].omega.value_or(0) == 320656143ull);

    CHECK(in[7].opcode == FH_SR_AUTOMORPH_EVAL);
    CHECK(in[7].k.value_or(0) == 65);
    CHECK(in[7].mask.value_or(0) == 2047);
    CHECK(in[7].logn.value_or(0) == 11);
    CHECK(in[7].modulus.value_or(99) == 1);

    CHECK(in[8].opcode == FH_SR_AUTOMORPH_COEFF && in[8].k.value_or(0) == 3);
    CHECK(in[9].opcode == FH_SR_ROT_AUTOMORPH_COEFF && in[9].offset.value_or(0) == 2);

    CHECK(in[10].opcode == FH_HALT);
    CHECK(in[10].dest == 0 && in[10].src1 == 0 && !in[10].modulus.has_value());

    // Line numbers are 1-based and count the table lines too.
    CHECK(in[0].line_number == 4);

    return g_checks_failed == before;
}

bool test_drift_hex_named_values() {
    std::cout << "\n[test] drift 1: hex named values are accepted" << std::endl;
    const int before = g_checks_failed;

    // The old simulator reader parsed this base-10 and silently produced 0;
    // the old driver reader produced 2748. Accepting the superset resolves it.
    const auto r = read_fhetch_text(with_table(
        "sr_ntt %2, %1, m=1, omega=0xABC\n"
        "sr_automorph_coeff %3, %2, k=0x10, m=1\n"));
    CHECK(r.ok);
    CHECK(r.program.instructions.size() == 2);
    if (r.program.instructions.size() != 2) return false;
    CHECK(r.program.instructions[0].omega.value_or(0) == 0xABCull);  // 2748, not 0
    CHECK(r.program.instructions[1].k.value_or(0) == 0x10ull);       // 16

    // Decimal still means decimal — 0x is opt-in, not required.
    const auto d = read_fhetch_text(with_table("sr_ntt %2, %1, m=1, omega=320656143\n"));
    CHECK(d.ok && d.program.instructions.size() == 1);
    if (d.program.instructions.size() == 1)
        CHECK(d.program.instructions[0].omega.value_or(0) == 320656143ull);

    return g_checks_failed == before;
}

bool test_drift_omega_survives() {
    std::cout << "\n[test] drift 2: omega survives the read" << std::endl;
    const int before = g_checks_failed;

    // The driver-side reader documented `omega=` and then never parsed it, so
    // a recorded root of unity was silently replaced by a derived one.
    const auto with = read_fhetch_text(with_table("sr_intt %2, %1, m=1, omega=99\n"));
    CHECK(with.ok);
    if (with.program.instructions.size() == 1)
        CHECK(with.program.instructions[0].omega.value_or(0) == 99);

    // Absent is absent, so a consumer can tell "derive one" from "use 0".
    const auto without = read_fhetch_text(with_table("sr_intt %2, %1, m=1\n"));
    CHECK(without.ok);
    if (without.program.instructions.size() == 1)
        CHECK(!without.program.instructions[0].omega.has_value());

    return g_checks_failed == before;
}

bool test_modulus_index_vs_literal() {
    std::cout << "\n[test] modulus: index vs literal by magnitude" << std::endl;
    const int before = g_checks_failed;

    const auto r = read_fhetch_text(with_table(
        "sr_addp %3, %1, %2, m=0\n"                        // index 0 (sentinel)
        "sr_addp %4, %1, %2, m=1\n"                        // index 1
        "sr_addp %5, %1, %2, m=1152921504606830593\n"));   // 60-bit literal prime
    CHECK(r.ok);
    CHECK(r.program.instructions.size() == 3);
    if (r.program.instructions.size() != 3) return false;

    CHECK(r.program.instructions[0].modulus.value_or(99) == 0);
    CHECK(!r.program.instructions[0].modulus_is_literal);
    CHECK(r.program.instructions[1].modulus.value_or(99) == 1);
    CHECK(!r.program.instructions[1].modulus_is_literal);
    CHECK(r.program.instructions[2].modulus.value_or(0) == 1152921504606830593ull);
    CHECK(r.program.instructions[2].modulus_is_literal);

    // With no table at all, every modulus is a literal — there is nothing for
    // an index to mean.
    const auto none = read_fhetch_text("sr_addp %3, %1, %2, m=1\n");
    CHECK(none.ok);
    if (none.program.instructions.size() == 1) {
        CHECK(none.program.instructions[0].modulus.value_or(0) == 1);
        CHECK(none.program.instructions[0].modulus_is_literal);
    }

    return g_checks_failed == before;
}

bool test_strict_where_silence_would_corrupt() {
    std::cout << "\n[test] rejects rather than truncates" << std::endl;
    const int before = g_checks_failed;

    // The previous reader truncated an address into uint32_t, producing a
    // different but perfectly valid-looking operand.
    const auto wide = read_fhetch_text(with_table("sr_addp %4294967296, %1, %2, m=1\n"));
    CHECK(!wide.ok);
    CHECK(!wide.errors.empty());
    CHECK(wide.program.instructions.empty());

    // The largest address that does fit is fine.
    const auto ok = read_fhetch_text(with_table("sr_addp %4294967295, %1, %2, m=1\n"));
    CHECK(ok.ok);
    if (ok.program.instructions.size() == 1)
        CHECK(ok.program.instructions[0].dest == 4294967295u);

    // A gapped or reordered modulus table would shift every m= silently.
    const auto gap = read_fhetch_text(
        "modulus_count 2\nm[0] 0x11\nm[2] 0x13\nsr_addp %3, %1, %2, m=1\n");
    CHECK(!gap.ok);

    // A negative number must not wrap into a huge address.
    const auto neg = read_fhetch_text(with_table("sr_addp %-1, %1, %2, m=1\n"));
    CHECK(neg.program.instructions.empty());

    return g_checks_failed == before;
}

bool test_lenient_where_it_should_be() {
    std::cout << "\n[test] skips what it cannot know, without failing"
              << std::endl;
    const int before = g_checks_failed;

    // Forward compatibility: an opcode from a newer writer is skipped with a
    // warning, and the rest of the program still reads.
    const auto r = read_fhetch_text(with_table(
        "sr_addp %3, %1, %2, m=1\n"
        "sr_quantum_flux %4, %3, m=1\n"
        "halt\n"));
    CHECK(r.ok);                       // not a hard failure
    CHECK(r.warnings.size() == 1);
    CHECK(r.program.instructions.size() == 2);

    // Too few operands is a skip, not a crash.
    const auto short_line = read_fhetch_text(with_table("sr_addp %3, %1\n"));
    CHECK(short_line.ok);
    CHECK(short_line.program.instructions.empty());
    CHECK(short_line.warnings.size() == 1);

    return g_checks_failed == before;
}

bool test_header_and_comments() {
    std::cout << "\n[test] header block and comments" << std::endl;
    const int before = g_checks_failed;

    const auto r = read_fhetch_text(
        "# =========================================\n"
        "# Niobium FHETCH Trace\n"
        "# =========================================\n"
        "# Program: matrix_ops v1.0\n"
        "# Description: round-trip fixture\n"
        "# Source: examples/mat_mul.cpp:42\n"
        "# Build: Mon Aug 17 2026\n"
        "# Instruction Count: 2\n"
        "# Modulus Count: 2\n"
        "# Generated: 1755400000000\n"
        "# Ring Dimension: 65536\n"
        "# =========================================\n"
        "modulus_count 2\n"
        "m[0] 0xFFFFFFFFFFFFFFFF\n"
        "m[1] 0x11\n"
        "sr_addp %3, %1, %2, m=1   # inline comment is not an operand\n"
        "# output result %3\n"
        "halt\n");
    CHECK(r.ok);

    const auto& m = r.program.metadata;
    // Name and version land in separate fields rather than the version being
    // glued onto the name.
    CHECK(m.program_name == "matrix_ops");
    CHECK(m.description == "round-trip fixture");
    CHECK(m.source_file == "examples/mat_mul.cpp");
    CHECK(m.source_line == 42);
    CHECK(m.build_timestamp == "Mon Aug 17 2026");
    CHECK(m.instruction_count == 2);
    CHECK(m.generated_timestamp == 1755400000000ull);
    CHECK(m.ring_dimension == 65536);
    CHECK(m.modulus_chain_length == 2);
    CHECK(m.modulus_chain == r.program.modulus_table);

    // An inline comment ends the instruction and is not mistaken for operands.
    CHECK(r.program.instructions.size() == 2);
    if (r.program.instructions.size() >= 1) {
        CHECK(r.program.instructions[0].opcode == FH_SR_ADDP);
        CHECK(r.program.instructions[0].modulus.value_or(99) == 1);
    }

    // Comments are kept, never instructions — including the structured ones
    // whose fate is still undecided.
    bool saw_output_marker = false;
    for (const auto& a : r.program.annotations)
        if (a.text == "output result %3") saw_output_marker = true;
    CHECK(saw_output_marker);
    // The '=' rules are separators, not content.
    for (const auto& a : r.program.annotations)
        CHECK(a.text.find_first_not_of('=') != std::string::npos);

    return g_checks_failed == before;
}

bool test_addresses_are_not_ssa() {
    std::cout << "\n[test] addresses may repeat and alias" << std::endl;
    const int before = g_checks_failed;

    // Recorded OpenFHE traces are ~70% read-modify-write; the reader must not
    // treat that as an error.
    const auto r = read_fhetch_text(with_table(
        "sr_mulps %1, %1, 0, m=1\n"                 // dest == src1
        "sr_automorph_eval %1, %1, m=1, k=3\n"      // aliasing automorph
        "sr_addp %1, %1, %1, m=1\n"));              // all three the same
    CHECK(r.ok);
    CHECK(r.warnings.empty());
    CHECK(r.program.instructions.size() == 3);
    if (r.program.instructions.size() == 3) {
        CHECK(r.program.instructions[0].dest == r.program.instructions[0].src1);
        CHECK(r.program.instructions[2].dest == r.program.instructions[2].src2);
    }

    return g_checks_failed == before;
}


bool test_reads_either_serialization() {
    std::cout << "\n[test] one entry point reads text or binary" << std::endl;
    const int before = g_checks_failed;

    const auto text = read_fhetch_text(with_table(
        "sr_addp %3, %1, %2, m=1\n"
        "sr_ntt %4, %3, m=1, omega=7\n"
        "sr_automorph_eval %5, %4, m=1, mask=2047, logn=11, k=65\n"
        "halt\n"));
    CHECK(text.ok);

    const auto dir = std::filesystem::temp_directory_path();
    const auto bin = dir / "nb_reader_either.fhex";
    CHECK(write_fhetch_binary(bin, text.program));

    // The same program, read back from the binary form, must give equal
    // instructions — that is what makes the format a consumer's non-problem.
    const auto binary = read_fhetch_file(bin);
    CHECK(binary.ok);
    CHECK(binary.program.instructions == text.program.instructions);
    CHECK(binary.program.modulus_table == text.program.modulus_table);

    // Detection is on content, not the file name: a binary trace named
    // `.fhetch` still reads.
    const auto misnamed = dir / "nb_reader_misnamed.fhetch";
    std::filesystem::copy_file(bin, misnamed,
                               std::filesystem::copy_options::overwrite_existing);
    const auto by_content = read_fhetch_file(misnamed);
    CHECK(by_content.ok);
    CHECK(by_content.program.instructions == text.program.instructions);

    std::filesystem::remove(bin);
    std::filesystem::remove(misnamed);

    // A truncated binary file is refused with a typed reason, not read as text.
    const std::byte junk[8] = {std::byte{'N'}, std::byte{'B'}, std::byte{'F'},
                               std::byte{'H'}, std::byte{0}, std::byte{0},
                               std::byte{0}, std::byte{0}};
    const auto damaged = read_fhetch_binary(junk, sizeof(junk));
    CHECK(!damaged.ok);
    CHECK(!damaged.errors.empty());

    return g_checks_failed == before;
}

}  // namespace

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << ".fhetch text reader" << std::endl;
    std::cout << "========================================" << std::endl;

    int passed = 0;
    int failed = 0;

    if (test_every_operand_shape()) passed++; else failed++;
    if (test_drift_hex_named_values()) passed++; else failed++;
    if (test_drift_omega_survives()) passed++; else failed++;
    if (test_modulus_index_vs_literal()) passed++; else failed++;
    if (test_strict_where_silence_would_corrupt()) passed++; else failed++;
    if (test_lenient_where_it_should_be()) passed++; else failed++;
    if (test_header_and_comments()) passed++; else failed++;
    if (test_addresses_are_not_ssa()) passed++; else failed++;
    if (test_reads_either_serialization()) passed++; else failed++;

    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << passed << " passed, " << failed << " failed"
              << std::endl;
    std::cout << "========================================" << std::endl;

    return failed > 0 ? 1 : 0;
}
