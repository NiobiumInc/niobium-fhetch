// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

#include "niobium/fhetch_reader.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>

namespace niobium::fhetch {
namespace {

std::string_view trim(std::string_view s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

/// Drop one trailing comma, which the text form uses as an operand separator.
std::string_view strip_comma(std::string_view s) {
    s = trim(s);
    if (!s.empty() && s.back() == ',') s.remove_suffix(1);
    return trim(s);
}

/// Unsigned integer in any base the text may use. Base 0 accepts `0x` — the
/// two previous readers disagreed here, one accepting hex and one silently
/// yielding zero for it, so accepting the superset is the resolution.
bool to_uint(std::string_view tok, uint64_t& out) {
    const std::string s(strip_comma(tok));
    if (s.empty()) return false;
    // Reject a leading sign outright: strtoull would accept "-1" and wrap it
    // into a huge address.
    if (s[0] == '-' || s[0] == '+') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 0);
    if (errno == ERANGE || end != s.c_str() + s.size()) return false;
    out = static_cast<uint64_t>(v);
    return true;
}

/// Floating-point immediate, for the non-integer poly-scalar forms.
bool to_double(std::string_view tok, double& out) {
    const std::string s(strip_comma(tok));
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (errno == ERANGE || end != s.c_str() + s.size()) return false;
    out = v;
    return true;
}

/// `%N` → N.
bool to_addr_token(std::string_view tok, uint64_t& out) {
    const std::string_view s = strip_comma(tok);
    if (s.empty() || s.front() != '%') return false;
    return to_uint(s.substr(1), out);
}

/// `<prefix>N` → N, e.g. `k=7`, `omega=0x1AB`.
bool to_named(std::string_view tok, std::string_view prefix, uint64_t& out) {
    const std::string_view s = strip_comma(tok);
    if (s.size() <= prefix.size() || s.compare(0, prefix.size(), prefix) != 0)
        return false;
    return to_uint(s.substr(prefix.size()), out);
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// ---------------------------------------------------------------------------
// Header comment block
// ---------------------------------------------------------------------------

/// `# <Key>: <value>` lines carry the metadata the writer emitted. Keys are
/// matched exactly as written, so this table and the writer's header block are
/// the two halves of one contract.
bool absorb_metadata(std::string_view body, TraceMetadata& meta) {
    const size_t colon = body.find(':');
    if (colon == std::string_view::npos) return false;
    const std::string key(trim(body.substr(0, colon)));
    const std::string value(trim(body.substr(colon + 1)));
    if (value.empty()) return false;

    auto as_int = [&](int& dst) {
        uint64_t v = 0;
        if (to_uint(value, v)) dst = static_cast<int>(v);
    };
    auto as_u64 = [&](uint64_t& dst) {
        uint64_t v = 0;
        if (to_uint(value, v)) dst = v;
    };

    if (key == "Program") {
        // The writer emits "<name> v<version>". Split into the two fields
        // rather than gluing the version onto the name, so a consumer never
        // has to take it apart again.
        const size_t sp = value.rfind(" v");
        if (sp == std::string::npos) {
            meta.program_name = value;
        } else {
            meta.program_name = value.substr(0, sp);
            meta.program_version = value.substr(sp + 2);
        }
    } else if (key == "Description") {
        meta.description = value;
    } else if (key == "Source") {
        const size_t colon2 = value.rfind(':');
        if (colon2 == std::string::npos) {
            meta.source_file = value;
        } else {
            meta.source_file = value.substr(0, colon2);
            uint64_t line = 0;
            if (to_uint(value.substr(colon2 + 1), line))
                meta.source_line = static_cast<int>(line);
            else
                meta.source_file = value;  // a colon that was not a line number
        }
    } else if (key == "Build") {
        meta.build_timestamp = value;
    } else if (key == "Instruction Count") {
        as_int(meta.instruction_count);
    } else if (key == "Modulus Count") {
        // The count of the table, which is also the chain length. Both
        // spellings of that one quantity are accepted; the writer emits this
        // one.
        as_int(meta.modulus_chain_length);
    } else if (key == "Modulus Chain Length") {
        as_int(meta.modulus_chain_length);
    } else if (key == "Generated") {
        as_u64(meta.generated_timestamp);
    } else if (key == "Scheme" || key == "Crypto Scheme") {
        meta.scheme = value;
    } else if (key == "Ring Dimension") {
        as_int(meta.ring_dimension);
    } else if (key == "Multiplicative Depth") {
        as_int(meta.multiplicative_depth);
    } else if (key == "Security Level") {
        meta.security_level = value;
    } else if (key == "Trace File") {
        meta.trace_file = value;
    } else {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

class Reader {
public:
    explicit Reader(std::string_view text) : text_(text) {}

    ReadResult run() {
        std::string line;
        std::istringstream stream{std::string(text_)};
        while (std::getline(stream, line)) {
            ++line_;
            const std::string_view t = trim(line);
            if (t.empty()) continue;
            if (t.front() == '#') { comment(t); continue; }
            if (t.rfind("modulus_count", 0) == 0) { modulus_count(t); continue; }
            if (t.size() > 2 && t[0] == 'm' && t[1] == '[') { modulus_entry(t); continue; }
            instruction(t);
        }

        // Whatever the header claimed, the table we actually read is the truth.
        result_.program.metadata.modulus_chain = result_.program.modulus_table;
        result_.ok = result_.errors.empty();
        return std::move(result_);
    }

private:
    void error(std::string msg) { result_.errors.push_back({line_, std::move(msg)}); }
    void warn(std::string msg) { result_.warnings.push_back({line_, std::move(msg)}); }

    void comment(std::string_view t) {
        std::string_view body = trim(t.substr(1));
        if (body.empty()) return;
        // A rule of '=' characters is the writer's separator, not content.
        if (body.find_first_not_of('=') == std::string_view::npos) return;
        // The writer re-emits its own header block and section labels from
        // metadata, so keeping them as annotations too would duplicate them on
        // a round trip. Only comments the writer would not otherwise produce
        // are content.
        if (body == "Niobium FHETCH Trace" || body == "Modulus Table" ||
            body == "Instructions")
            return;
        if (absorb_metadata(body, result_.program.metadata)) return;
        result_.program.annotations.push_back(
            {line_, std::string(body), result_.program.instructions.size()});
    }

    void modulus_count(std::string_view t) {
        uint64_t n = 0;
        if (!to_uint(t.substr(std::string_view("modulus_count").size()), n)) {
            error("malformed modulus_count");
            return;
        }
        result_.program.modulus_table.reserve(static_cast<size_t>(n));
    }

    void modulus_entry(std::string_view t) {
        const size_t close = t.find(']');
        const size_t space = t.find(' ');
        if (close == std::string_view::npos || space == std::string_view::npos ||
            space < close) {
            error("malformed modulus table entry");
            return;
        }
        uint64_t index = 0;
        uint64_t value = 0;
        if (!to_uint(t.substr(2, close - 2), index) ||
            !to_uint(trim(t.substr(space + 1)), value)) {
            error("malformed modulus table entry");
            return;
        }
        // Entries are written in order; an out-of-order or gapped table would
        // silently shift every m= reference in the program.
        if (index != result_.program.modulus_table.size()) {
            error("modulus table entry out of order: expected index " +
                  std::to_string(result_.program.modulus_table.size()) +
                  ", got " + std::to_string(index));
            return;
        }
        result_.program.modulus_table.push_back(value);
    }

    /// An address wide enough to lose bits is refused, not truncated.
    bool address(std::string_view tok, uint32_t& out) {
        uint64_t v = 0;
        if (!to_addr_token(tok, v)) return false;
        if (v > kMaxAddress) {
            error("address %" + std::to_string(v) + " exceeds the " +
                  std::to_string(kMaxAddress) + " an instruction can hold");
            return false;
        }
        out = static_cast<uint32_t>(v);
        return true;
    }

    /// `m=<N>`. Index when it fits the table, literal prime otherwise — see
    /// the note on read_fhetch_text().
    bool modulus(std::string_view tok, Instruction& inst) {
        uint64_t v = 0;
        if (!to_named(tok, "m=", v)) return false;
        const size_t table = result_.program.modulus_table.size();
        if (v < table) {
            if (v > kMaxModulusIndex) {
                error("modulus index " + std::to_string(v) +
                      " exceeds the encodable maximum of " +
                      std::to_string(kMaxModulusIndex));
                return false;
            }
            inst.modulus = v;
            inst.modulus_is_literal = false;
        } else {
            inst.modulus = v;
            inst.modulus_is_literal = true;
        }
        return true;
    }

    void instruction(std::string_view t) {
        // An inline comment ends the instruction.
        const size_t hash = t.find('#');
        const std::string_view body = (hash == std::string_view::npos)
                                          ? t : trim(t.substr(0, hash));
        if (body.empty()) return;

        std::istringstream tokens{std::string(body)};
        std::string mnemonic;
        tokens >> mnemonic;
        const auto opcode = fh_opcode_from_mnemonic(lower(mnemonic));
        if (!opcode) {
            // Forward compatibility: an opcode this build does not know is
            // skipped, not fatal.
            warn("unknown opcode '" + mnemonic + "', line skipped");
            return;
        }

        std::vector<std::string> args;
        for (std::string tok; tokens >> tok;) args.push_back(std::move(tok));

        Instruction inst;
        inst.opcode = *opcode;
        inst.line_number = line_;

        if (!operands(inst, args)) return;
        result_.program.instructions.push_back(std::move(inst));
    }

    /// Operand layout comes from the opcode's shape, so the reader and the
    /// writer are two readings of one table rather than two switches.
    bool operands(Instruction& inst, const std::vector<std::string>& args) {
        const OperandForm form = fh_operand_form(inst.opcode);

        auto need = [&](size_t n) {
            if (args.size() >= n) return true;
            warn(std::string(fh_opcode_mnemonic(inst.opcode)) + " needs " +
                 std::to_string(n) + " operands, found " +
                 std::to_string(args.size()) + ", line skipped");
            return false;
        };
        auto dest_src1 = [&] {
            return address(args[0], inst.dest) && address(args[1], inst.src1);
        };

        switch (form) {
        case OperandForm::None:
            return true;

        case OperandForm::PolyPoly:
            return need(4) && dest_src1() && address(args[2], inst.src2) &&
                   modulus(args[3], inst);

        case OperandForm::PolyPolyNI:
            return need(3) && dest_src1() && address(args[2], inst.src2);

        case OperandForm::PolyScalar: {
            if (!need(4) || !dest_src1()) return false;
            uint64_t imm = 0;
            if (!to_uint(args[2], imm)) { warn("malformed immediate"); return false; }
            inst.immediate = imm;
            return modulus(args[3], inst);
        }

        case OperandForm::PolyScalarNI: {
            if (!need(3) || !dest_src1()) return false;
            // These carry a NonInteger scalar, so the immediate is floating
            // point. Parsing it as an integer is what made the previous reader
            // reject every `_ni` scalar instruction outright.
            double imm = 0;
            if (!to_double(args[2], imm)) { warn("malformed immediate"); return false; }
            inst.fp_immediate = imm;
            return true;
        }

        case OperandForm::UnaryMod:
            return need(3) && dest_src1() && modulus(args[2], inst);

        case OperandForm::UnaryNoMod:
            return need(2) && dest_src1();

        case OperandForm::Ntt: {
            if (!need(3) || !dest_src1() || !modulus(args[2], inst)) return false;
            // omega is optional and, when present, authoritative: a consumer
            // prefers it over deriving a root, so dropping it changes results.
            for (size_t i = 3; i < args.size(); ++i) {
                uint64_t w = 0;
                if (to_named(args[i], "omega=", w)) { inst.omega = w; break; }
            }
            return true;
        }

        case OperandForm::AutomorphEval: {
            if (!need(3) || !dest_src1()) return false;
            // Named operands may appear in any order after src1, and mask/logn
            // are optional. Earlier readers parsed k and m and discarded
            // mask/logn outright; they are kept now because the binary form
            // carries them and the two must agree.
            bool saw_mod = false;
            for (size_t i = 2; i < args.size(); ++i) {
                uint64_t v = 0;
                if (to_named(args[i], "k=", v)) { inst.k = v; continue; }
                if (to_named(args[i], "mask=", v)) { inst.mask = v; continue; }
                if (to_named(args[i], "logn=", v)) {
                    if (v > 64) { warn("logn out of range, ignored"); continue; }
                    inst.logn = static_cast<uint8_t>(v);
                    continue;
                }
                if (modulus(args[i], inst)) { saw_mod = true; continue; }
                if (!result_.errors.empty()) return false;  // modulus() errored
            }
            if (!saw_mod) { warn("sr_automorph_eval without a modulus"); return false; }
            return true;
        }

        case OperandForm::AutomorphCoeff: {
            if (!need(4) || !dest_src1()) return false;
            uint64_t k = 0;
            if (!to_named(args[2], "k=", k)) { warn("expected k="); return false; }
            inst.k = k;
            return modulus(args[3], inst);
        }

        case OperandForm::RotAutomorphCoeff: {
            if (!need(4) || !dest_src1()) return false;
            uint64_t off = 0;
            if (!to_named(args[2], "offset=", off)) { warn("expected offset="); return false; }
            inst.offset = off;
            return modulus(args[3], inst);
        }
        }
        return false;
    }

    std::string_view text_;
    int line_ = 0;
    ReadResult result_;
};

}  // namespace

ReadResult read_fhetch_text(std::string_view text) {
    return Reader(text).run();
}

ReadResult read_fhetch_binary(const std::byte* bytes, size_t size) {
    ReadResult result;
    FhexDecodeError err = FhexDecodeError::None;
    auto decoded = decode_fhetch_program(bytes, size, &err);
    if (!decoded.has_value()) {
        result.ok = false;
        result.errors.push_back({0, fhex_decode_error_string(err)});
        return result;
    }

    result.program.metadata = std::move(decoded->metadata);
    result.program.modulus_table = std::move(decoded->modulus_table);
    result.program.instructions.reserve(decoded->records.size());
    for (size_t i = 0; i < decoded->records.size(); ++i) {
        Instruction inst = from_record(decoded->records[i]);
        // Number from 1 so diagnostics read the same whichever form was read,
        // even though a binary file has no lines.
        inst.line_number = static_cast<int>(i) + 1;
        result.program.instructions.push_back(std::move(inst));
    }
    // Comments have no binary representation; a program read from `.fhex`
    // simply has none.
    result.ok = true;
    return result;
}

ReadResult read_fhetch_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        ReadResult r;
        r.ok = false;
        r.errors.push_back({0, "cannot open " + path.string()});
        return r;
    }
    const std::string data((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());

    // Detect the form from the content, not the file name: a producer is free
    // to name a binary trace whatever it likes, and a consumer should not have
    // to care.
    if (data.size() >= sizeof(kFhexMagic) &&
        std::memcmp(data.data(), kFhexMagic, sizeof(kFhexMagic)) == 0) {
        return read_fhetch_binary(
            reinterpret_cast<const std::byte*>(data.data()), data.size());
    }
    return read_fhetch_text(data);
}

}  // namespace niobium::fhetch
