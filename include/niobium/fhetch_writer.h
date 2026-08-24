// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

/**
 * @file fhetch_writer.h
 * @brief Render the FHETCH IR back out — as `.fhetch` text or as `.fhex`.
 *
 * The writer and the reader are two readings of one table: operand layout
 * comes from `fh_operand_form()`, so they cannot disagree about what an
 * instruction looks like. Anything the reader accepts, the writer emits in
 * canonical form, and reading that back gives an equal `Instruction`.
 *
 * ### Canonical operand order
 *
 * The specification defines no textual syntax, so this is a choice rather than
 * a requirement. Positional operands first, then the opcode's named operands,
 * then the trailing optional extensions:
 *
 *     sr_addp                %dest, %src1, %src2, m=<N>
 *     sr_addps               %dest, %src1, <imm>, m=<N>
 *     sr_addp_ni             %dest, %src1, %src2
 *     sr_negp                %dest, %src1, m=<N>
 *     sr_ft                  %dest, %src1
 *     sr_ntt                 %dest, %src1, m=<N>[, omega=<V>]
 *     sr_automorph_eval      %dest, %src1, m=<N>[, mask=<M>][, logn=<L>], k=<K>
 *     sr_automorph_coeff     %dest, %src1, k=<K>, m=<N>
 *     sr_rot_automorph_coeff %dest, %src1, offset=<O>, m=<N>
 *     halt
 *
 * `sr_automorph_eval` is the one place where the existing producers disagreed:
 * the OpenFHE recorder wrote `m=`, `mask=`, `logn=`, `k=`, while the FHETCH API
 * wrote `k=`, `m=` and omitted mask/logn. Every reader treats those operands as
 * order-independent, which is why the split went unnoticed. The recorder's
 * order is adopted here because it is the one that carries every operand.
 *
 * Numbers are rendered in decimal, which is what every producer has always
 * written. The reader also accepts `0x`, so a hand-written trace can use hex,
 * but nothing emits it.
 */

#pragma once

#include "niobium/fhetch_encoding.h"
#include "niobium/fhetch_ir.h"

#include <filesystem>
#include <string>
#include <vector>

namespace niobium::fhetch {

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

/// Append one instruction's line to `out`, without the newline.
///
/// Appending rather than returning is what lets a caller stream a program
/// through a single reused buffer instead of allocating per instruction.
void render_instruction(const Instruction& inst, std::string& out);

/// Convenience form. Prefer the appending overload in a loop.
std::string render_instruction(const Instruction& inst);

/// The leading `#` comment block, newline-terminated. `instruction_count` and
/// `modulus_count` are passed rather than read from `metadata` because they
/// describe what is actually being written, which is the authority — a
/// metadata block carried over from an earlier program may disagree.
std::string render_header(const TraceMetadata& metadata,
                          size_t instruction_count, size_t modulus_count);

/// The `modulus_count` line and the `m[i] 0xHEX` entries, newline-terminated.
std::string render_modulus_table(const std::vector<uint64_t>& table);

/// Write a whole program as `.fhetch`. Returns false if the file could not be
/// opened; nothing is partially written in that case.
bool write_fhetch_text(const std::filesystem::path& path, const Program& program);

// ---------------------------------------------------------------------------
// Binary
// ---------------------------------------------------------------------------
// to_record() / from_record() live with the record itself, in
// fhetch_encoding.h — both the reader and the writer need them, and keeping
// the pair together is what stops the two directions drifting.

/// Write a whole program as `.fhex`, beside or instead of the text.
bool write_fhetch_binary(const std::filesystem::path& path, const Program& program,
                         FhetchSpec spec = kFhetchSpec);

}  // namespace niobium::fhetch
