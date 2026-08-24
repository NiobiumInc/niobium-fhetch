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
 * @brief Tests for the FHETCH writer.
 *
 * The load-bearing property is that writer and reader are inverses: whatever
 * the reader accepts, the writer renders canonically, and reading that back
 * yields an equal Instruction. Checked here per operand shape and over a whole
 * program, in both serializations.
 */

#include "niobium/fhetch_reader.h"
#include "niobium/fhetch_writer.h"

#include <iostream>
#include <string>
#include <vector>

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

const char* kTable =
    "modulus_count 2\n"
    "m[0] 0xFFFFFFFFFFFFFFFF\n"
    "m[1] 0x11\n";

/// Read one instruction back in the same modulus-table context, so the
/// index-vs-literal magnitude rule sees what it saw the first time.
Instruction reread(const Instruction& inst) {
    const std::string text =
        std::string(kTable) + render_instruction(inst) + "\n";
    const auto r = read_fhetch_text(text);
    if (!r.ok || r.program.instructions.size() != 1) return Instruction{};
    return r.program.instructions[0];
}

// ---------------------------------------------------------------------------

bool test_renders_every_shape() {
    std::cout << "\n[test] every operand shape renders canonically" << std::endl;
    const int before = g_checks_failed;

    // Round-tripped from text, so the expected strings below are also the
    // grammar the reader accepts.
    const auto r = read_fhetch_text(std::string(kTable) +
        "sr_addp %3, %1, %2, m=1\n"
        "sr_mulps %4, %3, 12345, m=1\n"
        "sr_addp_ni %5, %1, %2\n"
        "sr_mulps_ni %6, %5, 7.000000\n"
        "sr_negp %7, %6, m=1\n"
        "sr_ft %8, %7\n"
        "sr_ntt %9, %8, m=1, omega=320656143\n"
        "sr_intt %10, %9, m=1\n"
        "sr_automorph_eval %11, %10, m=1, mask=2047, logn=11, k=65\n"
        "sr_automorph_coeff %12, %11, k=3, m=1\n"
        "sr_rot_automorph_coeff %13, %12, offset=2, m=1\n"
        "halt\n");
    CHECK(r.ok);
    if (r.program.instructions.size() != 12) { CHECK(false); return false; }

    const std::vector<std::string> expect = {
        "sr_addp %3, %1, %2, m=1",
        "sr_mulps %4, %3, 12345, m=1",
        "sr_addp_ni %5, %1, %2",
        "sr_mulps_ni %6, %5, 7.000000",
        "sr_negp %7, %6, m=1",
        "sr_ft %8, %7",
        "sr_ntt %9, %8, m=1, omega=320656143",
        "sr_intt %10, %9, m=1",
        "sr_automorph_eval %11, %10, m=1, mask=2047, logn=11, k=65",
        "sr_automorph_coeff %12, %11, k=3, m=1",
        "sr_rot_automorph_coeff %13, %12, offset=2, m=1",
        "halt",
    };
    for (size_t i = 0; i < expect.size(); ++i) {
        const std::string got = render_instruction(r.program.instructions[i]);
        CHECK(got == expect[i]);
        if (got != expect[i])
            std::cout << "      want: " << expect[i] << "\n      got : " << got << "\n";
    }
    return g_checks_failed == before;
}

bool test_reader_writer_are_inverses() {
    std::cout << "\n[test] reader and writer are inverses" << std::endl;
    const int before = g_checks_failed;

    const auto r = read_fhetch_text(std::string(kTable) +
        "sr_addp %3, %1, %2, m=1\n"
        "sr_subp %4, %3, %2, m=0\n"
        "sr_mulps %5, %4, 0, m=1\n"
        "sr_addps_coeff %6, %5, 99, m=1\n"
        "sr_subps_ni %7, %6, 3.500000\n"
        "sr_negp_ni %8, %7\n"
        "sr_ift %9, %8\n"
        "sr_permute %10, %9, m=1\n"
        "sr_ntt %11, %10, m=1, omega=7\n"
        "sr_automorph_eval %12, %11, m=1, k=65\n"
        "halt\n");
    CHECK(r.ok);
    for (const auto& inst : r.program.instructions) {
        // text is lossy in exactly one respect — it cannot say whether a
        // modulus is an index or a literal — so a re-read in the same table
        // context must still land on the same instruction.
        CHECK(reread(inst) == inst);
        // binary is lossless outright, because the record flags the literal.
        CHECK(from_record(to_record(inst)) == inst);
    }
    return g_checks_failed == before;
}

bool test_absent_operands_stay_absent() {
    std::cout << "\n[test] absent operands are not rendered as zero"
              << std::endl;
    const int before = g_checks_failed;

    // omega absent means "derive a root", omega=0 would mean "use zero". The
    // writer must not turn the first into the second.
    Instruction ntt;
    ntt.opcode = FH_SR_NTT;
    ntt.dest = 2; ntt.src1 = 1; ntt.modulus = 1;
    CHECK(render_instruction(ntt) == "sr_ntt %2, %1, m=1");
    CHECK(!reread(ntt).omega.has_value());
    CHECK(!from_record(to_record(ntt)).omega.has_value());

    ntt.omega = 0;
    CHECK(render_instruction(ntt) == "sr_ntt %2, %1, m=1, omega=0");
    CHECK(reread(ntt).omega.value_or(99) == 0);
    CHECK(from_record(to_record(ntt)).omega.value_or(99) == 0);

    // Same for the automorph optionals.
    Instruction aut;
    aut.opcode = FH_SR_AUTOMORPH_EVAL;
    aut.dest = 3; aut.src1 = 2; aut.modulus = 1; aut.k = 5;
    CHECK(render_instruction(aut) == "sr_automorph_eval %3, %2, m=1, k=5");
    aut.mask = 2047;
    CHECK(render_instruction(aut) == "sr_automorph_eval %3, %2, m=1, mask=2047, k=5");
    aut.logn = 11;
    CHECK(render_instruction(aut) ==
          "sr_automorph_eval %3, %2, m=1, mask=2047, logn=11, k=5");

    // FH_ILL has no mnemonic, so it renders as nothing rather than as garbage.
    Instruction ill;
    CHECK(render_instruction(ill).empty());

    return g_checks_failed == before;
}

bool test_literal_modulus_survives_binary_not_text() {
    std::cout << "\n[test] the literal flag survives binary, not text"
              << std::endl;
    const int before = g_checks_failed;

    Instruction lit;
    lit.opcode = FH_SR_ADDP;
    lit.dest = 3; lit.src1 = 1; lit.src2 = 2;
    lit.modulus = 1152921504606830593ull;
    lit.modulus_is_literal = true;

    // Both render as a bare m=<number>; the text genuinely cannot express the
    // difference, which is why the record carries a flag for it.
    CHECK(render_instruction(lit) == "sr_addp %3, %1, %2, m=1152921504606830593");
    CHECK(from_record(to_record(lit)) == lit);            // binary: exact
    CHECK(reread(lit) == lit);                            // text: magnitude rule agrees

    // An index, by contrast, must not come back flagged as a literal.
    Instruction idx = lit;
    idx.modulus = 1;
    idx.modulus_is_literal = false;
    CHECK(from_record(to_record(idx)) == idx);
    CHECK(reread(idx) == idx);

    return g_checks_failed == before;
}

bool test_header_and_table() {
    std::cout << "\n[test] header and modulus table round-trip" << std::endl;
    const int before = g_checks_failed;

    TraceMetadata m;
    m.program_name = "matrix_ops";
    m.program_version = "1.0";
    m.description = "writer fixture";
    m.source_file = "examples/mat_mul.cpp";
    m.source_line = 42;
    m.build_timestamp = "Mon Aug 17 2026";
    m.generated_timestamp = 1755400000000ull;

    const std::vector<uint64_t> table{0xFFFFFFFFFFFFFFFFull, 0x11ull};
    const std::string text = render_header(m, 1, table.size()) + "\n" +
                             render_modulus_table(table) +
                             "sr_addp %3, %1, %2, m=1\n";

    // The version is written as "<name> v<version>" and read back into its own
    // field, so it no longer ends up glued onto the name.
    CHECK(text.find("# Program: matrix_ops v1.0\n") != std::string::npos);
    CHECK(text.find("# Source: examples/mat_mul.cpp:42\n") != std::string::npos);
    CHECK(text.find("# Modulus Count: 2\n") != std::string::npos);
    CHECK(text.find("m[0] 0xFFFFFFFFFFFFFFFF\n") != std::string::npos);

    const auto r = read_fhetch_text(text);
    CHECK(r.ok);
    CHECK(r.program.metadata.program_name == "matrix_ops");
    CHECK(r.program.metadata.description == "writer fixture");
    CHECK(r.program.metadata.source_file == "examples/mat_mul.cpp");
    CHECK(r.program.metadata.source_line == 42);
    CHECK(r.program.metadata.generated_timestamp == 1755400000000ull);
    CHECK(r.program.modulus_table == table);
    CHECK(r.program.instructions.size() == 1);

    // Counts describe what was written, not what a carried-over metadata block
    // claimed.
    TraceMetadata stale = m;
    stale.instruction_count = 999;
    const std::string h = render_header(stale, 7, 3);
    CHECK(h.find("# Instruction Count: 7\n") != std::string::npos);
    CHECK(h.find("# Modulus Count: 3\n") != std::string::npos);

    return g_checks_failed == before;
}

bool test_whole_program_round_trip() {
    std::cout << "\n[test] a whole program survives text and binary"
              << std::endl;
    const int before = g_checks_failed;

    const auto first = read_fhetch_text(std::string(kTable) +
        "sr_addp %3, %1, %2, m=1\n"
        "sr_ntt %4, %3, m=1, omega=7\n"
        "sr_automorph_eval %5, %4, m=1, mask=2047, logn=11, k=65\n"
        "halt\n");
    CHECK(first.ok);

    // Render the whole program and read it back: same instructions, same table.
    std::string text = render_modulus_table(first.program.modulus_table);
    for (const auto& inst : first.program.instructions)
        text += render_instruction(inst) + "\n";
    const auto second = read_fhetch_text(text);
    CHECK(second.ok);
    CHECK(second.program.modulus_table == first.program.modulus_table);
    CHECK(second.program.instructions == first.program.instructions);

    // Rendering twice must be stable — no state carried between calls.
    std::string again = render_modulus_table(second.program.modulus_table);
    for (const auto& inst : second.program.instructions)
        again += render_instruction(inst) + "\n";
    CHECK(again == text);

    return g_checks_failed == before;
}

bool test_appending_overload_matches() {
    std::cout << "\n[test] the appending overload matches, and reuses a buffer"
              << std::endl;
    const int before = g_checks_failed;

    const auto r = read_fhetch_text(std::string(kTable) +
        "sr_addp %3, %1, %2, m=1\n"
        "sr_ntt %4, %3, m=1, omega=7\n"
        "halt\n");
    CHECK(r.ok);

    // One buffer across the loop is the shape a streaming writer needs: the
    // appending overload must leave nothing behind between instructions.
    std::string buf;
    for (const auto& inst : r.program.instructions) {
        buf.clear();
        render_instruction(inst, buf);
        CHECK(buf == render_instruction(inst));
    }

    // Appending to a non-empty buffer appends rather than overwrites.
    std::string acc = "prefix|";
    render_instruction(r.program.instructions[0], acc);
    CHECK(acc == "prefix|" + render_instruction(r.program.instructions[0]));

    return g_checks_failed == before;
}


bool test_annotations_interleave() {
    std::cout << "\n[test] annotations are written back where they were"
              << std::endl;
    const int before = g_checks_failed;

    // Recorded traces interleave comments heavily — a measured CKKS bootstrap
    // carries 82,719 of them between its instructions — so position is content.
    const std::string text =
        "modulus_count 2\n"
        "m[0] 0xFFFFFFFFFFFFFFFF\n"
        "m[1] 0x11\n"
        "# before everything\n"
        "sr_addp %3, %1, %2, m=1\n"
        "# enter DCRTPoly::Automorphism\n"
        "# switchmodulus %3, %3, old_mod=1, new_mod=0\n"
        "sr_mulp %4, %3, %2, m=1\n"
        "# exit DCRTPoly::Automorphism\n"
        "halt\n"
        "# after the end\n";

    const auto r = read_fhetch_text(text);
    CHECK(r.ok);
    CHECK(r.program.instructions.size() == 3);
    CHECK(r.program.annotations.size() == 5);
    if (r.program.annotations.size() != 5) return false;

    // Positions are counted in instructions, so a writer can place them back.
    CHECK(r.program.annotations[0].after_instruction == 0);  // before any
    CHECK(r.program.annotations[1].after_instruction == 1);  // after sr_addp
    CHECK(r.program.annotations[2].after_instruction == 1);
    CHECK(r.program.annotations[3].after_instruction == 2);  // after sr_mulp
    CHECK(r.program.annotations[4].after_instruction == 3);  // after halt

    // The structured ones are preserved verbatim, undecided fate and all.
    CHECK(r.program.annotations[2].text ==
          "switchmodulus %3, %3, old_mod=1, new_mod=0");

    // Round trip: read what we write and get the same positions back.
    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "nb_fhetch_writer_annots.fhetch";
    CHECK(write_fhetch_text(tmp, r.program));
    const auto again = read_fhetch_file(tmp);
    std::filesystem::remove(tmp);
    CHECK(again.ok);
    CHECK(again.program.instructions == r.program.instructions);
    CHECK(again.program.annotations.size() == r.program.annotations.size());
    for (size_t i = 0; i < again.program.annotations.size() &&
                       i < r.program.annotations.size(); ++i) {
        CHECK(again.program.annotations[i].text == r.program.annotations[i].text);
        CHECK(again.program.annotations[i].after_instruction ==
              r.program.annotations[i].after_instruction);
    }

    return g_checks_failed == before;
}

bool test_header_comments_are_not_annotations() {
    std::cout << "\n[test] the header block does not become annotations"
              << std::endl;
    const int before = g_checks_failed;

    // The writer re-emits the header from metadata, so keeping those lines as
    // annotations too would duplicate every one of them on a round trip.
    const auto r = read_fhetch_text(
        "# =========================================\n"
        "# Niobium FHETCH Trace\n"
        "# =========================================\n"
        "# Program: p v2.5\n"
        "# Instruction Count: 1\n"
        "# Modulus Count: 2\n"
        "# =========================================\n"
        "\n# Modulus Table\n"
        "modulus_count 2\n"
        "m[0] 0xFFFFFFFFFFFFFFFF\n"
        "m[1] 0x11\n"
        "\n# Instructions\n"
        "sr_addp %3, %1, %2, m=1\n");
    CHECK(r.ok);
    CHECK(r.program.annotations.empty());
    CHECK(r.program.metadata.program_name == "p");
    CHECK(r.program.metadata.program_version == "2.5");

    // And the version survives a full round trip through the file.
    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "nb_fhetch_writer_header.fhetch";
    CHECK(write_fhetch_text(tmp, r.program));
    const auto again = read_fhetch_file(tmp);
    std::filesystem::remove(tmp);
    CHECK(again.ok);
    CHECK(again.program.annotations.empty());
    CHECK(again.program.metadata.program_name == "p");
    CHECK(again.program.metadata.program_version == "2.5");

    return g_checks_failed == before;
}

bool test_instruction_count_excludes_comments() {
    std::cout << "\n[test] the header count is instructions, not lines"
              << std::endl;
    const int before = g_checks_failed;

    // TraceWriter has always written instructions_.size() over a vector holding
    // instruction lines AND comment lines, so every recorded trace overstates
    // the count — 902,100 for a bootstrap that has 819,381 instructions and
    // 82,719 comments. The writer counts instructions.
    const auto r = read_fhetch_text(
        "modulus_count 2\nm[0] 0xFFFFFFFFFFFFFFFF\nm[1] 0x11\n"
        "sr_addp %3, %1, %2, m=1\n"
        "# a comment\n"
        "# another\n"
        "halt\n");
    CHECK(r.ok);
    CHECK(r.program.instructions.size() == 2);
    CHECK(r.program.annotations.size() == 2);

    const std::string h = render_header(r.program.metadata,
                                       r.program.instructions.size(),
                                       r.program.modulus_table.size());
    CHECK(h.find("# Instruction Count: 2\n") != std::string::npos);
    CHECK(h.find("# Instruction Count: 4\n") == std::string::npos);

    return g_checks_failed == before;
}


bool test_non_integer_immediates() {
    std::cout << "\n[test] non-integer immediates are floating point"
              << std::endl;
    const int before = g_checks_failed;

    // The `_ni` forms take a NonInteger scalar. Before this was modelled, the
    // reader rejected every one of them ("malformed immediate") because it
    // parsed the operand as an integer — so a trace using them silently lost
    // those instructions.
    const auto r = read_fhetch_text(std::string(kTable) +
        "sr_addps_ni %3, %2, 3.000000\n"
        "sr_mulps_ni %4, %3, 0.500000\n"
        "sr_subps_coeff_ni %5, %4, -1.250000\n");
    CHECK(r.ok);
    CHECK(r.warnings.empty());
    CHECK(r.program.instructions.size() == 3);
    if (r.program.instructions.size() != 3) return false;

    CHECK(r.program.instructions[0].fp_immediate.value_or(0.0) == 3.0);
    CHECK(r.program.instructions[1].fp_immediate.value_or(0.0) == 0.5);
    CHECK(r.program.instructions[2].fp_immediate.value_or(0.0) == -1.25);
    // The integer field stays empty, so a consumer cannot confuse the two.
    for (const auto& inst : r.program.instructions)
        CHECK(!inst.immediate.has_value());

    // Renders back to what a producer writes: std::to_string's 6 decimals.
    CHECK(render_instruction(r.program.instructions[1]) ==
          "sr_mulps_ni %4, %3, 0.500000");

    // Binary carries the exact bit pattern, so it round-trips a value the text
    // form would round. Text is lossy here and that is the text form's limit.
    Instruction precise;
    precise.opcode = FH_SR_MULPS_NI;
    precise.dest = 2; precise.src1 = 1;
    precise.fp_immediate = 0.1234567890123;
    CHECK(from_record(to_record(precise)) == precise);
    CHECK(reread(precise) != precise);  // text rounds to 6 places

    return g_checks_failed == before;
}

}  // namespace

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "FHETCH writer" << std::endl;
    std::cout << "========================================" << std::endl;

    int passed = 0;
    int failed = 0;

    if (test_renders_every_shape()) passed++; else failed++;
    if (test_reader_writer_are_inverses()) passed++; else failed++;
    if (test_absent_operands_stay_absent()) passed++; else failed++;
    if (test_literal_modulus_survives_binary_not_text()) passed++; else failed++;
    if (test_header_and_table()) passed++; else failed++;
    if (test_whole_program_round_trip()) passed++; else failed++;
    if (test_appending_overload_matches()) passed++; else failed++;
    if (test_annotations_interleave()) passed++; else failed++;
    if (test_header_comments_are_not_annotations()) passed++; else failed++;
    if (test_instruction_count_excludes_comments()) passed++; else failed++;
    if (test_non_integer_immediates()) passed++; else failed++;

    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << passed << " passed, " << failed << " failed"
              << std::endl;
    std::cout << "========================================" << std::endl;

    return failed > 0 ? 1 : 0;
}
