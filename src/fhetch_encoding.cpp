// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

#include "niobium/fhetch_encoding.h"

#include <cstring>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace niobium::fhetch {

namespace {

// ---------------------------------------------------------------------------
// Serialization primitives
//
// Fields are appended one at a time through fhex_le() rather than by
// memcpy'ing whole structs, so the byte order on disk is the format's, not
// the host's. On a little-endian host every fhex_le() is the identity and
// the compiler collapses these to plain stores.
// ---------------------------------------------------------------------------

void put_raw(std::vector<std::byte>& out, const void* src, size_t count) {
    const auto* first = static_cast<const std::byte*>(src);
    out.insert(out.end(), first, first + count);
}

template <typename T>
void put_scalar(std::vector<std::byte>& out, T value) {
    const T wire = fhex_le(value);
    put_raw(out, &wire, sizeof(wire));
}

/// Zero-fill to the next 8-byte boundary, keeping every section aligned.
void pad_to_align(std::vector<std::byte>& out) {
    while (out.size() % kFhexAlign != 0) out.push_back(std::byte{0});
}

/// Read a scalar at `offset`. The caller has already bounds-checked.
template <typename T>
T get_scalar(const std::byte* bytes, size_t offset) {
    T wire{};
    std::memcpy(&wire, bytes + offset, sizeof(wire));
    return fhex_le(wire);
}

// ---------------------------------------------------------------------------
// Varints (unsigned LEB128)
//
// Seven payload bits per byte, high bit set on every byte but the last.
// Endianness does not enter into it — a varint is a byte sequence by
// definition — so these do not go through fhex_le().
// ---------------------------------------------------------------------------

void put_varint(std::vector<std::byte>& out, uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<std::byte>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<std::byte>(value));
}

/// Read a varint at `offset`, advancing it past the value.
///
/// Returns false — leaving `offset` unspecified — if the value runs past
/// `size` or its continuation chain exceeds what 64 bits can hold. Both are
/// corruption, and both must terminate: an unbounded loop on a hostile file
/// is the failure mode this guards against.
bool get_varint(const std::byte* bytes, size_t size, size_t& offset,
                uint64_t& out) {
    uint64_t value = 0;
    for (size_t index = 0; index < kFhexMaxVarintBytes; ++index) {
        if (offset >= size) return false;
        const auto byte = static_cast<uint8_t>(bytes[offset++]);
        const uint64_t payload = byte & 0x7F;
        const size_t shift = index * 7;

        // The tenth byte of a 64-bit LEB128 has only one bit left to place, so
        // a payload with anything above that would be silently truncated by
        // the shift. Reject instead: letting it through would make two
        // different byte sequences decode to the same value, and a hostile
        // file could smuggle bits past a reader that had already validated
        // the value it thought it read.
        if (payload > (UINT64_MAX >> shift)) return false;

        value |= payload << shift;
        if ((byte & 0x80) == 0) {
            out = value;
            return true;
        }
    }
    return false;
}

/// Read a varint that must fit a narrower field. A value too wide for the
/// destination is corruption rather than something to silently truncate.
bool get_varint_u32(const std::byte* bytes, size_t size, size_t& offset,
                    uint32_t& out) {
    uint64_t wide = 0;
    if (!get_varint(bytes, size, offset, wide)) return false;
    if (wide > UINT32_MAX) return false;
    out = static_cast<uint32_t>(wide);
    return true;
}

// ---------------------------------------------------------------------------
// Metadata section
//
// Empty strings and zero integers are omitted rather than written as empty
// entries: every TraceMetadata field they map to already default-constructs to
// exactly that value, so omitting them round-trips faithfully and keeps the
// section small.
// ---------------------------------------------------------------------------

void put_meta_entry(std::vector<std::byte>& out, FhMetaTag tag,
                    const void* payload, size_t length) {
    put_scalar<uint16_t>(out, static_cast<uint16_t>(tag));
    put_scalar<uint16_t>(out, 0);  // reserved
    put_scalar<uint32_t>(out, static_cast<uint32_t>(length));
    put_raw(out, payload, length);
    pad_to_align(out);
}

void put_meta_string(std::vector<std::byte>& out, FhMetaTag tag,
                     const std::string& value) {
    if (value.empty()) return;
    put_meta_entry(out, tag, value.data(), value.size());
}

void put_meta_u64(std::vector<std::byte>& out, FhMetaTag tag, uint64_t value) {
    if (value == 0) return;
    const uint64_t wire = fhex_le(value);
    put_meta_entry(out, tag, &wire, sizeof(wire));
}

std::vector<std::byte> build_metadata(const TraceMetadata& metadata,
                                      FhetchSpec spec) {
    std::vector<std::byte> meta;
    // Spec identity first, so a reader that only wants to know what revision
    // it is looking at finds it in the first entry.
    if (spec.version.known()) {
        put_meta_u64(meta, FhMetaTag::FhetchSpecVersion,
                     fhex_pack_version(spec.version));
    }
    if (spec.date.known()) {
        put_meta_u64(meta, FhMetaTag::FhetchSpecDateTag,
                     fhex_pack_spec_date(spec.date));
    }
    put_meta_string(meta, FhMetaTag::ProgramName, metadata.program_name);
    put_meta_string(meta, FhMetaTag::ProgramVersion, metadata.program_version);
    put_meta_string(meta, FhMetaTag::TraceFile, metadata.trace_file);
    put_meta_string(meta, FhMetaTag::Description, metadata.description);
    put_meta_string(meta, FhMetaTag::SourceFile, metadata.source_file);
    put_meta_string(meta, FhMetaTag::BuildTimestamp, metadata.build_timestamp);
    put_meta_string(meta, FhMetaTag::Scheme, metadata.scheme);
    put_meta_string(meta, FhMetaTag::SecurityLevel, metadata.security_level);
    put_meta_u64(meta, FhMetaTag::SourceLine,
                 static_cast<uint64_t>(metadata.source_line));
    put_meta_u64(meta, FhMetaTag::InstructionCount,
                 static_cast<uint64_t>(metadata.instruction_count));
    put_meta_u64(meta, FhMetaTag::GeneratedTimestamp, metadata.generated_timestamp);
    put_meta_u64(meta, FhMetaTag::RingDimension,
                 static_cast<uint64_t>(metadata.ring_dimension));
    put_meta_u64(meta, FhMetaTag::MultiplicativeDepth,
                 static_cast<uint64_t>(metadata.multiplicative_depth));
    put_meta_u64(meta, FhMetaTag::ModulusChainLength,
                 static_cast<uint64_t>(metadata.modulus_chain_length));
    return meta;
}

/// Walk the TLV entries, filling `metadata` and `spec`. Unrecognized tags are
/// skipped — that is what lets a later toolchain add fields without a version
/// bump. Returns false if an entry's length runs past the section.
bool parse_metadata(const std::byte* meta, size_t size, TraceMetadata& metadata,
                    FhetchSpec& spec) {
    size_t offset = 0;
    while (offset + sizeof(FhMetaEntry) <= size) {
        const auto tag =
            get_scalar<uint16_t>(meta, offset + offsetof(FhMetaEntry, tag));
        const auto length =
            get_scalar<uint32_t>(meta, offset + offsetof(FhMetaEntry, length));
        const size_t payload = offset + sizeof(FhMetaEntry);
        if (payload + length > size) return false;

        const char* text = reinterpret_cast<const char*>(meta + payload);
        auto as_string = [&] { return std::string(text, length); };

        // Every numeric tag is eight bytes; a zero substituted for a wrong
        // width would read as a plausible ring dimension or count.
        const bool numeric_width_ok = (length == sizeof(uint64_t));
        auto as_u64 = [&] { return get_scalar<uint64_t>(meta, payload); };

        switch (static_cast<FhMetaTag>(tag)) {
        case FhMetaTag::ProgramName:    metadata.program_name = as_string(); break;
        case FhMetaTag::ProgramVersion: metadata.program_version = as_string(); break;
        case FhMetaTag::TraceFile:      metadata.trace_file = as_string(); break;
        case FhMetaTag::Description:    metadata.description = as_string(); break;
        case FhMetaTag::SourceFile:     metadata.source_file = as_string(); break;
        case FhMetaTag::BuildTimestamp: metadata.build_timestamp = as_string(); break;
        case FhMetaTag::Scheme:         metadata.scheme = as_string(); break;
        case FhMetaTag::SecurityLevel:  metadata.security_level = as_string(); break;
        case FhMetaTag::SourceLine:
            if (!numeric_width_ok) return false;
            metadata.source_line = static_cast<int>(as_u64());
            break;
        case FhMetaTag::InstructionCount:
            if (!numeric_width_ok) return false;
            metadata.instruction_count = static_cast<int>(as_u64());
            break;
        case FhMetaTag::GeneratedTimestamp:
            if (!numeric_width_ok) return false;
            metadata.generated_timestamp = as_u64();
            break;
        case FhMetaTag::RingDimension:
            if (!numeric_width_ok) return false;
            metadata.ring_dimension = static_cast<int>(as_u64());
            break;
        case FhMetaTag::MultiplicativeDepth:
            if (!numeric_width_ok) return false;
            metadata.multiplicative_depth = static_cast<int>(as_u64());
            break;
        case FhMetaTag::ModulusChainLength:
            if (!numeric_width_ok) return false;
            metadata.modulus_chain_length = static_cast<int>(as_u64());
            break;
        case FhMetaTag::FhetchSpecVersion:
            if (!numeric_width_ok) return false;
            spec.version = fhex_unpack_version(as_u64());
            break;
        case FhMetaTag::FhetchSpecDateTag:
            if (!numeric_width_ok) return false;
            spec.date = fhex_unpack_spec_date(as_u64());
            break;
        default:
            break;  // forward compatibility: skip what we don't know
        }
        offset = payload + fhex_align_up(length);
    }
    return true;
}

}  // namespace

FhetchRecord to_record(const Instruction& inst) {
    FhetchRecord r;
    r.opcode = static_cast<uint8_t>(inst.opcode);
    r.dst = inst.dest;
    r.src1 = inst.src1;

    // src2 is only meaningful on the three-address forms; the flag says so
    // rather than the value, because zero is a legal address.
    switch (fh_operand_form(inst.opcode)) {
    case OperandForm::PolyPoly:
    case OperandForm::PolyPolyNI:
        r.flags |= FH_FLAG_HAS_SRC2;
        r.src2 = inst.src2;
        break;
    default:
        break;
    }

    if (inst.modulus.has_value()) {
        r.flags |= FH_FLAG_HAS_MOD;
        if (inst.modulus_is_literal) r.flags |= FH_FLAG_MOD_LITERAL;
        r.modulus = *inst.modulus;
    }

    // A non-integer immediate goes across as its exact bit pattern, so the
    // binary form round-trips a double that the text form rounds to 6 places.
    // The opcode says which reading applies, so no extra flag is needed.
    if (inst.fp_immediate.has_value()) {
        r.flags |= FH_FLAG_HAS_IMM;
        double v = *inst.fp_immediate;
        uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        r.imm = bits;
    } else if (inst.immediate.has_value()) {
        r.flags |= FH_FLAG_HAS_IMM;
        r.imm = *inst.immediate;
    } else if (inst.k.has_value()) {
        r.flags |= FH_FLAG_HAS_IMM;
        r.imm = *inst.k;
    } else if (inst.offset.has_value()) {
        r.flags |= FH_FLAG_HAS_IMM;
        r.imm = *inst.offset;
    }

    // aux carries omega or mask; likewise mutually exclusive.
    if (inst.omega.has_value()) {
        r.flags |= FH_FLAG_HAS_AUX;
        r.aux = *inst.omega;
    } else if (inst.mask.has_value()) {
        r.flags |= FH_FLAG_HAS_AUX;
        r.aux = *inst.mask;
    }

    if (inst.logn.has_value()) {
        r.flags |= FH_FLAG_HAS_LOGN;
        r.logn = *inst.logn;
    }
    return r;
}

Instruction from_record(const FhetchRecord& record) {
    Instruction inst;
    inst.opcode = static_cast<FhOpcode>(record.opcode);
    inst.dest = record.dst;
    inst.src1 = record.src1;
    if ((record.flags & FH_FLAG_HAS_SRC2) != 0) inst.src2 = record.src2;
    if ((record.flags & FH_FLAG_HAS_MOD) != 0) {
        inst.modulus = record.modulus;
        inst.modulus_is_literal = (record.flags & FH_FLAG_MOD_LITERAL) != 0;
    }

    // imm and aux are multiplexed on the wire; the opcode says which operand
    // they held, which is why this switch and to_record()'s must agree.
    const OperandForm form = fh_operand_form(inst.opcode);
    if ((record.flags & FH_FLAG_HAS_IMM) != 0) {
        switch (form) {
        case OperandForm::AutomorphEval:
        case OperandForm::AutomorphCoeff:  inst.k = record.imm; break;
        case OperandForm::RotAutomorphCoeff: inst.offset = record.imm; break;
        case OperandForm::PolyScalarNI: {
            double v = 0;
            const uint64_t bits = record.imm;
            std::memcpy(&v, &bits, sizeof(v));
            inst.fp_immediate = v;
            break;
        }
        default:                           inst.immediate = record.imm; break;
        }
    }
    if ((record.flags & FH_FLAG_HAS_AUX) != 0) {
        if (form == OperandForm::Ntt) inst.omega = record.aux;
        else inst.mask = record.aux;
    }
    if ((record.flags & FH_FLAG_HAS_LOGN) != 0) inst.logn = record.logn;
    return inst;
}

std::vector<std::byte> encode_fhetch_program(
    const TraceMetadata& metadata, const std::vector<uint64_t>& modulus_table,
    const std::vector<FhetchRecord>& records, FhetchSpec spec) {
    // Refuse to write a file that cannot say what revision it is in. This is
    // a caller bug, not bad input, so it throws rather than returning empty —
    // an empty buffer would be written out as a zero-length sidecar and only
    // fail much later, on read.
    if (!spec.valid())
        throw std::invalid_argument(
            "encode_fhetch_program: FhetchSpec names neither a spec version "
            "nor a spec date — at least one is required");

    const std::vector<std::byte> meta = build_metadata(metadata, spec);

    std::vector<std::byte> out;
    // Records are variable length, so this is a typical size rather than an
    // exact one: opcode, flags, and a handful of small varint operands. Over-
    // or under-shooting only costs a reallocation.
    static constexpr size_t kTypicalRecordBytes = 8;
    out.reserve(kFhexPrologueBytes + meta.size() +
                modulus_table.size() * sizeof(uint64_t) +
                records.size() * kTypicalRecordBytes);

    put_raw(out, kFhexMagic, sizeof(kFhexMagic));
    put_scalar<uint16_t>(out, kFhexVersion);
    put_scalar<uint16_t>(out, 0);  // flags, reserved
    put_scalar<uint32_t>(out, static_cast<uint32_t>(meta.size()));
    put_scalar<uint32_t>(out, static_cast<uint32_t>(modulus_table.size()));
    put_scalar<uint64_t>(out, static_cast<uint64_t>(records.size()));

    put_raw(out, meta.data(), meta.size());
    for (uint64_t modulus : modulus_table) put_scalar<uint64_t>(out, modulus);

    for (const FhetchRecord& record : records) {
        put_scalar<uint8_t>(out, record.opcode);
        put_scalar<uint8_t>(out, record.flags);
        if ((record.flags & FH_FLAG_HAS_LOGN) != 0)
            put_scalar<uint8_t>(out, record.logn);
        if (fh_opcode_has_operands(record.opcode)) {
            put_varint(out, record.dst);
            put_varint(out, record.src1);
        }
        if ((record.flags & FH_FLAG_HAS_SRC2) != 0) put_varint(out, record.src2);
        if ((record.flags & FH_FLAG_HAS_MOD) != 0) put_varint(out, record.modulus);
        if ((record.flags & FH_FLAG_HAS_IMM) != 0) put_varint(out, record.imm);
        if ((record.flags & FH_FLAG_HAS_AUX) != 0) put_varint(out, record.aux);
    }
    return out;
}

std::optional<FhexProgram> decode_fhetch_program(const std::byte* bytes,
                                                 size_t size,
                                                 FhexDecodeError* err) {
    auto fail = [&](FhexDecodeError reason) -> std::optional<FhexProgram> {
        if (err) *err = reason;
        return std::nullopt;
    };
    if (err) *err = FhexDecodeError::None;

    if (bytes == nullptr || size < kFhexPrologueBytes)
        return fail(FhexDecodeError::BadMagic);
    if (std::memcmp(bytes + offsetof(FhexPrologue, magic), kFhexMagic,
                    sizeof(kFhexMagic)) != 0)
        return fail(FhexDecodeError::BadMagic);
    if (get_scalar<uint16_t>(bytes, offsetof(FhexPrologue, version)) !=
        kFhexVersion)
        return fail(FhexDecodeError::UnsupportedVersion);

    const auto meta_bytes =
        get_scalar<uint32_t>(bytes, offsetof(FhexPrologue, metadata_bytes));
    const auto modulus_count =
        get_scalar<uint32_t>(bytes, offsetof(FhexPrologue, modulus_count));
    const auto instruction_count =
        get_scalar<uint64_t>(bytes, offsetof(FhexPrologue, instruction_count));

    // Misaligned metadata would throw every later section off its 8-byte
    // boundary, so treat it as corruption rather than trying to recover.
    if (meta_bytes % kFhexAlign != 0) return fail(FhexDecodeError::Malformed);

    // Compute section bounds in a width that cannot wrap on a hostile file.
    const size_t meta_offset = kFhexPrologueBytes;
    if (meta_bytes > size - meta_offset) return fail(FhexDecodeError::Truncated);
    const size_t modulus_offset = meta_offset + meta_bytes;
    if (static_cast<uint64_t>(modulus_count) * sizeof(uint64_t) >
        size - modulus_offset) {
        return fail(FhexDecodeError::Truncated);
    }
    const size_t record_offset =
        modulus_offset + static_cast<size_t>(modulus_count) * sizeof(uint64_t);
    // Records are variable length, so the count cannot be validated against
    // the remaining bytes up front the way a fixed stride allows. The cheapest
    // sound bound is the smallest a record can be — opcode plus flags — which
    // stops a declared count of billions from driving a huge reserve() before
    // the stream is found to be short.
    static constexpr size_t kMinRecordBytes = 2;
    if (instruction_count > (size - record_offset) / kMinRecordBytes)
        return fail(FhexDecodeError::Truncated);

    FhexProgram program;
    program.format_version = kFhexVersion;  // checked equal above
    if (!parse_metadata(bytes + meta_offset, meta_bytes, program.metadata,
                        program.spec))
        return fail(FhexDecodeError::Malformed);
    // Mirror of the encode-side check: a file must name the revision it is
    // written in, so a reader can never silently interpret one revision's
    // program under another's rules.
    if (!program.spec.valid()) return fail(FhexDecodeError::MissingSpec);

    program.modulus_table.reserve(modulus_count);
    for (uint32_t i = 0; i < modulus_count; ++i) {
        program.modulus_table.push_back(
            get_scalar<uint64_t>(bytes, modulus_offset + i * sizeof(uint64_t)));
    }
    program.metadata.modulus_chain = program.modulus_table;

    program.records.reserve(instruction_count);
    size_t at = record_offset;
    for (uint64_t i = 0; i < instruction_count; ++i) {
        // Two fixed bytes, then only the operands the flags advertise.
        if (at + 2 > size) return fail(FhexDecodeError::Truncated);
        FhetchRecord record;
        record.opcode = static_cast<uint8_t>(bytes[at++]);
        record.flags = static_cast<uint8_t>(bytes[at++]);

        if ((record.flags & FH_FLAG_HAS_LOGN) != 0) {
            if (at >= size) return fail(FhexDecodeError::Truncated);
            record.logn = static_cast<uint8_t>(bytes[at++]);
        }
        if (fh_opcode_has_operands(record.opcode)) {
            if (!get_varint_u32(bytes, size, at, record.dst) ||
                !get_varint_u32(bytes, size, at, record.src1)) {
                return fail(FhexDecodeError::Truncated);
            }
        }
        if ((record.flags & FH_FLAG_HAS_SRC2) != 0 &&
            !get_varint_u32(bytes, size, at, record.src2)) {
            return fail(FhexDecodeError::Truncated);
        }
        if ((record.flags & FH_FLAG_HAS_MOD) != 0 &&
            !get_varint(bytes, size, at, record.modulus)) {
            return fail(FhexDecodeError::Truncated);
        }
        if ((record.flags & FH_FLAG_HAS_IMM) != 0 &&
            !get_varint(bytes, size, at, record.imm)) {
            return fail(FhexDecodeError::Truncated);
        }
        if ((record.flags & FH_FLAG_HAS_AUX) != 0 &&
            !get_varint(bytes, size, at, record.aux)) {
            return fail(FhexDecodeError::Truncated);
        }

        program.records.push_back(record);
    }
    return program;
}

std::optional<FhexProgram> read_fhetch_program(const std::string& path,
                                               FhexDecodeError* err) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        if (err) *err = FhexDecodeError::BadMagic;
        return std::nullopt;
    }
    const std::streamoff size = in.tellg();
    if (size < 0) {
        if (err) *err = FhexDecodeError::BadMagic;
        return std::nullopt;
    }
    // Bound the allocation before making it.
    if (static_cast<uint64_t>(size) > kFhexMaxFileBytes) {
        if (err) *err = FhexDecodeError::TooLarge;
        return std::nullopt;
    }
    in.seekg(0, std::ios::beg);

    std::vector<std::byte> bytes(static_cast<size_t>(size));
    if (size > 0 &&
        !in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        if (err) *err = FhexDecodeError::Truncated;
        return std::nullopt;
    }
    return decode_fhetch_program(bytes.data(), bytes.size(), err);
}

bool write_fhetch_program(const std::string& path, const TraceMetadata& metadata,
                          const std::vector<uint64_t>& modulus_table,
                          const std::vector<FhetchRecord>& records,
                          FhetchSpec spec) {
    const std::vector<std::byte> bytes =
        encode_fhetch_program(metadata, modulus_table, records, spec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

}  // namespace niobium::fhetch
