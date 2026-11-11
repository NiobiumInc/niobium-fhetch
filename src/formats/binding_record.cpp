// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

/**
 * @file binding_record.cpp
 * @brief Labels, and the binding record's reader, validator and writer
 *        (docs/BINDING_RECORD.md). C++17, no dependencies.
 */
#include "niobium/binding_record.h"

#include "file_io.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace niobium::binding {

namespace {

// --- On-disk structures ---------------------------------------------------------

using detail::ByteView;
using detail::load;
using detail::store;

constexpr std::array<char, 4> kMagic{'N', 'B', 'B', 'R'};
constexpr std::string_view kWhat = "binding record";
constexpr std::size_t kMaxRunLimbs = UINT16_MAX;

struct FileHeader {
    std::array<char, 4> magic;
    std::uint16_t version;
    std::uint16_t flags;
    std::uint64_t sentinel;
    std::uint32_t ring_dim;
    std::uint16_t num_moduli;
    std::uint16_t reserved0;
    std::uint32_t num_objects;
    std::uint32_t num_runs;
    std::uint32_t label_pool_len;
    std::uint32_t reserved1;
};

struct ObjectEntry {
    std::uint32_t label_offset;
    std::uint16_t label_len;
    std::uint8_t kind;
    std::uint8_t reserved0;
    std::uint16_t poly_count;
    std::uint16_t reserved1;
    std::uint32_t first_run;
    std::uint32_t run_count;
};

struct RunEntry {
    std::uint16_t big_poly;
    std::uint16_t first_modulus;
    std::uint16_t limb_count;
    std::uint16_t reserved0;
    std::uint64_t first_address;
};

static_assert(sizeof(FileHeader) == 40 && offsetof(FileHeader, ring_dim) == 16);
static_assert(offsetof(FileHeader, num_moduli) == 20 && offsetof(FileHeader, num_objects) == 24);
static_assert(offsetof(FileHeader, num_runs) == 28 && offsetof(FileHeader, label_pool_len) == 32);
static_assert(sizeof(ObjectEntry) == 20 && offsetof(ObjectEntry, kind) == 6);
static_assert(offsetof(ObjectEntry, poly_count) == 8 && offsetof(ObjectEntry, first_run) == 12);
static_assert(offsetof(ObjectEntry, run_count) == 16);
static_assert(sizeof(RunEntry) == 16 && offsetof(RunEntry, limb_count) == 4);
static_assert(offsetof(RunEntry, first_address) == 8);
static_assert(std::is_trivially_copyable_v<FileHeader> && std::is_trivially_copyable_v<ObjectEntry> &&
              std::is_trivially_copyable_v<RunEntry>);

// Where each table starts, from the counts. 64-bit, so no count overflows it.
struct Offsets {
    std::uint64_t moduli;
    std::uint64_t roots;
    std::uint64_t objects;
    std::uint64_t runs;
    std::uint64_t pool;
    std::uint64_t end;
};

constexpr std::uint64_t align8(std::uint64_t n) { return (n + 7U) & ~std::uint64_t{7}; }

Offsets offsets_of(std::uint64_t num_moduli, std::uint64_t num_objects, std::uint64_t num_runs,
                   std::uint64_t pool_len) {
    Offsets o{};
    o.moduli = sizeof(FileHeader);
    o.roots = o.moduli + (num_moduli * sizeof(std::uint64_t));
    o.objects = o.roots + (num_moduli * sizeof(std::uint64_t));
    o.runs = align8(o.objects + (num_objects * sizeof(ObjectEntry)));
    o.pool = o.runs + (num_runs * sizeof(RunEntry));
    o.end = o.pool + pool_len;
    return o;
}

std::string rule_what(int rule) { return std::string(kWhat) + ": rule " + std::to_string(rule); }

[[noreturn]] void fail(RecordErrc code, int rule, const std::string& message) {
    detail::fail(code, rule_what(rule), message);
}

[[noreturn]] void fail_arg(const std::string& message) { detail::fail(RecordErrc::InvalidArgument, kWhat, message); }

std::string in_quotes(std::string_view label) { return "'" + std::string(label) + "'"; }

// --- Labels ---------------------------------------------------------------------

constexpr std::array<std::string_view, 4> kKindNames{"input", "output", "key", "constant"};
constexpr std::array<std::string_view, 4> kKindPrefixes{"input:", "output:", "key:", "const:"};

bool starts_with(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool is_label_char(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
           c == ':' || c == '-';
}

// A canonical decimal: digits, no leading zero, fits u64.
bool is_decimal(std::string_view s) {
    if (s.empty() || (s.size() > 1 && s.front() == '0')) return false;
    std::uint64_t value = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = (value * 10) + digit;
    }
    return true;
}

// `name` is `<system_prefix><decimal>` for one of the prefixes.
bool is_numbered(std::string_view name, std::initializer_list<std::string_view> prefixes) {
    return std::any_of(prefixes.begin(), prefixes.end(), [&](std::string_view p) {
        return starts_with(name, p) && is_decimal(name.substr(p.size()));
    });
}

bool is_author_name(std::string_view name) { return !name.empty() && name.find(':') == std::string_view::npos; }

auto limb_key(const Limb& l) { return std::make_pair(l.big_poly, l.modulus_index); }

std::uint64_t pow_mod(std::uint64_t base, std::uint64_t exp, std::uint64_t modulus) {
    using u128 = unsigned __int128;
    std::uint64_t result = 1 % modulus;
    base %= modulus;
    for (; exp != 0; exp >>= 1U) {
        if ((exp & 1U) != 0) result = static_cast<std::uint64_t>((u128{result} * base) % modulus);
        base = static_cast<std::uint64_t>((u128{base} * base) % modulus);
    }
    return result;
}

}  // namespace

std::string_view kind_name(Kind kind) noexcept { return kKindNames.at(static_cast<std::size_t>(kind)); }

std::string_view kind_prefix(Kind kind) noexcept { return kKindPrefixes.at(static_cast<std::size_t>(kind)); }

std::optional<Kind> kind_of_label(std::string_view label) noexcept {
    for (std::size_t k = 0; k < kKindPrefixes.size(); ++k)
        if (starts_with(label, kKindPrefixes.at(k))) return static_cast<Kind>(k);
    return std::nullopt;
}

bool is_label_text(std::string_view label) noexcept {
    return !label.empty() && label.size() <= kMaxLabelLen && std::all_of(label.begin(), label.end(), is_label_char);
}

std::string label_problem(std::string_view label, Kind kind) {
    if (!is_label_text(label))
        return "label " + in_quotes(label) + " is not 1 to " + std::to_string(kMaxLabelLen) +
               " bytes of [A-Za-z0-9_.:-]";
    const auto prefix = kind_prefix(kind);
    if (!starts_with(label, prefix))
        return "label " + in_quotes(label) + " of a " + std::string(kind_name(kind)) + " does not start with '" +
               std::string(prefix) + "'";
    const auto name = label.substr(prefix.size());
    switch (kind) {
        case Kind::Input:
        case Kind::Output:
            if (!is_author_name(name))
                return "label " + in_quotes(label) + ": an " + std::string(kind_name(kind)) +
                       " name is non-empty and contains no ':'";
            break;
        case Kind::Key:
            if (name != "relin" && !is_numbered(name, {"galois:"}))
                return "label " + in_quotes(label) + ": a key is named 'relin' or 'galois:<decimal>'";
            break;
        case Kind::Constant:
            if (!is_author_name(name) && !is_numbered(name, {"boot:", "op:"}))
                return "label " + in_quotes(label) +
                       ": a constant name contains no ':', or is 'boot:<decimal>' or 'op:<decimal>'";
            break;
    }
    return {};
}

std::string make_label(Kind kind, std::string_view name) {
    std::string label = std::string(kind_prefix(kind)) + std::string(name);
    if (auto problem = label_problem(label, kind); !problem.empty()) fail_arg(problem);
    return label;
}

// --- Comparison -------------------------------------------------------------------

bool operator==(const Run& a, const Run& b) noexcept {
    return std::tie(a.big_poly, a.first_modulus, a.limb_count, a.first_address) ==
           std::tie(b.big_poly, b.first_modulus, b.limb_count, b.first_address);
}

bool operator==(const Limb& a, const Limb& b) noexcept {
    return std::tie(a.big_poly, a.modulus_index, a.address) == std::tie(b.big_poly, b.modulus_index, b.address);
}

bool operator==(const Object& a, const Object& b) noexcept {
    return std::tie(a.label, a.kind, a.poly_count, a.runs) == std::tie(b.label, b.kind, b.poly_count, b.runs);
}

bool operator==(const BindingRecord& a, const BindingRecord& b) noexcept {
    return std::tie(a.ring_dim, a.moduli, a.roots, a.objects) == std::tie(b.ring_dim, b.moduli, b.roots, b.objects);
}

// --- Runs -----------------------------------------------------------------------

std::vector<Run> make_runs(const std::vector<Limb>& sorted_limbs) {
    std::vector<Run> runs;
    const Limb* prev = nullptr;
    for (const auto& limb : sorted_limbs) {
        const bool extends = prev != nullptr && limb.big_poly == prev->big_poly &&
                             limb.modulus_index == prev->modulus_index + 1U && limb.address == prev->address + 1U &&
                             runs.back().limb_count < kMaxRunLimbs;
        if (extends)
            ++runs.back().limb_count;
        else
            runs.push_back(Run{limb.big_poly, limb.modulus_index, 1, limb.address});
        prev = &limb;
    }
    return runs;
}

std::filesystem::path record_path(const std::filesystem::path& trace) {
    return std::filesystem::path(trace).replace_extension(".nbbr");
}

// --- BindingRecord: building ------------------------------------------------------

const Object* BindingRecord::find(std::string_view label) const noexcept {
    const auto it = std::find_if(objects.begin(), objects.end(), [&](const Object& o) { return o.label == label; });
    return it == objects.end() ? nullptr : &*it;
}

bool BindingRecord::remove_object(std::string_view label) {
    const auto it = std::find_if(objects.begin(), objects.end(), [&](const Object& o) { return o.label == label; });
    if (it == objects.end()) return false;
    objects.erase(it);
    return true;
}

void BindingRecord::add_object(std::string label, Kind kind, std::uint16_t poly_count, const std::vector<Limb>& limbs) {
    if (auto problem = label_problem(label, kind); !problem.empty()) fail_arg(problem);
    if (find(label) != nullptr) fail_arg("object " + in_quotes(label) + " added twice");
    if (poly_count == 0) fail_arg("object " + in_quotes(label) + " has poly_count 0");
    for (const auto& limb : limbs) {
        if (limb.big_poly >= poly_count || limb.modulus_index >= moduli.size())
            fail_arg("object " + in_quotes(label) + ": limb (big_poly " + std::to_string(limb.big_poly) +
                     ", modulus_index " + std::to_string(limb.modulus_index) + ") lies outside its " +
                     std::to_string(poly_count) + " big polynomials or the " + std::to_string(moduli.size()) +
                     " moduli");
    }
    std::vector<Limb> sorted = limbs;
    const auto by_key = [](const Limb& a, const Limb& b) { return limb_key(a) < limb_key(b); };
    std::sort(sorted.begin(), sorted.end(), by_key);
    const auto dup = std::adjacent_find(sorted.begin(), sorted.end(),
                                        [](const Limb& a, const Limb& b) { return limb_key(a) == limb_key(b); });
    if (dup != sorted.end())
        fail_arg("object " + in_quotes(label) + ": limb (big_poly " + std::to_string(dup->big_poly) +
                 ", modulus_index " + std::to_string(dup->modulus_index) + ") given twice");
    objects.push_back(Object{std::move(label), kind, poly_count, make_runs(sorted)});
}

// --- BindingRecord: validation ----------------------------------------------------

void BindingRecord::validate() const {
    if (moduli.size() != roots.size())
        fail_arg(std::to_string(moduli.size()) + " moduli but " + std::to_string(roots.size()) + " roots");
    if (moduli.size() > UINT16_MAX) fail_arg(std::to_string(moduli.size()) + " moduli; at most 65535 fit");

    // Rule 2: distinct primes, and a valid root for every modulus a run uses.
    {
        std::unordered_set<std::uint64_t> seen;
        for (const auto q : moduli)
            if (!seen.insert(q).second)
                fail(RecordErrc::DuplicateModulus, 2, "prime " + std::to_string(q) + " appears twice in moduli");
        std::vector<const Object*> user(moduli.size(), nullptr);
        for (const auto& obj : objects)
            for (const auto& run : obj.runs) {
                const std::size_t end = std::min<std::size_t>(std::size_t{run.first_modulus} + run.limb_count, moduli.size());
                for (std::size_t k = run.first_modulus; k < end; ++k)
                    if (user[k] == nullptr) user[k] = &obj;
            }
        for (std::size_t k = 0; k < moduli.size(); ++k) {
            if (user[k] == nullptr) continue;
            const auto q = moduli[k];
            const auto root = roots[k];
            if (root == 0 || root >= q || pow_mod(root, ring_dim, q) != q - 1)
                fail(RecordErrc::BadRoot, 2,
                     "roots[" + std::to_string(k) + "] = " + std::to_string(root) +
                         " is not a primitive root of unity of order 2*" + std::to_string(ring_dim) + " modulo " +
                         std::to_string(q) + ", which object " + in_quotes(user[k]->label) + " uses");
        }
    }

    // Rules 3 and 4, per object; 5 and 6, per run.
    std::set<std::string_view> labels;
    for (const auto& obj : objects) {
        const auto who = "object " + in_quotes(obj.label);
        if (static_cast<std::uint8_t>(obj.kind) > kMaxKindCode)
            fail(RecordErrc::BadKind, 3, who + " has kind " + std::to_string(static_cast<int>(obj.kind)));
        if (auto problem = label_problem(obj.label, obj.kind); !problem.empty()) fail(RecordErrc::BadLabel, 3, problem);
        if (obj.poly_count == 0) fail(RecordErrc::BadPolyCount, 3, who + " has poly_count 0");
        if (!labels.insert(obj.label).second)
            fail(RecordErrc::DuplicateLabel, 4, "label " + in_quotes(obj.label) + " appears twice");

        std::set<std::pair<std::uint16_t, std::size_t>> limbs;
        for (const auto& run : obj.runs) {
            const auto at = who + " run (big_poly " + std::to_string(run.big_poly) + ", first_modulus " +
                            std::to_string(run.first_modulus) + ", limb_count " + std::to_string(run.limb_count) + ")";
            if (run.big_poly >= obj.poly_count)
                fail(RecordErrc::BadRun, 5, at + ": big_poly is not below poly_count " + std::to_string(obj.poly_count));
            if (run.limb_count == 0) fail(RecordErrc::BadRun, 5, at + ": limb_count is 0");
            if (std::size_t{run.first_modulus} + run.limb_count > moduli.size())
                fail(RecordErrc::BadRun, 5, at + ": runs past the " + std::to_string(moduli.size()) + " moduli");
            if (run.first_address > UINT64_MAX - (run.limb_count - 1U))
                fail(RecordErrc::BadRun, 5, at + ": addresses overflow u64");
            for (std::size_t i = 0; i < run.limb_count; ++i)
                if (!limbs.emplace(run.big_poly, run.first_modulus + i).second)
                    fail(RecordErrc::DuplicateLimb, 6,
                         who + ": big polynomial " + std::to_string(run.big_poly) + ", modulus index " +
                             std::to_string(run.first_modulus + i) + " appears in two runs");
        }
    }

    // Rule 6: no address in two runs. Sorted by start, runs are disjoint iff each
    // ends before the next starts.
    std::vector<std::tuple<std::uint64_t, std::uint64_t, const Object*>> spans;  // [first, last], owner
    for (const auto& obj : objects)
        for (const auto& run : obj.runs)
            spans.emplace_back(run.first_address, run.first_address + run.limb_count - 1U, &obj);
    std::sort(spans.begin(), spans.end());
    for (std::size_t i = 1; i < spans.size(); ++i) {
        const auto first = std::get<0>(spans[i]);
        if (first <= std::get<1>(spans[i - 1]))
            fail(RecordErrc::DuplicateAddress, 6,
                 "address " + std::to_string(first) + " appears in a run of " + in_quotes(std::get<2>(spans[i])->label) +
                     " and in one of " + in_quotes(std::get<2>(spans[i - 1])->label));
    }
}

// --- BindingRecord: reading -------------------------------------------------------

BindingRecord BindingRecord::parse(const std::byte* data, std::size_t size) {
    const ByteView bytes(data, size);
    detail::check_preamble(bytes, sizeof(FileHeader), kMagic, kVersion, rule_what(1));
    const auto h = load<FileHeader>(bytes, 0);
    const auto off = offsets_of(h.num_moduli, h.num_objects, h.num_runs, h.label_pool_len);
    detail::check_size(bytes, off.end, rule_what(1));

    BindingRecord rec;
    rec.ring_dim = h.ring_dim;
    for (std::uint64_t k = 0; k < h.num_moduli; ++k) {
        rec.moduli.push_back(load<std::uint64_t>(bytes, off.moduli + (k * 8)));
        rec.roots.push_back(load<std::uint64_t>(bytes, off.roots + (k * 8)));
    }
    const auto pool = bytes.sub(off.pool);

    // Rule 3's format part, then rule 4's run ranges: they must tile the runs table.
    std::vector<ObjectEntry> entries;
    std::vector<std::pair<std::uint32_t, std::size_t>> ranges;  // (first_run, object) for non-empty ranges
    for (std::uint64_t i = 0; i < h.num_objects; ++i) {
        const auto e = load<ObjectEntry>(bytes, off.objects + (i * sizeof(ObjectEntry)));
        const auto who = "object " + std::to_string(i);
        if (e.kind > kMaxKindCode) fail(RecordErrc::BadKind, 3, who + " has kind " + std::to_string(e.kind));
        const auto label = detail::pool_label(pool, e.label_offset, e.label_len, rule_what(3), who);
        if (auto problem = label_problem(label, static_cast<Kind>(e.kind)); !problem.empty())
            fail(RecordErrc::BadLabel, 3, problem);
        if (std::uint64_t{e.first_run} + e.run_count > h.num_runs)
            fail(RecordErrc::BadRunRange, 3,
                 "object " + in_quotes(label) + "'s runs " + std::to_string(e.first_run) + " + " +
                     std::to_string(e.run_count) + " pass the " + std::to_string(h.num_runs) + " runs");
        if (e.run_count != 0) ranges.emplace_back(e.first_run, rec.objects.size());
        rec.objects.push_back(Object{label, static_cast<Kind>(e.kind), e.poly_count, {}});
        entries.push_back(e);
    }
    std::sort(ranges.begin(), ranges.end());
    std::uint64_t next = 0;
    for (const auto& [first, obj] : ranges) {
        if (first < next)
            fail(RecordErrc::OverlappingRuns, 4,
                 "the run range of object " + in_quotes(rec.objects[obj].label) + " overlaps another object's");
        if (first > next) fail(RecordErrc::OrphanRun, 4, "run " + std::to_string(next) + " belongs to no object");
        next = std::uint64_t{first} + entries[obj].run_count;
    }
    if (next != h.num_runs) fail(RecordErrc::OrphanRun, 4, "run " + std::to_string(next) + " belongs to no object");

    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        for (std::uint64_t r = e.first_run; r < std::uint64_t{e.first_run} + e.run_count; ++r) {
            const auto run = load<RunEntry>(bytes, off.runs + (r * sizeof(RunEntry)));
            rec.objects[i].runs.push_back(Run{run.big_poly, run.first_modulus, run.limb_count, run.first_address});
        }
    }
    rec.validate();
    return rec;
}

BindingRecord BindingRecord::read(const std::filesystem::path& path) { return parse(detail::read_file(path, kWhat)); }

// --- BindingRecord: writing -------------------------------------------------------

std::vector<std::byte> BindingRecord::serialize() const {
    validate();
    std::uint64_t num_runs = 0;
    std::uint64_t pool_len = 0;
    for (const auto& obj : objects) {
        num_runs += obj.runs.size();
        pool_len += obj.label.size();
    }
    if (objects.size() > UINT32_MAX || num_runs > UINT32_MAX || pool_len > UINT32_MAX)
        fail_arg("too many objects, runs or label bytes for the u32 fields");

    const auto off = offsets_of(moduli.size(), objects.size(), num_runs, pool_len);
    std::vector<std::byte> out(off.end);
    const auto pre = detail::make_preamble(kMagic, kVersion);
    store(out, 0,
          FileHeader{pre.magic, pre.version, pre.flags, pre.sentinel, ring_dim,
                     static_cast<std::uint16_t>(moduli.size()), 0, static_cast<std::uint32_t>(objects.size()),
                     static_cast<std::uint32_t>(num_runs), static_cast<std::uint32_t>(pool_len), 0});
    for (std::size_t k = 0; k < moduli.size(); ++k) {
        store(out, off.moduli + (k * 8), moduli[k]);
        store(out, off.roots + (k * 8), roots[k]);
    }
    std::uint32_t run = 0;
    std::uint32_t label_at = 0;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const auto& obj = objects[i];
        store(out, off.objects + (i * sizeof(ObjectEntry)),
              ObjectEntry{label_at, static_cast<std::uint16_t>(obj.label.size()), static_cast<std::uint8_t>(obj.kind),
                          0, obj.poly_count, 0, run, static_cast<std::uint32_t>(obj.runs.size())});
        for (const auto& r : obj.runs)
            store(out, off.runs + (std::uint64_t{run++} * sizeof(RunEntry)),
                  RunEntry{r.big_poly, r.first_modulus, r.limb_count, 0, r.first_address});
        detail::put_label(out, off.pool, label_at, obj.label);
    }
    return out;
}

void BindingRecord::write(const std::filesystem::path& path) const {
    detail::write_atomically(path, serialize(), kWhat);
}

}  // namespace niobium::binding
