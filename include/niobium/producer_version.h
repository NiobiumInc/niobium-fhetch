// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// Identity of whatever produced a fhetch project bundle.
//
// The bundle is consumed by a Niobium-operated compiler, which needs to know
// which client emitted it in order to decide whether it can still accept it.
// See COMPATIBILITY.md for the versioning scheme.
//
// These values are compile-time: NIOBIUM_FHETCH_VERSION comes from this repo's
// VERSION file, and NIOBIUM_CLIENT_VERSION is injected by whichever
// distribution links this library. It is "unknown" for a standalone build,
// which is exactly right -- a standalone build is not a client release.

#ifndef NIOBIUM_PRODUCER_VERSION_H
#define NIOBIUM_PRODUCER_VERSION_H

#ifndef NIOBIUM_FHETCH_VERSION
#define NIOBIUM_FHETCH_VERSION "unknown"
#endif

#ifndef NIOBIUM_CLIENT_VERSION
#define NIOBIUM_CLIENT_VERSION "unknown"
#endif

namespace niobium {

// Version of this FHETCH implementation (e.g. "1.0.0").
inline const char* fhetch_version() { return NIOBIUM_FHETCH_VERSION; }

// Version of the distribution embedding this library (e.g. "1.2.0+g1d7db00"),
// or "unknown" when built standalone rather than as part of a client release.
inline const char* client_version() { return NIOBIUM_CLIENT_VERSION; }

}  // namespace niobium

#endif  // NIOBIUM_PRODUCER_VERSION_H
