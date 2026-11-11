// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

/**
 * @file formats_test.cpp
 * @brief The binding record (.nbbr) and output metadata (.nbom) formats, on
 *        small synthetic records (n = 4). C++17, no dependencies: runs in every
 *        test build.
 *
 * Covers:
 *  - labels: prefixes, author and system names, make_label;
 *  - run splitting: contiguous, address gaps, modulus skips, big-polynomial
 *    changes, one limb per run, and limbs given out of order;
 *  - write -> read and serialize -> parse round trips, atomic write;
 *  - every Validation rule failing on a mutated file;
 *  - output metadata: round trip, the layout pinned by byte offsets, every
 *    check failing, reserved fields ignored.
 */
#include "test_util.h"

#include "niobium/binding_record.h"
#include "niobium/output_metadata.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace binding_test;

namespace {

// --- Labels -----------------------------------------------------------------------

void test_labels() {
    std::cout << "labels\n";
    check(make_label(Kind::Input, "x") == "input:x", "make_label(input, x)");
    check(make_label(Kind::Constant, "boot:0") == "const:boot:0", "make_label(constant, boot:0)");
    check(kind_of_label("key:relin") == Kind::Key && kind_of_label("const:w") == Kind::Constant &&
              !kind_of_label("weights").has_value(),
          "kind_of_label");
    for (const auto* good : {"key:relin", "key:galois:5", "key:galois:0"})
        check(is_valid_label(good, Kind::Key), std::string("valid key label ") + good);
    for (const auto* good : {"const:w", "const:boot:0", "const:op:17", "const:a.b-c_d"})
        check(is_valid_label(good, Kind::Constant), std::string("valid constant label ") + good);
    const std::vector<std::pair<std::string, Kind>> bad{
        {"input:", Kind::Input},          {"input:a:b", Kind::Input},       {"output:x", Kind::Input},
        {"key:x", Kind::Key},             {"key:galois:", Kind::Key},       {"key:galois:05", Kind::Key},
        {"key:galois:-1", Kind::Key},     {"const:boot:x", Kind::Constant}, {"const:foo:1", Kind::Constant},
        {"input:a b", Kind::Input},       {"input:" + std::string(250, 'a'), Kind::Input},
    };
    for (const auto& [label, kind] : bad)
        check(!is_valid_label(label, kind), "rejects '" + label.substr(0, 20) + "' as " + std::string(kind_name(kind)));
    check_record_error([] { (void)make_label(Kind::Output, "a:b"); }, RecordErrc::InvalidArgument, "output:a:b",
                       "make_label rejects an author name with ':'");
}

// --- Runs -------------------------------------------------------------------------

Run mk(std::uint16_t big_poly, std::uint16_t first_modulus, std::uint16_t limb_count, std::uint64_t first_address) {
    return Run{big_poly, first_modulus, limb_count, first_address};
}

void test_runs() {
    std::cout << "run splitting\n";
    using V = std::vector<Run>;
    check(make_runs(limbs(0, {0, 1, 2}, {100, 101, 102})) == V{mk(0, 0, 3, 100)}, "contiguous limbs form one run");
    check(make_runs(limbs(0, {0, 1, 2}, {100, 101, 105})) == V{mk(0, 0, 2, 100), mk(0, 2, 1, 105)},
          "an address gap starts a run");
    check(make_runs(limbs(0, {0, 2, 3}, {100, 101, 102})) == V{mk(0, 0, 1, 100), mk(0, 2, 2, 101)},
          "a skipped modulus starts a run");
    check(make_runs(concat(limbs(0, {0}, {100}), limbs(1, {1}, {101}))) == V{mk(0, 0, 1, 100), mk(1, 1, 1, 101)},
          "a new big polynomial starts a run");
    check(make_runs(limbs(0, {0, 1, 2}, {12, 11, 10})) == V{mk(0, 0, 1, 12), mk(0, 1, 1, 11), mk(0, 2, 1, 10)},
          "worst case: one limb per run");

    BindingRecord sorted = empty_record();
    BindingRecord shuffled = sorted;
    sorted.add_object("input:x", Kind::Input, 2, concat(limbs(0, {0, 1, 2}, {100, 101, 102}), limbs(1, {0, 1}, {200, 201})));
    shuffled.add_object("input:x", Kind::Input, 2,
                        concat(limbs(1, {1, 0}, {201, 200}), limbs(0, {2, 0, 1}, {102, 100, 101})));
    check(sorted == shuffled && sorted.objects[0].runs == V{mk(0, 0, 3, 100), mk(1, 0, 2, 200)},
          "add_object sorts limbs before splitting");

    BindingRecord rec = empty_record();
    check_record_error([&] { rec.add_object("input:x", Kind::Input, 1, limbs(0, {0, 0}, {1, 2})); },
                       RecordErrc::InvalidArgument, "given twice", "add_object rejects a limb given twice");
    check_record_error([&] { rec.add_object("input:x", Kind::Input, 1, limbs(1, {0}, {1})); },
                       RecordErrc::InvalidArgument, "input:x", "add_object rejects a big_poly past poly_count");
    check_record_error([&] { rec.add_object("input:x", Kind::Input, 1, limbs(0, {4}, {1})); },
                       RecordErrc::InvalidArgument, "input:x", "add_object rejects a modulus index past the moduli");
    check_record_error([&] { rec.add_object("key:x", Kind::Key, 1, {}); }, RecordErrc::InvalidArgument, "key:x",
                       "add_object rejects a malformed label");
    rec.add_object("input:x", Kind::Input, 1, {});
    check_record_error([&] { rec.add_object("input:x", Kind::Input, 1, {}); }, RecordErrc::InvalidArgument,
                       "added twice", "add_object rejects a label added twice");
}

// --- Round trip -------------------------------------------------------------------

void test_round_trip(const std::filesystem::path& dir) {
    std::cout << "round trip\n";
    const auto rec = make_record();
    const auto bytes = rec.serialize();
    check(BindingRecord::parse(bytes) == rec, "serialize -> parse is the identity");
    check(rec.serialize() == bytes, "serialize is deterministic");

    const auto path = dir / "program.nbbr";
    rec.write(path);
    rec.write(path);  // replaces the file in place
    check(BindingRecord::read(path) == rec, "write -> read is the identity");
    const auto entries = std::distance(std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator{});
    check(entries == 1, "the atomic write leaves no temp file");
    check(record_path("dir/prog.fhex") == "dir/prog.nbbr", "record_path sits next to the trace");
    check_record_error([&] { (void)BindingRecord::read(dir / "missing.nbbr"); }, RecordErrc::Io, "missing.nbbr",
                       "reading a missing file");

    // A modulus no run uses needs no valid root.
    auto spare = rec;
    spare.moduli.push_back(241);
    spare.roots.push_back(0);
    check(BindingRecord::parse(spare.serialize()) == spare, "an unused modulus may have root 0");
}

// --- Validation -------------------------------------------------------------------

// Byte offsets of the fixture record's tables.
struct Where {
    std::size_t moduli, roots, objects, runs, pool;
};

Where where_of(const BindingRecord& rec) {
    std::size_t runs = 0;
    for (const auto& o : rec.objects) runs += o.runs.size();
    const std::size_t m = rec.moduli.size();
    const std::size_t objects = 40 + (16 * m);
    const std::size_t run_at = (objects + (20 * rec.objects.size()) + 7) / 8 * 8;
    return Where{40, 40 + (8 * m), objects, run_at, run_at + (16 * runs)};
}


using Mutation = std::function<void(std::vector<std::byte>&)>;

// A checker: `good` mutated must fail Parsed::parse with `code`, naming `needle`.
template <class Parsed>
auto mutation_checker(std::vector<std::byte> good) {
    return [good = std::move(good)](const Mutation& mutate, RecordErrc code, const std::string& needle,
                                    const std::string& what) {
        auto bytes = good;
        mutate(bytes);
        check_record_error([&] { (void)Parsed::parse(bytes); }, code, needle, what);
    };
}

// The checks both formats make on their 16-byte preamble and their size.
// `header_size` is the fixed header's; `tag` prefixes each description.
template <class Checker>
void check_preamble_failures(const Checker& fails, std::size_t header_size, const std::string& tag) {
    fails([](auto& b) { b[0] = std::byte{'X'}; }, RecordErrc::BadMagic, "magic", tag + "bad magic");
    fails([](auto& b) { poke<std::uint64_t>(b, 8, 0xEFCDAB8967452301ULL); }, RecordErrc::BigEndianWriter, "big-endian",
          tag + "byte-swapped sentinel is a distinct error");
    fails([](auto& b) { poke<std::uint64_t>(b, 8, 1); }, RecordErrc::BadSentinel, "sentinel", tag + "bad sentinel");
    fails([](auto& b) { poke<std::uint16_t>(b, 4, 2); }, RecordErrc::UnsupportedVersion, "version 2", tag + "version 2");
    fails([](auto& b) { b.pop_back(); }, RecordErrc::SizeMismatch, "layout needs", tag + "truncated file");
    fails([](auto& b) { b.push_back(std::byte{0}); }, RecordErrc::SizeMismatch, "layout needs", tag + "trailing byte");
    fails([&](auto& b) { b.resize(header_size - 1); }, RecordErrc::SizeMismatch, "shorter than the header",
          tag + "shorter than the header");
}

void test_validation() {
    std::cout << "validation\n";
    const auto rec = make_record();
    const auto good = rec.serialize();
    const auto w = where_of(rec);
    const auto obj = [&](std::size_t i, std::size_t field) { return w.objects + (20 * i) + field; };
    const auto run = [&](std::size_t i, std::size_t field) { return w.runs + (16 * i) + field; };

    const auto fails = mutation_checker<BindingRecord>(good);

    // Rule 1
    check_preamble_failures(fails, 40, "1: ");
    fails([](auto& b) { b[0] = std::byte{'X'}; }, RecordErrc::BadMagic, "rule 1", "1: errors name the rule");
    fails([](auto& b) { poke<std::uint32_t>(b, 28, peek<std::uint32_t>(b, 28) + 1); }, RecordErrc::SizeMismatch,
          "rule 1", "1: num_runs disagrees with the size");

    // Rule 2
    fails([&](auto& b) { poke<std::uint64_t>(b, w.moduli + 8, kModuli[0]); }, RecordErrc::DuplicateModulus, "17",
          "2: a prime twice in moduli");
    fails([&](auto& b) { poke<std::uint64_t>(b, w.roots + 8, 0); }, RecordErrc::BadRoot, "roots[1]",
          "2: a used modulus with root 0");
    fails([&](auto& b) { poke<std::uint64_t>(b, w.roots, 1); }, RecordErrc::BadRoot, "input:x",
          "2: a root that is not primitive (names the object)");
    fails([&](auto& b) { poke<std::uint64_t>(b, w.roots, rec.roots[0] + kModuli[0]); }, RecordErrc::BadRoot,
          "roots[0]", "2: a root not below its prime");

    // Rule 3 (object 0 is input:x, object 2 is key:relin)
    fails([&](auto& b) { poke<std::uint8_t>(b, obj(0, 6), 4); }, RecordErrc::BadKind, "kind 4", "3: kind 4");
    fails([&](auto& b) { poke<std::uint32_t>(b, obj(6, 0), 1000); }, RecordErrc::BadLabel, "outside the label pool",
          "3: label outside the pool");
    fails([&](auto& b) { poke<std::uint16_t>(b, obj(0, 4), 0); }, RecordErrc::BadLabel, "rule 3", "3: empty label");
    fails([&](auto& b) { poke<std::uint8_t>(b, obj(0, 6), 2); }, RecordErrc::BadLabel, "input:x",
          "3: prefix does not match kind");
    fails([&](auto& b) { b[w.pool + 6] = std::byte{'!'}; }, RecordErrc::BadLabel, "input:!", "3: disallowed label byte");
    fails([&](auto& b) { b[w.pool + 6] = std::byte{':'}; }, RecordErrc::BadLabel, "contains no ':'",
          "3: author name with ':'");
    fails([&](auto& b) { poke<std::uint32_t>(b, obj(6, 16), 100); }, RecordErrc::BadRunRange, "output:y",
          "3: first_run + run_count past the runs");
    fails([&](auto& b) { poke<std::uint16_t>(b, obj(0, 8), 0); }, RecordErrc::BadPolyCount, "poly_count 0",
          "3: poly_count 0");

    // Rule 4. Objects 0 (input:x) and 1 (input:w) have labels of one length.
    fails([&](auto& b) { poke<std::uint32_t>(b, obj(1, 0), peek<std::uint32_t>(b, obj(0, 0))); },
          RecordErrc::DuplicateLabel, "input:x", "4: two objects with one label");
    fails([&](auto& b) { poke<std::uint32_t>(b, obj(1, 12), 0); }, RecordErrc::OverlappingRuns, "overlaps",
          "4: overlapping run ranges");
    fails([&](auto& b) { poke<std::uint32_t>(b, obj(6, 16), peek<std::uint32_t>(b, obj(6, 16)) - 1); },
          RecordErrc::OrphanRun, "belongs to no object", "4: a run no object covers");

    // Rule 5. Run 0 is input:x's (big 0, moduli 0..2, address 100).
    fails([&](auto& b) { poke<std::uint16_t>(b, run(0, 0), 2); }, RecordErrc::BadRun, "poly_count",
          "5: big_poly past poly_count");
    fails([&](auto& b) { poke<std::uint16_t>(b, run(0, 2), 2); }, RecordErrc::BadRun, "past the 4 moduli",
          "5: first_modulus + limb_count past the moduli");
    fails([&](auto& b) { poke<std::uint16_t>(b, run(0, 4), 0); }, RecordErrc::BadRun, "limb_count is 0",
          "5: limb_count 0");
    fails([&](auto& b) { poke<std::uint64_t>(b, run(0, 8), UINT64_MAX); }, RecordErrc::BadRun, "overflow",
          "5: addresses past u64");

    // Rule 6. Run 1 is input:x's (big 1, moduli 0..1, address 200), run 2 (big 1, modulus 2, 210).
    fails([&](auto& b) { poke<std::uint16_t>(b, run(2, 2), 1); }, RecordErrc::DuplicateLimb, "input:x",
          "6: a limb in two runs");
    fails([&](auto& b) { poke<std::uint64_t>(b, run(2, 8), 101); }, RecordErrc::DuplicateAddress, "address 101",
          "6: an address in two runs");

    // The in-memory checks run on write too.
    auto dup = rec;
    dup.objects[1].label = "input:x";
    check_record_error([&] { (void)dup.serialize(); }, RecordErrc::DuplicateLabel, "input:x",
                       "serialize validates first");
}

// --- Output metadata ---------------------------------------------------------------

OutputMetadata make_metadata() {
    OutputMetadata meta;
    meta.set(OutputMetadataEntry{"output:y", 0x1p40, 1, 8});
    meta.set(OutputMetadataEntry{"output:z", 0.5, 2, 4096});
    return meta;
}

void test_output_metadata(const std::filesystem::path& dir) {
    std::cout << "output metadata\n";
    const auto meta = make_metadata();
    const auto bytes = meta.serialize();
    check(OutputMetadata::parse(bytes) == meta, "serialize -> parse is the identity");
    const auto path = nbom_path(dir / "program.fhex");
    check(path == dir / "program.nbom", "nbom_path sits next to the trace");
    meta.write(path);
    meta.write(path);
    check(OutputMetadata::read(path) == meta, "write -> read is the identity");
    check(std::distance(std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator{}) == 1,
          "the atomic write leaves no temp file");
    check(meta.find("output:z") == &meta.entries[1] && meta.find("output:q") == nullptr, "find");
    check(OutputMetadata::parse(OutputMetadata{}.serialize()).entries.empty(), "an empty file round-trips");

    // The layout, pinned by byte offsets.
    const std::string pool = "output:youtput:z";
    bool pinned = bytes.size() == 24 + (2 * 24) + pool.size() && std::memcmp(bytes.data(), "NBOM", 4) == 0 &&
                  peek<std::uint16_t>(bytes, 4) == 1 && peek<std::uint16_t>(bytes, 6) == 0 &&
                  peek<std::uint64_t>(bytes, 8) == 0x0123456789ABCDEFULL && peek<std::uint32_t>(bytes, 16) == 2 &&
                  peek<std::uint32_t>(bytes, 20) == pool.size();
    const auto entry_is = [&](std::size_t at, std::uint32_t offset, double scale, std::uint32_t degree,
                              std::uint32_t slots) {
        return peek<std::uint32_t>(bytes, at) == offset && peek<std::uint16_t>(bytes, at + 4) == 8 &&
               peek<std::uint16_t>(bytes, at + 6) == 0 && peek<double>(bytes, at + 8) == scale &&
               peek<std::uint32_t>(bytes, at + 16) == degree && peek<std::uint32_t>(bytes, at + 20) == slots;
    };
    pinned = pinned && entry_is(24, 0, 0x1p40, 1, 8) && entry_is(48, 8, 0.5, 2, 4096) &&
             std::memcmp(bytes.data() + 72, pool.data(), pool.size()) == 0;
    check(pinned, "header, entries and pool at the specified offsets");

    // Reserved fields are ignored on read.
    auto reserved = bytes;
    poke<std::uint16_t>(reserved, 6, 0xFFFF);   // flags
    poke<std::uint16_t>(reserved, 30, 0xFFFF);  // entry 0 reserved
    check(OutputMetadata::parse(reserved) == meta, "reserved fields are ignored");

    const auto fails = mutation_checker<OutputMetadata>(bytes);
    check_preamble_failures(fails, 24, "nbom: ");
    fails([](auto& b) { b.resize(23); }, RecordErrc::SizeMismatch, "output metadata", "nbom: errors name the format");
    fails([](auto& b) { poke<std::uint32_t>(b, 16, 3); }, RecordErrc::SizeMismatch, "layout needs",
          "nbom: num_entries disagrees with the size");
    fails([](auto& b) { poke<std::uint32_t>(b, 20, 15); }, RecordErrc::SizeMismatch, "layout needs",
          "nbom: label_pool_len disagrees with the size");
    fails([](auto& b) { poke<std::uint32_t>(b, 48, 9); }, RecordErrc::BadLabel, "entry 1",
          "nbom: a label past the end of the pool");
    fails([](auto& b) { poke<std::uint32_t>(b, 24, 0xFFFFFFFF); }, RecordErrc::BadLabel, "outside the label pool",
          "nbom: a label offset far outside the pool");
    fails([](auto& b) { poke<std::uint16_t>(b, 28, 0); }, RecordErrc::BadLabel, "output metadata", "nbom: an empty label");
    fails([](auto& b) { b[72] = std::byte{'i'}; }, RecordErrc::BadLabel, "does not start with 'output:'",
          "nbom: a label of another kind");
    fails([](auto& b) { b[79] = std::byte{':'}; }, RecordErrc::BadLabel, "contains no ':'",
          "nbom: an output name with ':'");
    fails([](auto& b) { b[79] = std::byte{'!'}; }, RecordErrc::BadLabel, "output:!", "nbom: a disallowed label byte");
    fails([](auto& b) { poke<std::uint32_t>(b, 48, 0); }, RecordErrc::DuplicateLabel, "output:y",
          "nbom: two entries with one label");

    auto bad = meta;
    bad.entries[1].label = "const:z";
    check_record_error([&] { (void)bad.serialize(); }, RecordErrc::BadLabel, "const:z", "nbom: writing a non-output label");
    bad.entries[1].label = "output:y";
    check_record_error([&] { bad.write(dir / "bad.nbom"); }, RecordErrc::DuplicateLabel, "output:y",
                       "nbom: writing a duplicate label (and no file)");
    check(!std::filesystem::exists(dir / "bad.nbom"), "nbom: a refused write leaves no file");
    check_record_error([&] { (void)OutputMetadata::read(dir / "missing.nbom"); }, RecordErrc::Io, "missing.nbom",
                       "nbom: reading a missing file");
}


}  // namespace

int main() {
    return run_tests("binding_formats_test.tmp", [](const std::filesystem::path& dir) {
        test_labels();
        test_runs();
        test_round_trip(dir);
        test_validation();
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        test_output_metadata(dir);
    });
}
