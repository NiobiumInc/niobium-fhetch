// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//

/**
 * @file test_util.h
 * @brief What the binding format tests and the bind tests share: the checks,
 *        roots for small primes, and the fixture record. C++17.
 */
#pragma once

#include "niobium/binding_record.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace binding_test {

using namespace niobium::binding;

inline int g_failed = 0;
inline int g_passed = 0;

inline void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  ok   " : "  FAIL ") << what << '\n';
    ++(ok ? g_passed : g_failed);
}

// f() must throw E with `code`, and its message must contain `needle`.
template <class E, class Code, class F>
void check_throws(F&& f, Code code, const std::string& needle, const std::string& what) {
    try {
        std::forward<F>(f)();
        check(false, what + " (no error)");
    } catch (const E& e) {
        const bool ok = e.code() == code && std::string(e.what()).find(needle) != std::string::npos;
        check(ok, what + ": " + e.what());
    } catch (const std::exception& e) {
        check(false, what + ": wrong exception: " + e.what());
    }
}

template <class F>
void check_record_error(F&& f, RecordErrc code, const std::string& needle, const std::string& what) {
    check_throws<RecordError>(std::forward<F>(f), code, needle, what);
}


// --- Arithmetic for valid roots -------------------------------------------------

inline constexpr std::uint32_t kN = 4;

inline std::uint64_t pow_mod(std::uint64_t b, std::uint64_t e, std::uint64_t q) {
    std::uint64_t r = 1;
    for (b %= q; e != 0; e >>= 1U, b = b * b % q)
        if ((e & 1U) != 0) r = r * b % q;
    return r;
}

// The smallest primitive 2n-th root of unity modulo q (q = 1 mod 2n, n a power of two).
inline std::uint64_t root_of(std::uint64_t q, std::uint32_t n) {
    for (std::uint64_t g = 2; g < q; ++g)
        if (pow_mod(g, n, q) == q - 1) return g;
    return 0;
}

// Primes = 1 mod 16, so they have roots for n = 4 and n = 8.
inline const std::vector<std::uint64_t> kModuli{17, 97, 113, 193};

inline std::vector<std::uint64_t> roots_for(const std::vector<std::uint64_t>& moduli, std::uint32_t n) {
    std::vector<std::uint64_t> roots;
    roots.reserve(moduli.size());
    for (const auto q : moduli) roots.push_back(root_of(q, n));
    return roots;
}

// --- The fixture record ---------------------------------------------------------

// Limbs of big polynomial `poly` under the given modulus indices, at addresses.
inline std::vector<Limb> limbs(std::uint16_t poly, const std::vector<std::uint16_t>& ks, const std::vector<std::uint64_t>& addrs) {
    std::vector<Limb> out;
    out.reserve(ks.size());
    for (std::size_t i = 0; i < ks.size(); ++i) {
        Limb limb;  // member by member: this header is also compiled as C++17
        limb.big_poly = poly;
        limb.modulus_index = ks[i];
        limb.address = addrs[i];
        out.push_back(limb);
    }
    return out;
}

inline std::vector<Limb> concat(std::vector<Limb> a, const std::vector<Limb>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// A record with the fixture's ring and moduli and no objects.
inline BindingRecord empty_record() {
    BindingRecord rec;
    rec.ring_dim = kN;
    rec.moduli = kModuli;
    rec.roots = roots_for(kModuli, kN);
    return rec;
}

inline BindingRecord make_record() {
    BindingRecord rec = empty_record();
    rec.add_object("input:x", Kind::Input, 2,
                   concat(limbs(0, {0, 1, 2}, {100, 101, 102}), limbs(1, {0, 1, 2}, {200, 201, 210})));
    rec.add_object("input:w", Kind::Input, 1, limbs(0, {0, 1}, {300, 301}));
    rec.add_object("key:relin", Kind::Key, 2,
                   concat(limbs(0, {0, 1, 2, 3}, {400, 401, 402, 403}), limbs(1, {0, 1, 2, 3}, {410, 411, 412, 413})));
    rec.add_object("key:galois:5", Kind::Key, 2,
                   concat(limbs(0, {0, 1, 2, 3}, {500, 501, 502, 503}), limbs(1, {0, 1, 2, 3}, {510, 511, 512, 513})));
    rec.add_object("const:boot:0", Kind::Constant, 1, limbs(0, {1, 3}, {600, 601}));
    rec.add_object("const:weights", Kind::Constant, 1, limbs(0, {0}, {650}));
    rec.add_object("output:y", Kind::Output, 2, concat(limbs(1, {0, 1}, {710, 711}), limbs(0, {1, 0}, {701, 700})));
    return rec;
}

template <class T>
inline void poke(std::vector<std::byte>& bytes, std::size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

template <class T>
T peek(const std::vector<std::byte>& bytes, std::size_t offset) {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

// Runs `body` with a fresh scratch directory `name` under the current
// directory, then prints the tally and returns the exit status.
inline int run_tests(const char* name, const std::function<void(const std::filesystem::path&)>& body) {
    const auto dir = std::filesystem::current_path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    try {
        body(dir);
    } catch (const std::exception& e) {
        check(false, std::string("unexpected exception: ") + e.what());
    }
    std::filesystem::remove_all(dir);
    std::cout << g_passed << " passed, " << g_failed << " failed\n";
    return g_failed != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

}  // namespace binding_test
