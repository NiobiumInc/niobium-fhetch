// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// FHETCH Polynomial IR API — client-side implementation.
//
// Each FHETCH instruction function records one operation into the trace
// via the TraceWriter. The server-side niobium-compiler parses the FHETCH
// trace and lowers it to internal hardware instructions.

#include "niobium/fhetch_api.h"
#include "niobium/compiler.h"
#include "compiler_internal.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>

namespace niobium::fhetch {

// ============================================================================
// Synthetic address allocator
// ============================================================================

static std::atomic<uintptr_t> next_address_{0};

static uintptr_t alloc_address() {
    return next_address_.fetch_add(1, std::memory_order_relaxed);
}

// ----------------------------------------------------------------------------
// Address -> modulus map. Populated as sr_* instructions are emitted; used by
// sync_fhetch_state_to_compiler() to resolve a modulus for each tagged
// single-residue input before replay().
// ----------------------------------------------------------------------------

static std::unordered_map<uintptr_t, uint64_t>& address_modulus_map() {
    static std::unordered_map<uintptr_t, uint64_t> map;
    return map;
}

static void remember_modulus(uintptr_t a, uint64_t q) {
    // Never cache the copy-sentinel modulus (0xFFFFFFFFFFFFFFFF emitted with
    // m=0 from copy-style sr_addps %d, %s, 0 instructions). It doesn't
    // describe the polynomial's actual modulus — the real modulus shows up
    // on the first non-copy op that uses the address.
    constexpr uint64_t COPY_SENTINEL = 0xFFFFFFFFFFFFFFFFULL;
    if (q == COPY_SENTINEL) return;
    auto& m = address_modulus_map();
    auto it = m.find(a);
    if (it == m.end()) m.emplace(a, q);  // first *real* write wins
}

// ============================================================================
// Helper: emit a FHETCH instruction to the trace
// ============================================================================

static void emit(const std::string& instruction) {
    niobium::detail::trace_writer().emit(instruction);
}

// Register a modulus and return "m=<index>" for the trace
static std::string midx(uint64_t q) {
    uint32_t idx = niobium::detail::trace_writer().register_modulus(q);
    return "m=" + std::to_string(idx);
}

// Format an address as %N
static std::string addr(uintptr_t a) {
    return "%" + std::to_string(a);
}

// ============================================================================
// Probe and input registries
// ============================================================================

struct ProbeEntry {
    std::string name;
    enum Kind { POLY, MRP_KIND, MRPA, SRPA } kind;
    Polynomial poly;
    MRP mrp;
    MRPArray mrpa;
    SRPArray srpa;
};

static std::vector<ProbeEntry>& probe_registry() {
    static std::vector<ProbeEntry> registry;
    return registry;
}

struct InputEntry {
    std::string name;
    enum Kind { POLY, MRP_KIND, MRPA, SRPA } kind;
    Polynomial poly;
    MRP mrp;
    MRPArray mrpa;
    SRPArray srpa;
};

static std::vector<InputEntry>& input_registry() {
    static std::vector<InputEntry> registry;
    return registry;
}

// ============================================================================
// Impl structs
// ============================================================================

struct PolynomialImpl {
    uintptr_t address;
    uint64_t ring_dim;
    Format fmt;
    NumberType ntype;
    std::vector<uint64_t> int_data;
    std::vector<double> fp_data;

    PolynomialImpl(uint64_t rd, Format f, NumberType nt)
        : address(alloc_address()), ring_dim(rd), fmt(f), ntype(nt) {}
};

struct ScalarImpl {
    uintptr_t address;
    NumberType ntype;
    uint64_t int_value;
    double fp_value;

    ScalarImpl(NumberType nt, uint64_t iv, double fv)
        : address(alloc_address()), ntype(nt), int_value(iv), fp_value(fv) {}
};

struct SRPArrayImpl {
    std::vector<Polynomial> elements;
};

struct MRPImpl {
    ModuliBase base_;
    std::map<uint64_t, Polynomial> residues_;
    uint64_t ring_dim_;

    MRPImpl() : ring_dim_(0) {}
    MRPImpl(const ModuliBase& b, uint64_t rd) : base_(b), ring_dim_(rd) {
        for (auto q : b)
            residues_[q] = Polynomial::zeros(rd);
    }
};

struct MRSImpl {
    ModuliBase base_;
    std::map<uint64_t, Scalar> scalars_;

    MRSImpl() = default;
    explicit MRSImpl(const ModuliBase& b) : base_(b) {
        for (auto q : b)
            scalars_[q] = Scalar::from_int(0);
    }
};

struct MRPArrayImpl {
    std::vector<MRP> elements;
};

// ============================================================================
// Helper: create result polynomial from a template
// ============================================================================

static Polynomial make_result(const Polynomial& from, Format fmt) {
    if (from.number_type() == NumberType::NonInteger)
        return Polynomial::zeros_ni(from.ring_dimension(), fmt);
    return Polynomial::zeros(from.ring_dimension(), fmt);
}

static Polynomial make_result(const Polynomial& from) {
    return make_result(from, from.format());
}

// ============================================================================
// Polynomial
// ============================================================================

Polynomial::Polynomial() = default;
Polynomial::~Polynomial() = default;
Polynomial::Polynomial(const Polynomial&) = default;
Polynomial& Polynomial::operator=(const Polynomial&) = default;
Polynomial::Polynomial(Polynomial&&) noexcept = default;
Polynomial& Polynomial::operator=(Polynomial&&) noexcept = default;
Polynomial::Polynomial(std::shared_ptr<PolynomialImpl> p) : impl_(std::move(p)) {}

Polynomial Polynomial::zeros(uint64_t ring_dim, Format fmt) {
    return Polynomial(std::make_shared<PolynomialImpl>(ring_dim, fmt, NumberType::Integer));
}

Polynomial Polynomial::zeros_ni(uint64_t ring_dim, Format fmt) {
    return Polynomial(std::make_shared<PolynomialImpl>(ring_dim, fmt, NumberType::NonInteger));
}

Polynomial Polynomial::from_data(const std::vector<uint64_t>& components,
                                 uint64_t ring_dim, Format fmt) {
    auto p = std::make_shared<PolynomialImpl>(ring_dim, fmt, NumberType::Integer);
    p->int_data = components;
    return Polynomial(p);
}

Polynomial Polynomial::from_data_ni(const std::vector<double>& components,
                                    uint64_t ring_dim, Format fmt) {
    auto p = std::make_shared<PolynomialImpl>(ring_dim, fmt, NumberType::NonInteger);
    p->fp_data = components;
    return Polynomial(p);
}

uint64_t Polynomial::ring_dimension() const { return impl_ ? impl_->ring_dim : 0; }
NumberType Polynomial::number_type() const { return impl_ ? impl_->ntype : NumberType::Integer; }
Format Polynomial::format() const { return impl_ ? impl_->fmt : Format::Evaluation; }
bool Polynomial::is_valid() const { return impl_ != nullptr; }
PolynomialImpl* Polynomial::impl() const { return impl_.get(); }

const std::vector<uint64_t>& Polynomial::int_data() const {
    if (!impl_)
        throw std::runtime_error("Polynomial::int_data() called on invalid polynomial");
    if (impl_->ntype != NumberType::Integer)
        throw std::runtime_error("Polynomial::int_data() requires an Integer polynomial");
    return impl_->int_data;
}

// ============================================================================
// Scalar
// ============================================================================

Scalar::Scalar() = default;
Scalar::~Scalar() = default;
Scalar::Scalar(const Scalar&) = default;
Scalar& Scalar::operator=(const Scalar&) = default;
Scalar::Scalar(Scalar&&) noexcept = default;
Scalar& Scalar::operator=(Scalar&&) noexcept = default;
Scalar::Scalar(std::shared_ptr<ScalarImpl> p) : impl_(std::move(p)) {}

Scalar Scalar::from_int(uint64_t value) {
    return Scalar(std::make_shared<ScalarImpl>(NumberType::Integer, value, 0.0));
}

Scalar Scalar::from_double(double value) {
    return Scalar(std::make_shared<ScalarImpl>(NumberType::NonInteger, 0, value));
}

NumberType Scalar::number_type() const { return impl_ ? impl_->ntype : NumberType::Integer; }
bool Scalar::is_valid() const { return impl_ != nullptr; }
ScalarImpl* Scalar::impl() const { return impl_.get(); }

// ============================================================================
// SRPArray
// ============================================================================

SRPArray::SRPArray() : impl_(std::make_shared<SRPArrayImpl>()) {}
SRPArray::~SRPArray() = default;
SRPArray::SRPArray(const SRPArray&) = default;
SRPArray& SRPArray::operator=(const SRPArray&) = default;
SRPArray::SRPArray(SRPArray&&) noexcept = default;
SRPArray& SRPArray::operator=(SRPArray&&) noexcept = default;
SRPArray::SRPArray(std::shared_ptr<SRPArrayImpl> p) : impl_(std::move(p)) {}

SRPArray::SRPArray(size_t n) : impl_(std::make_shared<SRPArrayImpl>()) {
    impl_->elements.resize(n);
}

SRPArray::SRPArray(std::initializer_list<Polynomial> polys)
    : impl_(std::make_shared<SRPArrayImpl>()) {
    impl_->elements.assign(polys.begin(), polys.end());
}

size_t SRPArray::length() const { return impl_ ? impl_->elements.size() : 0; }

Polynomial& SRPArray::operator[](size_t i) {
    if (!impl_ || i >= impl_->elements.size())
        throw std::out_of_range("SRPArray index out of range");
    return impl_->elements[i];
}

const Polynomial& SRPArray::operator[](size_t i) const {
    if (!impl_ || i >= impl_->elements.size())
        throw std::out_of_range("SRPArray index out of range");
    return impl_->elements[i];
}

void SRPArray::append(const Polynomial& p) {
    if (impl_) impl_->elements.push_back(p);
}

bool SRPArray::is_valid() const { return impl_ != nullptr; }
SRPArrayImpl* SRPArray::impl() const { return impl_.get(); }

// ============================================================================
// MRP
// ============================================================================

MRP::MRP() : impl_(std::make_shared<MRPImpl>()) {}
MRP::~MRP() = default;
MRP::MRP(const MRP&) = default;
MRP& MRP::operator=(const MRP&) = default;
MRP::MRP(MRP&&) noexcept = default;
MRP& MRP::operator=(MRP&&) noexcept = default;
MRP::MRP(std::shared_ptr<MRPImpl> p) : impl_(std::move(p)) {}

MRP::MRP(const ModuliBase& base, uint64_t ring_dim)
    : impl_(std::make_shared<MRPImpl>(base, ring_dim)) {}

MRP MRP::from_pairs(const std::vector<std::pair<Polynomial, uint64_t>>& pairs) {
    auto impl = std::make_shared<MRPImpl>();
    for (const auto& [poly, q] : pairs) {
        impl->base_.push_back(q);
        impl->residues_[q] = poly;
    }
    if (!pairs.empty() && pairs[0].first.is_valid())
        impl->ring_dim_ = pairs[0].first.ring_dimension();
    return MRP(impl);
}

const ModuliBase& MRP::base() const {
    static const ModuliBase empty;
    return impl_ ? impl_->base_ : empty;
}

size_t MRP::num_residues() const { return impl_ ? impl_->base_.size() : 0; }

Polynomial& MRP::operator[](uint64_t q) {
    if (!impl_) throw std::out_of_range("MRP is empty");
    auto it = impl_->residues_.find(q);
    if (it == impl_->residues_.end())
        throw std::out_of_range("Modulus not in MRP base");
    return it->second;
}

const Polynomial& MRP::operator[](uint64_t q) const {
    if (!impl_) throw std::out_of_range("MRP is empty");
    auto it = impl_->residues_.find(q);
    if (it == impl_->residues_.end())
        throw std::out_of_range("Modulus not in MRP base");
    return it->second;
}

bool MRP::is_valid() const { return impl_ != nullptr && !impl_->base_.empty(); }
MRPImpl* MRP::impl() const { return impl_.get(); }

// ============================================================================
// MRS
// ============================================================================

MRS::MRS() : impl_(std::make_shared<MRSImpl>()) {}
MRS::~MRS() = default;
MRS::MRS(const MRS&) = default;
MRS& MRS::operator=(const MRS&) = default;
MRS::MRS(MRS&&) noexcept = default;
MRS& MRS::operator=(MRS&&) noexcept = default;
MRS::MRS(std::shared_ptr<MRSImpl> p) : impl_(std::move(p)) {}

MRS::MRS(const ModuliBase& base) : impl_(std::make_shared<MRSImpl>(base)) {}

MRS MRS::from_pairs(const std::vector<std::pair<Scalar, uint64_t>>& pairs) {
    auto impl = std::make_shared<MRSImpl>();
    for (const auto& [s, q] : pairs) {
        impl->base_.push_back(q);
        impl->scalars_[q] = s;
    }
    return MRS(impl);
}

const ModuliBase& MRS::base() const {
    static const ModuliBase empty;
    return impl_ ? impl_->base_ : empty;
}

size_t MRS::num_residues() const { return impl_ ? impl_->base_.size() : 0; }

Scalar& MRS::operator[](uint64_t q) {
    if (!impl_) throw std::out_of_range("MRS is empty");
    auto it = impl_->scalars_.find(q);
    if (it == impl_->scalars_.end())
        throw std::out_of_range("Modulus not in MRS base");
    return it->second;
}

const Scalar& MRS::operator[](uint64_t q) const {
    if (!impl_) throw std::out_of_range("MRS is empty");
    auto it = impl_->scalars_.find(q);
    if (it == impl_->scalars_.end())
        throw std::out_of_range("Modulus not in MRS base");
    return it->second;
}

bool MRS::is_valid() const { return impl_ != nullptr && !impl_->base_.empty(); }
MRSImpl* MRS::impl() const { return impl_.get(); }

// ============================================================================
// MRPArray
// ============================================================================

MRPArray::MRPArray() : impl_(std::make_shared<MRPArrayImpl>()) {}
MRPArray::~MRPArray() = default;
MRPArray::MRPArray(const MRPArray&) = default;
MRPArray& MRPArray::operator=(const MRPArray&) = default;
MRPArray::MRPArray(MRPArray&&) noexcept = default;
MRPArray& MRPArray::operator=(MRPArray&&) noexcept = default;
MRPArray::MRPArray(std::shared_ptr<MRPArrayImpl> p) : impl_(std::move(p)) {}

MRPArray::MRPArray(size_t n) : impl_(std::make_shared<MRPArrayImpl>()) {
    impl_->elements.resize(n);
}

MRPArray::MRPArray(std::initializer_list<MRP> mrps)
    : impl_(std::make_shared<MRPArrayImpl>()) {
    impl_->elements.assign(mrps.begin(), mrps.end());
}

size_t MRPArray::length() const { return impl_ ? impl_->elements.size() : 0; }

MRP& MRPArray::operator[](size_t i) {
    if (!impl_ || i >= impl_->elements.size())
        throw std::out_of_range("MRPArray index out of range");
    return impl_->elements[i];
}

const MRP& MRPArray::operator[](size_t i) const {
    if (!impl_ || i >= impl_->elements.size())
        throw std::out_of_range("MRPArray index out of range");
    return impl_->elements[i];
}

void MRPArray::append(const MRP& m) {
    if (impl_) impl_->elements.push_back(m);
}

bool MRPArray::is_valid() const { return impl_ != nullptr; }
MRPArrayImpl* MRPArray::impl() const { return impl_.get(); }

// ============================================================================
// Epoch reset
// ============================================================================

void reset_for_epoch() {
    next_address_.store(0, std::memory_order_relaxed);
    input_registry().clear();
    probe_registry().clear();
}

// ============================================================================
// INPUT / OUTPUT TAGGING
// ============================================================================

void tag_input(const std::string& name, const Polynomial& p) {
    InputEntry e; e.name = name; e.kind = InputEntry::POLY; e.poly = p;
    input_registry().push_back(std::move(e));
}

void tag_input(const std::string& name, const MRP& m) {
    InputEntry e; e.name = name; e.kind = InputEntry::MRP_KIND; e.mrp = m;
    input_registry().push_back(std::move(e));
}

void tag_input(const std::string& name, const MRPArray& arr) {
    InputEntry e; e.name = name; e.kind = InputEntry::MRPA; e.mrpa = arr;
    input_registry().push_back(std::move(e));
}

void tag_input(const std::string& name, const SRPArray& arr) {
    InputEntry e; e.name = name; e.kind = InputEntry::SRPA; e.srpa = arr;
    input_registry().push_back(std::move(e));
}

void tag_output(const std::string& name, const Polynomial& p) {
    emit("# output " + name + " " + addr(p.impl()->address));
    ProbeEntry e; e.name = name; e.kind = ProbeEntry::POLY; e.poly = p;
    probe_registry().push_back(std::move(e));
}

void tag_output(const std::string& name, const MRP& m) {
    for (auto q : m.base())
        emit("# output " + name + " " + addr(m[q].impl()->address));
    ProbeEntry e; e.name = name; e.kind = ProbeEntry::MRP_KIND; e.mrp = m;
    probe_registry().push_back(std::move(e));
}

void tag_output(const std::string& name, const MRPArray& arr) {
    ProbeEntry e; e.name = name; e.kind = ProbeEntry::MRPA; e.mrpa = arr;
    probe_registry().push_back(std::move(e));
}

void tag_output(const std::string& name, const SRPArray& arr) {
    ProbeEntry e; e.name = name; e.kind = ProbeEntry::SRPA; e.srpa = arr;
    probe_registry().push_back(std::move(e));
}

uint64_t get_input_ring_dimension() {
    for (const auto& input : input_registry()) {
        switch (input.kind) {
        case InputEntry::POLY:
            if (input.poly.is_valid() && input.poly.ring_dimension() > 0)
                return input.poly.ring_dimension();
            break;
        case InputEntry::MRP_KIND:
            if (input.mrp.is_valid() && !input.mrp.base().empty()) {
                const auto& p = input.mrp[input.mrp.base()[0]];
                if (p.is_valid() && p.ring_dimension() > 0)
                    return p.ring_dimension();
            }
            break;
        case InputEntry::MRPA:
            if (input.mrpa.length() > 0 && input.mrpa[0].is_valid()) {
                const auto& b = input.mrpa[0].base();
                if (!b.empty()) return input.mrpa[0][b[0]].ring_dimension();
            }
            break;
        case InputEntry::SRPA:
            if (input.srpa.length() > 0 && input.srpa[0].is_valid())
                return input.srpa[0].ring_dimension();
            break;
        }
    }
    return 0;
}

void save_input_data() {
    // TODO: Serialize input polynomial data to JSON for server consumption
    auto& registry = input_registry();
    if (registry.empty()) return;
    std::cout << "[FHETCH] " << registry.size() << " inputs registered" << std::endl;
}

void save_probe_outputs() {
    // TODO: Serialize output probe metadata to JSON for server consumption
    auto& registry = probe_registry();
    if (registry.empty()) return;
    std::cout << "[FHETCH] " << registry.size() << " outputs registered" << std::endl;
}

// ============================================================================
// BASELINE INSTRUCTIONS
// ============================================================================

Polynomial sr_addp(const Polynomial& a, const Polynomial& b, uint64_t q) {
    auto result = make_result(a);
    remember_modulus(a.impl()->address, q);
    remember_modulus(b.impl()->address, q);
    remember_modulus(result.impl()->address, q);
    emit("sr_addp " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address) + ", " + addr(b.impl()->address) +
         ", " + midx(q));
    return result;
}

Polynomial sr_addps(const Polynomial& a, const Scalar& s, uint64_t q) {
    auto result = make_result(a);
    remember_modulus(a.impl()->address, q);
    remember_modulus(result.impl()->address, q);
    emit("sr_addps " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address) + ", " +
         std::to_string(s.impl()->int_value) + ", " + midx(q));
    return result;
}

Polynomial sr_addps_coeff(const Polynomial& a, const Scalar& s, uint64_t q) {
    auto result = make_result(a, Format::Coefficient);
    remember_modulus(a.impl()->address, q);
    remember_modulus(result.impl()->address, q);
    emit("sr_addps_coeff " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address) + ", " +
         std::to_string(s.impl()->int_value) + ", " + midx(q));
    return result;
}

Polynomial sr_negp(const Polynomial& a, uint64_t q) {
    auto result = make_result(a);
    remember_modulus(a.impl()->address, q);
    remember_modulus(result.impl()->address, q);
    emit("sr_negp " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address) + ", " + midx(q));
    return result;
}

Polynomial sr_subp(const Polynomial& a, const Polynomial& b, uint64_t q) {
    auto result = make_result(a);
    remember_modulus(a.impl()->address, q);
    remember_modulus(b.impl()->address, q);
    remember_modulus(result.impl()->address, q);
    emit("sr_subp " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address) + ", " + addr(b.impl()->address) +
         ", " + midx(q));
    return result;
}

Polynomial sr_subps(const Polynomial& a, const Scalar& s, uint64_t q) {
    auto result = make_result(a);
    remember_modulus(a.impl()->address, q);
    remember_modulus(result.impl()->address, q);
    emit("sr_subps " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address) + ", " +
         std::to_string(s.impl()->int_value) + ", " + midx(q));
    return result;
}

Polynomial sr_subps_coeff(const Polynomial& a, const Scalar& s, uint64_t q) {
    auto result = make_result(a, Format::Coefficient);
    remember_modulus(a.impl()->address, q);
    remember_modulus(result.impl()->address, q);
    emit("sr_subps_coeff " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address) + ", " +
         std::to_string(s.impl()->int_value) + ", " + midx(q));
    return result;
}

Polynomial sr_mulp(const Polynomial& a, const Polynomial& b, uint64_t q) {
    auto result = make_result(a);
    remember_modulus(a.impl()->address, q);
    remember_modulus(b.impl()->address, q);
    remember_modulus(result.impl()->address, q);
    emit("sr_mulp " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address) + ", " + addr(b.impl()->address) +
         ", " + midx(q));
    return result;
}

Polynomial sr_mulps(const Polynomial& a, const Scalar& s, uint64_t q) {
    auto result = make_result(a);
    remember_modulus(a.impl()->address, q);
    remember_modulus(result.impl()->address, q);
    emit("sr_mulps " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address) + ", " +
         std::to_string(s.impl()->int_value) + ", " + midx(q));
    return result;
}

Polynomial sr_ntt(const Polynomial& a, uint64_t q) {
    auto result = make_result(a, Format::Evaluation);
    remember_modulus(a.impl()->address, q);
    remember_modulus(result.impl()->address, q);
    emit("sr_ntt " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address) + ", " + midx(q));
    return result;
}

Polynomial sr_intt(const Polynomial& a, uint64_t q) {
    auto result = make_result(a, Format::Coefficient);
    remember_modulus(a.impl()->address, q);
    remember_modulus(result.impl()->address, q);
    emit("sr_intt " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address) + ", " + midx(q));
    return result;
}

Polynomial sr_permute(const Polynomial& a,
                      const std::vector<uint64_t>& /*srcs*/,
                      const std::vector<int>& /*signs*/,
                      uint64_t q) {
    auto result = make_result(a);
    remember_modulus(a.impl()->address, q);
    remember_modulus(result.impl()->address, q);
    emit("sr_permute " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address) + ", " + midx(q));
    return result;
}

void halt() {
    emit("halt");
}

// ============================================================================
// OPTIONAL OPERATIONS — non-integer arithmetic
// ============================================================================

Polynomial sr_addp_ni(const Polynomial& a, const Polynomial& b) {
    auto result = make_result(a);
    emit("sr_addp_ni " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address) + ", " + addr(b.impl()->address));
    return result;
}

Polynomial sr_addps_ni(const Polynomial& a, const Scalar& s) {
    auto result = make_result(a);
    emit("sr_addps_ni " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address) + ", " + std::to_string(s.impl()->fp_value));
    return result;
}

Polynomial sr_addps_coeff_ni(const Polynomial& a, const Scalar& s) {
    auto result = make_result(a, Format::Coefficient);
    emit("sr_addps_coeff_ni " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address) + ", " + std::to_string(s.impl()->fp_value));
    return result;
}

Polynomial sr_negp_ni(const Polynomial& a) {
    auto result = make_result(a);
    emit("sr_negp_ni " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address));
    return result;
}

Polynomial sr_subp_ni(const Polynomial& a, const Polynomial& b) {
    auto result = make_result(a);
    emit("sr_subp_ni " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address) + ", " + addr(b.impl()->address));
    return result;
}

Polynomial sr_subps_ni(const Polynomial& a, const Scalar& s) {
    auto result = make_result(a);
    emit("sr_subps_ni " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address) + ", " + std::to_string(s.impl()->fp_value));
    return result;
}

Polynomial sr_subps_coeff_ni(const Polynomial& a, const Scalar& s) {
    auto result = make_result(a, Format::Coefficient);
    emit("sr_subps_coeff_ni " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address) + ", " + std::to_string(s.impl()->fp_value));
    return result;
}

Polynomial sr_mulp_ni(const Polynomial& a, const Polynomial& b) {
    auto result = make_result(a);
    emit("sr_mulp_ni " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address) + ", " + addr(b.impl()->address));
    return result;
}

Polynomial sr_mulps_ni(const Polynomial& a, const Scalar& s) {
    auto result = make_result(a);
    emit("sr_mulps_ni " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address) + ", " + std::to_string(s.impl()->fp_value));
    return result;
}

// ============================================================================
// OPTIONAL OPERATIONS — Fourier transforms
// ============================================================================

Polynomial sr_ft(const Polynomial& a) {
    auto result = make_result(a, Format::Evaluation);
    emit("sr_ft " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address));
    return result;
}

Polynomial sr_ift(const Polynomial& a) {
    auto result = make_result(a, Format::Coefficient);
    emit("sr_ift " + addr(result.impl()->address) + ", " +
         addr(a.impl()->address));
    return result;
}

// ============================================================================
// OPTIONAL OPERATIONS — coefficient access
// ============================================================================

Scalar sr_coeff_extract(const Polynomial& /*p*/, uint64_t /*i*/) {
    return Scalar::from_int(0);  // TODO
}

Polynomial sr_coeff_assign(const Polynomial& p, uint64_t /*i*/, const Scalar& /*val*/) {
    return make_result(p);  // TODO
}

// ============================================================================
// OPTIONAL OPERATIONS — torus and sample operations
// ============================================================================

Polynomial sr_torus_mod_reduce(const Polynomial& p, double /*c*/) {
    return make_result(p);  // TODO
}

std::vector<uint64_t> sr_sample_extract(const SRPArray& /*rlwe*/, uint64_t lwe_dim) {
    return std::vector<uint64_t>(lwe_dim + 1, 0);  // TODO
}

// ============================================================================
// GADGETS — Polynomial level
// ============================================================================

Polynomial sr_automorph_eval(const Polynomial& x, uint64_t k) {
    auto result = make_result(x);
    emit("sr_automorph_eval " + addr(result.impl()->address) + ", " +
         addr(x.impl()->address) + ", k=" + std::to_string(k));
    return result;
}

Polynomial sr_automorph_coeff(const Polynomial& x, uint64_t k, uint64_t q) {
    auto result = make_result(x, Format::Coefficient);
    emit("sr_automorph_coeff " + addr(result.impl()->address) + ", " +
         addr(x.impl()->address) + ", k=" + std::to_string(k) +
         ", " + midx(q));
    return result;
}

Polynomial sr_rot_automorph_coeff(const Polynomial& x, uint64_t offset, uint64_t q) {
    auto result = make_result(x, Format::Coefficient);
    emit("sr_rot_automorph_coeff " + addr(result.impl()->address) + ", " +
         addr(x.impl()->address) + ", offset=" + std::to_string(offset) +
         ", " + midx(q));
    return result;
}

SRPArray sr_batch_ft(const SRPArray& x) {
    SRPArray y(x.length());
    for (size_t i = 0; i < x.length(); ++i)
        y[i] = sr_ft(x[i]);
    return y;
}

SRPArray sr_batch_ift(const SRPArray& x) {
    SRPArray y(x.length());
    for (size_t i = 0; i < x.length(); ++i)
        y[i] = sr_ift(x[i]);
    return y;
}

// ============================================================================
// GADGETS — Multi-Residue basic arithmetic
// ============================================================================

MRP mr_addp(const MRP& x, const MRP& y) {
    const auto& base = x.base();
    MRP z(base, x[base[0]].ring_dimension());
    for (auto q : base) z[q] = sr_addp(x[q], y[q], q);
    return z;
}

MRP mr_subp(const MRP& x, const MRP& y) {
    const auto& base = x.base();
    MRP z(base, x[base[0]].ring_dimension());
    for (auto q : base) z[q] = sr_subp(x[q], y[q], q);
    return z;
}

MRP mr_mulp(const MRP& x, const MRP& y) {
    const auto& base = x.base();
    MRP z(base, x[base[0]].ring_dimension());
    for (auto q : base) z[q] = sr_mulp(x[q], y[q], q);
    return z;
}

MRP mr_mulps(const MRP& x, const MRS& s) {
    const auto& base = x.base();
    MRP z(base, x[base[0]].ring_dimension());
    for (auto q : base) z[q] = sr_mulps(x[q], s[q], q);
    return z;
}

MRP mr_addps(const MRP& x, const MRS& s) {
    const auto& base = x.base();
    MRP z(base, x[base[0]].ring_dimension());
    for (auto q : base) z[q] = sr_addps(x[q], s[q], q);
    return z;
}

MRP mr_subps(const MRP& x, const MRS& s) {
    const auto& base = x.base();
    MRP z(base, x[base[0]].ring_dimension());
    for (auto q : base) z[q] = sr_subps(x[q], s[q], q);
    return z;
}

MRP mr_ntt(const MRP& x) {
    const auto& base = x.base();
    MRP z(base, x[base[0]].ring_dimension());
    for (auto q : base) z[q] = sr_ntt(x[q], q);
    return z;
}

MRP mr_intt(const MRP& x) {
    const auto& base = x.base();
    MRP z(base, x[base[0]].ring_dimension());
    for (auto q : base) z[q] = sr_intt(x[q], q);
    return z;
}

MRP mr_automorph_eval(const MRP& x, uint64_t k) {
    const auto& base = x.base();
    MRP z(base, x[base[0]].ring_dimension());
    for (auto q : base) z[q] = sr_automorph_eval(x[q], k);
    return z;
}

MRP mr_rot_automorph_coeff(const MRP& x, uint64_t offset) {
    const auto& base = x.base();
    MRP z(base, x[base[0]].ring_dimension());
    for (auto q : base) z[q] = sr_rot_automorph_coeff(x[q], offset, q);
    return z;
}

MRP mr_zeros(const ModuliBase& target_base, uint64_t ring_dim) {
    return MRP(target_base, ring_dim);
}

// ============================================================================
// GADGETS — MRP residue manipulation
// ============================================================================

MRP mr_append_srp(const MRP& x, const Polynomial& a, uint64_t q_a) {
    ModuliBase new_base = x.base();
    new_base.push_back(q_a);
    auto impl = std::make_shared<MRPImpl>();
    impl->base_ = new_base;
    impl->ring_dim_ = a.ring_dimension();
    for (auto q : x.base()) impl->residues_[q] = x[q];
    impl->residues_[q_a] = a;
    return MRP(impl);
}

MRP mr_union(const MRP& x, const MRP& y) {
    ModuliBase new_base = x.base();
    for (auto q : y.base()) new_base.push_back(q);
    auto impl = std::make_shared<MRPImpl>();
    impl->base_ = new_base;
    impl->ring_dim_ = x.impl()->ring_dim_;
    for (auto q : x.base()) impl->residues_[q] = x[q];
    for (auto q : y.base()) impl->residues_[q] = y[q];
    return MRP(impl);
}

MRP mr_subset(const MRP& x, const ModuliBase& subbase) {
    auto impl = std::make_shared<MRPImpl>();
    impl->base_ = subbase;
    impl->ring_dim_ = x.impl()->ring_dim_;
    for (auto q : subbase) impl->residues_[q] = x[q];
    return MRP(impl);
}

// ============================================================================
// GADGETS — Fast Base Conversion and CKKS Rescale
// ============================================================================
//
// Ported from the canonical niobium-compiler implementation
// (niobium-compiler/src/FhetchApi.cpp). The original libnbfhetch stub here
// emitted `sr_mulps(x[q], 1, p)` + `sr_addp` per (q, p) pair — a placeholder
// that produced sum_q(x[q] mod p) instead of the CRT-correct
// sum_i((x[q_i] * q_hat[i]) mod q_i) * q_star[i,p] mod p, and lacked
// pass-through for shared primes. Two adaptations from the canonical
// generator-based source:
//   1. `mod_arith::*` helpers don't ship with libnbfhetch; inlined below.
//   2. The simulator (fhetch_sim/simulator.cpp) has no SR_SWITCHMODULUS
//      opcode, so center_mod_q_into_p is lowered into 3 sr_* ops
//      (shift/rebase/unshift) using the available primitives.

namespace {

// Modular multiplication via __uint128_t — required because primes are ~60 bits.
inline uint64_t mulmod_u64(uint64_t a, uint64_t b, uint64_t m) {
    return static_cast<uint64_t>((static_cast<__uint128_t>(a) * b) % m);
}

// Modular exponentiation. Used by modinv_prime for Fermat's little theorem.
inline uint64_t powmod_u64(uint64_t a, uint64_t e, uint64_t m) {
    uint64_t r = 1U % m;
    a %= m;
    while (e > 0U) {
        if ((e & 1U) != 0U) r = mulmod_u64(r, a, m);
        a = mulmod_u64(a, a, m);
        e >>= 1U;
    }
    return r;
}

// Modular inverse of a mod prime p, via Fermat: a^(p-2) mod p. Caller
// guarantees p is prime and gcd(a, p) == 1.
inline uint64_t modinv_prime(uint64_t a, uint64_t p) {
    return powmod_u64(a % p, p - 2U, p);
}

// Product of every prime in `base` reduced mod `m`. Replaces
// niobium-compiler's mod_arith::prod_mod.
inline uint64_t prod_mod(const ModuliBase& base, uint64_t m) {
    uint64_t r = 1U % m;
    for (uint64_t q : base) r = mulmod_u64(r, q % m, m);
    return r;
}

// Product of every prime in `base` except `base[skip]`, reduced mod `m`.
// Replaces niobium-compiler's mod_arith::prod_mod_skip_index.
inline uint64_t prod_mod_skip_index(const ModuliBase& base, size_t skip, uint64_t m) {
    uint64_t r = 1U % m;
    for (size_t i = 0; i < base.size(); ++i) {
        if (i == skip) continue;
        r = mulmod_u64(r, base[i] % m, m);
    }
    return r;
}

// q_hat[i] = (Q/q_i)^{-1} mod q_i, where Q = prod(source_base).
std::vector<uint64_t> q_hat_inv_mod_q(const ModuliBase& source_base) {
    std::vector<uint64_t> q_hat(source_base.size());
    for (size_t i = 0; i < source_base.size(); ++i) {
        const uint64_t q_i = source_base[i];
        const uint64_t Q_div_qi_mod_qi = prod_mod_skip_index(source_base, i, q_i);
        q_hat[i] = modinv_prime(Q_div_qi_mod_qi, q_i);
    }
    return q_hat;
}

// q_star[i] = (Q/q_i) mod target_p, one entry per source prime.
std::vector<uint64_t> q_hat_mod_p(const ModuliBase& source_base, uint64_t target_p) {
    std::vector<uint64_t> q_star(source_base.size());
    for (size_t i = 0; i < source_base.size(); ++i) {
        q_star[i] = prod_mod_skip_index(source_base, i, target_p);
    }
    return q_star;
}

// scaled[i] = (x[q_i] * q_hat[i]) mod q_i. Emits one sr_mulps per source prime.
std::vector<Polynomial> prescale_by_q_hat(const MRP& x, const std::vector<uint64_t>& q_hat) {
    const ModuliBase& source_base = x.base();
    std::vector<Polynomial> scaled;
    scaled.reserve(source_base.size());
    for (size_t i = 0; i < source_base.size(); ++i) {
        scaled.push_back(sr_mulps(x[source_base[i]], Scalar::from_int(q_hat[i]), source_base[i]));
    }
    return scaled;
}

// Center scaled_i mod q_i (signed-preserving) and rebase to mod target_p.
// niobium-compiler's generator emits a single `switchmodulus` IR op that the
// hardware/optimizer lowers to four scalar instructions; libnbfhetch's
// simulator has no such opcode, so we lower to 3 available sr_* ops directly:
//   shifted   = (scaled + halfQ_i)   mod q_i      (sr_addps)
//   rebased   = shifted              mod target_p (sr_mulps with imm=1)
//   unshifted = (rebased + (-halfQ_i mod p)) mod p (sr_addps with additive inverse)
// For scaled in [0, halfQ]:    unshifted = scaled mod p              (centered ≥ 0)
// For scaled in (halfQ, q_i):  unshifted = (scaled - q_i) mod p      (centered < 0)
Polynomial center_mod_q_into_p(const Polynomial& scaled_i, uint64_t q_i, uint64_t target_p) {
    const uint64_t halfQ = (q_i - 1U) / 2U;
    auto shifted = sr_addps(scaled_i, Scalar::from_int(halfQ), q_i);
    auto rebased = sr_mulps(shifted, Scalar::from_int(1), target_p);
    const uint64_t neg_half_mod_p = (target_p - (halfQ % target_p)) % target_p;
    return sr_addps(rebased, Scalar::from_int(neg_half_mod_p), target_p);
}

// One term of the FBC sum at target prime p:
//   Standard:     (scaled[i] mod p) * q_star[i] mod p
//   ReducedNoise: (center_qi(scaled[i]) mod p) * q_star[i] mod p
Polynomial fbc_term(const Polynomial& scaled_i, uint64_t q_i, uint64_t target_p,
                    uint64_t q_star_i, FbcVariant variant) {
    Polynomial coeff = scaled_i;
    if (variant == FbcVariant::ReducedNoise) {
        coeff = center_mod_q_into_p(scaled_i, q_i, target_p);
    }
    return sr_mulps(coeff, Scalar::from_int(q_star_i), target_p);
}

// y[p] = sum_i fbc_term_i (mod p). Seeds with term 0 so the first write is a
// real instruction (avoids leaving an add-against-zero seed for DCE to elide).
Polynomial sum_fbc_terms(const std::vector<Polynomial>& scaled, const ModuliBase& source_base,
                         uint64_t target_p, const std::vector<uint64_t>& q_star,
                         FbcVariant variant) {
    Polynomial acc = fbc_term(scaled[0], source_base[0], target_p, q_star[0], variant);
    for (size_t i = 1; i < source_base.size(); ++i) {
        Polynomial term = fbc_term(scaled[i], source_base[i], target_p, q_star[i], variant);
        acc = sr_addp(acc, term, target_p);
    }
    return acc;
}

// Pass-through copy via sr_addps with imm=0 and the COPY_SENTINEL modulus
// (0xFFFF...FF). remember_modulus already special-cases this sentinel so it
// doesn't pollute the address-modulus map; the simulator's exec_addps with
// imm=0 does a vector copy regardless of q.
Polynomial copy_residue(const Polynomial& src) {
    return sr_addps(src, Scalar::from_int(0), /*q=*/0xFFFFFFFFFFFFFFFFULL);
}

// target_base = source_base \ rescale_base, preserving source_base order.
ModuliBase target_after_rescale(const ModuliBase& source_base, const ModuliBase& rescale_base) {
    const std::unordered_set<uint64_t> rescale_set(rescale_base.begin(), rescale_base.end());
    ModuliBase target_base;
    target_base.reserve(source_base.size());
    for (uint64_t q : source_base) {
        if (rescale_set.find(q) == rescale_set.end()) target_base.push_back(q);
    }
    return target_base;
}

// P_inv[i] = (prod(rescale_base))^{-1} mod target_base[i].
std::vector<uint64_t> P_inv_mod_q(const ModuliBase& rescale_base, const ModuliBase& target_base) {
    std::vector<uint64_t> P_inv(target_base.size());
    for (size_t i = 0; i < target_base.size(); ++i) {
        const uint64_t q = target_base[i];
        P_inv[i] = modinv_prime(prod_mod(rescale_base, q), q);
    }
    return P_inv;
}

// z[q] = (x[q] - y[q]) * P_inv (mod q).
Polynomial rescale_residue(const Polynomial& x_q, const Polynomial& y_q, uint64_t q,
                           uint64_t P_inv_q) {
    Polynomial diff = sr_subp(x_q, y_q, q);
    return sr_mulps(diff, Scalar::from_int(P_inv_q), q);
}

}  // namespace

// Approximate fast base conversion. Math:
//   q_hat   = (Q/q_i)^{-1} mod q_i              for each source prime q_i
//   scaled  = x[q_i] * q_hat[i]   (mod q_i)     for each i
//   for each target prime p:
//     if p in source_base:  y[p] = copy(x[p])              (pass-through)
//     else:                 y[p] = sum_i fbc_term_i(p)     (mod p)
//
// Source primes that overlap the target are passed through unchanged. The FBC
// sum at those primes would otherwise mix x[p] with noise from j != p,
// breaking dig_decomp's invariant that the lifted value equals the original
// at the digit's own primes.
//
// Reference: Cheon-Han-Kim-Kim-Song SAC 2018 §4.1 (BasisExtension).
MRP fast_base_convert(const MRP& x, const ModuliBase& target_base, FbcVariant variant) {
    const ModuliBase& source_base = x.base();
    std::vector<std::pair<Polynomial, uint64_t>> pairs;
    pairs.reserve(target_base.size());

    if (source_base.empty()) {
        for (uint64_t p : target_base) {
            pairs.emplace_back(Polynomial::zeros(x.impl()->ring_dim_), p);
        }
        return MRP::from_pairs(pairs);
    }

    const auto q_hat = q_hat_inv_mod_q(source_base);
    const auto scaled = prescale_by_q_hat(x, q_hat);

    const std::unordered_set<uint64_t> source_set(source_base.begin(), source_base.end());
    for (uint64_t p : target_base) {
        if (source_set.find(p) != source_set.end()) {
            pairs.emplace_back(copy_residue(x[p]), p);
            continue;
        }
        const auto q_star = q_hat_mod_p(source_base, p);
        pairs.emplace_back(sum_fbc_terms(scaled, source_base, p, q_star, variant), p);
    }
    return MRP::from_pairs(pairs);
}

// 2-arg overload defaults to ReducedNoise — the variant that matches the
// niobium-instrumented OpenFHE built with WITH_REDUCED_NOISE=ON.
MRP fast_base_convert(const MRP& x, const ModuliBase& target_base) {
    return fast_base_convert(x, target_base, FbcVariant::ReducedNoise);
}

// Approximate mod-down (CKKS rescale). Math:
//   target_base = x.base() \ rescale_base
//   y           = fast_base_convert(x|_rescale_base, target_base)
//   z[q]        = (x[q] - y[q]) * (P^{-1} mod q)   (mod q)   for q in target
// where P = prod(rescale_base).
//
// Reference: Cheon-Han-Kim-Kim-Song SAC 2018 §4.2 (ModulusReduction). Same
// algorithm as lbcrypto::DCRTPolyImpl::ApproxModDown.
MRP rescale_fbc(const MRP& x, const ModuliBase& rescale_base, FbcVariant variant) {
    assert(!rescale_base.empty() && "rescale_base must be non-empty");
    const ModuliBase target_base = target_after_rescale(x.base(), rescale_base);
    assert(target_base.size() < x.base().size() &&
           "rescale_base must be a proper subset of x.base()");

    const MRP x_rescale = mr_subset(x, rescale_base);
    const MRP y = fast_base_convert(x_rescale, target_base, variant);
    const auto P_inv = P_inv_mod_q(rescale_base, target_base);

    std::vector<std::pair<Polynomial, uint64_t>> pairs;
    pairs.reserve(target_base.size());
    for (size_t i = 0; i < target_base.size(); ++i) {
        const uint64_t q = target_base[i];
        pairs.emplace_back(rescale_residue(x[q], y[q], q, P_inv[i]), q);
    }
    return MRP::from_pairs(pairs);
}

// 2-arg overload — same ReducedNoise default rationale as fast_base_convert.
MRP rescale_fbc(const MRP& x, const ModuliBase& rescale_base) {
    return rescale_fbc(x, rescale_base, FbcVariant::ReducedNoise);
}

// ============================================================================
// GADGETS — MRP Array operations
// ============================================================================

MRP mrpa_dotproduct(const MRPArray& x, const MRPArray& y) {
    assert(x.length() == y.length() && x.length() > 0);
    MRP z = mr_mulp(x[0], y[0]);
    for (size_t i = 1; i < x.length(); ++i) {
        MRP temp = mr_mulp(x[i], y[i]);
        z = mr_addp(z, temp);
    }
    return z;
}

// ============================================================================
// GADGETS — Decomposition
// ============================================================================

MRPArray dig_decomp(const MRP& x,
                     const std::vector<ModuliBase>& digit_bases,
                     const ModuliBase& p_base) {
    ModuliBase target_base = x.base();
    for (auto p : p_base) target_base.push_back(p);
    size_t d = digit_bases.size();
    MRPArray z(d);
    for (size_t i = 0; i < d; ++i) {
        MRP temp = mr_subset(x, digit_bases[i]);
        z[i] = fast_base_convert(temp, target_base);
    }
    return z;
}

SRPArray gadget_decomp(const Polynomial& x, uint64_t /*base*/, uint64_t n_levels) {
    SRPArray z(n_levels);
    for (size_t i = 0; i < n_levels; ++i) z[i] = make_result(x);
    return z;  // TODO: full implementation needs shift instructions
}

SRPArray gadget_decomp_pow2(const Polynomial& x, uint64_t /*log_base*/, uint64_t n_levels) {
    SRPArray z(n_levels);
    for (size_t i = 0; i < n_levels; ++i) z[i] = make_result(x);
    return z;  // TODO
}

// ============================================================================
// GADGETS — GSW/RLWE External Product
// ============================================================================

SRPArray gsw_rlwe_ext_prod(const SRPArray& gsw,
                           const SRPArray& rlwe_in,
                           uint64_t l, uint64_t base) {
    SRPArray decomp0 = gadget_decomp(rlwe_in[0], base, l);
    SRPArray decomp1 = gadget_decomp(rlwe_in[1], base, l);
    SRPArray decomp0_ft = sr_batch_ft(decomp0);
    SRPArray decomp1_ft = sr_batch_ft(decomp1);

    Polynomial r0 = Polynomial::zeros_ni(rlwe_in[0].ring_dimension());
    Polynomial r1 = Polynomial::zeros_ni(rlwe_in[0].ring_dimension());

    for (size_t level = 0; level < l; ++level) {
        r0 = sr_addp_ni(r0, sr_mulp_ni(decomp0_ft[level], gsw[4 * level]));
        r0 = sr_addp_ni(r0, sr_mulp_ni(decomp1_ft[level], gsw[(4 * level) + 1]));
        r1 = sr_addp_ni(r1, sr_mulp_ni(decomp0_ft[level], gsw[(4 * level) + 2]));
        r1 = sr_addp_ni(r1, sr_mulp_ni(decomp1_ft[level], gsw[(4 * level) + 3]));
    }

    r0 = sr_ift(r0);
    r1 = sr_ift(r1);

    SRPArray result(2);
    result[0] = r0;
    result[1] = r1;
    return result;
}

// ============================================================================
// CKKS Bootstrapping
// ============================================================================

MRPArray ckks_bootstrap(const MRPArray& ct_in, const MRPArray& /*aux_data*/) {
    // TODO: Full CKKS bootstrapping circuit
    return ct_in;
}

// ============================================================================
// FILE I/O — JSON
// ============================================================================

bool save_polynomial_json(const Polynomial& p, const std::filesystem::path& file) {
    if (!p.is_valid()) return false;
    std::ofstream out(file);
    if (!out) return false;
    auto* impl = p.impl();
    if (!impl) return false;
    // Hand-rolled to keep nlohmann/json out of this TU; schema is a
    // {"values":[...]} object that downstream readers can walk.
    out << "{\"ring_dim\":" << impl->ring_dim
        << ",\"format\":" << static_cast<int>(impl->fmt)
        << ",\"number_type\":" << static_cast<int>(impl->ntype)
        << ",\"values\":[";
    if (impl->ntype == NumberType::Integer) {
        for (size_t i = 0; i < impl->int_data.size(); ++i) {
            if (i > 0) out << ',';
            out << impl->int_data[i];
        }
    } else {
        for (size_t i = 0; i < impl->fp_data.size(); ++i) {
            if (i > 0) out << ',';
            out << impl->fp_data[i];
        }
    }
    out << "]}";
    return out.good();
}

bool load_polynomial_json(Polynomial& /*p*/, const std::filesystem::path& /*file*/) {
    return false;
}

bool save_mrp_json(const MRP& /*m*/, const std::filesystem::path& /*file*/) {
    return false;
}

bool load_mrp_json(MRP& /*m*/, const std::filesystem::path& /*file*/) {
    return false;
}

bool save_mrp_array_json(const MRPArray& /*arr*/, const std::filesystem::path& /*file*/) {
    return false;
}

bool load_mrp_array_json(MRPArray& /*arr*/, const std::filesystem::path& /*file*/) {
    return false;
}

}  // namespace niobium::fhetch

// ============================================================================
// Internal: sync tagged inputs/outputs to the Compiler so replay() can find
// them. Called from Compiler::replay() before populating the simulator.
// ============================================================================

namespace niobium::detail {

uintptr_t polynomial_address(const niobium::fhetch::Polynomial& p) {
    return p.impl() ? p.impl()->address : static_cast<uintptr_t>(-1);
}

void sync_fhetch_state_to_compiler() {
    using namespace niobium::fhetch;
    auto& cc = niobium::compiler();
    const auto& mod_map = address_modulus_map();

    auto data_or_zeros = [](const Polynomial& p) {
        if (!p.impl()->int_data.empty()) return p.impl()->int_data;
        return std::vector<uint64_t>(p.ring_dimension(), 0);
    };

    // ---- Inputs ----
    for (const auto& e : input_registry()) {
        switch (e.kind) {
        case InputEntry::POLY: {
            if (!e.poly.is_valid()) break;
            auto it = mod_map.find(e.poly.impl()->address);
            if (it == mod_map.end()) break;  // never used in a modulus-bearing op
            cc.store_input_element(e.name, niobium::CapturedKind::SRP,
                                   /*starts_new_element=*/false,
                                   e.poly.impl()->address, it->second,
                                   data_or_zeros(e.poly));
            break;
        }
        case InputEntry::MRP_KIND:
            if (e.mrp.is_valid()) {
                for (auto q : e.mrp.base()) {
                    const auto& res = e.mrp[q];
                    if (!res.is_valid()) continue;
                    cc.store_input_element(e.name, niobium::CapturedKind::MRP,
                                           /*starts_new_element=*/false,
                                           res.impl()->address, q,
                                           data_or_zeros(res));
                }
            }
            break;
        case InputEntry::MRPA:
            if (e.mrpa.is_valid()) {
                for (size_t i = 0; i < e.mrpa.length(); ++i) {
                    const auto& m = e.mrpa[i];
                    bool first = true;
                    for (auto q : m.base()) {
                        const auto& res = m[q];
                        if (!res.is_valid()) continue;
                        cc.store_input_element(e.name, niobium::CapturedKind::MRPArray,
                                               /*starts_new_element=*/first,
                                               res.impl()->address, q,
                                               data_or_zeros(res));
                        first = false;
                    }
                }
            }
            break;
        case InputEntry::SRPA:
            if (e.srpa.is_valid()) {
                for (size_t i = 0; i < e.srpa.length(); ++i) {
                    const auto& p = e.srpa[i];
                    if (!p.is_valid()) continue;
                    auto it = mod_map.find(p.impl()->address);
                    if (it == mod_map.end()) continue;
                    cc.store_input_element(e.name, niobium::CapturedKind::SRPArray,
                                           /*starts_new_element=*/true,
                                           p.impl()->address, it->second,
                                           data_or_zeros(p));
                }
            }
            break;
        }
    }

    // ---- Outputs ----
    // Outputs without a known modulus still get recorded (modulus=0) so
    // downstream consumers can at least emit a template.
    auto modulus_for = [&](const Polynomial& p) -> uint64_t {
        auto it = mod_map.find(p.impl()->address);
        return (it == mod_map.end()) ? 0 : it->second;
    };

    for (const auto& e : probe_registry()) {
        switch (e.kind) {
        case ProbeEntry::POLY:
            if (e.poly.is_valid()) {
                cc.store_output_probe(e.name, niobium::CapturedKind::SRP,
                                        /*starts_new_element=*/false,
                                        e.poly.impl()->address, modulus_for(e.poly));
            }
            break;
        case ProbeEntry::MRP_KIND:
            if (e.mrp.is_valid()) {
                for (auto q : e.mrp.base()) {
                    cc.store_output_probe(e.name, niobium::CapturedKind::MRP,
                                            /*starts_new_element=*/false,
                                            e.mrp[q].impl()->address, q);
                }
            }
            break;
        case ProbeEntry::MRPA:
            if (e.mrpa.is_valid()) {
                for (size_t i = 0; i < e.mrpa.length(); ++i) {
                    const auto& m = e.mrpa[i];
                    bool first = true;
                    for (auto q : m.base()) {
                        cc.store_output_probe(e.name, niobium::CapturedKind::MRPArray,
                                                /*starts_new_element=*/first,
                                                m[q].impl()->address, q);
                        first = false;
                    }
                }
            }
            break;
        case ProbeEntry::SRPA:
            if (e.srpa.is_valid()) {
                for (size_t i = 0; i < e.srpa.length(); ++i) {
                    const auto& p = e.srpa[i];
                    if (!p.is_valid()) continue;
                    cc.store_output_probe(e.name, niobium::CapturedKind::SRPArray,
                                            /*starts_new_element=*/true,
                                            p.impl()->address, modulus_for(p));
                }
            }
            break;
        }
    }
}

}  // namespace niobium::detail
