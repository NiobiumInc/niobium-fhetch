// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0.
//
// Implementation of niobium/fhetch_parser.h.
//
// The parser is a line-based reader for the .fhetch text format produced by
// TraceWriter:
//
//     # comments
//     modulus_count N
//     m[0] 0xHEX       (sentinel)
//     m[1] 0xHEX
//     ...
//     sr_addp  %dst, %src1, %src2, m=K
//     sr_mulps %dst, %src,  imm,   m=K
//     sr_ntt   %dst, %src,  m=K [, omega=V]
//     sr_automorph_eval %dst, %src, m=K, mask=M, logn=L, k=K
//     halt
//
// For each instruction line the parser calls the matching niobium::fhetch::
// free function, re-driving the recording path. Addresses in the input file
// are opaque — they're keys into a map that yields a Polynomial object; the
// FHETCH API then assigns its own synthetic address to the result, which the
// parser stores back in the map under the input file's destination address.

#include "niobium/fhetch_parser.h"
#include "niobium/fhetch_api.h"
#include "niobium/compiler.h"
#include "compiler_internal.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace niobium::fhetch {

namespace {

// ---------------------------------------------------------------------------
// Tokenizing helpers
// ---------------------------------------------------------------------------

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// "%42"  or  "%42,"  → 42
bool parse_addr(const std::string& tok, uint64_t& out) {
    std::string s = trim(tok);
    if (!s.empty() && s.back() == ',') s.pop_back();
    if (s.empty() || s[0] != '%') return false;
    try { out = std::stoull(s.substr(1)); } catch (...) { return false; }
    return true;
}

// "m=3" → 3
bool parse_modulus_ref(const std::string& tok, uint32_t& out) {
    std::string s = trim(tok);
    if (!s.empty() && s.back() == ',') s.pop_back();
    if (s.rfind("m=", 0) != 0) return false;
    try { out = static_cast<uint32_t>(std::stoul(s.substr(2))); }
    catch (...) { return false; }
    return true;
}

// Plain integer, optional trailing comma. Accepts 0x... hex as well.
bool parse_uint(const std::string& tok, uint64_t& out) {
    std::string s = trim(tok);
    if (!s.empty() && s.back() == ',') s.pop_back();
    if (s.empty()) return false;
    try { out = std::stoull(s, nullptr, 0); }
    catch (...) { return false; }
    return true;
}

// "k=7" / "offset=3" / "omega=0xABC" → 7 / 3 / 2748
bool parse_named_uint(const std::string& tok, const std::string& prefix,
                      uint64_t& out) {
    std::string s = trim(tok);
    if (!s.empty() && s.back() == ',') s.pop_back();
    if (s.rfind(prefix, 0) != 0) return false;
    try { out = std::stoull(s.substr(prefix.size()), nullptr, 0); }
    catch (...) { return false; }
    return true;
}

// ---------------------------------------------------------------------------
// Opcode table
// ---------------------------------------------------------------------------

enum class OpCode {
    SR_ADDP, SR_SUBP, SR_MULP,
    SR_ADDPS, SR_SUBPS, SR_MULPS,
    SR_ADDPS_COEFF, SR_SUBPS_COEFF,
    SR_NEGP,
    SR_NTT, SR_INTT,
    SR_PERMUTE,
    SR_AUTOMORPH_EVAL, SR_AUTOMORPH_COEFF, SR_ROT_AUTOMORPH_COEFF,
    SR_ADDP_NI, SR_SUBP_NI, SR_MULP_NI,
    SR_ADDPS_NI, SR_SUBPS_NI, SR_MULPS_NI,
    SR_ADDPS_COEFF_NI, SR_SUBPS_COEFF_NI,
    SR_NEGP_NI,
    SR_FT, SR_IFT,
    HALT,
    UNKNOWN
};

OpCode lookup_opcode(const std::string& s) {
    static const std::unordered_map<std::string, OpCode> kMap = {
        {"sr_addp",                OpCode::SR_ADDP},
        {"sr_subp",                OpCode::SR_SUBP},
        {"sr_mulp",                OpCode::SR_MULP},
        {"sr_addps",               OpCode::SR_ADDPS},
        {"sr_subps",               OpCode::SR_SUBPS},
        {"sr_mulps",               OpCode::SR_MULPS},
        {"sr_addps_coeff",         OpCode::SR_ADDPS_COEFF},
        {"sr_subps_coeff",         OpCode::SR_SUBPS_COEFF},
        {"sr_negp",                OpCode::SR_NEGP},
        {"sr_ntt",                 OpCode::SR_NTT},
        {"sr_intt",                OpCode::SR_INTT},
        {"sr_permute",             OpCode::SR_PERMUTE},
        {"sr_automorph_eval",      OpCode::SR_AUTOMORPH_EVAL},
        {"sr_automorph_coeff",     OpCode::SR_AUTOMORPH_COEFF},
        {"sr_rot_automorph_coeff", OpCode::SR_ROT_AUTOMORPH_COEFF},
        {"sr_addp_ni",             OpCode::SR_ADDP_NI},
        {"sr_subp_ni",             OpCode::SR_SUBP_NI},
        {"sr_mulp_ni",             OpCode::SR_MULP_NI},
        {"sr_addps_ni",            OpCode::SR_ADDPS_NI},
        {"sr_subps_ni",            OpCode::SR_SUBPS_NI},
        {"sr_mulps_ni",            OpCode::SR_MULPS_NI},
        {"sr_addps_coeff_ni",      OpCode::SR_ADDPS_COEFF_NI},
        {"sr_subps_coeff_ni",      OpCode::SR_SUBPS_COEFF_NI},
        {"sr_negp_ni",             OpCode::SR_NEGP_NI},
        {"sr_ft",                  OpCode::SR_FT},
        {"sr_ift",                 OpCode::SR_IFT},
        {"halt",                   OpCode::HALT},
    };
    auto it = kMap.find(s);
    return it == kMap.end() ? OpCode::UNKNOWN : it->second;
}

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
    // address, so later instructions that reference it resolve correctly.
    // If the caller marked this destination as an output probe via
    // DriveOutputs, also emit a tag_output() so Compiler::result() can
    // reconstruct the ciphertext after replay.
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

    // Resolve m=K to the modulus value from the table. Returns 0 if out of
    // range (caller typically records an error and skips the instruction).
    uint64_t resolve(uint32_t mod_idx) const {
        if (mod_idx < stats.modulus_table.size())
            return stats.modulus_table[mod_idx];
        return 0;
    }
};

bool parse_line(const std::string& raw, int line_num, Driver& drv) {
    std::string line = trim(raw);
    if (line.empty() || line[0] == '#') return true;

    // --- Modulus table ---
    if (line.rfind("modulus_count", 0) == 0) {
        // Nothing to do — entries follow.
        return true;
    }
    if (line.size() > 2 && line[0] == 'm' && line[1] == '[') {
        // m[N] 0xHEX
        size_t sp = line.find(' ');
        if (sp == std::string::npos) {
            drv.stats.skipped_lines++;
            return false;
        }
        try {
            uint64_t v = std::stoull(trim(line.substr(sp + 1)), nullptr, 16);
            drv.stats.modulus_table.push_back(v);
            return true;
        } catch (...) {
            drv.stats.skipped_lines++;
            return false;
        }
    }

    // --- Strip trailing inline comment ---
    size_t hash = line.find('#');
    if (hash != std::string::npos) {
        line = trim(line.substr(0, hash));
        if (line.empty()) return true;
    }

    // --- Tokenize ---
    std::istringstream in(line);
    std::string op_str;
    in >> op_str;
    std::transform(op_str.begin(), op_str.end(), op_str.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    std::vector<std::string> args;
    std::string tok;
    while (in >> tok) args.push_back(tok);

    OpCode op = lookup_opcode(op_str);
    drv.stats.instructions_parsed++;

    // --- Dispatch ---
    switch (op) {
    case OpCode::HALT:
        halt();
        drv.stats.instructions_replayed++;
        return true;

    case OpCode::SR_ADDP:
    case OpCode::SR_SUBP:
    case OpCode::SR_MULP: {
        if (args.size() < 4) { drv.stats.skipped_lines++; return false; }
        uint64_t d = 0;
        uint64_t s1 = 0;
        uint64_t s2 = 0; uint32_t m = 0;
        if (!parse_addr(args[0], d) || !parse_addr(args[1], s1)
            || !parse_addr(args[2], s2) || !parse_modulus_ref(args[3], m)) {
            drv.stats.skipped_lines++; return false;
        }
        uint64_t q = drv.resolve(m);
        const Polynomial& a = drv.get_or_create_src(s1);
        const Polynomial& b = drv.get_or_create_src(s2);
        Polynomial r = (op == OpCode::SR_ADDP) ? sr_addp(a, b, q)
                     : (op == OpCode::SR_SUBP) ? sr_subp(a, b, q)
                                                : sr_mulp(a, b, q);
        drv.put(d, std::move(r));
        drv.stats.instructions_replayed++;
        return true;
    }

    case OpCode::SR_ADDPS:
    case OpCode::SR_SUBPS:
    case OpCode::SR_MULPS:
    case OpCode::SR_ADDPS_COEFF:
    case OpCode::SR_SUBPS_COEFF: {
        if (args.size() < 4) { drv.stats.skipped_lines++; return false; }
        uint64_t d = 0;
        uint64_t s1 = 0;
        uint64_t imm = 0; uint32_t m = 0;
        if (!parse_addr(args[0], d) || !parse_addr(args[1], s1)
            || !parse_uint(args[2], imm) || !parse_modulus_ref(args[3], m)) {
            drv.stats.skipped_lines++; return false;
        }
        uint64_t q = drv.resolve(m);
        // sr_mulps with imm==0 is a zero-init: it writes zeros and reads
        // nothing (the simulator's classify_uses agrees). Do not treat its
        // source operand as a live-in input — the recorded idiom aliases
        // src to dest, and registering a placeholder input per zero-init
        // materializes hundreds of thousands of dense zero polys at sync.
        const Polynomial& a = (op == OpCode::SR_MULPS && imm == 0)
                                  ? drv.get_or_create_zero_src(s1)
                                  : drv.get_or_create_src(s1);
        Scalar s = Scalar::from_int(imm);
        Polynomial r;
        switch (op) {
            case OpCode::SR_ADDPS:       r = sr_addps(a, s, q);       break;
            case OpCode::SR_SUBPS:       r = sr_subps(a, s, q);       break;
            case OpCode::SR_MULPS:       r = sr_mulps(a, s, q);       break;
            case OpCode::SR_ADDPS_COEFF: r = sr_addps_coeff(a, s, q); break;
            case OpCode::SR_SUBPS_COEFF: r = sr_subps_coeff(a, s, q); break;
            default: break;
        }
        drv.put(d, std::move(r));
        drv.stats.instructions_replayed++;
        return true;
    }

    case OpCode::SR_NEGP: {
        if (args.size() < 3) { drv.stats.skipped_lines++; return false; }
        uint64_t d = 0;
        uint64_t s1 = 0; uint32_t m = 0;
        if (!parse_addr(args[0], d) || !parse_addr(args[1], s1)
            || !parse_modulus_ref(args[2], m)) {
            drv.stats.skipped_lines++; return false;
        }
        const Polynomial& a = drv.get_or_create_src(s1);
        drv.put(d, sr_negp(a, drv.resolve(m)));
        drv.stats.instructions_replayed++;
        return true;
    }

    case OpCode::SR_NTT:
    case OpCode::SR_INTT: {
        if (args.size() < 3) { drv.stats.skipped_lines++; return false; }
        uint64_t d = 0;
        uint64_t s1 = 0; uint32_t m = 0;
        if (!parse_addr(args[0], d) || !parse_addr(args[1], s1)
            || !parse_modulus_ref(args[2], m)) {
            drv.stats.skipped_lines++; return false;
        }
        uint64_t q = drv.resolve(m);
        const Polynomial& a = drv.get_or_create_src(s1);
        drv.put(d, (op == OpCode::SR_NTT) ? sr_ntt(a, q) : sr_intt(a, q));
        drv.stats.instructions_replayed++;
        return true;
    }

    case OpCode::SR_PERMUTE: {
        if (args.size() < 3) { drv.stats.skipped_lines++; return false; }
        uint64_t d = 0;
        uint64_t s1 = 0; uint32_t m = 0;
        if (!parse_addr(args[0], d) || !parse_addr(args[1], s1)
            || !parse_modulus_ref(args[2], m)) {
            drv.stats.skipped_lines++; return false;
        }
        const Polynomial& a = drv.get_or_create_src(s1);
        // The trace doesn't preserve srcs/signs — feed empties so the API
        // emits a permute placeholder with the correct dst/src/mod wiring.
        drv.put(d, sr_permute(a, {}, {}, drv.resolve(m)));
        drv.stats.instructions_replayed++;
        return true;
    }

    case OpCode::SR_AUTOMORPH_EVAL: {
        if (args.size() < 2) { drv.stats.skipped_lines++; return false; }
        uint64_t d = 0;
        uint64_t s1 = 0;
        if (!parse_addr(args[0], d) || !parse_addr(args[1], s1)) {
            drv.stats.skipped_lines++; return false;
        }
        uint64_t k = 1;
        for (size_t i = 2; i < args.size(); ++i) {
            uint64_t v = 0;
            if (parse_named_uint(args[i], "k=", v)) { k = v; break; }
        }
        const Polynomial& a = drv.get_or_create_src(s1);
        drv.put(d, sr_automorph_eval(a, k));
        drv.stats.instructions_replayed++;
        return true;
    }

    case OpCode::SR_AUTOMORPH_COEFF: {
        if (args.size() < 4) { drv.stats.skipped_lines++; return false; }
        uint64_t d = 0;
        uint64_t s1 = 0;
        uint64_t k = 0;
        uint32_t m = 0;
        if (!parse_addr(args[0], d) || !parse_addr(args[1], s1)) {
            drv.stats.skipped_lines++; return false;
        }
        for (size_t i = 2; i < args.size(); ++i) {
            uint64_t v = 0;
            if (parse_named_uint(args[i], "k=", v)) k = v;
            else parse_modulus_ref(args[i], m);
        }
        const Polynomial& a = drv.get_or_create_src(s1);
        drv.put(d, sr_automorph_coeff(a, k, drv.resolve(m)));
        drv.stats.instructions_replayed++;
        return true;
    }

    case OpCode::SR_ROT_AUTOMORPH_COEFF: {
        if (args.size() < 4) { drv.stats.skipped_lines++; return false; }
        uint64_t d = 0;
        uint64_t s1 = 0;
        uint64_t off = 0;
        uint32_t m = 0;
        if (!parse_addr(args[0], d) || !parse_addr(args[1], s1)) {
            drv.stats.skipped_lines++; return false;
        }
        for (size_t i = 2; i < args.size(); ++i) {
            uint64_t v = 0;
            if (parse_named_uint(args[i], "offset=", v)) off = v;
            else parse_modulus_ref(args[i], m);
        }
        const Polynomial& a = drv.get_or_create_src(s1);
        drv.put(d, sr_rot_automorph_coeff(a, off, drv.resolve(m)));
        drv.stats.instructions_replayed++;
        return true;
    }

    // Non-integer variants — no modulus.
    case OpCode::SR_ADDP_NI:
    case OpCode::SR_SUBP_NI:
    case OpCode::SR_MULP_NI: {
        if (args.size() < 3) { drv.stats.skipped_lines++; return false; }
        uint64_t d = 0;
        uint64_t s1 = 0;
        uint64_t s2 = 0;
        if (!parse_addr(args[0], d) || !parse_addr(args[1], s1)
            || !parse_addr(args[2], s2)) {
            drv.stats.skipped_lines++; return false;
        }
        const Polynomial& a = drv.get_or_create_src(s1);
        const Polynomial& b = drv.get_or_create_src(s2);
        Polynomial r = (op == OpCode::SR_ADDP_NI) ? sr_addp_ni(a, b)
                     : (op == OpCode::SR_SUBP_NI) ? sr_subp_ni(a, b)
                                                  : sr_mulp_ni(a, b);
        drv.put(d, std::move(r));
        drv.stats.instructions_replayed++;
        return true;
    }

    case OpCode::SR_ADDPS_NI:
    case OpCode::SR_SUBPS_NI:
    case OpCode::SR_MULPS_NI:
    case OpCode::SR_ADDPS_COEFF_NI:
    case OpCode::SR_SUBPS_COEFF_NI: {
        if (args.size() < 3) { drv.stats.skipped_lines++; return false; }
        uint64_t d = 0;
        uint64_t s1 = 0;
        uint64_t imm = 0;
        if (!parse_addr(args[0], d) || !parse_addr(args[1], s1)
            || !parse_uint(args[2], imm)) {
            drv.stats.skipped_lines++; return false;
        }
        const Polynomial& a = drv.get_or_create_src(s1);
        Scalar s = Scalar::from_int(imm);
        Polynomial r;
        switch (op) {
            case OpCode::SR_ADDPS_NI:       r = sr_addps_ni(a, s);       break;
            case OpCode::SR_SUBPS_NI:       r = sr_subps_ni(a, s);       break;
            case OpCode::SR_MULPS_NI:       r = sr_mulps_ni(a, s);       break;
            case OpCode::SR_ADDPS_COEFF_NI: r = sr_addps_coeff_ni(a, s); break;
            case OpCode::SR_SUBPS_COEFF_NI: r = sr_subps_coeff_ni(a, s); break;
            default: break;
        }
        drv.put(d, std::move(r));
        drv.stats.instructions_replayed++;
        return true;
    }

    case OpCode::SR_NEGP_NI:
    case OpCode::SR_FT:
    case OpCode::SR_IFT: {
        if (args.size() < 2) { drv.stats.skipped_lines++; return false; }
        uint64_t d = 0;
        uint64_t s1 = 0;
        if (!parse_addr(args[0], d) || !parse_addr(args[1], s1)) {
            drv.stats.skipped_lines++; return false;
        }
        const Polynomial& a = drv.get_or_create_src(s1);
        Polynomial r = (op == OpCode::SR_NEGP_NI) ? sr_negp_ni(a)
                     : (op == OpCode::SR_FT)     ? sr_ft(a)
                                                  : sr_ift(a);
        drv.put(d, std::move(r));
        drv.stats.instructions_replayed++;
        return true;
    }

    case OpCode::UNKNOWN:
        drv.stats.unknown_opcodes++;
        drv.stats.errors.push_back(
            "line " + std::to_string(line_num)
            + ": unknown opcode '" + op_str + "'");
        return false;
    }
    return false;
}

bool drive_stream(std::istream& in, uint64_t ring_dim, DriveStats& stats,
                  const DriveInputs& inputs, const DriveOutputs& outputs) {
    Driver drv(ring_dim, stats, inputs, outputs);
    std::string line;
    int line_num = 0;
    while (std::getline(in, line)) {
        line_num++;
        parse_line(line, line_num, drv);
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
    std::ifstream in(path);
    if (!in.is_open()) {
        stats.errors.push_back("failed to open: " + path.string());
        return false;
    }
    return drive_stream(in, ring_dim, stats, inputs, outputs);
}

bool parse_and_drive_text(const std::string& text,
                          uint64_t ring_dim,
                          DriveStats& stats,
                          const DriveInputs& inputs,
                          const DriveOutputs& outputs) {
    std::istringstream in(text);
    return drive_stream(in, ring_dim, stats, inputs, outputs);
}

}  // namespace niobium::fhetch
