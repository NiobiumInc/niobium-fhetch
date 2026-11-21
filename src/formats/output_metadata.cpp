// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

/**
 * @file output_metadata.cpp
 * @brief The output metadata file (.nbom): reader, validator and writer
 *        (docs/BINDING_RECORD.md, Output metadata). C++17, no dependencies.
 */
#include "niobium/output_metadata.h"

#include "file_io.h"
#include "niobium/binding_record.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace niobium::binding {

namespace {

using detail::ByteView;
using detail::load;
using detail::store;

constexpr std::array<char, 4> kMagic{'N', 'B', 'O', 'M'};
constexpr std::string_view kWhat = "output metadata";

struct FileHeader {
    detail::Preamble preamble;
    std::uint32_t num_entries;
    std::uint32_t label_pool_len;
};

struct Entry {
    std::uint32_t label_offset;
    std::uint16_t label_len;
    std::uint16_t reserved0;
    double scaling_factor;
    std::uint32_t scale_degree;
    std::uint32_t slots;
};

static_assert(sizeof(FileHeader) == 24 && offsetof(FileHeader, num_entries) == 16);
static_assert(offsetof(FileHeader, label_pool_len) == 20);
static_assert(sizeof(Entry) == 24 && offsetof(Entry, label_len) == 4 && offsetof(Entry, scaling_factor) == 8);
static_assert(offsetof(Entry, scale_degree) == 16 && offsetof(Entry, slots) == 20);
static_assert(std::is_trivially_copyable_v<FileHeader> && std::is_trivially_copyable_v<Entry>);

constexpr std::uint64_t entries_at = sizeof(FileHeader);

constexpr std::uint64_t pool_at(std::uint64_t num_entries) { return entries_at + (num_entries * sizeof(Entry)); }

}  // namespace

std::filesystem::path nbom_path(const std::filesystem::path& trace) {
    return std::filesystem::path(trace).replace_extension(".nbom");
}

const OutputMetadataEntry* OutputMetadata::find(std::string_view label) const noexcept {
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [&](const OutputMetadataEntry& e) { return e.label == label; });
    return it == entries.end() ? nullptr : &*it;
}

void OutputMetadata::set(OutputMetadataEntry entry) {
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [&](const OutputMetadataEntry& e) { return e.label == entry.label; });
    if (it == entries.end())
        entries.push_back(std::move(entry));
    else
        *it = std::move(entry);
}

bool operator==(const OutputMetadataEntry& a, const OutputMetadataEntry& b) noexcept {
    return std::tie(a.label, a.scaling_factor, a.scale_degree, a.slots) ==
           std::tie(b.label, b.scaling_factor, b.scale_degree, b.slots);
}

bool operator==(const OutputMetadata& a, const OutputMetadata& b) noexcept { return a.entries == b.entries; }

void OutputMetadata::validate() const {
    std::set<std::string_view> seen;
    std::uint64_t pool_len = 0;
    for (const auto& e : entries) {
        if (auto problem = label_problem(e.label, Kind::Output); !problem.empty())
            detail::fail(RecordErrc::BadLabel, kWhat, problem);
        if (!seen.insert(e.label).second)
            detail::fail(RecordErrc::DuplicateLabel, kWhat, "label '" + e.label + "' appears twice");
        pool_len += e.label.size();
    }
    if (entries.size() > UINT32_MAX || pool_len > UINT32_MAX)
        detail::fail(RecordErrc::InvalidArgument, kWhat, "too many entries or label bytes for the u32 fields");
}

OutputMetadata OutputMetadata::parse(const std::byte* data, std::size_t size) {
    const ByteView bytes(data, size);
    detail::check_preamble(bytes, sizeof(FileHeader), kMagic, kVersion, kWhat);
    const auto h = load<FileHeader>(bytes, 0);
    const auto pool_start = pool_at(h.num_entries);
    detail::check_size(bytes, pool_start + h.label_pool_len, kWhat);

    const auto pool = bytes.sub(pool_start);
    OutputMetadata meta;
    meta.entries.reserve(h.num_entries);
    for (std::uint64_t i = 0; i < h.num_entries; ++i) {
        const auto e = load<Entry>(bytes, entries_at + (i * sizeof(Entry)));
        meta.entries.push_back(OutputMetadataEntry{
            detail::pool_label(pool, e.label_offset, e.label_len, kWhat, "entry " + std::to_string(i)),
            e.scaling_factor, e.scale_degree, e.slots});
    }
    meta.validate();
    return meta;
}

OutputMetadata OutputMetadata::read(const std::filesystem::path& path) { return parse(detail::read_file(path, kWhat)); }

std::vector<std::byte> OutputMetadata::serialize() const {
    validate();
    std::uint32_t pool_len = 0;
    for (const auto& e : entries) pool_len += static_cast<std::uint32_t>(e.label.size());
    const auto pool_start = pool_at(entries.size());
    std::vector<std::byte> out(pool_start + pool_len);
    store(out, 0,
          FileHeader{detail::make_preamble(kMagic, kVersion), static_cast<std::uint32_t>(entries.size()), pool_len});
    std::uint32_t label_at = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        store(out, entries_at + (i * sizeof(Entry)),
              Entry{label_at, static_cast<std::uint16_t>(e.label.size()), 0, e.scaling_factor, e.scale_degree,
                    e.slots});
        detail::put_label(out, pool_start, label_at, e.label);
    }
    return out;
}

void OutputMetadata::write(const std::filesystem::path& path) const {
    detail::write_atomically(path, serialize(), kWhat);
}

}  // namespace niobium::binding
