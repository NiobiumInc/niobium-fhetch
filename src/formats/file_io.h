// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

/**
 * @file file_io.h
 * @brief Internal: what the binding record (.nbbr) and output metadata (.nbom)
 *        files share -- the 16-byte preamble, little-endian field access, the
 *        label pool, reading a file, and the atomic write. Errors are
 *        RecordError; `what` prefixes every message ("binding record: rule 1").
 *        C++17.
 */
#pragma once

#include "niobium/binding_record.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace niobium::binding::detail {

// The formats overlay little-endian file bytes onto structs.
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "binding files are read by overlaying little-endian file bytes; big-endian hosts are not supported"
#endif

inline constexpr std::uint64_t kSentinel = 0x0123456789ABCDEFULL;
inline constexpr std::uint64_t kSentinelByteSwapped = 0xEFCDAB8967452301ULL;

/// A read-only view of bytes (std::span is C++20).
class ByteView {
public:
    ByteView() = default;
    ByteView(const std::byte* data, std::size_t size) : data_(data), size_(size) {}

    const std::byte* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }

    /// Bytes [offset, offset + count); throws std::out_of_range past the end.
    ByteView sub(std::uint64_t offset, std::uint64_t count) const;
    ByteView sub(std::uint64_t offset) const { return sub(offset, offset <= size_ ? size_ - offset : 0); }

private:
    const std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};

/// The first 16 bytes of both formats.
struct Preamble {
    std::array<char, 4> magic;
    std::uint16_t version;
    std::uint16_t flags;
    std::uint64_t sentinel;
};
static_assert(sizeof(Preamble) == 16 && offsetof(Preamble, sentinel) == 8);
static_assert(std::is_trivially_copyable_v<Preamble>);

template <class T>
T load(ByteView bytes, std::uint64_t offset) {
    static_assert(std::is_trivially_copyable_v<T>);
    T value{};
    std::memcpy(&value, bytes.sub(offset, sizeof(T)).data(), sizeof(T));
    return value;
}

template <class T>
void store(std::vector<std::byte>& bytes, std::uint64_t offset, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (offset + sizeof(T) > bytes.size()) throw std::out_of_range("binding formats: store past the end");
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

constexpr Preamble make_preamble(std::array<char, 4> magic, std::uint16_t version) {
    return Preamble{magic, version, 0, kSentinel};
}

[[noreturn]] void fail(RecordErrc code, std::string_view what, const std::string& message);

/// Checks that `bytes` holds at least `header_size` bytes, then magic, sentinel
/// (byte-swapped: BigEndianWriter) and version.
void check_preamble(ByteView bytes, std::size_t header_size, std::array<char, 4> magic, std::uint16_t version,
                    std::string_view what);

/// Checks that the file is exactly as long as its layout needs.
void check_size(ByteView bytes, std::uint64_t expected, std::string_view what);

/// The label at [offset, offset + len) of `pool`, or BadLabel naming `who`
/// when it lies outside the pool.
std::string pool_label(ByteView pool, std::uint32_t offset, std::uint16_t len, std::string_view what,
                       const std::string& who);

/// Appends `label` to the pool at `pool_at` and advances it.
void put_label(std::vector<std::byte>& out, std::uint64_t pool_start, std::uint32_t& pool_at, std::string_view label);

std::vector<std::byte> read_file(const std::filesystem::path& path, std::string_view what);

/// Writes `bytes` to a temp file beside `path`, fsyncs it, renames it over
/// `path` and fsyncs the directory.
void write_atomically(const std::filesystem::path& path, const std::vector<std::byte>& bytes, std::string_view what);

}  // namespace niobium::binding::detail
