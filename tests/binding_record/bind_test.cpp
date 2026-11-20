// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

/**
 * @file bind_test.cpp
 * @brief Bind and output limbs (niobium/binding.h) on small synthetic NBBF
 *        uploads (n = 4). C++20 and nbbf: built with NIOBIUM_FHETCH_WITH_NBBF.
 *
 * Covers every failure and ignore rule of the specification's Bind section, an
 * upload whose moduli are listed in another order (and with an extra prime),
 * the header-region overload, the output limbs, and the result: what it writes
 * and each of its failures.
 */
#include "test_util.h"

#include "niobium/binding.h"
#include "niobium/binding_record.h"
#include "niobium/output_metadata.h"

#include <nbbf/label_index.hpp>
#include <nbbf/nbbf.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace binding_test;

namespace {

template <class F>
void check_bind_error(F&& f, BindErrc code, const std::string& needle, const std::string& what) {
    check_throws<BindError>(std::forward<F>(f), code, needle, what);
}


// --- Bind -------------------------------------------------------------------------

// One labelled object of an upload: the moduli (by value) of each big polynomial.
struct UpObject {
    std::string label;
    std::vector<std::vector<std::uint64_t>> polys;
};

struct UpOptions {
    std::vector<std::uint64_t> moduli = kModuli;
    std::uint32_t ring_dim = kN;
    nbbf::Domain domain = nbbf::Domain::Evaluation;
    nbbf::Order order = nbbf::Order::BitReversed;
    std::map<std::uint64_t, std::uint64_t> roots;  // overrides, by prime
    std::vector<std::pair<std::string, std::uint32_t>> extra_entries;  // label, poly_count: appended polys
};

// An NBBF file in memory and its label index.
struct Upload {
    std::vector<std::uint64_t> storage;  // 8-byte aligned
    nbbf::FileView view;
    nbbf::LabelIndex index;
    std::map<std::string, std::uint32_t> first_poly;
};

std::shared_ptr<Upload> make_upload(const std::vector<UpObject>& objects, const UpOptions& opt = {}) {
    const auto index_of = [&](std::uint64_t q) {
        return static_cast<std::uint16_t>(std::ranges::find(opt.moduli, q) - opt.moduli.begin());
    };
    std::vector<std::uint64_t> roots(opt.moduli.size(), 0);
    auto up = std::make_shared<Upload>();
    std::vector<nbbf::LabelIndex::Entry> labels;
    std::vector<std::tuple<std::uint32_t, std::uint16_t>> blocks;
    std::uint32_t poly = 0;
    for (const auto& o : objects) {
        up->first_poly[o.label] = poly;
        labels.push_back({.label = o.label, .first_poly = poly, .poly_count = static_cast<std::uint32_t>(o.polys.size())});
        for (const auto& qs : o.polys) {
            for (const auto q : qs)
                if (const auto k = index_of(q); k < opt.moduli.size()) blocks.emplace_back(poly, k);  // else left out
            ++poly;
        }
    }
    for (const auto& [label, count] : opt.extra_entries) {
        labels.push_back({.label = label, .first_poly = poly, .poly_count = count});
        for (std::uint32_t i = 0; i < count; ++i) blocks.emplace_back(poly++, 0);
    }
    if (opt.domain == nbbf::Domain::Evaluation)
        for (const auto& [p, k] : blocks) {
            const auto q = opt.moduli[k];
            roots[k] = opt.roots.contains(q) ? opt.roots.at(q) : root_of(q, opt.ring_dim);
        }
    nbbf::Builder builder(opt.moduli, roots);
    for (const auto& [p, k] : blocks) {
        std::vector<std::uint64_t> residues(opt.ring_dim);
        for (std::size_t i = 0; i < residues.size(); ++i) residues[i] = (p * 7 + k * 3 + i) % opt.moduli[k];
        builder.add_block(p, k, std::move(residues), opt.domain, opt.order);
    }
    const auto bytes = builder.serialize().value();
    up->storage.resize((bytes.size() + 7) / 8);
    std::memcpy(up->storage.data(), bytes.data(), bytes.size());
    up->view = nbbf::FileView::parse(std::as_bytes(std::span(up->storage)).first(bytes.size())).value();
    up->index = nbbf::LabelIndex::make(nbbf::header_digest(up->view), std::move(labels)).value();
    return up;
}

// What the record needs of an object: per big polynomial, the primes its runs name.
UpObject needed(const BindingRecord& rec, const std::string& label) {
    const auto* o = rec.find(label);
    UpObject up{.label = label, .polys = std::vector<std::vector<std::uint64_t>>(o->poly_count)};
    for (const auto& r : o->runs)
        for (std::uint16_t i = 0; i < r.limb_count; ++i) up.polys[r.big_poly].push_back(rec.moduli[r.first_modulus + i]);
    for (auto& qs : up.polys) std::ranges::sort(qs);
    return up;
}

std::vector<UpObject> needed_of_kind(const BindingRecord& rec, Kind kind) {
    std::vector<UpObject> out;
    for (const auto& o : rec.objects)
        if (o.kind == kind) out.push_back(needed(rec, o.label));
    return out;
}

// Every address of every object of `kind` is bound to the block of the right
// big polynomial under the right prime, and nothing else is bound.
bool binds_exactly(const BindingRecord& rec, Kind kind, const Upload& up, const BindResult& res) {
    std::map<std::uint64_t, std::pair<std::uint32_t, std::uint64_t>> want;  // address -> (poly, prime)
    for (const auto& o : rec.objects) {
        if (o.kind != kind) continue;
        for (const auto& r : o.runs)
            for (std::uint16_t i = 0; i < r.limb_count; ++i)
                want[r.first_address + i] = {up.first_poly.at(o.label) + r.big_poly, rec.moduli[r.first_modulus + i]};
    }
    if (res.bindings.size() != want.size()) return false;
    if (!std::ranges::is_sorted(res.bindings, {}, &Binding::address)) return false;
    const auto& layout = up.view.layout();
    return std::ranges::all_of(res.bindings, [&](const Binding& b) {
        const auto it = want.find(b.address);
        return it != want.end() && layout.blocks()[b.block].poly_index == it->second.first &&
               layout.modulus_of(b.block) == it->second.second;
    });
}

void test_bind() {
    std::cout << "bind\n";
    const auto rec = make_record();
    const auto inputs = needed_of_kind(rec, Kind::Input);
    const auto keys = needed_of_kind(rec, Kind::Key);
    const auto consts = needed_of_kind(rec, Kind::Constant);

    for (const auto kind : {Kind::Input, Kind::Key, Kind::Constant}) {
        const auto up = make_upload(needed_of_kind(rec, kind));
        const auto res = bind_upload(rec, kind, up->view, up->index);
        check(binds_exactly(rec, kind, *up, res) && res.ignored_labels.empty() && res.unused_limbs.empty(),
              "binds every " + std::string(kind_name(kind)) + " address to its block");
    }

    // Moduli in another order, with a prime the record does not know.
    {
        const auto up = make_upload(keys, {.moduli = {193, 241, 113, 17, 97}});
        check(binds_exactly(rec, Kind::Key, *up, bind_upload(rec, Kind::Key, up->view, up->index)),
              "moduli are matched by value, not index");
    }
    // The header-digest overload.
    {
        const auto up = make_upload(inputs);
        const auto res = bind_upload(rec, Kind::Input, nbbf::header_digest(up->view), up->view.layout(), up->index);
        check(binds_exactly(rec, Kind::Input, *up, res), "binds from the header digest and Layout alone");
    }

    // Ignored and reported.
    {
        auto more = keys;
        more.push_back({.label = "key:galois:7", .polys = {{17}}});
        const auto up = make_upload(more);
        const auto res = bind_upload(rec, Kind::Key, up->view, up->index);
        check(binds_exactly(rec, Kind::Key, *up, res) && res.ignored_labels == std::vector<std::string>{"key:galois:7"},
              "a key label the record lacks is ignored and reported");
    }
    {
        auto more = consts;
        more.push_back({.label = "const:op:3", .polys = {{17}}});
        more.push_back({.label = "input:x", .polys = {{17}, {17}}});
        const auto up = make_upload(more);
        const auto res = bind_upload(rec, Kind::Constant, up->view, up->index);
        check(binds_exactly(rec, Kind::Constant, *up, res) &&
                  res.ignored_labels == std::vector<std::string>{"const:op:3", "input:x"},
              "a constant upload's other labels are ignored and reported");
    }
    {
        auto higher = inputs;
        higher[0].polys[0].push_back(193);  // input:x big polynomial 0 at a higher level
        const auto up = make_upload(higher);
        const auto res = bind_upload(rec, Kind::Input, up->view, up->index);
        const auto extra = up->view.layout().find_block(up->first_poly.at("input:x"), 3);
        check(binds_exactly(rec, Kind::Input, *up, res) && res.unused_limbs.size() == 1 &&
                  res.unused_limbs[0].label == "input:x" && extra &&
                  res.unused_limbs[0].blocks == std::vector<std::size_t>{*extra},
              "a limb no run needs is ignored and reported");
    }

    // Failures.
    {
        const auto up = make_upload(needed_of_kind(rec, Kind::Output));
        check_bind_error([&] { (void)bind_upload(rec, Kind::Output, up->view, up->index); }, BindErrc::OutputKind, "output",
                         "kind output is never bound");
    }
    {
        const auto up = make_upload(inputs);
        const auto other = make_upload(keys);
        check_bind_error([&] { (void)bind_upload(rec, Kind::Input, up->view, other->index); }, BindErrc::LabelIndexMismatch,
                         "does not belong", "a label index of another NBBF file");
    }
    {
        const auto up = make_upload(inputs, {.ring_dim = 8});
        check_bind_error([&] { (void)bind_upload(rec, Kind::Input, up->view, up->index); }, BindErrc::RingDimMismatch,
                         "ring_dim 8", "a block of another ring dimension");
    }
    {
        const auto psi = root_of(97, kN);
        const auto up = make_upload(keys, {.roots = {{97, pow_mod(psi, 3, 97)}}});
        check_bind_error([&] { (void)bind_upload(rec, Kind::Key, up->view, up->index); }, BindErrc::RootMismatch,
                         "key:relin", "a modulus evaluated at another root");
    }
    {
        auto fewer = keys;
        std::erase_if(fewer, [](const UpObject& o) { return o.label == "key:galois:5"; });
        const auto up = make_upload(fewer);
        check_bind_error([&] { (void)bind_upload(rec, Kind::Key, up->view, up->index); }, BindErrc::MissingLabel,
                         "key:galois:5", "an object of the kind missing from the label index");
    }
    {
        auto wider = keys;
        wider[0].polys.push_back({17});
        const auto up = make_upload(wider);
        check_bind_error([&] { (void)bind_upload(rec, Kind::Key, up->view, up->index); }, BindErrc::PolyCountMismatch,
                         "key:relin", "an entry with another poly_count");
    }
    {
        auto lower = inputs;
        lower[0].polys[1].pop_back();  // input:x big polynomial 1 lacks prime 113
        const auto up = make_upload(lower);
        check_bind_error([&] { (void)bind_upload(rec, Kind::Input, up->view, up->index); }, BindErrc::MissingLimb,
                         "big polynomial 1 under modulus 113", "a limb missing from the upload");
    }
    {
        const auto up = make_upload(inputs, {.moduli = {17, 113, 193}});
        check_bind_error([&] { (void)bind_upload(rec, Kind::Input, up->view, up->index); }, BindErrc::MissingLimb,
                         "modulus 97", "a prime missing from the upload's moduli");
    }
    {
        const auto up = make_upload(inputs, {.domain = nbbf::Domain::Coefficient, .order = nbbf::Order::Natural});
        check_bind_error([&] { (void)bind_upload(rec, Kind::Input, up->view, up->index); }, BindErrc::BadRepresentation,
                         "evaluation domain", "a block in coefficient domain");
    }
    {
        const auto up = make_upload(inputs, {.order = nbbf::Order::Natural});
        check_bind_error([&] { (void)bind_upload(rec, Kind::Input, up->view, up->index); }, BindErrc::BadRepresentation,
                         "bit-reversed", "a block in natural order");
    }
    {
        auto more = inputs;
        more.push_back({.label = "input:z", .polys = {{17}}});
        const auto up = make_upload(more);
        check_bind_error([&] { (void)bind_upload(rec, Kind::Input, up->view, up->index); }, BindErrc::UnexpectedLabel,
                         "input:z", "an input label the record lacks");
    }
    {
        auto more = inputs;
        more.push_back(needed(rec, "key:relin"));
        const auto up = make_upload(more);
        check_bind_error([&] { (void)bind_upload(rec, Kind::Input, up->view, up->index); }, BindErrc::WrongPrefix,
                         "key:relin", "an input upload with a non-input label");
    }
}

// --- Output limbs -----------------------------------------------------------------

void test_outputs() {
    std::cout << "output limbs\n";
    const auto out = output_limbs(make_record());
    const std::vector<OutputLimb> want{
        {.big_poly = 0, .modulus_index = 0, .modulus = 17, .address = 700},
        {.big_poly = 0, .modulus_index = 1, .modulus = 97, .address = 701},
        {.big_poly = 1, .modulus_index = 0, .modulus = 17, .address = 710},
        {.big_poly = 1, .modulus_index = 1, .modulus = 97, .address = 711},
    };
    check(out.size() == 1 && out[0].label == "output:y" && out[0].poly_count == 2 && out[0].limbs == want,
          "every output limb, in (big_poly, modulus) order, with its prime and address");
}

// --- Result -----------------------------------------------------------------------

// What a run computed at `address`: kN residues derived from the address.
std::vector<std::uint64_t> computed_at(std::uint64_t address, std::uint64_t q) {
    std::vector<std::uint64_t> values(kN);
    for (std::size_t i = 0; i < values.size(); ++i) values[i] = (address * 5 + i) % q;
    return values;
}

void test_result(const std::filesystem::path& tmp) {
    std::cout << "result\n";
    const auto rec = make_record();
    OutputMetadata meta;
    meta.set({.label = "output:y", .scaling_factor = 0x1p40, .scale_degree = 1, .slots = 2});

    const auto dir = tmp / "result";
    write_result(rec, meta, computed_at, dir);
    const auto file = nbbf::MappedFile::open(dir / "result.nbbf").value();
    const auto index = nbbf::LabelIndex::read(dir / "result.nbli").value();
    const auto& view = file.view();
    const auto& layout = view.layout();
    const auto* entry = index.find("output:y");
    check(index.check_against(view).has_value() && index.entries().size() == 1 && entry != nullptr &&
              entry->first_poly == 0 && entry->poly_count == 2,
          "the label index belongs to the file and names output:y, big polynomials 0 and 1");
    const std::vector<std::uint64_t> moduli(layout.moduli().begin(), layout.moduli().end());
    const std::vector<std::uint64_t> roots(layout.roots().begin(), layout.roots().end());
    check(moduli == std::vector<std::uint64_t>{17, 97} && roots == std::vector<std::uint64_t>{rec.roots[0], rec.roots[1]},
          "only the primes the outputs use, with the record's roots");
    bool blocks_ok = layout.blocks().size() == 4;
    const auto outputs = output_limbs(rec);
    for (const auto& limb : outputs[0].limbs) {
        const auto block = layout.find_block(limb.big_poly, limb.modulus_index);
        if (!block) {
            blocks_ok = false;
            continue;
        }
        const auto data = view.block_data(*block);
        const auto& e = layout.blocks()[*block];
        blocks_ok = blocks_ok && std::ranges::equal(data, computed_at(limb.address, limb.modulus)) &&
                    nbbf::domain_of(e) == nbbf::Domain::Evaluation && nbbf::order_of(e) == nbbf::Order::BitReversed;
    }
    check(blocks_ok, "each limb holds what the run computed at its address, evaluation domain, bit-reversed");
    check(OutputMetadata::read(dir / "result.nbom") == meta, "the output metadata is shipped with the result");

    // Failures.
    check_bind_error([&] { write_result(rec, OutputMetadata{}, computed_at, tmp / "no_meta"); },
                     BindErrc::MissingOutputMetadata, "output:y", "an output without output metadata");
    check_bind_error(
        [&] {
            write_result(rec, meta, [](std::uint64_t a, std::uint64_t q) { return std::vector<std::uint64_t>(kN - 1, a % q); },
                         tmp / "short");
        },
        BindErrc::ResidueCountMismatch, "has 3 residues", "a limb with too few residues");
    bool out_of_range = false;
    try {
        write_result(rec, meta, [](std::uint64_t, std::uint64_t q) { return std::vector<std::uint64_t>(kN, q); },
                     tmp / "range");
    } catch (const nbbf::Exception&) {
        out_of_range = true;
    }
    check(out_of_range && !std::filesystem::exists(tmp / "range" / "result.nbbf"),
          "a residue out of range fails before any file is written");
    // A failure where an earlier run wrote its result leaves none of it either.
    bool rewrite_failed = false;
    try {
        write_result(rec, meta, [](std::uint64_t, std::uint64_t q) { return std::vector<std::uint64_t>(kN, q); }, dir);
    } catch (const nbbf::Exception&) {
        rewrite_failed = true;
    }
    check(rewrite_failed && !std::filesystem::exists(dir / "result.nbbf") && !std::filesystem::exists(dir / "result.nbli") &&
              !std::filesystem::exists(dir / "result.nbom") && !std::filesystem::exists(dir / ".result.staging"),
          "a failed result removes an earlier run's result and leaves no staging directory");
}


}  // namespace

int main() {
    return run_tests("binding_bind_test.tmp", [](const std::filesystem::path& tmp) {
        test_bind();
        test_outputs();
        test_result(tmp);
    });
}
