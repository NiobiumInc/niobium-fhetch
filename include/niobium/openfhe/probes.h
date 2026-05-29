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

#ifdef __cplusplus
extern "C" {
#endif

// Distinct from openfhe_cprobe_pause_recording: that gates
// TraceWriter::emit (silencing fhetch_api's sr_* path too); this only
// short-circuits the OpenFHE-side CPROBE callbacks. suppress != 0
// mutes; suppress == 0 unmutes.
void openfhe_suppress_probes(int suppress);

#ifdef __cplusplus
}
#endif
