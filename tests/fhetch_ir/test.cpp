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
 * @brief Tests for the FHETCH IR: the opcode vocabulary and the instruction.
 *
 * The point of kOpcodeTable is that a reader and a writer driving off it
 * cannot disagree about what an instruction looks like. These tests pin that
 * down: every opcode has exactly one row, mnemonics round-trip both ways, and
 * every enumerator has an operand shape. A new opcode added to the enum but
 * not to the table fails to compile (static_assert on kOpcodeCount); one
 * added to both but mis-shaped fails here.
 */

#include "niobium/fhetch_ir.h"

#include <cstdio>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
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

/// Every enumerator except FH_ILL, so a test can assert over the whole set
/// rather than a sample of it.
std::vector<FhOpcode> all_real_opcodes() {
    std::vector<FhOpcode> v;
    for (uint8_t o = FH_SR_ADDP; o <= FH_HALT; ++o)
        v.push_back(static_cast<FhOpcode>(o));
    return v;
}

// ---------------------------------------------------------------------------

bool test_table_covers_every_opcode() {
    std::cout << "\n[test] the table covers every opcode, exactly once"
              << std::endl;
    const int before = g_checks_failed;

    // Contiguous numbering: the static_assert in the header pins the count,
    // this pins that no value in the range is missing or doubled.
    std::set<uint8_t> seen;
    for (const auto& row : kOpcodeTable) {
        const bool fresh = seen.insert(static_cast<uint8_t>(row.opcode)).second;
        CHECK(fresh);  // no opcode listed twice
        CHECK(row.mnemonic != nullptr);
        CHECK(row.mnemonic[0] != '\0');
    }
    CHECK(seen.size() == kOpcodeCount);

    for (FhOpcode o : all_real_opcodes())
        CHECK(seen.count(static_cast<uint8_t>(o)) == 1);

    // FH_ILL names "no opcode" and is deliberately absent.
    CHECK(seen.count(FH_ILL) == 0);
    CHECK(fh_opcode_info(FH_ILL) == nullptr);
    CHECK(fh_opcode_mnemonic(FH_ILL) == nullptr);

    return g_checks_failed == before;
}

bool test_mnemonics_round_trip() {
    std::cout << "\n[test] mnemonics round-trip in both directions"
              << std::endl;
    const int before = g_checks_failed;

    std::set<std::string> mnemonics;
    for (FhOpcode o : all_real_opcodes()) {
        const char* m = fh_opcode_mnemonic(o);
        CHECK(m != nullptr);
        if (!m) continue;
        // Distinct: two opcodes sharing a mnemonic would make the text form
        // ambiguous on the way back in.
        CHECK(mnemonics.insert(m).second);
        const auto back = fh_opcode_from_mnemonic(m);
        CHECK(back.has_value());
        if (back) CHECK(*back == o);
    }
    CHECK(mnemonics.size() == kOpcodeCount);

    // Tokens that are not mnemonics report so, rather than landing on FH_ILL
    // or on a neighbouring opcode.
    for (std::string_view junk : {"", "sr_", "SR_ADDP", "sr_addpp", "addp",
                                  "halt ", "#", "%1", "m=1"})
        CHECK(!fh_opcode_from_mnemonic(junk).has_value());

    // Case matters here: readers lowercase before lookup, so the table itself
    // must not also accept upper case, or the two layers would disagree.
    CHECK(!fh_opcode_from_mnemonic("SR_NTT").has_value());
    CHECK(fh_opcode_from_mnemonic("sr_ntt").has_value());

    return g_checks_failed == before;
}

bool test_operand_forms() {
    std::cout << "\n[test] operand shapes" << std::endl;
    const int before = g_checks_failed;

    // Only halt has no operands; everything else does. This is the property
    // the binary codec's dst/src1 presence keys on.
    for (FhOpcode o : all_real_opcodes()) {
        const bool none = fh_operand_form(o) == OperandForm::None;
        CHECK(none == (o == FH_HALT));
        CHECK(fh_opcode_has_operands(o) == (o != FH_HALT));
    }

    // FH_ILL carries nothing and reports nothing.
    CHECK(fh_operand_form(FH_ILL) == OperandForm::None);
    CHECK(!fh_opcode_has_operands(FH_ILL));

    // An opcode this build does not know still reports operands, so a `.fhex`
    // from a newer toolchain decodes its dst/src1 the same way. Changing this
    // changes the wire behaviour — see the note in the header.
    CHECK(fh_opcode_has_operands(200));
    CHECK(fh_operand_form(200) == OperandForm::None);
    CHECK(fh_opcode_mnemonic(200) == nullptr);

    // Spot-check the shapes that differ in ways a hand-written parser has
    // historically got wrong.
    CHECK(fh_operand_form(FH_SR_ADDP)   == OperandForm::PolyPoly);
    CHECK(fh_operand_form(FH_SR_MULPS)  == OperandForm::PolyScalar);
    CHECK(fh_operand_form(FH_SR_NTT)    == OperandForm::Ntt);
    CHECK(fh_operand_form(FH_SR_INTT)   == OperandForm::Ntt);
    CHECK(fh_operand_form(FH_SR_NEGP)   == OperandForm::UnaryMod);
    CHECK(fh_operand_form(FH_SR_PERMUTE) == OperandForm::UnaryMod);
    CHECK(fh_operand_form(FH_SR_AUTOMORPH_EVAL) == OperandForm::AutomorphEval);
    CHECK(fh_operand_form(FH_SR_AUTOMORPH_COEFF) == OperandForm::AutomorphCoeff);
    CHECK(fh_operand_form(FH_SR_ROT_AUTOMORPH_COEFF) ==
          OperandForm::RotAutomorphCoeff);

    // The `_NI` forms take no modulus. That is the distinction three separate
    // parsers each had to encode by hand.
    for (FhOpcode o : {FH_SR_ADDP_NI, FH_SR_SUBP_NI, FH_SR_MULP_NI})
        CHECK(fh_operand_form(o) == OperandForm::PolyPolyNI);
    for (FhOpcode o : {FH_SR_ADDPS_NI, FH_SR_SUBPS_NI, FH_SR_MULPS_NI,
                       FH_SR_ADDPS_COEFF_NI, FH_SR_SUBPS_COEFF_NI})
        CHECK(fh_operand_form(o) == OperandForm::PolyScalarNI);
    for (FhOpcode o : {FH_SR_NEGP_NI, FH_SR_FT, FH_SR_IFT})
        CHECK(fh_operand_form(o) == OperandForm::UnaryNoMod);

    // The coeff-mode integer forms DO take a modulus, unlike their _ni twins.
    CHECK(fh_operand_form(FH_SR_ADDPS_COEFF) == OperandForm::PolyScalar);
    CHECK(fh_operand_form(FH_SR_SUBPS_COEFF) == OperandForm::PolyScalar);

    return g_checks_failed == before;
}

bool test_accessors_are_constexpr() {
    std::cout << "\n[test] the accessors are usable at compile time"
              << std::endl;
    const int before = g_checks_failed;

    // Not decoration: the binary codec calls fh_opcode_has_operands() in
    // contexts where a runtime call would be a regression, and a
    // constexpr-callable table means a switch on a shape can be folded.
    static_assert(fh_opcode_has_operands(FH_SR_ADDP));
    static_assert(!fh_opcode_has_operands(FH_HALT));
    static_assert(!fh_opcode_has_operands(FH_ILL));
    static_assert(fh_operand_form(FH_SR_NTT) == OperandForm::Ntt);
    static_assert(fh_opcode_info(FH_SR_ADDP) != nullptr);
    static_assert(fh_opcode_info(FH_ILL) == nullptr);
    static_assert(fh_opcode_from_mnemonic("sr_addp").has_value());
    static_assert(!fh_opcode_from_mnemonic("nope").has_value());
    CHECK(true);  // reaching here means all of the above compiled

    return g_checks_failed == before;
}

bool test_instruction_defaults_and_equality() {
    std::cout << "\n[test] instruction defaults and equality" << std::endl;
    const int before = g_checks_failed;

    Instruction empty;
    CHECK(empty.opcode == FH_ILL);
    CHECK(empty.dest == 0 && empty.src1 == 0 && empty.src2 == 0);
    CHECK(!empty.immediate.has_value());
    CHECK(!empty.modulus.has_value());
    CHECK(!empty.modulus_is_literal);
    CHECK(!empty.omega.has_value());
    CHECK(!empty.mask.has_value());
    CHECK(!empty.logn.has_value());
    CHECK(!empty.k.has_value());
    CHECK(!empty.offset.has_value());
    CHECK(empty.line_number == 0);

    // Absent is not zero: zero is a legal address, immediate, and modulus
    // index, so a consumer must be able to tell "m=0" from "no modulus".
    Instruction with_zero_mod;
    with_zero_mod.modulus = 0;
    CHECK(with_zero_mod.modulus.has_value());
    CHECK(with_zero_mod != empty);

    // line_number is provenance, not content: the same instruction read from
    // text and from binary must compare equal even though only one has a line.
    Instruction a;
    a.opcode = FH_SR_ADDP;
    a.dest = 7; a.src1 = 3; a.src2 = 5;
    a.modulus = 1;
    Instruction b = a;
    b.line_number = 42;
    CHECK(a == b);

    // The literal flag is content: it is the one thing the text form cannot
    // express, so two instructions differing only in it are not equal.
    Instruction lit = a;
    lit.modulus_is_literal = true;
    CHECK(lit != a);

    return g_checks_failed == before;
}

bool test_operand_limits() {
    std::cout << "\n[test] operand limits" << std::endl;
    const int before = g_checks_failed;

    // Addresses are pre-compaction polynomial ids — a dense counter — and the
    // widest real trace measured reaches ~2^19, so 32 bits is ~9000x headroom.
    CHECK(kMaxAddress == 0xFFFFFFFFull);
    CHECK(kMaxAddress >= 472086);  // widest address seen in a real trace

    // The ISA's mod_index field is 8 bits; real traces carry ~40 moduli.
    CHECK(kMaxModulusIndex == 255);

    Instruction wide;
    wide.dest = static_cast<uint32_t>(kMaxAddress);
    CHECK(wide.dest == 0xFFFFFFFFu);

    return g_checks_failed == before;
}

}  // namespace

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "FHETCH IR — opcode vocabulary + instruction" << std::endl;
    std::cout << "========================================" << std::endl;

    int passed = 0;
    int failed = 0;

    if (test_table_covers_every_opcode()) passed++; else failed++;
    if (test_mnemonics_round_trip()) passed++; else failed++;
    if (test_operand_forms()) passed++; else failed++;
    if (test_accessors_are_constexpr()) passed++; else failed++;
    if (test_instruction_defaults_and_equality()) passed++; else failed++;
    if (test_operand_limits()) passed++; else failed++;

    // Reported, not asserted: the number is a consequence of the field set,
    // and pinning it would make every future field a test failure. It is
    // printed so a jump is noticed in review.
    std::cout << "\n  sizeof(Instruction) = " << sizeof(Instruction)
              << " bytes (" << kOpcodeCount << " opcodes in the table)"
              << std::endl;

    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << passed << " passed, " << failed << " failed"
              << std::endl;
    std::cout << "========================================" << std::endl;

    return failed > 0 ? 1 : 0;
}
