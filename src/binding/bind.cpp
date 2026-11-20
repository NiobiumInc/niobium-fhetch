// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

/**
 * @file bind.cpp
 * @brief Bind: joins an NBBF upload and its label index with a binding record
 *        (docs/BINDING_RECORD.md, Bind); and the result, which writes a run's
 *        outputs back as NBBF (Result).
 */
#include "niobium/binding.h"
#include "niobium/binding_record.h"
#include "niobium/output_metadata.h"

#include <nbbf/label_index.hpp>
#include <nbbf/nbbf.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <system_error>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace niobium::binding {

namespace {

// The trace consumes and produces residues in evaluation domain, bit-reversed order.
constexpr nbbf::Domain kDomain = nbbf::Domain::Evaluation;
constexpr nbbf::Order kOrder = nbbf::Order::BitReversed;

[[noreturn]] void fail_in(const char* what, BindErrc code, const std::string& label, const std::string& message) {
    throw BindError(code, label,
                    std::string(what) + ": " + (label.empty() ? std::string() : "'" + label + "': ") + message);
}

[[noreturn]] void fail(BindErrc code, const std::string& label, const std::string& message) {
    fail_in("bind", code, label, message);
}

void reject_output(Kind kind) {
    if (kind == Kind::Output) fail(BindErrc::OutputKind, {}, "kind output is never bound; results are written back");
}

void check_index(const nbbf::Result<void>& checked) {
    if (!checked)
        fail(BindErrc::LabelIndexMismatch, {},
             "the label index does not belong to this NBBF file: " + checked.error().message);
}

// The checks a block passes before it fills an address: ring dimension,
// representation, and the root of the record's modulus k.
void check_block(const BindingRecord& record, const nbbf::Layout& layout, std::size_t block, std::size_t k,
                 const std::string& label, const std::string& where) {
    const auto& entry = layout.blocks()[block];
    if (entry.ring_dim != record.ring_dim)
        fail(BindErrc::RingDimMismatch, label,
             where + " has ring_dim " + std::to_string(entry.ring_dim) + "; the record's is " +
                 std::to_string(record.ring_dim));
    if (nbbf::domain_of(entry) != kDomain || nbbf::order_of(entry) != kOrder)
        fail(BindErrc::BadRepresentation, label, where + " is not in evaluation domain, bit-reversed order");
    const auto root = layout.roots()[entry.modulus_index];
    if (root != record.roots[k])
        fail(BindErrc::RootMismatch, label,
             where + " was evaluated at root " + std::to_string(root) + "; the record's root is " +
                 std::to_string(record.roots[k]));
}

// The label index is already checked against the NBBF file.
BindResult bind_checked(const BindingRecord& record, Kind kind, const nbbf::Layout& layout,
                        const nbbf::LabelIndex& index) {
    const auto names_object_of_kind = [&](const std::string& label) {
        const auto* obj = record.find(label);
        return obj != nullptr && obj->kind == kind;
    };

    BindResult result;
    // An input upload carries exactly the record's inputs; any other label fails.
    // Key and constant uploads may carry more: those labels are reported.
    for (const auto& entry : index.entries()) {
        if (names_object_of_kind(entry.label)) continue;
        if (kind != Kind::Input) {
            result.ignored_labels.push_back(entry.label);
        } else if (kind_of_label(entry.label) != Kind::Input) {
            fail(BindErrc::WrongPrefix, entry.label, "an input upload names a label not prefixed 'input:'");
        } else {
            fail(BindErrc::UnexpectedLabel, entry.label, "the record has no input of this label");
        }
    }

    std::unordered_map<std::uint64_t, std::uint16_t> upload_index;  // prime -> the upload's modulus_index
    const auto moduli = layout.moduli();
    for (std::size_t i = 0; i < moduli.size(); ++i) upload_index.emplace(moduli[i], static_cast<std::uint16_t>(i));

    for (const auto& obj : record.objects) {
        if (obj.kind != kind) continue;
        const auto* entry = index.find(obj.label);
        if (entry == nullptr) fail(BindErrc::MissingLabel, obj.label, "the label index has no entry of this label");
        if (entry->poly_count != obj.poly_count)
            fail(BindErrc::PolyCountMismatch, obj.label,
                 "the upload has " + std::to_string(entry->poly_count) + " big polynomials; the record has " +
                     std::to_string(obj.poly_count));

        std::set<std::size_t> used;
        for (const auto& run : obj.runs) {
            const std::uint32_t poly = entry->first_poly + run.big_poly;
            for (std::size_t i = 0; i < run.limb_count; ++i) {
                const std::size_t k = run.first_modulus + i;
                const auto q = record.moduli[k];
                const auto where = "big polynomial " + std::to_string(run.big_poly) + " under modulus " + std::to_string(q);
                const auto it = upload_index.find(q);
                const auto block = it == upload_index.end() ? std::nullopt : layout.find_block(poly, it->second);
                if (!block) fail(BindErrc::MissingLimb, obj.label, where + " is missing from the upload");
                check_block(record, layout, *block, k, obj.label, where);
                result.bindings.push_back({.address = run.first_address + i, .block = *block});
                used.insert(*block);
            }
        }

        UnusedLimbs unused{.label = obj.label, .blocks = {}};
        for (std::uint32_t p = entry->first_poly; p < entry->first_poly + entry->poly_count; ++p)
            for (const auto b : layout.poly_blocks(p))
                if (!used.contains(b)) unused.blocks.push_back(b);
        if (!unused.blocks.empty()) {
            std::ranges::sort(unused.blocks);
            result.unused_limbs.push_back(std::move(unused));
        }
    }
    std::ranges::sort(result.bindings, {}, &Binding::address);
    return result;
}

}  // namespace

BindResult bind_upload(const BindingRecord& record, Kind kind, const nbbf::FileView& file, const nbbf::LabelIndex& index) {
    reject_output(kind);
    check_index(index.check_against(file));
    return bind_checked(record, kind, file.layout(), index);
}

BindResult bind_upload(const BindingRecord& record, Kind kind, const nbbf::Sha256& header_digest,
                       const nbbf::Layout& layout, const nbbf::LabelIndex& index) {
    reject_output(kind);
    check_index(index.check_against(header_digest, layout));
    return bind_checked(record, kind, layout, index);
}

std::vector<OutputObject> output_limbs(const BindingRecord& record) {
    std::vector<OutputObject> out;
    for (const auto& obj : record.objects) {
        if (obj.kind != Kind::Output) continue;
        OutputObject o{.label = obj.label, .poly_count = obj.poly_count, .limbs = {}};
        for (const auto& run : obj.runs)
            for (std::uint16_t i = 0; i < run.limb_count; ++i) {
                const auto k = static_cast<std::uint16_t>(run.first_modulus + i);
                o.limbs.push_back({.big_poly = run.big_poly,
                                   .modulus_index = k,
                                   .modulus = record.moduli.at(k),
                                   .address = run.first_address + i});
            }
        std::ranges::sort(o.limbs, {}, [](const OutputLimb& l) { return std::pair{l.big_poly, l.modulus_index}; });
        out.push_back(std::move(o));
    }
    return out;
}

void write_result(const BindingRecord& record, const OutputMetadata& metadata, const ResidueSource& residues,
                  const std::filesystem::path& dir) {
    // No file of an earlier run may outlive a failure here next to new ones.
    const std::array<const char*, 3> names{"result.nbbf", "result.nbli", "result.nbom"};
    const auto staging = dir / ".result.staging";
    const auto remove_all = [&] {
        std::error_code ec;  // best effort
        for (const auto* name : names) std::filesystem::remove(dir / name, ec);
        std::filesystem::remove_all(staging, ec);
    };
    remove_all();

    const auto objects = output_limbs(record);

    // The file declares only the primes the outputs use, in the record's order.
    std::set<std::uint16_t> used;
    for (const auto& object : objects)
        for (const auto& limb : object.limbs) used.insert(limb.modulus_index);
    std::vector<std::uint16_t> file_index(record.moduli.size(), 0);
    std::vector<std::uint64_t> moduli;
    std::vector<std::uint64_t> roots;
    for (const auto k : used) {
        file_index[k] = static_cast<std::uint16_t>(moduli.size());
        moduli.push_back(record.moduli[k]);
        roots.push_back(record.roots[k]);
    }

    nbbf::Builder builder(moduli, roots);
    std::vector<nbbf::LabelIndex::Entry> labels;
    OutputMetadata result_metadata;
    std::uint32_t first_poly = 0;
    for (const auto& object : objects) {
        const auto* entry = metadata.find(object.label);
        if (entry == nullptr)
            fail_in("result", BindErrc::MissingOutputMetadata, object.label,
                    "the output metadata has no entry of this label");
        for (const auto& limb : object.limbs) {
            auto values = residues(limb.address, limb.modulus);
            if (values.size() != record.ring_dim)
                fail_in("result", BindErrc::ResidueCountMismatch, object.label,
                     "big polynomial " + std::to_string(limb.big_poly) + " under modulus " +
                         std::to_string(limb.modulus) + " has " + std::to_string(values.size()) +
                         " residues; the record's ring_dim is " + std::to_string(record.ring_dim));
            builder.add_block(first_poly + limb.big_poly, file_index[limb.modulus_index], std::move(values), kDomain,
                              kOrder);
        }
        labels.push_back({.label = object.label, .first_poly = first_poly, .poly_count = object.poly_count});
        first_poly += object.poly_count;
        result_metadata.entries.push_back(*entry);
    }

    // Written into a staging directory beside the result, then moved in.
    try {
        std::filesystem::create_directories(staging);
        const auto plan = builder.plan().value();
        builder.write(staging / names[0]).value();
        nbbf::LabelIndex::make(nbbf::header_digest(plan), std::move(labels)).value().write(staging / names[1]).value();
        result_metadata.write(staging / names[2]);
        for (const auto* name : names) std::filesystem::rename(staging / name, dir / name);
        std::filesystem::remove(staging);
    } catch (...) {
        remove_all();
        throw;
    }
}

}  // namespace niobium::binding
