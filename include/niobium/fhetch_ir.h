// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

/**
 * @file fhetch_ir.h
 * @brief The in-memory form of a FHETCH program.
 *
 * FHETCH is an IR, not an API that performs operations. A program is a
 * sequence of `Instruction`s over polynomial addresses; the text (`.fhetch`)
 * and binary (`.fhex`) forms are serializations of it, and the builders
 * (the FHETCH API, the OpenFHE recorder) produce it. Readers, writers,
 * the simulator, and any consumer downstream all speak this one type.
 *
 * The specification (fhetch.org) defines data types, baseline instructions,
 * and gadgets. It defines no memory model and no textual syntax, so the
 * address operands here and the `%N` / `m=N` notation are this
 * implementation's, not the specification's.
 *
 * One table, `kOpcodeTable`, is the single source of truth for the opcode
 * vocabulary: it carries each opcode's mnemonic and its operand shape, and
 * every accessor below is derived from it. A reader and a writer that both
 * drive off `fh_operand_form()` cannot disagree about what an instruction
 * looks like — which three hand-written parser switches previously did.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace niobium::fhetch {

// ---------------------------------------------------------------------------
// Opcodes
// ---------------------------------------------------------------------------

/// FHETCH mnemonics, numbered stably — these values are on the wire (see
/// fhetch_encoding.h), so existing entries must never be renumbered.
///
/// The vocabulary covers every mnemonic the recorder can emit, so any FHETCH
/// program is representable. A given consumer may accept only a subset.
enum FhOpcode : uint8_t {
    FH_ILL                     = 0,
    FH_SR_ADDP                 = 1,
    FH_SR_SUBP                 = 2,
    FH_SR_MULP                 = 3,
    FH_SR_ADDPS                = 4,
    FH_SR_SUBPS                = 5,
    FH_SR_MULPS                = 6,
    FH_SR_ADDPS_COEFF          = 7,
    FH_SR_SUBPS_COEFF          = 8,
    FH_SR_NEGP                 = 9,
    FH_SR_NTT                  = 10,
    FH_SR_INTT                 = 11,
    FH_SR_PERMUTE              = 12,
    FH_SR_AUTOMORPH_EVAL       = 13,
    FH_SR_AUTOMORPH_COEFF      = 14,
    FH_SR_ROT_AUTOMORPH_COEFF  = 15,
    FH_SR_ADDP_NI              = 16,
    FH_SR_SUBP_NI              = 17,
    FH_SR_MULP_NI              = 18,
    FH_SR_ADDPS_NI             = 19,
    FH_SR_SUBPS_NI             = 20,
    FH_SR_MULPS_NI             = 21,
    FH_SR_ADDPS_COEFF_NI       = 22,
    FH_SR_SUBPS_COEFF_NI       = 23,
    FH_SR_NEGP_NI              = 24,
    FH_SR_FT                   = 25,
    FH_SR_IFT                  = 26,
    FH_HALT                    = 27,
};

/// Which operands an opcode carries, and therefore how its text line is laid
/// out. Named for the shape rather than for one member of it, since several
/// opcodes share each shape.
///
/// `_NI` forms are the non-integer (floating-point) variants. They take no
/// modulus at all — a real distinction, not an omission: there is no ring to
/// reduce into.
enum class OperandForm : uint8_t {
    None,               ///< halt
    PolyPoly,           ///< dest, src1, src2, m=
    PolyPolyNI,         ///< dest, src1, src2
    PolyScalar,         ///< dest, src1, imm, m=
    PolyScalarNI,       ///< dest, src1, imm
    UnaryMod,           ///< dest, src1, m=
    UnaryNoMod,         ///< dest, src1
    Ntt,                ///< dest, src1, m= [, omega=]
    AutomorphEval,      ///< dest, src1, m= [, mask=, logn=], k=
    AutomorphCoeff,     ///< dest, src1, k=, m=
    RotAutomorphCoeff,  ///< dest, src1, offset=, m=
};

/// One row of the opcode vocabulary. Adding an opcode means adding a row
/// here and nothing else — the mnemonic table, the reverse lookup, and the
/// operand shape all read from this.
struct OpcodeInfo {
    FhOpcode    opcode;
    const char* mnemonic;
    OperandForm form;
};

/// The vocabulary. Order is irrelevant to correctness; it follows the opcode
/// numbering so a reader can eyeball it against the enum above.
inline constexpr OpcodeInfo kOpcodeTable[] = {
    {FH_SR_ADDP,                "sr_addp",                OperandForm::PolyPoly},
    {FH_SR_SUBP,                "sr_subp",                OperandForm::PolyPoly},
    {FH_SR_MULP,                "sr_mulp",                OperandForm::PolyPoly},
    {FH_SR_ADDPS,               "sr_addps",               OperandForm::PolyScalar},
    {FH_SR_SUBPS,               "sr_subps",               OperandForm::PolyScalar},
    {FH_SR_MULPS,               "sr_mulps",               OperandForm::PolyScalar},
    {FH_SR_ADDPS_COEFF,         "sr_addps_coeff",         OperandForm::PolyScalar},
    {FH_SR_SUBPS_COEFF,         "sr_subps_coeff",         OperandForm::PolyScalar},
    {FH_SR_NEGP,                "sr_negp",                OperandForm::UnaryMod},
    {FH_SR_NTT,                 "sr_ntt",                 OperandForm::Ntt},
    {FH_SR_INTT,                "sr_intt",                OperandForm::Ntt},
    {FH_SR_PERMUTE,             "sr_permute",             OperandForm::UnaryMod},
    {FH_SR_AUTOMORPH_EVAL,      "sr_automorph_eval",      OperandForm::AutomorphEval},
    {FH_SR_AUTOMORPH_COEFF,     "sr_automorph_coeff",     OperandForm::AutomorphCoeff},
    {FH_SR_ROT_AUTOMORPH_COEFF, "sr_rot_automorph_coeff", OperandForm::RotAutomorphCoeff},
    {FH_SR_ADDP_NI,             "sr_addp_ni",             OperandForm::PolyPolyNI},
    {FH_SR_SUBP_NI,             "sr_subp_ni",             OperandForm::PolyPolyNI},
    {FH_SR_MULP_NI,             "sr_mulp_ni",             OperandForm::PolyPolyNI},
    {FH_SR_ADDPS_NI,            "sr_addps_ni",            OperandForm::PolyScalarNI},
    {FH_SR_SUBPS_NI,            "sr_subps_ni",            OperandForm::PolyScalarNI},
    {FH_SR_MULPS_NI,            "sr_mulps_ni",            OperandForm::PolyScalarNI},
    {FH_SR_ADDPS_COEFF_NI,      "sr_addps_coeff_ni",      OperandForm::PolyScalarNI},
    {FH_SR_SUBPS_COEFF_NI,      "sr_subps_coeff_ni",      OperandForm::PolyScalarNI},
    {FH_SR_NEGP_NI,             "sr_negp_ni",             OperandForm::UnaryNoMod},
    {FH_SR_FT,                  "sr_ft",                  OperandForm::UnaryNoMod},
    {FH_SR_IFT,                 "sr_ift",                 OperandForm::UnaryNoMod},
    {FH_HALT,                   "halt",                   OperandForm::None},
};

/// FH_ILL is deliberately absent from the table: it names "no opcode", so it
/// has no mnemonic and no operand shape. Every other enumerator has a row.
inline constexpr size_t kOpcodeCount = sizeof(kOpcodeTable) / sizeof(kOpcodeTable[0]);
static_assert(kOpcodeCount == FH_HALT,
              "every opcode but FH_ILL needs a row in kOpcodeTable");

/// The table row for an opcode, or nullptr for FH_ILL and unassigned values.
inline constexpr const OpcodeInfo* fh_opcode_info(uint8_t opcode) {
    for (const auto& row : kOpcodeTable)
        if (static_cast<uint8_t>(row.opcode) == opcode) return &row;
    return nullptr;
}

/// Mnemonic string for an opcode — the exact token the `.fhetch` text form
/// uses, so an instruction can be rendered back to its line verbatim.
/// Returns nullptr for FH_ILL and unassigned values.
inline constexpr const char* fh_opcode_mnemonic(uint8_t opcode) {
    const OpcodeInfo* info = fh_opcode_info(opcode);
    return info ? info->mnemonic : nullptr;
}

/// The opcode a mnemonic names, or nullopt when the token is not one. The
/// reverse of fh_opcode_mnemonic(), so a reader and a writer agree by
/// construction rather than by two tables being kept in step.
inline constexpr std::optional<FhOpcode> fh_opcode_from_mnemonic(
    std::string_view mnemonic) {
    for (const auto& row : kOpcodeTable)
        if (mnemonic == row.mnemonic) return row.opcode;
    return std::nullopt;
}

/// Operand shape for an opcode. FH_ILL and unassigned values report None,
/// which is what a consumer that cannot identify an instruction should
/// assume it can read off it: nothing.
inline constexpr OperandForm fh_operand_form(uint8_t opcode) {
    const OpcodeInfo* info = fh_opcode_info(opcode);
    return info ? info->form : OperandForm::None;
}

/// Whether an opcode carries a destination and first source.
///
/// Unassigned values report true, matching the binary decoder's assumption
/// that an unrecognized record still has the two operands every real opcode
/// but `halt` has. Changing that would change how a `.fhex` written by a
/// newer toolchain decodes here, so it is behaviour, not a detail.
inline constexpr bool fh_opcode_has_operands(uint8_t opcode) {
    if (opcode == FH_ILL) return false;
    const OpcodeInfo* info = fh_opcode_info(opcode);
    return info ? info->form != OperandForm::None : true;
}

// ---------------------------------------------------------------------------
// Instruction
// ---------------------------------------------------------------------------

/// One FHETCH instruction.
///
/// Operands are **addresses**, not SSA values: a program may write the same
/// address more than once, and a destination may alias a source. That is not
/// a stylistic choice — the OpenFHE recorder observes a library that mutates
/// in place, so its traces genuinely are address-based (a measured CKKS
/// bootstrap writes 819,380 instructions into 187,540 addresses, ~70% of them
/// read-modify-write). Consumers that need single-assignment build it
/// themselves.
///
/// Absent operands are `nullopt` rather than zero, because zero is a legal
/// address, immediate, and modulus index. `line_number` is the only
/// diagnostic carried: an instruction deliberately does **not** hold its own
/// source text, which is the reader's and writer's business alone.
struct Instruction {
    FhOpcode opcode = FH_ILL;

    /// Polynomial addresses. 32-bit: these are pre-compaction polynomial ids
    /// — a dense counter from zero, not a machine address — and the widest
    /// measured real trace reaches ~2^19. A reader that meets a wider value
    /// must reject it rather than truncate.
    uint32_t dest = 0;
    uint32_t src1 = 0;
    uint32_t src2 = 0;

    /// Scalar immediate, for the integer poly-scalar forms.
    std::optional<uint64_t> immediate;

    /// Scalar immediate for the *non-integer* poly-scalar forms
    /// (`OperandForm::PolyScalarNI`). A separate field because those operands
    /// are genuinely floating point — the specification's NonInteger scalars —
    /// and squeezing them into the integer one would misrepresent them.
    ///
    /// The text form renders this with 6 decimal places, so it does **not**
    /// round-trip an arbitrary double. The binary form carries the exact bit
    /// pattern and does. That asymmetry is the text form's, not the IR's.
    std::optional<double> fp_immediate;

    /// Modulus, absent on the `_NI` forms and on `halt`. Normally an index
    /// into the program's modulus table; `modulus_is_literal` marks the case
    /// where a writer had no table entry and emitted the prime itself. The
    /// text form renders both as a bare `m=<number>` and so cannot tell them
    /// apart — only this flag can.
    std::optional<uint64_t> modulus;
    bool modulus_is_literal = false;

    /// Root of unity for sr_ntt / sr_intt. A Niobium extension: the
    /// specification defines the transforms abstractly and says nothing about
    /// how omega is obtained. When present a consumer should prefer it over
    /// deriving one, so dropping it changes results rather than saving space.
    std::optional<uint64_t> omega;

    /// sr_automorph_eval's mask and log2(ring dimension).
    std::optional<uint64_t> mask;
    std::optional<uint8_t>  logn;

    /// Automorphism index, and the rotation offset of
    /// sr_rot_automorph_coeff. Distinct fields because they are distinct
    /// operands, even though no instruction carries both.
    std::optional<uint64_t> k;
    std::optional<uint64_t> offset;

    /// 1-based line in the source text, or 0 when there was none (a builder
    /// produced this instruction directly). For diagnostics only.
    int line_number = 0;

    friend bool operator==(const Instruction& a, const Instruction& b) {
        return a.opcode == b.opcode && a.dest == b.dest && a.src1 == b.src1 &&
               a.src2 == b.src2 && a.immediate == b.immediate &&
               a.fp_immediate == b.fp_immediate &&
               a.modulus == b.modulus &&
               a.modulus_is_literal == b.modulus_is_literal &&
               a.omega == b.omega && a.mask == b.mask && a.logn == b.logn &&
               a.k == b.k && a.offset == b.offset;
        // line_number is provenance, not content: the same instruction read
        // from text and from binary must compare equal.
    }
    friend bool operator!=(const Instruction& a, const Instruction& b) {
        return !(a == b);
    }
};

/// Largest address an Instruction can hold. A reader rejects anything wider
/// instead of truncating it into a different, valid-looking address.
inline constexpr uint64_t kMaxAddress = UINT32_MAX;

/// Largest modulus *index* the Niobium ISA can encode (its mod_index field is
/// 8 bits). A literal modulus is unaffected — it is a prime, not an index.
/// Exposed so a reader can reject an out-of-range index at the door rather
/// than letting it fail later during encoding.
inline constexpr uint64_t kMaxModulusIndex = 255;


// ---------------------------------------------------------------------------
// Program
// ---------------------------------------------------------------------------

/// Program and build metadata.
///
/// Both serializations carry this: the text form writes it as the leading `#`
/// comment block, the binary form as TLV entries (one per FhMetaTag, see
/// fhetch_encoding.h). It lives here rather than with either encoding because
/// it describes the program, not the file. Every field default-constructs to
/// the value a writer treats as "absent", which is why empty strings and
/// zeros are omitted rather than written as empty entries.
struct TraceMetadata {
    std::string program_name;
    /// Version the text header renders as `# Program: <name> v<version>`.
    /// Held apart from the name so neither side has to glue the two together
    /// or split them back out.
    std::string program_version;
    std::string trace_file;
    std::string description;
    std::string source_file;
    int source_line = 0;
    std::string build_timestamp;
    int instruction_count = 0;
    uint64_t generated_timestamp = 0;

    // Crypto context
    std::string scheme;
    int ring_dimension = 0;
    int multiplicative_depth = 0;
    std::string security_level;
    std::vector<uint64_t> modulus_chain;
    int modulus_chain_length = 0;
};

/// A comment the reader kept but did not interpret.
///
/// Comments are legal in the text form and are handled entirely by the
/// reader — they are never instructions. Most are prose, but two carry
/// structured data the recorder smuggled through them (`# output <name> %addr`
/// and `# switchmodulus ...`). Whether anything depends on those is unsettled,
/// so they are preserved verbatim here rather than discarded, which keeps the
/// question open without giving comments a place in the instruction stream.
struct Annotation {
    int line_number = 0;
    std::string text;   ///< comment body, with the leading '#' and space removed

    /// How many instructions precede this comment. A writer emits the
    /// annotation once that many instructions have been written, which is what
    /// makes a read/write round trip reproduce the file rather than gather all
    /// the comments at one end. Recorded traces interleave heavily — a measured
    /// CKKS bootstrap carries 82,719 comments between its instructions — so
    /// position is content, not decoration.
    size_t after_instruction = 0;
};

/// A whole FHETCH program in memory — what a reader produces, a writer
/// consumes, and a consumer walks. Source-format-independent by
/// construction: the same program read from `.fhetch` and from `.fhex` gives
/// equal `instructions`.
struct Program {
    TraceMetadata metadata;
    /// Moduli the program's `m=<index>` operands refer to. Index 0 is by
    /// convention the copy/zero-init sentinel (0xFFFFFFFFFFFFFFFF), not a
    /// prime.
    std::vector<uint64_t> modulus_table;
    std::vector<Instruction> instructions;
    std::vector<Annotation> annotations;
};

}  // namespace niobium::fhetch
