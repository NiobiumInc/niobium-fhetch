// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0.
//
// Implementation of niobium/fhetch_parser.h.
//
// Reading is not this file's job. The shared reader turns a `.fhetch` or
// `.fhex` file into a Program, and what remains here is the only part that
// was ever specific to driving: which API call each opcode maps to, and how
// a source address in the file becomes a Polynomial.
//
// Addresses in the input file are opaque — they are keys into a map that
// yields a Polynomial object; the FHETCH API then assigns its own synthetic
// address to the result, which the driver stores back in the map under the
// input file's destination address.

#include "niobium/fhetch_parser.h"
#include "niobium/fhetch_api.h"
#include "niobium/fhetch_reader.h"
#include "niobium/compiler.h"
#include "compiler_internal.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace niobium::fhetch {

namespace {

// ---------------------------------------------------------------------------
// Driver state
// ---------------------------------------------------------------------------

struct Driver {
    uint64_t ring_dim;
    DriveStats& stats;
    const DriveInputs& inputs;
    const DriveOutputs& outputs;

    // Input-file address -> Polynomial
    std::unordered_map<uint64_t, Polynomial> polys;

    Driver(uint64_t rd, DriveStats& s,
           const DriveInputs& in, const DriveOutputs& out)
        : ring_dim(rd), stats(s), inputs(in), outputs(out) {}

    // Get-or-create a source polynomial. Reads before writes are treated as
    // live-in inputs: if the caller supplied real values for the address via
    // DriveInputs, we use those AND go straight to the Compiler's
    // store_input_element() so the known (values, modulus) pair lands in
    // captured_inputs at the Polynomial's synthetic FHETCH address. If no
    // caller-provided data is available, fall back to a zero-filled
    // placeholder tagged via the normal tag_input() path — whose sync hook
    // infers the modulus from the first sr_* op that uses the address.
    Polynomial& get_or_create_src(uint64_t file_addr) {
        auto it = polys.find(file_addr);
        if (it != polys.end()) return it->second;

        auto in_it = inputs.data.find(file_addr);
        if (in_it != inputs.data.end() && !in_it->second.values.empty()) {
            auto p = Polynomial::from_data(in_it->second.values, ring_dim,
                                          Format::Evaluation);
            niobium::compiler().store_input_element(
                "in_" + std::to_string(file_addr), niobium::CapturedKind::SRP,
                /*starts_new_element=*/false, niobium::detail::polynomial_address(p),
                in_it->second.modulus, in_it->second.values);
            stats.inputs_materialized++;
            auto [ins, _] = polys.emplace(file_addr, std::move(p));
            return ins->second;
        }
        auto p = Polynomial::zeros(ring_dim, Format::Evaluation);
        tag_input("in_" + std::to_string(file_addr), p);
        auto [ins, _] = polys.emplace(file_addr, std::move(p));
        return ins->second;
    }

    // Source operand of a zero-init (sr_mulps imm==0): the op reads
    // nothing, so never consult DriveInputs and never tag an input —
    // just hand back a hollow placeholder for the API call.
    Polynomial& get_or_create_zero_src(uint64_t file_addr) {
        auto it = polys.find(file_addr);
        if (it != polys.end()) return it->second;
        auto [ins, _] = polys.emplace(
            file_addr, Polynomial::zeros(ring_dim, Format::Evaluation));
        return ins->second;
    }

    // Store the FHETCH API's output under the input file's destination
    // address. Output probes are tagged at the very end of the trace
    // (finalize_outputs()) so the *final* polynomial at each source-file
    // address is what gets sent to the simulator — not intermediates from
    // copy instructions that happen to share the same destination address.
    void put(uint64_t file_dst, Polynomial r) {
        polys[file_dst] = std::move(r);
    }

    // Called once at end of trace. For each source-file address that the
    // caller marked as an output probe, tag the *last* Polynomial stored at
    // that address, ordered by poly_index so the resulting captured_outputs
    // entry lists addresses in ciphertext-tower order.
    void finalize_outputs() {
        if (outputs.map.empty()) return;
        std::vector<std::pair<uint64_t, DriveOutputs::Tag>> ordered(
            outputs.map.begin(), outputs.map.end());
        std::sort(ordered.begin(), ordered.end(),
                  [](const auto& a, const auto& b) {
                      if (a.second.name != b.second.name)
                          return a.second.name < b.second.name;
                      return a.second.poly_index < b.second.poly_index;
                  });
        for (const auto& [file_addr, tag] : ordered) {
            auto it = polys.find(file_addr);
            if (it == polys.end()) continue;
            tag_output(tag.name, it->second);
            stats.outputs_tagged++;
        }
    }

    // Resolve an instruction's modulus to a value. A literal carries its own
    // prime; an index has to be in the table. An index that is not used to
    // return 0 here, which meant the arithmetic below ran modulo zero on a
    // trace we had mis-read — so it is an error now, and the instruction is
    // skipped rather than replayed against a nonsense modulus.
    bool resolve(const Instruction& inst, uint64_t& out) {
        if (!inst.modulus.has_value()) { out = 0; return true; }
        if (inst.modulus_is_literal) { out = *inst.modulus; return true; }
        const uint64_t idx = *inst.modulus;
        if (idx >= stats.modulus_table.size()) {
            stats.errors.push_back(
                "line " + std::to_string(inst.line_number)
                + ": modulus index " + std::to_string(idx)
                + " is outside the table of "
                + std::to_string(stats.modulus_table.size()) + " entries");
            return false;
        }
        out = stats.modulus_table[idx];
        return true;
    }
};

// Drive one instruction through the FHETCH API. Returns false if it was
// skipped (the only reason today is an unresolvable modulus).
bool drive_instruction(const Instruction& inst, Driver& drv) {
    uint64_t q = 0;
    if (!drv.resolve(inst, q)) return false;

    const uint64_t d = inst.dest;
    const uint64_t s1 = inst.src1;
    const uint64_t s2 = inst.src2;

    switch (inst.opcode) {
    case FH_HALT:
        halt();
        return true;

    case FH_SR_ADDP:
    case FH_SR_SUBP:
    case FH_SR_MULP: {
        const Polynomial& a = drv.get_or_create_src(s1);
        const Polynomial& b = drv.get_or_create_src(s2);
        Polynomial r = (inst.opcode == FH_SR_ADDP) ? sr_addp(a, b, q)
                     : (inst.opcode == FH_SR_SUBP) ? sr_subp(a, b, q)
                                                   : sr_mulp(a, b, q);
        drv.put(d, std::move(r));
        return true;
    }

    case FH_SR_ADDPS:
    case FH_SR_SUBPS:
    case FH_SR_MULPS:
    case FH_SR_ADDPS_COEFF:
    case FH_SR_SUBPS_COEFF: {
        const uint64_t imm = inst.immediate.value_or(0);
        // sr_mulps with imm==0 is a zero-init: it writes zeros and reads
        // nothing (the simulator's classify_uses agrees). Do not treat its
        // source operand as a live-in input — the recorded idiom aliases
        // src to dest, and registering a placeholder input per zero-init
        // materializes hundreds of thousands of dense zero polys at sync.
        const Polynomial& a = (inst.opcode == FH_SR_MULPS && imm == 0)
                                  ? drv.get_or_create_zero_src(s1)
                                  : drv.get_or_create_src(s1);
        Scalar s = Scalar::from_int(imm);
        Polynomial r;
        switch (inst.opcode) {
            case FH_SR_ADDPS:       r = sr_addps(a, s, q);       break;
            case FH_SR_SUBPS:       r = sr_subps(a, s, q);       break;
            case FH_SR_MULPS:       r = sr_mulps(a, s, q);       break;
            case FH_SR_ADDPS_COEFF: r = sr_addps_coeff(a, s, q); break;
            case FH_SR_SUBPS_COEFF: r = sr_subps_coeff(a, s, q); break;
            default: break;
        }
        drv.put(d, std::move(r));
        return true;
    }

    case FH_SR_NEGP:
        drv.put(d, sr_negp(drv.get_or_create_src(s1), q));
        return true;

    case FH_SR_NTT:
    case FH_SR_INTT: {
        // omega is recovered by the reader but the API derives its own root,
        // so there is nowhere to put it. Dropping it here is deliberate.
        const Polynomial& a = drv.get_or_create_src(s1);
        drv.put(d, (inst.opcode == FH_SR_NTT) ? sr_ntt(a, q) : sr_intt(a, q));
        return true;
    }

    case FH_SR_PERMUTE:
        // The trace doesn't preserve srcs/signs — feed empties so the API
        // emits a permute placeholder with the correct dst/src/mod wiring.
        drv.put(d, sr_permute(drv.get_or_create_src(s1), {}, {}, q));
        return true;

    case FH_SR_AUTOMORPH_EVAL:
        // The modulus is not passed on: the recorded op carries the copy
        // sentinel and the API derives what it needs from the polynomial.
        drv.put(d, sr_automorph_eval(drv.get_or_create_src(s1),
                                     inst.k.value_or(1)));
        return true;

    case FH_SR_AUTOMORPH_COEFF:
        drv.put(d, sr_automorph_coeff(drv.get_or_create_src(s1),
                                      inst.k.value_or(0), q));
        return true;

    case FH_SR_ROT_AUTOMORPH_COEFF:
        drv.put(d, sr_rot_automorph_coeff(drv.get_or_create_src(s1),
                                          inst.offset.value_or(0), q));
        return true;

    // Non-integer variants — no modulus.
    case FH_SR_ADDP_NI:
    case FH_SR_SUBP_NI:
    case FH_SR_MULP_NI: {
        const Polynomial& a = drv.get_or_create_src(s1);
        const Polynomial& b = drv.get_or_create_src(s2);
        Polynomial r = (inst.opcode == FH_SR_ADDP_NI) ? sr_addp_ni(a, b)
                     : (inst.opcode == FH_SR_SUBP_NI) ? sr_subp_ni(a, b)
                                                      : sr_mulp_ni(a, b);
        drv.put(d, std::move(r));
        return true;
    }

    case FH_SR_ADDPS_NI:
    case FH_SR_SUBPS_NI:
    case FH_SR_MULPS_NI:
    case FH_SR_ADDPS_COEFF_NI:
    case FH_SR_SUBPS_COEFF_NI: {
        const Polynomial& a = drv.get_or_create_src(s1);
        // These take a NonInteger scalar. Building it with from_int was wrong
        // twice over: the operand is a double, and the previous reader parsed
        // it as an integer, which truncated "5.000000" to 5.
        Scalar s = Scalar::from_double(inst.fp_immediate.value_or(0.0));
        Polynomial r;
        switch (inst.opcode) {
            case FH_SR_ADDPS_NI:       r = sr_addps_ni(a, s);       break;
            case FH_SR_SUBPS_NI:       r = sr_subps_ni(a, s);       break;
            case FH_SR_MULPS_NI:       r = sr_mulps_ni(a, s);       break;
            case FH_SR_ADDPS_COEFF_NI: r = sr_addps_coeff_ni(a, s); break;
            case FH_SR_SUBPS_COEFF_NI: r = sr_subps_coeff_ni(a, s); break;
            default: break;
        }
        drv.put(d, std::move(r));
        return true;
    }

    case FH_SR_NEGP_NI:
    case FH_SR_FT:
    case FH_SR_IFT: {
        const Polynomial& a = drv.get_or_create_src(s1);
        Polynomial r = (inst.opcode == FH_SR_NEGP_NI) ? sr_negp_ni(a)
                     : (inst.opcode == FH_SR_FT)      ? sr_ft(a)
                                                      : sr_ift(a);
        drv.put(d, std::move(r));
        return true;
    }

    case FH_ILL:
        break;
    }
    return false;
}

// Fold a read into the stats the caller sees. The reader reports diagnostics
// as text rather than as kinds, so `skipped_lines` is the count of everything
// it declined to hand back — unknown opcodes and malformed lines alike. The
// messages themselves carry the distinction.
void absorb_diagnostics(const ReadResult& read, DriveStats& stats) {
    for (const auto& e : read.errors)
        stats.errors.push_back("line " + std::to_string(e.line_number)
                               + ": " + e.message);
    for (const auto& w : read.warnings)
        stats.errors.push_back("line " + std::to_string(w.line_number)
                               + ": " + w.message);
    stats.skipped_lines += read.warnings.size();
}

bool drive_program(const ReadResult& read, uint64_t ring_dim,
                   DriveStats& stats, const DriveInputs& inputs,
                   const DriveOutputs& outputs) {
    absorb_diagnostics(read, stats);
    if (!read.ok) return false;

    stats.modulus_table = read.program.modulus_table;
    stats.instructions_parsed += read.program.instructions.size();

    Driver drv(ring_dim, stats, inputs, outputs);
    for (const auto& inst : read.program.instructions) {
        if (drive_instruction(inst, drv)) stats.instructions_replayed++;
        else stats.skipped_lines++;
    }
    drv.finalize_outputs();
    return stats.instructions_replayed > 0;
}

}  // namespace

bool parse_and_drive(const std::filesystem::path& path,
                     uint64_t ring_dim,
                     DriveStats& stats,
                     const DriveInputs& inputs,
                     const DriveOutputs& outputs) {
    // read_fhetch_file detects the serialization from the file's own magic
    // bytes, so a `.fhex` drives exactly like a `.fhetch`.
    return drive_program(read_fhetch_file(path), ring_dim, stats, inputs,
                         outputs);
}

bool parse_and_drive_text(const std::string& text,
                          uint64_t ring_dim,
                          DriveStats& stats,
                          const DriveInputs& inputs,
                          const DriveOutputs& outputs) {
    return drive_program(read_fhetch_text(text), ring_dim, stats, inputs,
                         outputs);
}

}  // namespace niobium::fhetch
