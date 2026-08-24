// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

#include "niobium/fhetch_writer.h"

#include <cstring>
#include <fstream>
#include <sstream>

namespace niobium::fhetch {
namespace {

void append_uint(std::string& out, uint64_t v) { out += std::to_string(v); }

void append_addr(std::string& out, uint32_t a) {
    out += '%';
    append_uint(out, a);
}

void append_named(std::string& out, const char* name, uint64_t v) {
    out += ", ";
    out += name;
    out += '=';
    append_uint(out, v);
}

/// `m=<N>`. The text cannot say whether the number is a table index or a
/// literal prime — that is the ambiguity the reader resolves by magnitude and
/// the binary form records with a flag. Both render the same way here, which
/// is why a round trip through text loses the distinction while a round trip
/// through `.fhex` keeps it.
void append_modulus(std::string& out, const Instruction& inst) {
    if (inst.modulus.has_value()) append_named(out, "m", *inst.modulus);
}

}  // namespace

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

void render_instruction(const Instruction& inst, std::string& out) {
    const char* mnemonic = fh_opcode_mnemonic(inst.opcode);
    if (mnemonic == nullptr) return;  // FH_ILL and unassigned render as nothing
    out += mnemonic;

    const OperandForm form = fh_operand_form(inst.opcode);
    if (form == OperandForm::None) return;

    out += ' ';
    append_addr(out, inst.dest);
    out += ", ";
    append_addr(out, inst.src1);

    switch (form) {
    case OperandForm::None:
        break;

    case OperandForm::PolyPoly:
        out += ", ";
        append_addr(out, inst.src2);
        append_modulus(out, inst);
        break;

    case OperandForm::PolyPolyNI:
        out += ", ";
        append_addr(out, inst.src2);
        break;

    case OperandForm::PolyScalar:
        out += ", ";
        append_uint(out, inst.immediate.value_or(0));
        append_modulus(out, inst);
        break;

    case OperandForm::PolyScalarNI:
        // std::to_string on a double gives 6 decimal places, which is what
        // every producer has always written — and why the text form cannot
        // round-trip an arbitrary double.
        out += ", ";
        out += std::to_string(inst.fp_immediate.value_or(0.0));
        break;

    case OperandForm::UnaryMod:
        append_modulus(out, inst);
        break;

    case OperandForm::UnaryNoMod:
        break;

    case OperandForm::Ntt:
        append_modulus(out, inst);
        // omega is a trailing extension: present when the recorder knew the
        // root of unity, absent when a consumer should derive one.
        if (inst.omega.has_value()) append_named(out, "omega", *inst.omega);
        break;

    case OperandForm::AutomorphEval:
        append_modulus(out, inst);
        if (inst.mask.has_value()) append_named(out, "mask", *inst.mask);
        if (inst.logn.has_value()) append_named(out, "logn", *inst.logn);
        if (inst.k.has_value()) append_named(out, "k", *inst.k);
        break;

    case OperandForm::AutomorphCoeff:
        append_named(out, "k", inst.k.value_or(0));
        append_modulus(out, inst);
        break;

    case OperandForm::RotAutomorphCoeff:
        append_named(out, "offset", inst.offset.value_or(0));
        append_modulus(out, inst);
        break;
    }
}

std::string render_instruction(const Instruction& inst) {
    std::string out;
    out.reserve(48);  // a typical line is 29-43 chars
    render_instruction(inst, out);
    return out;
}

std::string render_header(const TraceMetadata& metadata,
                          size_t instruction_count, size_t modulus_count) {
    const char* kRule = "# =========================================\n";
    std::ostringstream out;
    out << kRule << "# Niobium FHETCH Trace\n" << kRule;
    if (!metadata.program_name.empty()) {
        out << "# Program: " << metadata.program_name;
        if (!metadata.program_version.empty())
            out << " v" << metadata.program_version;
        out << "\n";
    }
    if (!metadata.description.empty())
        out << "# Description: " << metadata.description << "\n";
    if (!metadata.source_file.empty())
        out << "# Source: " << metadata.source_file << ":" << metadata.source_line
            << "\n";
    if (!metadata.build_timestamp.empty())
        out << "# Build: " << metadata.build_timestamp << "\n";
    out << "# Instruction Count: " << instruction_count << "\n";
    out << "# Modulus Count: " << modulus_count << "\n";
    if (metadata.generated_timestamp != 0)
        out << "# Generated: " << metadata.generated_timestamp << "\n";
    out << kRule;
    return out.str();
}

std::string render_modulus_table(const std::vector<uint64_t>& table) {
    std::ostringstream out;
    out << "modulus_count " << table.size() << "\n";
    for (size_t i = 0; i < table.size(); ++i) {
        out << "m[" << i << "] 0x" << std::hex << std::uppercase << table[i]
            << std::dec << std::nouppercase << "\n";
    }
    return out.str();
}

bool write_fhetch_text(const std::filesystem::path& path, const Program& program) {
    std::ofstream out(path);
    if (!out.is_open()) return false;

    out << render_header(program.metadata, program.instructions.size(),
                         program.modulus_table.size());
    out << "\n# Modulus Table\n" << render_modulus_table(program.modulus_table);
    out << "\n# Instructions\n";

    // Annotations are interleaved by position, not gathered at one end: a
    // recorded trace carries them between instructions and a round trip has to
    // put them back where they were. They arrive in order, so one cursor walks
    // them alongside the instruction stream.
    size_t next_annotation = 0;
    const auto& notes = program.annotations;
    auto flush_annotations_before = [&](size_t instruction_index) {
        while (next_annotation < notes.size() &&
               notes[next_annotation].after_instruction <= instruction_index) {
            out << "# " << notes[next_annotation].text << "\n";
            ++next_annotation;
        }
    };

    // One buffer for the whole stream: rendering appends, so no per-instruction
    // allocation. This is the shape a streaming writer needs.
    std::string line;
    for (size_t i = 0; i < program.instructions.size(); ++i) {
        flush_annotations_before(i);
        line.clear();
        render_instruction(program.instructions[i], line);
        if (line.empty()) continue;
        out << line << "\n";
    }
    // Anything positioned after the last instruction.
    while (next_annotation < notes.size()) {
        out << "# " << notes[next_annotation].text << "\n";
        ++next_annotation;
    }
    return out.good();
}

// ---------------------------------------------------------------------------
// Binary
// ---------------------------------------------------------------------------

bool write_fhetch_binary(const std::filesystem::path& path, const Program& program,
                         FhetchSpec spec) {
    std::vector<FhetchRecord> records;
    records.reserve(program.instructions.size());
    for (const Instruction& inst : program.instructions)
        records.push_back(to_record(inst));

    TraceMetadata metadata = program.metadata;
    metadata.instruction_count = static_cast<int>(program.instructions.size());
    metadata.modulus_chain = program.modulus_table;
    metadata.modulus_chain_length = static_cast<int>(program.modulus_table.size());

    return write_fhetch_program(path.string(), metadata, program.modulus_table,
                                records, spec);
}

}  // namespace niobium::fhetch
