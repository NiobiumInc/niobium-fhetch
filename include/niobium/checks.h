// Copyright 2024-present Niobium Microsystems, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <cstdint>

namespace niobium {

  // Check if `prime` is compatible with Niobium hardware
  constexpr bool is_compatible_prime(uint64_t prime) {
    return (prime & 0xFFFF) == 0x0001;
  }

  // Check if `ring_dim` is compatible with Niobium hardware
  constexpr bool is_compatible_ring_dim(uint64_t ring_dim) {
    return ring_dim == (1 << 16);
  }

  // Check if `root` is a primitive 2N-th root of unity mod `q`, with N =
  // `ring_dim`, in canonical form: a root the negacyclic NTT over
  // Z_q[X]/(X^N + 1) can use. For a power-of-two N and q > 2 that is exactly
  // root in [1, q) with root^N == q - 1 (mod q), since an element whose N-th
  // power is -1 has order 2N. False for any other N or q, for root 0, and for
  // root >= q even when root mod q is a root, so each root has one spelling.
  constexpr bool is_primitive_2n_root(uint64_t root, uint64_t ring_dim, uint64_t q) {
    if (q <= 2 || root >= q || ring_dim == 0 || (ring_dim & (ring_dim - 1)) != 0)
      return false;
    uint64_t power = root;
    for (uint64_t n = 1; n < ring_dim; n <<= 1)
      power = static_cast<uint64_t>(static_cast<__uint128_t>(power) * power % q);
    return power == q - 1;
  }

}
