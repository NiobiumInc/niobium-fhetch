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

}
