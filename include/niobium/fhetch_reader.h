// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

/**
 * @file fhetch_reader.h
 * @brief Read a `.fhetch` text trace into the FHETCH IR.
 *
 * One reader, driven by `kOpcodeTable`'s operand shapes rather than by a
 * hand-written switch, so it cannot disagree with the writer about what an
 * instruction looks like.
 *
 * The specification defines no textual syntax, so the grammar below is this
 * implementation's. It is what the writer emits:
 *
 *     # <header key>: <value>      leading metadata block
 *     modulus_count <N>
 *     m[<i>] 0x<HEX>
 *     <mnemonic> %<dest>, %<src1>[, %<src2> | , <imm>][, m=<N>][, <name>=<V>]
 *     halt
 *
 * Operands are addresses, not SSA values: a program may write an address more
 * than once and a destination may alias a source. Anything that needs single
 * assignment builds it itself.
 *
 * Reading is **strict where silence would corrupt** and lenient elsewhere. An
 * address too wide for the IR, or a modulus index outside the table, is an
 * error rather than a truncation — the previous reader truncated into
 * `uint32_t` and produced a different, valid-looking operand. A line whose
 * opcode is unknown is a warning and is skipped, which is what a
 * forward-compatible reader should do.
 */

#pragma once

#include "niobium/fhetch_encoding.h"
#include "niobium/fhetch_ir.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace niobium::fhetch {

/// One thing the reader has to say about a line.
struct ReadDiagnostic {
    int line_number = 0;
    std::string message;
};

/// Outcome of a read. `ok` is false only when something was wrong in a way
/// that would have silently changed the program — a malformed modulus table,
/// or an operand that does not fit. Unknown opcodes and unparseable lines are
/// warnings: they are skipped, and the caller decides whether that matters.
struct ReadResult {
    bool ok = false;
    Program program;
    std::vector<ReadDiagnostic> errors;
    std::vector<ReadDiagnostic> warnings;

    /// First error's message, or empty. For callers that just want to log
    /// something and give up.
    std::string first_error() const {
        return errors.empty() ? std::string{} : errors.front().message;
    }
};

/// Parse a `.fhetch` trace from memory.
///
/// `m=<N>` is ambiguous in the text — a writer renders both a modulus-table
/// index and a literal prime the same way, because the text form has no way to
/// distinguish them. It is resolved by magnitude: `N` is an index when it is
/// less than the modulus table's size, and a literal prime otherwise. That is
/// sound rather than a guess, since chain moduli are 50-60 bit primes and a
/// table has tens of entries, so the ranges cannot overlap. With no table
/// present, every modulus is a literal. `Instruction::modulus_is_literal`
/// records which reading applied, so a consumer never has to repeat it.
ReadResult read_fhetch_text(std::string_view text);

/// Decode a `.fhex` byte stream into the same Program a text trace gives.
///
/// The binary form is unambiguous where the text is not — its records flag a
/// literal modulus, and it carries `omega`/`mask`/`logn` — so nothing has to be
/// inferred here.
ReadResult read_fhetch_binary(const std::byte* bytes, size_t size);

/// Read a program from a file, in whichever serialization it holds.
///
/// The form is detected from the file's own magic bytes rather than its name,
/// so a `.fhetch` that happens to contain binary still reads. A file that
/// cannot be opened is an error, not a warning.
ReadResult read_fhetch_file(const std::filesystem::path& path);

}  // namespace niobium::fhetch
