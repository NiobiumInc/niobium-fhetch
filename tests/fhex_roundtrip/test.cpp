// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

/**
 * @file test.cpp
 * @brief Round-trip tests for the FHETCH binary sidecar (`.fhex`).
 *
 * Encodes a program covering every operand shape the format carries —
 * three-address ops, scalar immediates, omega on sr_ntt, mask/logn/k on
 * sr_automorph_eval, and the literal-prime modulus fallback — then decodes
 * it and checks the records field-for-field. Also covers the container and
 * FHETCH-spec identity a file records, the varint packing, and that a
 * damaged file fails with a typed error rather than crashing or returning
 * something plausible.
 *
 * These are tests of the *format*. What a consumer converts the records into
 * is the consumer's business and is tested on its side.
 */
#include "niobium/fhetch_encoding.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace niobium::fhetch;

namespace {

int g_checks_failed = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cout << "  FAIL " << __FILE__ << ":" << __LINE__ << "  "      \
                      << #cond << std::endl;                                   \
            ++g_checks_failed;                                                 \
        }                                                                      \
    } while (0)

/// Removes the fixture directory however the test is left.
class FixtureDirGuard {
public:
    explicit FixtureDirGuard(std::filesystem::path dir) : dir_(std::move(dir)) {}
    ~FixtureDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    FixtureDirGuard(const FixtureDirGuard&) = delete;
    FixtureDirGuard& operator=(const FixtureDirGuard&) = delete;

private:
    std::filesystem::path dir_;
};

TraceMetadata make_metadata() {
    TraceMetadata metadata;
    metadata.program_name = "matrix_ops";
    metadata.trace_file = "matrix_ops.fhetch";
    metadata.description = "round-trip fixture";
    metadata.source_file = "examples/mat_mul.cpp";
    metadata.source_line = 42;
    metadata.build_timestamp = "Mon Aug 17 2026";
    metadata.scheme = "CKKS";
    metadata.ring_dimension = 65536;
    metadata.multiplicative_depth = 9;
    metadata.security_level = "HEStd_128_classic";
    metadata.instruction_count = 6;
    metadata.generated_timestamp = 1755400000000ULL;
    metadata.modulus_chain_length = 3;
    return metadata;
}

std::vector<uint64_t> make_moduli() {
    // Index 0 is the copy/zero-init sentinel, not a prime — it mirrors
    // TraceWriter::COPY_MODULUS_VALUE, which every real chain carries there.
    // 1 and 2 are CKKS-sized primes.
    return {0xFFFFFFFFFFFFFFFFULL, 4398046523393ULL, 1152921504606830593ULL};
}

/// Fields in order: opcode, flags, logn, dst, src1, src2, modulus, imm, aux.
std::vector<FhetchRecord> make_records() {
    std::vector<FhetchRecord> records;
    // sr_addp %7, %3, %5, m=1
    records.push_back({FH_SR_ADDP, FH_FLAG_HAS_MOD | FH_FLAG_HAS_SRC2, 0,
                       7, 3, 5, 1, 0, 0});
    // sr_mulps %8, %7, 12345, m=2
    records.push_back({FH_SR_MULPS, FH_FLAG_HAS_MOD | FH_FLAG_HAS_IMM, 0,
                       8, 7, 0, 2, 12345, 0});
    // sr_ntt %9, %8, m=1, omega=320656143
    records.push_back({FH_SR_NTT, FH_FLAG_HAS_MOD | FH_FLAG_HAS_AUX, 0,
                       9, 8, 0, 1, 0, 320656143ULL});
    // sr_automorph_eval %10, %9, m=1, mask=2047, logn=11, k=65
    records.push_back({FH_SR_AUTOMORPH_EVAL,
                       FH_FLAG_HAS_MOD | FH_FLAG_HAS_IMM | FH_FLAG_HAS_AUX |
                           FH_FLAG_HAS_LOGN,
                       11, 10, 9, 0, 1, 65, 2047});
    // sr_subp %11, %10, %9, m=<literal prime not in the chain's index space>
    records.push_back({FH_SR_SUBP,
                       FH_FLAG_HAS_MOD | FH_FLAG_MOD_LITERAL | FH_FLAG_HAS_SRC2,
                       0, 11, 10, 9, 1152921504606830593ULL, 0, 0});
    records.push_back({FH_HALT, 0, 0, 0, 0, 0, 0, 0, 0});
    return records;
}

// ---------------------------------------------------------------------------

bool test_program_roundtrip() {
    std::cout << "\n[test] program round-trip" << std::endl;
    const int before = g_checks_failed;

    const TraceMetadata metadata = make_metadata();
    const std::vector<uint64_t> moduli = make_moduli();
    const std::vector<FhetchRecord> records = make_records();

    const auto bytes = encode_fhetch_program(metadata, moduli, records);
    FhexDecodeError err = FhexDecodeError::None;
    const auto decoded = decode_fhetch_program(bytes.data(), bytes.size(), &err);
    CHECK(decoded.has_value());
    CHECK(err == FhexDecodeError::None);
    if (!decoded.has_value()) return false;

    const TraceMetadata& out = decoded->metadata;
    CHECK(out.program_name == metadata.program_name);
    CHECK(out.trace_file == metadata.trace_file);
    CHECK(out.description == metadata.description);
    CHECK(out.source_file == metadata.source_file);
    CHECK(out.source_line == metadata.source_line);
    CHECK(out.build_timestamp == metadata.build_timestamp);
    CHECK(out.scheme == metadata.scheme);
    CHECK(out.ring_dimension == metadata.ring_dimension);
    CHECK(out.multiplicative_depth == metadata.multiplicative_depth);
    CHECK(out.security_level == metadata.security_level);
    CHECK(out.instruction_count == metadata.instruction_count);
    CHECK(out.generated_timestamp == metadata.generated_timestamp);
    CHECK(out.modulus_chain_length == metadata.modulus_chain_length);
    CHECK(out.modulus_chain == moduli);
    CHECK(decoded->modulus_table == moduli);

    CHECK(decoded->records.size() == records.size());
    for (size_t i = 0; i < records.size(); ++i) CHECK(decoded->records[i] == records[i]);

    // The operands the text form discards must survive here.
    CHECK(decoded->records[2].aux == 320656143ULL);  // omega
    CHECK(decoded->records[3].aux == 2047);          // mask
    CHECK(decoded->records[3].logn == 11);
    CHECK(decoded->records[3].imm == 65);            // k

    return g_checks_failed == before;
}

bool test_literal_modulus_flagged() {
    std::cout << "\n[test] literal modulus is flagged, not conflated with an index"
              << std::endl;
    const int before = g_checks_failed;

    const TraceMetadata metadata = make_metadata();
    const std::vector<uint64_t> moduli = make_moduli();

    // The text form writes a chain index and its literal-prime fallback
    // identically, as a bare `m=<number>`. The record keeps them apart with
    // FH_FLAG_MOD_LITERAL, which is the distinction the text cannot express.
    std::vector<FhetchRecord> records;
    // `m=0` on an automorph: a literal zero, not chain entry 0.
    records.push_back({FH_SR_AUTOMORPH_EVAL,
                       FH_FLAG_HAS_MOD | FH_FLAG_MOD_LITERAL | FH_FLAG_HAS_IMM,
                       0, 4, 3, 0, 0, 65, 0});
    // A 60-bit prime overflows std::stoi, which leaves the field unset.
    records.push_back({FH_SR_ADDP,
                       FH_FLAG_HAS_MOD | FH_FLAG_MOD_LITERAL | FH_FLAG_HAS_SRC2,
                       0, 5, 4, 3, 1152921504606830593ULL, 0, 0});

    const auto bytes = encode_fhetch_program(metadata, moduli, records);
    const auto decoded = decode_fhetch_program(bytes.data(), bytes.size(), nullptr);
    CHECK(decoded.has_value());
    if (!decoded.has_value()) return false;

    // `m=0` here is a literal zero, not chain entry 0 — only the flag says so.
    CHECK((decoded->records[0].flags & FH_FLAG_MOD_LITERAL) != 0);
    CHECK(decoded->records[0].modulus == 0);
    // A 60-bit prime survives at full width, which a chain index never needs.
    CHECK((decoded->records[1].flags & FH_FLAG_MOD_LITERAL) != 0);
    CHECK(decoded->records[1].modulus == 1152921504606830593ULL);

    return g_checks_failed == before;
}

bool test_spec_identity() {
    std::cout << "\n[test] container version and FHETCH spec identity"
              << std::endl;
    const int before = g_checks_failed;

    const TraceMetadata metadata = make_metadata();
    const std::vector<uint64_t> moduli = make_moduli();
    const std::vector<FhetchRecord> records = make_records();

    const auto bytes = encode_fhetch_program(metadata, moduli, records);
    const auto decoded = decode_fhetch_program(bytes.data(), bytes.size(), nullptr);
    CHECK(decoded.has_value());
    if (!decoded.has_value()) return false;

    CHECK(decoded->format_version == kFhexVersion);
    CHECK(decoded->spec == kFhetchSpec);
    CHECK(decoded->spec.valid());
    // The current draft carries no version, so the date identifies it.
    CHECK(!decoded->spec.version.known());
    CHECK(decoded->spec.date.year == 2025);
    CHECK(decoded->spec.date.month == 5);
    CHECK(decoded->spec.date.day == 19);
    CHECK(decoded->spec.authoritative_date().has_value());

    {   // a version supersedes the date, which stays recorded but advisory
        const FhetchSpec versioned{FhetchVersion{2, 1, 3},
                                   FhetchSpecDate{2025, 5, 19}};
        CHECK(versioned.valid());
        CHECK(!versioned.authoritative_date().has_value());
        const auto vb = encode_fhetch_program(metadata, moduli, records, versioned);
        const auto vd = decode_fhetch_program(vb.data(), vb.size(), nullptr);
        CHECK(vd.has_value());
        if (vd.has_value()) {
            CHECK(vd->spec == versioned);
            CHECK(vd->spec.version.major_version == 2);
            CHECK(vd->spec.version.minor_version == 1);
            CHECK(vd->spec.version.patch_version == 3);
            CHECK(vd->spec.date.year == 2025);
            CHECK(!vd->spec.authoritative_date().has_value());
        }
    }

    {   // a date alone is authoritative and legal
        const FhetchSpec dated{FhetchVersion{}, FhetchSpecDate{2026, 1, 31}};
        const auto db = encode_fhetch_program(metadata, moduli, records, dated);
        const auto dd = decode_fhetch_program(db.data(), db.size(), nullptr);
        CHECK(dd.has_value());
        if (dd.has_value()) {
            const auto date = dd->spec.authoritative_date();
            CHECK(date.has_value());
            if (date.has_value()) {
                CHECK(date->year == 2026);
                CHECK(date->month == 1);
                CHECK(date->day == 31);
            }
        }
    }

    {   // naming neither is refused on the way out
        bool threw = false;
        try {
            encode_fhetch_program(metadata, moduli, records, FhetchSpec{});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }

    {   // ...and on the way in. The spec date is the first metadata entry, so
        // blanking its tag id leaves a file that names no revision at all.
        auto stripped = encode_fhetch_program(metadata, moduli, records);
        stripped[kFhexPrologueBytes] = std::byte{0};
        stripped[kFhexPrologueBytes + 1] = std::byte{0};
        FhexDecodeError err = FhexDecodeError::None;
        CHECK(!decode_fhetch_program(stripped.data(), stripped.size(), &err));
        CHECK(err == FhexDecodeError::MissingSpec);
    }

    return g_checks_failed == before;
}

bool test_varint_packing() {
    std::cout << "\n[test] varint packing" << std::endl;
    const int before = g_checks_failed;

    const TraceMetadata metadata = make_metadata();
    const std::vector<uint64_t> moduli = make_moduli();
    const std::vector<FhetchRecord> records = make_records();

    // Isolate the record stream from the fixed sections to measure what the
    // packing actually costs per instruction.
    const auto full = encode_fhetch_program(metadata, moduli, records);
    const auto empty = encode_fhetch_program(metadata, moduli, {});
    const size_t record_bytes = full.size() - empty.size();
    std::cout << "  record stream: " << record_bytes << " bytes for "
              << records.size() << " records" << std::endl;

    // A fixed-width record for the same fields would be 40 bytes; the whole
    // point of the packing is to come in well under that.
    CHECK(record_bytes < records.size() * 40);

    {   // values at the width limits survive
        std::vector<FhetchRecord> wide;
        wide.push_back({FH_SR_ADDP, FH_FLAG_HAS_MOD | FH_FLAG_HAS_SRC2, 0,
                        4000000, 3999999, 16777216, 5, 0, 0});
        wide.push_back({FH_SR_MULPS, FH_FLAG_HAS_MOD | FH_FLAG_HAS_IMM, 0,
                        UINT32_MAX, 1, 0, 2, UINT64_MAX, 0});
        const auto wb = encode_fhetch_program(metadata, moduli, wide);
        const auto wd = decode_fhetch_program(wb.data(), wb.size(), nullptr);
        CHECK(wd.has_value());
        if (wd.has_value()) {
            CHECK(wd->records[0] == wide[0]);
            CHECK(wd->records[1] == wide[1]);
            CHECK(wd->records[1].dst == UINT32_MAX);
            CHECK(wd->records[1].imm == UINT64_MAX);
        }
    }

    return g_checks_failed == before;
}

bool test_damaged_file_rejected() {
    std::cout << "\n[test] damaged files fail with a typed error" << std::endl;
    const int before = g_checks_failed;

    const TraceMetadata metadata = make_metadata();
    const std::vector<uint64_t> moduli = make_moduli();
    const std::vector<FhetchRecord> records = make_records();
    const auto bytes = encode_fhetch_program(metadata, moduli, records);

    for (size_t cut : {size_t(0), size_t(8), size_t(23), bytes.size() / 2,
                       bytes.size() - 3, bytes.size() - 1}) {
        FhexDecodeError err = FhexDecodeError::None;
        CHECK(!decode_fhetch_program(bytes.data(), cut, &err));
        CHECK(err != FhexDecodeError::None);
    }

    {   // not a .fhex at all
        auto corrupt = bytes;
        corrupt[1] = std::byte{'X'};
        FhexDecodeError err = FhexDecodeError::None;
        CHECK(!decode_fhetch_program(corrupt.data(), corrupt.size(), &err));
        CHECK(err == FhexDecodeError::BadMagic);
    }
    {   // written by a newer toolchain
        auto corrupt = bytes;
        corrupt[offsetof(FhexPrologue, version)] = std::byte{99};
        FhexDecodeError err = FhexDecodeError::None;
        CHECK(!decode_fhetch_program(corrupt.data(), corrupt.size(), &err));
        CHECK(err == FhexDecodeError::UnsupportedVersion);
    }
    {   // a count that could not possibly fit must not drive a huge reserve
        auto corrupt = bytes;
        const uint64_t huge = fhex_le<uint64_t>(0xFFFFFFFFULL);
        std::memcpy(corrupt.data() + offsetof(FhexPrologue, instruction_count),
                    &huge, sizeof(huge));
        FhexDecodeError err = FhexDecodeError::None;
        CHECK(!decode_fhetch_program(corrupt.data(), corrupt.size(), &err));
        CHECK(err == FhexDecodeError::Truncated);
    }

    // A varint whose continuation chain never terminates must not walk off the
    // buffer, nor spin past what 64 bits can hold.
    {
        const size_t header_bytes =
            encode_fhetch_program(metadata, moduli, {}).size();
        const auto one = encode_fhetch_program(metadata, moduli, {records[0]});

        {   // chain runs to the end of the buffer
            auto corrupt = one;
            for (size_t i = header_bytes + 2; i < corrupt.size(); ++i)
                corrupt[i] = std::byte{0xFF};
            FhexDecodeError err = FhexDecodeError::None;
            CHECK(!decode_fhetch_program(corrupt.data(), corrupt.size(), &err));
            CHECK(err == FhexDecodeError::Truncated);
        }
        {   // chain stays in bounds but overruns 64 bits
            auto corrupt = one;
            corrupt.resize(header_bytes + 2);
            for (int i = 0; i < 16; ++i) corrupt.push_back(std::byte{0xFF});
            corrupt.push_back(std::byte{0x00});
            FhexDecodeError err = FhexDecodeError::None;
            CHECK(!decode_fhetch_program(corrupt.data(), corrupt.size(), &err));
            CHECK(err == FhexDecodeError::Truncated);
        }
    }

    return g_checks_failed == before;
}

bool test_hostile_field_widths() {
    std::cout << "\n[test] hostile field widths" << std::endl;
    const int before = g_checks_failed;

    const TraceMetadata metadata = make_metadata();
    const std::vector<uint64_t> moduli = make_moduli();
    const std::vector<FhetchRecord> records = make_records();
    const size_t header_bytes = encode_fhetch_program(metadata, moduli, {}).size();

    // A varint whose payload would not survive its own shift must be
    // rejected, not silently truncated -- otherwise two byte sequences decode
    // to the same value. The tenth byte of a 64-bit LEB128 has one bit left,
    // so a payload above 0x01 there overflows.
    {
        auto corrupt = encode_fhetch_program(metadata, moduli, {records[0]});
        corrupt.resize(header_bytes + 2);
        for (int i = 0; i < 9; ++i) corrupt.push_back(std::byte{0xFF});
        corrupt.push_back(std::byte{0x7F});  // 10th byte, payload > 1 bit
        FhexDecodeError err = FhexDecodeError::None;
        CHECK(!decode_fhetch_program(corrupt.data(), corrupt.size(), &err));
        CHECK(err == FhexDecodeError::Truncated);
    }
    // The largest genuinely representable varint must still decode.
    {
        std::vector<FhetchRecord> max_imm;
        max_imm.push_back({FH_SR_MULPS, FH_FLAG_HAS_MOD | FH_FLAG_HAS_IMM, 0,
                           1, 2, 0, 1, UINT64_MAX, 0});
        const auto bytes = encode_fhetch_program(metadata, moduli, max_imm);
        const auto decoded = decode_fhetch_program(bytes.data(), bytes.size(), nullptr);
        CHECK(decoded.has_value());
        if (decoded.has_value()) CHECK(decoded->records[0].imm == UINT64_MAX);
    }

    // A numeric metadata entry of the wrong width is corruption, not a zero.
    // RingDimension is the second entry, after the spec date.
    {
        auto corrupt = encode_fhetch_program(metadata, moduli, records);
        bool patched = false;
        for (size_t at = kFhexPrologueBytes; at + 8 <= corrupt.size(); at += 8) {
            uint16_t tag = 0;
            std::memcpy(&tag, corrupt.data() + at, sizeof(tag));
            if (fhex_le(tag) == static_cast<uint16_t>(FhMetaTag::RingDimension)) {
                const uint32_t bad = fhex_le<uint32_t>(4);  // claim 4, not 8
                std::memcpy(corrupt.data() + at + 4, &bad, sizeof(bad));
                patched = true;
                break;
            }
        }
        CHECK(patched);
        FhexDecodeError err = FhexDecodeError::None;
        CHECK(!decode_fhetch_program(corrupt.data(), corrupt.size(), &err));
        CHECK(err == FhexDecodeError::Malformed);
    }

    // byteswap must round-trip at every width it is instantiated for.
    CHECK(fhex_byteswap<uint16_t>(fhex_byteswap<uint16_t>(0x1234)) == 0x1234);
    CHECK(fhex_byteswap<uint32_t>(0x11223344U) == 0x44332211U);
    CHECK(fhex_byteswap<uint64_t>(0x0102030405060708ULL) == 0x0807060504030201ULL);
    CHECK(fhex_byteswap<uint8_t>(0xAB) == 0xAB);

    return g_checks_failed == before;
}

bool test_file_roundtrip(const std::filesystem::path& dir) {
    std::cout << "\n[test] file write/read round-trip" << std::endl;
    const int before = g_checks_failed;

    const TraceMetadata metadata = make_metadata();
    const std::vector<uint64_t> moduli = make_moduli();
    const std::vector<FhetchRecord> records = make_records();
    const std::string path = (dir / "fixture.fhex").string();

    CHECK(write_fhetch_program(path, metadata, moduli, records));

    FhexDecodeError err = FhexDecodeError::None;
    const auto decoded = read_fhetch_program(path, &err);
    CHECK(decoded.has_value());
    CHECK(err == FhexDecodeError::None);
    if (decoded.has_value()) {
        CHECK(decoded->records == records);
        CHECK(decoded->modulus_table == moduli);
        CHECK(decoded->metadata.program_name == metadata.program_name);
        CHECK(decoded->spec == kFhetchSpec);
    }

    {   // a path that does not exist reports rather than throwing
        FhexDecodeError missing_err = FhexDecodeError::None;
        CHECK(!read_fhetch_program((dir / "absent.fhex").string(), &missing_err));
        CHECK(missing_err == FhexDecodeError::BadMagic);
    }

    return g_checks_failed == before;
}

}  // namespace

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "FHETCH binary sidecar (.fhex) round-trip" << std::endl;
    std::cout << "========================================" << std::endl;

    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec) /
                     "nb_fhex_roundtrip_fixture";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::cout << "Cannot create fixture directory: " << ec.message()
                  << std::endl;
        return 1;
    }
    FixtureDirGuard cleanup(dir);

    int passed = 0;
    int failed = 0;

    if (test_program_roundtrip()) passed++; else failed++;
    if (test_literal_modulus_flagged()) passed++; else failed++;
    if (test_spec_identity()) passed++; else failed++;
    if (test_varint_packing()) passed++; else failed++;
    if (test_damaged_file_rejected()) passed++; else failed++;
    if (test_hostile_field_widths()) passed++; else failed++;
    if (test_file_roundtrip(dir)) passed++; else failed++;

    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << passed << " passed, " << failed << " failed"
              << std::endl;
    std::cout << "========================================" << std::endl;

    return failed > 0 ? 1 : 0;
}
