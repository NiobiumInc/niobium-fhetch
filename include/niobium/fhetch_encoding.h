// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

/**
 * @file fhetch_encoding.h
 * @brief Binary encoding for FHETCH programs — the `.fhex` sidecar.
 *
 * Layout — the fixed sections are 8-byte aligned; the record stream that
 * follows them is varint-packed and therefore not indexable by stride:
 *
 *     offset  size  field
 *     0       4     magic "NBFH"
 *     4       2     format version
 *     6       2     flags (reserved, must be 0)
 *     8       4     metadata section length in bytes (multiple of 8)
 *     12      4     modulus count
 *     16      8     instruction count
 *     24      M     metadata section — TLV entries, zero-padded to 8
 *     24+M    8*C   modulus table (fixed u64 — moduli are 50-60 bit primes,
 *                   which a varint would store in 8-9 bytes, so packing them
 *                   would cost space rather than save it)
 *     ...     var   instruction records, varint-packed, `instruction count`
 *                   of them, decoded sequentially
 *
 * Records are variable length. Each carries a presence bitmap, and an
 * operand a given opcode does not use is omitted outright rather than
 * written as a reserved zero — which is where most of the saving comes from,
 * since no instruction uses every field. A typical three-address op with
 * small addresses packs into 6 bytes.
 *
 * The trade is deliberate: a fixed-stride record could be mmap'd and indexed
 * directly, and varint gives that up — decoding is strictly sequential — in
 * exchange for roughly a third of the bytes, which matters because the FHETCH
 * transport POSTs entire projects over HTTP.
 *
 * Every integer is stored little-endian, matching cereal_io.h's raw-array
 * files. That is a property of the format, not of the writer: fields go
 * through fhex_le() on both sides, so a `.fhex` written on one host decodes
 * identically on another. On a little-endian host the conversion folds away
 * to nothing.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "niobium/fhetch_ir.h"

namespace niobium::fhetch {

/// Byte-swap an unsigned integer. C++23 has std::byteswap; until the project
/// moves off C++17 this stands in for it.
template <typename T>
inline constexpr T fhex_byteswap(T v) {
    static_assert(std::is_unsigned_v<T>, "fhex_byteswap is for unsigned types");
    T out = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        out = static_cast<T>(out << 8) |
              static_cast<T>((v >> (i * 8)) & static_cast<T>(0xFF));
    }
    return out;
}

/// Whether the host is little-endian. C++20 has std::endian; until this
/// library moves off C++17, the compiler's own byte-order macro stands in for
/// it. GCC and Clang both define it, and needing no header is why it is
/// preferred here over the <endian.h>/<machine/endian.h> split.
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
inline constexpr bool kFhexHostIsLittleEndian =
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
#else
#error "fhex needs __BYTE_ORDER__ to know the host byte order"
#endif

/// Host value to/from the file's little-endian byte order. The conversion is
/// its own inverse, so one helper serves both directions. On a little-endian
/// host it folds away to nothing; the big-endian arm exists so the byte order
/// is a property of the *format* rather than of whatever host happened to
/// write the file.
template <typename T>
inline constexpr T fhex_le(T v) {
    if constexpr (kFhexHostIsLittleEndian) {
        return v;
    } else {
        return fhex_byteswap(v);
    }
}

/// Magic bytes at offset 0. Spelled out rather than a packed integer so the
/// value reads the same in a hex dump on any host.
inline constexpr char kFhexMagic[4] = {'N', 'B', 'F', 'H'};

/// Bumped whenever the record layout or prologue changes incompatibly. A
/// reader refuses a version it does not know rather than mis-decoding it.
///
/// This versions the *container*. It is independent of the FHETCH IR version
/// below, which versions the *language* the recorded program is written in —
/// the two move for different reasons and a file carries both.
inline constexpr uint16_t kFhexVersion = 1;

/// Version number of the FHETCH specification.
///
/// Deliberately not named `major`/`minor`: glibc's <sys/sysmacros.h> defines
/// those as function-like macros, and a struct member of the same name is a
/// standing trap for any translation unit that pulls it in.
struct FhetchVersion {
    uint16_t major_version = 0;
    uint16_t minor_version = 0;
    uint16_t patch_version = 0;

    /// False when no version is known — which is the case for every file
    /// written today, because the current specification carries no version
    /// number. 0.0.0 is not a release any revision could bear, so it is a
    /// safe sentinel.
    bool known() const {
        return major_version != 0 || minor_version != 0 || patch_version != 0;
    }

    friend bool operator==(const FhetchVersion& a, const FhetchVersion& b) {
        return a.major_version == b.major_version &&
               a.minor_version == b.minor_version &&
               a.patch_version == b.patch_version;
    }
    friend bool operator!=(const FhetchVersion& a, const FhetchVersion& b) { return !(a == b); }
};

/// Date of the FHETCH specification document.
///
/// Stored as its three components rather than a day count so a hex dump and
/// a diagnostic both read as the date on the document's cover.
struct FhetchSpecDate {
    uint16_t year = 0;
    uint8_t month = 0;
    uint8_t day = 0;

    /// False when the file carried no spec-date tag — i.e. it predates this
    /// field. Year 0 is not a date any revision could bear.
    bool known() const { return year != 0; }

    friend bool operator==(const FhetchSpecDate& a, const FhetchSpecDate& b) {
        return a.year == b.year &&
               a.month == b.month &&
               a.day == b.day;
    }
    friend bool operator!=(const FhetchSpecDate& a, const FhetchSpecDate& b) { return !(a == b); }
};

/// Which revision of the FHETCH IR a program is written in.
///
/// FHETCH is specified by "Polynomial Intermediate Representation for Fully
/// Homomorphic Encryption" (fhetch.org). That draft carries no version
/// number, only a last-updated date, so today the date is all a writer has.
///
/// At least one of the two must be present — a file identifying itself by
/// neither is rejected.
///
/// Exactly one of them is authoritative at a time, and which one depends on
/// what is available:
///
///   - A version is present: the version is authoritative. It names the
///     revision exactly, whereas a date only says which document was on the
///     author's desk, so an accompanying date drops to informational.
///   - No version: the date is authoritative. This is the case for every
///     file written today, and it is a real identifier in that situation,
///     not a fallback hint.
///
/// Consumers should branch on authoritative_date() rather than re-deriving
/// that rule, which is easy to invert by accident.
struct FhetchSpec {
    FhetchVersion version;
    FhetchSpecDate date;

    /// Whether this names a revision at all. Enforced on both encode and
    /// decode, so a `.fhex` always says what it is written in.
    bool valid() const { return version.known() || date.known(); }

    /// The date when it is the thing identifying the revision, nullopt when
    /// a version supersedes it. A caller that gets a value here should key
    /// its behaviour on the date; one that gets nullopt should use `version`.
    std::optional<FhetchSpecDate> authoritative_date() const {
        if (version.known() || !date.known()) return std::nullopt;
        return date;
    }

    friend bool operator==(const FhetchSpec& a, const FhetchSpec& b) {
        return a.version == b.version &&
               a.date == b.date;
    }
    friend bool operator!=(const FhetchSpec& a, const FhetchSpec& b) { return !(a == b); }
};

/// Revision of the FHETCH specification this implementation targets:
/// 2025-05-19, per the draft at
/// https://fhetch.org/wp-content/uploads/2025/05/COPY-FOR-SHARING-2025-05-19-Polynomial-Intermediate-Representation-for-Fully-Homomorphic-Encryption-DRAFT.pdf
///
/// The version is left unset because that draft has none; set it here once
/// the specification carries one, and the date drops to advisory on its own.
/// This is a claim about what this code implements, so it changes only when
/// the code does, and deliberately is not derived from the submodule (whose
/// CMake version tracks the library's releases, not the spec's revisions).
inline constexpr FhetchSpec kFhetchSpec{FhetchVersion{}, FhetchSpecDate{2025, 5, 19}};

/// Pack a version into the u64 its metadata tag carries, and back.
inline constexpr uint64_t fhex_pack_version(FhetchVersion version) {
    return (static_cast<uint64_t>(version.major_version) << 32) |
           (static_cast<uint64_t>(version.minor_version) << 16) |
           static_cast<uint64_t>(version.patch_version);
}

inline constexpr FhetchVersion fhex_unpack_version(uint64_t packed) {
    return FhetchVersion{static_cast<uint16_t>((packed >> 32) & 0xFFFF),
                         static_cast<uint16_t>((packed >> 16) & 0xFFFF),
                         static_cast<uint16_t>(packed & 0xFFFF)};
}

/// Pack a spec date as YYYYMMDD, the form its metadata tag carries. Chosen
/// over a day count because it orders correctly as an integer *and* reads as
/// the date itself in a dump.
inline constexpr uint64_t fhex_pack_spec_date(FhetchSpecDate date) {
    return static_cast<uint64_t>(date.year) * 10000U +
           static_cast<uint64_t>(date.month) * 100U +
           static_cast<uint64_t>(date.day);
}

inline constexpr FhetchSpecDate fhex_unpack_spec_date(uint64_t packed) {
    return FhetchSpecDate{static_cast<uint16_t>(packed / 10000U),
                          static_cast<uint8_t>((packed / 100U) % 100U),
                          static_cast<uint8_t>(packed % 100U)};
}

/// Size of the fixed prologue, in bytes.
inline constexpr size_t kFhexPrologueBytes = 24;

/// Alignment the fixed sections (metadata, modulus table) are padded to.
/// The varint record stream that follows them is unaligned by nature.
inline constexpr size_t kFhexAlign = 8;

/// Sanity bound on a `.fhex` read into memory, not a format limit. A
/// 200k-instruction trace is under 2 MB, so this is far above any real one.
inline constexpr size_t kFhexMaxFileBytes = 1ULL << 30;

/// Largest number of bytes an unsigned LEB128 value can occupy: 64 bits at
/// 7 payload bits per byte. Used to bound reads on a corrupt stream so a
/// runaway continuation-bit chain terminates instead of walking off the end.
inline constexpr size_t kFhexMaxVarintBytes = 10;

// ---------------------------------------------------------------------------
// Opcodes
// ---------------------------------------------------------------------------
//
// FhOpcode, its mnemonic table, and fh_opcode_has_operands() live in
// fhetch_ir.h and are re-exported here by inclusion. The dependency runs that
// way round on purpose: the IR is the program, and this file is one
// serialization of it, so the vocabulary belongs to the IR. The enumerator
// values are on the wire regardless, which is why fhetch_ir.h says so.

// ---------------------------------------------------------------------------
// Instruction record
// ---------------------------------------------------------------------------

/// Per-record flag bits. Absent operands are not merely zero — zero is a
/// legal address, immediate, and modulus index — so presence is explicit.
enum FhRecordFlags : uint8_t {
    FH_FLAG_HAS_MOD     = 1u << 0,  ///< modulus field is meaningful
    FH_FLAG_MOD_LITERAL = 1u << 1,  ///< modulus holds a prime, not a table index
    FH_FLAG_HAS_IMM     = 1u << 2,  ///< imm holds an immediate / k / offset
    FH_FLAG_HAS_SRC2    = 1u << 3,  ///< src2 is meaningful
    FH_FLAG_HAS_AUX     = 1u << 4,  ///< aux holds omega (NTT/INTT) or mask (automorph)
    FH_FLAG_HAS_LOGN    = 1u << 5,  ///< logn is meaningful
};

/// One instruction, in memory. This is *not* the wire layout — records are
/// varint-packed on disk, so this struct's size and field order are free to
/// change without touching the format.
///
/// The wire form of a record is:
///
///     opcode    u8       always
///     flags     u8       always (FhRecordFlags)
///     logn      u8       iff FH_FLAG_HAS_LOGN
///     dst       varint   unless the opcode takes no operands (halt)
///     src1      varint   likewise
///     src2      varint   iff FH_FLAG_HAS_SRC2
///     modulus   varint   iff FH_FLAG_HAS_MOD
///     imm       varint   iff FH_FLAG_HAS_IMM
///     aux       varint   iff FH_FLAG_HAS_AUX
///
/// Operand presence is keyed on the flags rather than on the opcode so the
/// decoder can skip a record it does not recognize; the one exception is
/// dst/src1, which every opcode but `halt` has, and which are therefore
/// keyed on the opcode itself.
///
/// `modulus` is a full 64 bits rather than a narrow table index because
/// a writer falls back to the literal prime when a modulus is not in the
/// chain, exactly as the text form does. Giving that case its own flag
/// keeps it from colliding with `aux`, which `sr_automorph_eval` needs for
/// `mask` at the same time. A varint charges for the width only when the
/// literal case actually occurs.
///
/// `aux` carries `omega` for sr_ntt/sr_intt and `mask` for
/// sr_automorph_eval — the two never co-occur on one instruction.
/// `imm` carries the scalar immediate, `k`, or `offset` by opcode.
struct FhetchRecord {
    uint8_t  opcode = FH_ILL;  ///< FhOpcode
    uint8_t  flags = 0;        ///< FhRecordFlags
    uint8_t  logn = 0;         ///< sr_automorph_eval's logn (fits: logn <= 64)
    uint32_t dst = 0;
    uint32_t src1 = 0;
    uint32_t src2 = 0;
    uint64_t modulus = 0;      ///< table index, or literal prime
    uint64_t imm = 0;          ///< immediate | k | offset
    uint64_t aux = 0;          ///< omega | mask

    friend bool operator==(const FhetchRecord& a, const FhetchRecord& b) {
        return a.opcode == b.opcode &&
               a.flags == b.flags &&
               a.logn == b.logn &&
               a.dst == b.dst &&
               a.src1 == b.src1 &&
               a.src2 == b.src2 &&
               a.modulus == b.modulus &&
               a.imm == b.imm &&
               a.aux == b.aux;
    }
    friend bool operator!=(const FhetchRecord& a, const FhetchRecord& b) { return !(a == b); }
};

// ---------------------------------------------------------------------------
// Metadata section
// ---------------------------------------------------------------------------

/// TLV tags for the metadata section. Numbering is on the wire; a reader
/// skips tags it does not recognize, so new fields can be added without a
/// version bump as long as old readers can safely ignore them.
enum class FhMetaTag : uint16_t {
    ProgramName         = 1,   ///< string
    TraceFile           = 2,   ///< string
    Description         = 3,   ///< string
    SourceFile          = 4,   ///< string
    BuildTimestamp      = 5,   ///< string
    Scheme              = 6,   ///< string
    SecurityLevel       = 7,   ///< string
    SourceLine          = 8,   ///< u64
    InstructionCount    = 9,   ///< u64
    GeneratedTimestamp  = 10,  ///< u64
    RingDimension       = 11,  ///< u64
    MultiplicativeDepth = 12,  ///< u64
    ModulusChainLength  = 13,  ///< u64
    FhetchSpecVersion   = 14,  ///< u64, packed by fhex_pack_version()
    FhetchSpecDateTag   = 15,  ///< u64, YYYYMMDD per fhex_pack_spec_date()
    ProgramVersion      = 16,  ///< string
};

/// One TLV entry header, followed by `length` payload bytes zero-padded to
/// the next multiple of 8.
struct FhMetaEntry {
    uint16_t tag;       ///< 0  FhMetaTag
    uint16_t reserved;  ///< 2  must be 0
    uint32_t length;    ///< 4  payload bytes, excluding padding
};

static_assert(sizeof(FhMetaEntry) == 8,
              "FhMetaEntry is a wire format — 8 bytes, keeping the section aligned");

/// Fixed prologue, laid out to match the offsets documented at the top.
struct FhexPrologue {
    char     magic[4];           ///<  0
    uint16_t version;            ///<  4
    uint16_t flags;              ///<  6
    uint32_t metadata_bytes;     ///<  8
    uint32_t modulus_count;      ///< 12
    uint64_t instruction_count;  ///< 16
};

static_assert(sizeof(FhexPrologue) == kFhexPrologueBytes,
              "FhexPrologue is a wire format — its size is fixed at 24 bytes");
static_assert(sizeof(FhexPrologue::magic) + sizeof(FhexPrologue::version) +
                      sizeof(FhexPrologue::flags) +
                      sizeof(FhexPrologue::metadata_bytes) +
                      sizeof(FhexPrologue::modulus_count) +
                      sizeof(FhexPrologue::instruction_count) ==
                  sizeof(FhexPrologue),
              "FhexPrologue must have no interior padding — see the note on "
              "FhetchRecord");

/// Round `n` up to the next multiple of kFhexAlign.
inline constexpr size_t fhex_align_up(size_t n) {
    return (n + kFhexAlign - 1) & ~(kFhexAlign - 1);
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

/// Why a decode failed, including the two cases a self-describing container
/// adds: a file that isn't a `.fhex` at all, and one
/// written by a newer toolchain than this reader knows.
enum class FhexDecodeError : uint8_t {
    None,
    BadMagic,           ///< not a `.fhex` (or truncated before the prologue)
    UnsupportedVersion, ///< written by a newer format version
    Truncated,          ///< a section runs past the end of the buffer
    Malformed,          ///< internally inconsistent (bad padding, overlong TLV)
    UnknownOpcode,      ///< opcode is one the consumer cannot represent
    MissingSpec,        ///< names neither a FHETCH spec version nor a date
    TooLarge,           ///< exceeds kFhexMaxFileBytes
};

/// Human-readable form of a decode error, for the WARNING a caller logs
/// before falling back to the text trace.
inline const char* fhex_decode_error_string(FhexDecodeError err) {
    switch (err) {
    case FhexDecodeError::None:               return "none";
    case FhexDecodeError::BadMagic:           return "bad magic (not a .fhex file)";
    case FhexDecodeError::UnsupportedVersion: return "unsupported format version";
    case FhexDecodeError::Truncated:          return "truncated";
    case FhexDecodeError::Malformed:          return "malformed";
    case FhexDecodeError::UnknownOpcode:      return "unknown opcode";
    case FhexDecodeError::MissingSpec:
        return "names neither a FHETCH spec version nor a spec date";
    case FhexDecodeError::TooLarge:          return "implausibly large";
    }
    return "unrecognized error";
}

/// Everything a `.fhex` carries, decoded.
///
/// `records` are the program as the file holds it, which is strictly richer
/// than what a consumer's own instruction type may be able to represent:
/// omega, mask, and logn are all carried here whether or not the consumer
/// has a home for them. Decoding to records first means the *file* stays
/// faithful even when the form it is converted into cannot be.
struct FhexProgram {
    TraceMetadata metadata;
    /// Version of the `.fhex` container the file was written in, as read
    /// from the prologue. Always equals kFhexVersion for a file this build
    /// accepted — a mismatch is refused rather than decoded — but it is
    /// reported so a caller can log what it actually read.
    uint16_t format_version = 0;
    /// Revision of the FHETCH spec the program is written in. Always
    /// `valid()` — a file naming neither a version nor a date is refused.
    FhetchSpec spec;
    std::vector<uint64_t> modulus_table;
    std::vector<FhetchRecord> records;
};

/// The wire record for an instruction. Absent operands are left out of the
/// record's flags rather than written as zero, so the record and the
/// instruction carry exactly the same information.
FhetchRecord to_record(const Instruction& inst);

/// The instruction a wire record stands for — the inverse of to_record().
///
/// `modulus_table_size` is needed for nothing here: the record's own
/// FH_FLAG_MOD_LITERAL says whether its modulus is an index or a prime, which
/// is the ambiguity the text form has and the binary form does not.
Instruction from_record(const FhetchRecord& record);

/// Serialize a program to the `.fhex` byte stream.
///
/// `records` is written verbatim — the caller has already chosen each
/// record's opcode and flags, so this does not re-derive them from
/// `metadata`. `modulus_table` is written as its own section and is what a
/// reader will populate TraceMetadata::modulus_chain from.
/// `spec` defaults to the revision this implementation targets, which is what
/// a writer wants; pass it explicitly only when re-encoding a program written
/// against a different revision, so the original is preserved rather than
/// silently restamped with ours. An invalid `spec` (neither version nor date)
/// is a programming error and throws — the invariant is enforced on the way
/// out as well as the way in, so a malformed file is never produced.
std::vector<std::byte> encode_fhetch_program(
    const TraceMetadata& metadata, const std::vector<uint64_t>& modulus_table,
    const std::vector<FhetchRecord>& records, FhetchSpec spec = kFhetchSpec);

/// Decode a `.fhex` byte stream.
///
/// Returns nullopt on any error, setting `*err` to the reason, so a caller
/// can log it and fall back to the text `.fhetch` — the same
/// corrupt-sidecar posture a binary sidecar wants. Never throws and never
/// reads outside `bytes`; a corrupt file is a fallback, not a crash.
std::optional<FhexProgram> decode_fhetch_program(
    const std::byte* bytes, size_t size, FhexDecodeError* err = nullptr);

/// Read and decode a `.fhex` file. Convenience wrapper over
/// decode_fhetch_program(); a missing or unreadable file reports BadMagic,
/// which the caller treats the same way — fall back to text.
std::optional<FhexProgram> read_fhetch_program(const std::string& path,
                                              FhexDecodeError* err = nullptr);

/// Write a program to `path`. Returns false if the file could not be
/// written; the caller keeps the text trace either way.
bool write_fhetch_program(const std::string& path, const TraceMetadata& metadata,
                          const std::vector<uint64_t>& modulus_table,
                          const std::vector<FhetchRecord>& records,
                          FhetchSpec spec = kFhetchSpec);

}  // namespace niobium::fhetch
