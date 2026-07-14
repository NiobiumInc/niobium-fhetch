// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// FHETCH instruction-set simulator — executes .fhetch traces using
// OpenFHE modular arithmetic.

#include "niobium/fhetch_sim/simulator.h"
#include "instruction.h"
#include "memory.h"

#include <chrono>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <iterator>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>

// OpenFHE math
#include "math/math-hal.h"
#include "math/nbtheory.h"
#include "core/lattice/hal/lat-backend.h"
#include "lattice/lat-hal.h"

using namespace lbcrypto;

namespace niobium::fhetch_sim {

// ============================================================================
// Impl
// ============================================================================

struct Simulator::Impl {
    uint64_t ring_dim = 0;
    ParsedTrace trace;
    Memory memory;
    size_t error_count = 0;

    // Resolve a modulus index to the actual value
    uint64_t resolve_modulus(uint32_t idx) const {
        if (idx < trace.modulus_table.size())
            return trace.modulus_table[idx];
        return 0;
    }

    // Addresses we've already warned about once — keeps uninit-read
    // warnings readable when the same address is consumed by many ops.
    std::unordered_set<uint64_t> warned_uninit_addrs;

    // Per-instruction schedule of addresses to free after that instruction
    // executes. Built by compute_liveness(); empty by default (no freeing).
    std::vector<std::vector<uint64_t>> free_after_;

    // Addresses that must never be freed (probe outputs, etc.).
    std::unordered_set<uint64_t> live_out_;

    // What addresses an instruction reads and writes. The liveness
    // pass uses this to find each address's last read. FHETCH
    // instructions have at most two source operands (src1, src2), so
    // `reads` is a fixed two-slot array; `n_reads` tells the caller
    // how many of those slots are populated.
    struct InstUses {
        uint64_t write;     // dest address; meaningful iff writes==true
        bool writes;        // true if this instruction writes to mem_
        uint64_t reads[2];  // source addresses; only first n_reads valid
        int n_reads;        // 0, 1, or 2
    };

    // Decode `inst` into the addresses it reads and writes.
    static InstUses classify_uses(const Instruction& inst) {
        switch (inst.opcode) {
        // No-ops: comments, unknowns, halt, and unimplemented stubs
        // (execute() runs these as bare `ok = true`, no memory.set, so
        // for liveness they neither read nor write).
        case OpCode::COMMENT:
        case OpCode::UNKNOWN:
        case OpCode::HALT:
        case OpCode::SR_PERMUTE:
        case OpCode::SR_AUTOMORPH_COEFF:
        case OpCode::SR_NEGP_NI:
        case OpCode::SR_FT:
        case OpCode::SR_IFT:
        case OpCode::SR_ADDP_NI:
        case OpCode::SR_SUBP_NI:
        case OpCode::SR_MULP_NI:
        case OpCode::SR_ADDPS_NI:
        case OpCode::SR_SUBPS_NI:
        case OpCode::SR_MULPS_NI:
        case OpCode::SR_ADDPS_COEFF_NI:
        case OpCode::SR_SUBPS_COEFF_NI:
            return {.write = 0, .writes = false, .reads = {0, 0}, .n_reads = 0};

        // sr_mulps with imm=0 writes a zero vector and reads nothing
        // (see exec_mulps); the other immediate-form ops read src1.
        case OpCode::SR_MULPS:
            return inst.immediate == 0
                ? InstUses{.write = inst.dest, .writes = true,
                           .reads = {0, 0}, .n_reads = 0}
                : InstUses{.write = inst.dest, .writes = true,
                           .reads = {inst.src1, 0}, .n_reads = 1};

        // One-source ops: write dest, read src1.
        case OpCode::SR_NEGP:
        case OpCode::SR_NTT:
        case OpCode::SR_INTT:
        case OpCode::SR_AUTOMORPH_EVAL:
        case OpCode::SR_ROT_AUTOMORPH_COEFF:
        case OpCode::SR_ADDPS:
        case OpCode::SR_SUBPS:
        case OpCode::SR_ADDPS_COEFF:
        case OpCode::SR_SUBPS_COEFF:
            return {.write = inst.dest, .writes = true,
                    .reads = {inst.src1, 0}, .n_reads = 1};

        // Two-source arithmetic (sr_addp / sr_subp / sr_mulp).
        default:
            return {.write = inst.dest, .writes = true,
                    .reads = {inst.src1, inst.src2}, .n_reads = 2};
        }
    }

    // Get polynomial from memory, returning zero-initialized if missing
    const std::vector<uint64_t>& get_or_zero(uint64_t addr,
                                              std::vector<uint64_t>& scratch,
                                              const Instruction& inst) {
        if (memory.is_initialized(addr))
            return memory.get(addr).values;
        if (warned_uninit_addrs.insert(addr).second) {
            std::cerr << "[FHETCH_SIM] WARNING: read from uninitialized address %"
                      << addr << " (first seen at line " << inst.line_number
                      << ": " << inst.raw_line << ")" << std::endl;
        }
        scratch.assign(ring_dim, 0);
        return scratch;
    }

    void error(const Instruction& inst, const std::string& msg) {
        std::cerr << "[FHETCH_SIM] ERROR line " << inst.line_number
                  << ": " << msg << "\n  " << inst.raw_line << std::endl;
        error_count++;
    }

    // --- Arithmetic dispatch ---

    bool exec_addp(const Instruction& inst) {
        uint64_t q = resolve_modulus(inst.modulus_index);
        std::vector<uint64_t> scratch1;
        std::vector<uint64_t> scratch2;
        const auto& a = get_or_zero(inst.src1, scratch1, inst);
        const auto& b = get_or_zero(inst.src2, scratch2, inst);
        if (a.size() != ring_dim || b.size() != ring_dim) {
            error(inst, "ring dimension mismatch"); return false;
        }

        NativeInteger mod(q);
        NativeVector va(ring_dim, mod);
        NativeVector vb(ring_dim, mod);
        for (size_t i = 0; i < ring_dim; i++) {
            va[i] = NativeInteger(a[i]);
            vb[i] = NativeInteger(b[i]);
        }
        NativeVector vr = va.ModAdd(vb);
        auto& result = memory.reserve_dest(inst.dest);
        for (size_t i = 0; i < ring_dim; i++)
            result[i] = vr[i].ConvertToInt();
        memory.commit_dest(inst.dest, q);
        return true;
    }

    bool exec_subp(const Instruction& inst) {
        uint64_t q = resolve_modulus(inst.modulus_index);
        std::vector<uint64_t> scratch1;
        std::vector<uint64_t> scratch2;
        const auto& a = get_or_zero(inst.src1, scratch1, inst);
        const auto& b = get_or_zero(inst.src2, scratch2, inst);
        if (a.size() != ring_dim || b.size() != ring_dim) {
            error(inst, "ring dimension mismatch"); return false;
        }

        NativeInteger mod(q);
        NativeVector va(ring_dim, mod);
        NativeVector vb(ring_dim, mod);
        for (size_t i = 0; i < ring_dim; i++) {
            va[i] = NativeInteger(a[i]);
            vb[i] = NativeInteger(b[i]);
        }
        NativeVector vr = va.ModSub(vb);
        auto& result = memory.reserve_dest(inst.dest);
        for (size_t i = 0; i < ring_dim; i++)
            result[i] = vr[i].ConvertToInt();
        memory.commit_dest(inst.dest, q);
        return true;
    }

    bool exec_mulp(const Instruction& inst) {
        uint64_t q = resolve_modulus(inst.modulus_index);
        std::vector<uint64_t> scratch1;
        std::vector<uint64_t> scratch2;
        const auto& a = get_or_zero(inst.src1, scratch1, inst);
        const auto& b = get_or_zero(inst.src2, scratch2, inst);
        if (a.size() != ring_dim || b.size() != ring_dim) {
            error(inst, "ring dimension mismatch"); return false;
        }

        NativeInteger mod(q);
        NativeVector va(ring_dim, mod);
        NativeVector vb(ring_dim, mod);
        for (size_t i = 0; i < ring_dim; i++) {
            va[i] = NativeInteger(a[i]);
            vb[i] = NativeInteger(b[i]);
        }
        NativeVector vr = va.ModMul(vb);
        auto& result = memory.reserve_dest(inst.dest);
        for (size_t i = 0; i < ring_dim; i++)
            result[i] = vr[i].ConvertToInt();
        memory.commit_dest(inst.dest, q);
        return true;
    }

    bool exec_addps(const Instruction& inst) {
        uint64_t q = resolve_modulus(inst.modulus_index);
        std::vector<uint64_t> scratch;
        const auto& a = get_or_zero(inst.src1, scratch, inst);
        if (a.size() != ring_dim) { error(inst, "ring dimension mismatch"); return false; }

        // Special case: immediate 0 = copy (no modular reduction).
        // Used by copy probes where the modulus may not match the data.
        if (inst.immediate == 0) {
            auto& out = memory.reserve_dest(inst.dest);
            // When dest == src1, `&out == &a` and the copy is a no-op
            // — skip to keep std::copy well-defined.
            if (&out != &a) {
                std::copy(a.begin(), a.end(), out.begin());
            }
            memory.commit_dest(inst.dest, q);
            return true;
        }

        NativeInteger mod(q);
        NativeInteger imm(inst.immediate);
        NativeVector va(ring_dim, mod);
        for (size_t i = 0; i < ring_dim; i++) va[i] = NativeInteger(a[i]);
        va.ModAddEq(imm);
        auto& result = memory.reserve_dest(inst.dest);
        for (size_t i = 0; i < ring_dim; i++) result[i] = va[i].ConvertToInt();
        memory.commit_dest(inst.dest, q);
        return true;
    }

    bool exec_subps(const Instruction& inst) {
        uint64_t q = resolve_modulus(inst.modulus_index);
        std::vector<uint64_t> scratch;
        const auto& a = get_or_zero(inst.src1, scratch, inst);
        if (a.size() != ring_dim) { error(inst, "ring dimension mismatch"); return false; }

        NativeInteger mod(q);
        NativeInteger imm(inst.immediate);
        NativeVector va(ring_dim, mod);
        for (size_t i = 0; i < ring_dim; i++) va[i] = NativeInteger(a[i]);
        va.ModSubEq(imm);
        auto& result = memory.reserve_dest(inst.dest);
        for (size_t i = 0; i < ring_dim; i++) result[i] = va[i].ConvertToInt();
        memory.commit_dest(inst.dest, q);
        return true;
    }

    bool exec_mulps(const Instruction& inst) {
        uint64_t q = resolve_modulus(inst.modulus_index);

        // Special case: multiply by 0 always produces zero
        if (inst.immediate == 0) {
            auto& out = memory.reserve_dest(inst.dest);
            std::fill(out.begin(), out.end(), 0);
            memory.commit_dest(inst.dest, q);
            return true;
        }

        std::vector<uint64_t> scratch;
        const auto& a = get_or_zero(inst.src1, scratch, inst);
        if (a.size() != ring_dim) { error(inst, "ring dimension mismatch"); return false; }

        NativeInteger mod(q);
        NativeInteger imm(inst.immediate);
        NativeVector va(ring_dim, mod);
        for (size_t i = 0; i < ring_dim; i++) va[i] = NativeInteger(a[i]);
        va.ModMulEq(imm);
        auto& result = memory.reserve_dest(inst.dest);
        for (size_t i = 0; i < ring_dim; i++) result[i] = va[i].ConvertToInt();
        memory.commit_dest(inst.dest, q);
        return true;
    }

    bool exec_negp(const Instruction& inst) {
        uint64_t q = resolve_modulus(inst.modulus_index);
        std::vector<uint64_t> scratch;
        const auto& a = get_or_zero(inst.src1, scratch, inst);
        if (a.size() != ring_dim) { error(inst, "ring dimension mismatch"); return false; }

        auto& result = memory.reserve_dest(inst.dest);
        for (size_t i = 0; i < ring_dim; i++)
            result[i] = (a[i] == 0) ? 0 : q - a[i];
        memory.commit_dest(inst.dest, q);
        return true;
    }

    bool exec_ntt(const Instruction& inst) {
        uint64_t q = resolve_modulus(inst.modulus_index);
        std::vector<uint64_t> scratch;
        const auto& a = get_or_zero(inst.src1, scratch, inst);
        if (a.size() != ring_dim) { error(inst, "ring dimension mismatch"); return false; }

        NativeInteger mod(q);
        NativeVector va(ring_dim, mod);
        for (size_t i = 0; i < ring_dim; i++) va[i] = NativeInteger(a[i]);

        // Use omega from trace if available, otherwise compute
        NativeInteger root;
        if (inst.omega.has_value() && inst.omega.value() != 0) {
            root = NativeInteger(inst.omega.value());
        } else {
            root = RootOfUnity<NativeInteger>(2 * ring_dim, mod);
        }
        ChineseRemainderTransformFTT<NativeVector> transformer;
        transformer.ForwardTransformToBitReverse(va, root, 2 * ring_dim, &va);

        auto& result = memory.reserve_dest(inst.dest);
        for (size_t i = 0; i < ring_dim; i++) result[i] = va[i].ConvertToInt();
        memory.commit_dest(inst.dest, q);
        return true;
    }

    // AutomorphismTransform for power-of-2 ring in EVALUATION form.
    // Mirrors OpenFHE's PolyImpl::AutomorphismTransform(k) (poly-impl.h:494):
    //   logn = log2(N)
    //   mask = N - 1
    //   for (j=0, jk=k; j < N; ++j, jk += 2k):
    //       result[ReverseBits(j, logn)] = values[ReverseBits((jk>>1)&mask, logn)]
    bool exec_automorph_eval(const Instruction& inst) {
        uint64_t q = resolve_modulus(inst.modulus_index);
        std::vector<uint64_t> scratch;
        const auto& a = get_or_zero(inst.src1, scratch, inst);
        if (a.size() != ring_dim) { error(inst, "ring dimension mismatch"); return false; }
        if (!inst.k.has_value()) {
            error(inst, "automorphism missing k"); return false;
        }
        uint32_t k = static_cast<uint32_t>(inst.k.value());
        // logn such that (1 << logn) == ring_dim
        uint32_t logn = 0;
        for (uint32_t n = static_cast<uint32_t>(ring_dim); n > 1; n >>= 1) ++logn;
        uint32_t mask = (1U << logn) - 1U;

        auto rev_bits = [](uint32_t x, uint32_t bits) {
            uint32_t r = 0;
            for (uint32_t i = 0; i < bits; ++i)
                if (x & (1U << i)) r |= 1U << (bits - 1 - i);
            return r;
        };

        // Snapshot src: the permutation reads `a[idxrev]` and writes
        // `result[jrev]` at different indices, so a dest==src1 trace
        // would corrupt source positions mid-loop.
        std::vector<uint64_t> snapshot(a.begin(), a.end());

        // rev_bits is a bijection on [0, ring_dim), so every position
        // of `result` is written exactly once — no zero-fill needed.
        auto& result = memory.reserve_dest(inst.dest);
        uint32_t jk = k;
        for (uint32_t j = 0; j < ring_dim; ++j, jk += 2 * k) {
            uint32_t jrev = rev_bits(j, logn);
            uint32_t idxrev = rev_bits((jk >> 1) & mask, logn);
            result[jrev] = snapshot[idxrev];
        }
        memory.commit_dest(inst.dest, q);
        return true;
    }

    bool exec_rot_automorph_coeff(const Instruction& inst) {
        // Negacyclic-coefficient rotation per the FHETCH spec:
        //   result[i] = signs[i] * src[(i + offset) mod N]
        //   signs[i] = (-1)^((i + offset) // N)
        // i.e. result[i] reads src at the position offset slots ahead,
        // with a sign flip when the read wraps past N (because X^N = -1).
        // This is multiplication by X^{-offset} (= X^{2N-offset}) in
        // R_q = Z_q[X]/(X^N+1), a left shift of the coefficient vector.
        uint64_t q = resolve_modulus(inst.modulus_index);
        std::vector<uint64_t> scratch;
        const auto& a = get_or_zero(inst.src1, scratch, inst);
        if (a.size() != ring_dim) { error(inst, "ring dimension mismatch"); return false; }
        if (!inst.offset.has_value()) {
            error(inst, "rotation missing offset"); return false;
        }
        const uint64_t offset = inst.offset.value();

        // Snapshot src: the wraparound case reads from low indices
        // that prior iterations have already written if dest=src1.
        std::vector<uint64_t> snapshot(a.begin(), a.end());

        auto& result = memory.reserve_dest(inst.dest);
        for (uint64_t i = 0; i < ring_dim; ++i) {
            const uint64_t src_pos = i + offset;
            if (src_pos < ring_dim) {
                result[i] = snapshot[src_pos];
            } else {
                // (q - a[k]) % q yields 0 when a[k]==0 and (q - a[k]) when
                // a[k] != 0, keeping result in [0, q).
                result[i] = (q - snapshot[src_pos - ring_dim]) % q;
            }
        }
        memory.commit_dest(inst.dest, q);
        return true;
    }

    bool exec_intt(const Instruction& inst) {
        uint64_t q = resolve_modulus(inst.modulus_index);
        std::vector<uint64_t> scratch;
        const auto& a = get_or_zero(inst.src1, scratch, inst);
        if (a.size() != ring_dim) { error(inst, "ring dimension mismatch"); return false; }

        NativeInteger mod(q);
        NativeVector va(ring_dim, mod);
        for (size_t i = 0; i < ring_dim; i++) va[i] = NativeInteger(a[i]);

        NativeInteger root;
        if (inst.omega.has_value() && inst.omega.value() != 0) {
            root = NativeInteger(inst.omega.value());
        } else {
            root = RootOfUnity<NativeInteger>(2 * ring_dim, mod);
        }
        ChineseRemainderTransformFTT<NativeVector> transformer;
        transformer.InverseTransformFromBitReverse(va, root, 2 * ring_dim, &va);

        auto& result = memory.reserve_dest(inst.dest);
        for (size_t i = 0; i < ring_dim; i++) result[i] = va[i].ConvertToInt();
        memory.commit_dest(inst.dest, q);
        return true;
    }

    // --- Main execution loop ---

    SimResult execute() {
        auto start = std::chrono::steady_clock::now();
        size_t executed = 0;
        error_count = 0;
        size_t total = trace.instructions.size();
        size_t peak_mem = memory.size();  // tracks max polys live at any point

        std::cout << "[FHETCH_SIM] Executing " << total << " instructions, "
                  << trace.modulus_table.size() << " moduli, N=" << ring_dim
                  << std::endl;

        // Per-instruction dump: NIOBIUM_DEBUG_INSTR=N prints every instruction's
        // result for the first N instructions. Set N=-1 for all.
        const char* dbg_env = std::getenv("NIOBIUM_DEBUG_INSTR");
        long dbg_limit = dbg_env ? std::atol(dbg_env) : 0;

        auto last_report = start;
        for (size_t i = 0; i < total; i++) {
            const auto& inst = trace.instructions[i];
            bool ok = true;

            switch (inst.opcode) {
            case OpCode::SR_ADDP:        ok = exec_addp(inst);  break;
            case OpCode::SR_SUBP:        ok = exec_subp(inst);  break;
            case OpCode::SR_MULP:        ok = exec_mulp(inst);  break;
            case OpCode::SR_ADDPS:
            case OpCode::SR_ADDPS_COEFF: ok = exec_addps(inst); break;
            case OpCode::SR_SUBPS:
            case OpCode::SR_SUBPS_COEFF: ok = exec_subps(inst); break;
            case OpCode::SR_MULPS:       ok = exec_mulps(inst); break;
            case OpCode::SR_NEGP:        ok = exec_negp(inst);  break;
            case OpCode::SR_NTT:         ok = exec_ntt(inst);   break;
            case OpCode::SR_INTT:        ok = exec_intt(inst);  break;

            case OpCode::SR_AUTOMORPH_EVAL: ok = exec_automorph_eval(inst); break;

            case OpCode::SR_ROT_AUTOMORPH_COEFF: ok = exec_rot_automorph_coeff(inst); break;

            case OpCode::SR_PERMUTE:
            case OpCode::SR_AUTOMORPH_COEFF:
                // TODO: general permutation and Galois X^k in coefficient form
                // not yet implemented; the eval-form Galois automorphism and
                // the negacyclic-coefficient rotation handlers above cover
                // the cases haze surfaces.
                ok = true;
                break;

            // Non-integer ops: pass through (no modular reduction)
            case OpCode::SR_ADDP_NI:
            case OpCode::SR_SUBP_NI:
            case OpCode::SR_MULP_NI:
            case OpCode::SR_ADDPS_NI:
            case OpCode::SR_SUBPS_NI:
            case OpCode::SR_MULPS_NI:
            case OpCode::SR_ADDPS_COEFF_NI:
            case OpCode::SR_SUBPS_COEFF_NI:
            case OpCode::SR_NEGP_NI:
            case OpCode::SR_FT:
            case OpCode::SR_IFT:
                ok = true;  // TODO: non-integer arithmetic
                break;

            case OpCode::HALT:
                ok = true;
                break;

            case OpCode::COMMENT:
            case OpCode::UNKNOWN:
                ok = true;
                break;
            }

            if (ok) executed++;

            // Per-instruction result dump (for pinpointing divergence)
            if (ok && (dbg_limit < 0 || (long)i < dbg_limit)) {
                if (memory.is_initialized(inst.dest)) {
                    const auto& v = memory.get(inst.dest).values;
                    std::cout << "[FHETCH_SIM-DBG] #" << i
                              << " " << inst.raw_line
                              << "  →  %" << inst.dest
                              << " v[0..3]=" << (!v.empty()?v[0]:0)
                              << "," << (v.size()>1?v[1]:0)
                              << "," << (v.size()>2?v[2]:0)
                              << "," << (v.size()>3?v[3]:0)
                              << std::endl;
                }
            }

            // Sample BEFORE the free hook so peak captures the working set
            // at its largest, just after this instruction wrote its dest.
            if (memory.size() > peak_mem) peak_mem = memory.size();

            // Release addresses whose last use was this instruction.
            // free_after_ is empty when NIOBIUM_DISABLE_SIM_FREES=1
            // skipped compute_liveness; otherwise it is sized to the
            // instruction count and the bound is always satisfied.
            if (i < free_after_.size()) {
                for (uint64_t a : free_after_[i]) memory.erase(a);
            }

            // Progress reporting every 2 seconds
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_report).count() >= 2) {
                int pct = static_cast<int>(100 * (i + 1) / total);
                std::cout << "\r[FHETCH_SIM] Progress: " << pct << "% ("
                          << (i + 1) << "/" << total << ")" << std::flush;
                last_report = now;
            }
        }

        auto end = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(end - start).count();

        std::cout << "\r[FHETCH_SIM] Complete: " << executed << " executed, "
                  << error_count << " errors, "
                  << std::fixed << std::setprecision(2) << elapsed << "s"
                  << std::endl;

        // Without freeing, peak == final size == total distinct addresses
        // touched, so diffing the two runs gives the savings.
        const double mib_per_poly = (ring_dim * sizeof(uint64_t)) / (1024.0 * 1024.0);
        std::cout << "[FHETCH_SIM] Peak working set: " << peak_mem
                  << " polys (~" << std::fixed << std::setprecision(2)
                  << peak_mem * mib_per_poly << " MiB at N=" << ring_dim << ")"
                  << ", final size: " << memory.size() << std::endl;

        return {executed, error_count, elapsed};
    }
};

// ============================================================================
// Public API
// ============================================================================

Simulator::Simulator() : impl_(std::make_unique<Impl>()) {}
Simulator::~Simulator() = default;

void Simulator::set_ring_dimension(uint64_t N) {
    impl_->ring_dim = N;
    impl_->memory.set_ring_dim(N);
}

bool Simulator::load_trace(const std::filesystem::path& trace_file) {
    std::ifstream in(trace_file);
    if (!in.is_open()) {
        std::cerr << "[FHETCH_SIM] Cannot open: " << trace_file << std::endl;
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    impl_->trace = parse_trace(content);

    std::cout << "[FHETCH_SIM] Loaded: " << trace_file << "\n"
              << "  Modulus table: " << impl_->trace.modulus_table.size() << " entries\n"
              << "  Instructions:  " << impl_->trace.instructions.size() << std::endl;
    return !impl_->trace.instructions.empty();
}

void Simulator::store_polynomial(uint64_t address,
                                 const std::vector<uint64_t>& values,
                                 uint64_t modulus) {
    impl_->memory.set(address, values, modulus);
}

void Simulator::store_polynomial(uint64_t address, std::vector<uint64_t>&& values,
                                 uint64_t modulus) {
    impl_->memory.set_owned(address, std::move(values), modulus);
}

SimResult Simulator::run() {
    if (impl_->ring_dim == 0) {
        std::cerr << "[FHETCH_SIM] ring dimension not set" << std::endl;
        return {0, 1, 0.0};
    }
    return impl_->execute();
}

std::vector<uint64_t> Simulator::get_polynomial(uint64_t address) const {
    if (impl_->memory.is_initialized(address))
        return impl_->memory.get(address).values;
    return {};
}

uint64_t Simulator::get_modulus(uint64_t address) const {
    return impl_->memory.get(address).modulus;
}

bool Simulator::is_initialized(uint64_t address) const {
    return impl_->memory.is_initialized(address);
}

std::vector<uint64_t> Simulator::get_read_before_write_addresses() const {
    std::set<uint64_t> written;
    std::vector<uint64_t> rbw;
    for (const auto& inst : impl_->trace.instructions) {
        auto u = Impl::classify_uses(inst);
        for (int i = 0; i < u.n_reads; ++i) {
            uint64_t a = u.reads[i];
            if (!a) continue;
            if (written.insert(a).second)
                rbw.push_back(a);
        }
        if (u.writes) written.insert(u.write);
    }
    return rbw;
}

void Simulator::set_live_out_addresses(const std::vector<uint64_t>& addrs) {
    impl_->live_out_.clear();
    impl_->live_out_.insert(addrs.begin(), addrs.end());
}

void Simulator::compute_liveness() {
    // Opt-out for A/B comparison: leave free_after_ empty.
    if (std::getenv("NIOBIUM_DISABLE_SIM_FREES")) {
        impl_->free_after_.clear();
        return;
    }

    auto& insts = impl_->trace.instructions;

    // Forward last-use scan: the final write to last_use[a] is a's
    // last read. Live-out probes are excluded so they never get freed.
    std::unordered_map<uint64_t, size_t> last_use;
    for (size_t i = 0; i < insts.size(); ++i) {
        auto u = Impl::classify_uses(insts[i]);
        for (int j = 0; j < u.n_reads; ++j) {
            uint64_t a = u.reads[j];
            if (a == 0) continue;
            if (impl_->live_out_.count(a) != 0U) continue;
            last_use[a] = i;
        }
    }

    std::vector<std::vector<uint64_t>> free_after(insts.size());

    // Dead-write scan: a write with no read anywhere, or whose address's
    // last read precedes it, produces a value nothing consumes — free it
    // at the writing instruction. Near-SSA traces make this the dominant
    // class (every value chain's final version). A write AT the last-read
    // instruction (dest==src aliasing) is not dead and is handled by the
    // read schedule below; an address freed here is simply re-created by
    // reserve_dest if a later write reuses it, then re-freed the same way.
    for (size_t i = 0; i < insts.size(); ++i) {
        auto u = Impl::classify_uses(insts[i]);
        if (!u.writes || u.write == 0) continue;
        if (impl_->live_out_.count(u.write) != 0U) continue;
        auto it = last_use.find(u.write);
        if (it == last_use.end() || it->second < i)
            free_after[i].push_back(u.write);
    }

    // Schedule a memory.erase() at the end of each address's last-use
    // instruction. The kernel for that instruction has already read
    // its sources by the time the execute loop consults free_after_.
    for (const auto& [addr, death] : last_use) {
        free_after[death].push_back(addr);
    }
    impl_->free_after_ = std::move(free_after);

    size_t total_frees = 0;
    for (const auto& v : impl_->free_after_) total_frees += v.size();
    std::cout << "[FHETCH_SIM] Liveness: " << total_frees
              << " frees scheduled across " << insts.size()
              << " instructions, " << impl_->live_out_.size()
              << " live-out pinned" << std::endl;
}

const std::vector<std::vector<uint64_t>>&
Simulator::get_free_after_for_test() const {
    return impl_->free_after_;
}

}  // namespace niobium::fhetch_sim
