# niobium-fhetch

Open-source FHETCH Polynomial IR library and local simulator for the **Niobium Mistic** FHE accelerator.

The IR interface implemented here follows the FHETCH Polynomial IR specification published at [fhetch.org](https://fhetch.org), extended with Niobium-specific additions (multi-residue gadgets, decomposition gadgets, CKKS bootstrap, and the session/replay tooling described below).

This repository provides:

1. **`fhetch_api.h`** — the complete FHETCH Polynomial IR instruction set as a C++ API (baseline ops, multi-residue gadgets, decomposition gadgets, CKKS bootstrap, file I/O).
2. **`compiler.h`** — a minimal session-level API (`init` / `start` / `stop` / `tag_input` / `probe` / `replay`) that a host application calls to bracket a computation and emit a `.fhetch` instruction trace.
3. **`fhetch_sim`** — an OpenFHE-backed executor that replays a `.fhetch` trace locally and reconstructs the ciphertexts at probed outputs. The simulator library is always linked into `libnbfhetch` (used by `Compiler::replay()`); the standalone `fhetch_sim` CLI is built when `WITH_FHETCH_SIM=ON` (default).

This library is linked by host integrations (e.g. [`niobium-client`](https://github.com/NiobiumInc/niobium-client) for OpenFHE-instrumented applications) and produces the trace consumed by the Niobium compilation server.

## How it fits together

```
 Host application
   (e.g. an OpenFHE program linked against niobium-client)
        |
        | calls niobium::compiler().start() / stop(),
        | tag_input(), probe()
        v
 +-------------------------------------------------+
 |  niobium-fhetch                                  |
 |                                                  |
 |  fhetch_api.h   complete FHETCH instruction set  |
 |                  (sr_addp, sr_mulp, sr_ntt, ...) |
 |                                                  |
 |  compiler.h     session API (init/start/stop,    |
 |                  tag_input, probe, replay)       |
 |                                                  |
 |  fhetch_sim     optional OpenFHE-backed replay   |
 +-------------------------------------------------+
        |
        | emits .fhetch + fhetch_replay.json
        v
   +-------------+            +-----------------+
   | fhetch_sim  |            | Niobium Server  |
   | (local)     |            | (compiler)      |
   | OpenFHE     |            | optimize +      |
   | executor    |            | deploy to HW    |
   +-------------+            +-----------------+
```

The FHETCH library itself is scheme-agnostic. The FHE-scheme bindings (e.g. "this OpenFHE `EvalMult` call becomes these `sr_mulp` instructions") live in a host integration — typically `niobium-client`, which links this library and an instrumented OpenFHE build.

## Relationship to OpenFHE

**The FHETCH API is library-agnostic.** `fhetch_api.h` is a pure Polynomial IR: polynomials, scalars, multi-residue objects, and the instructions that operate on them. Nothing in the API surface names OpenFHE, references OpenFHE types, or assumes a particular FHE scheme. A hypothetical host using HElib, SEAL, TFHE-rs, Lattigo, or a hand-rolled ring backend would interact with the same `sr_addp` / `sr_ntt` / `tag_input` / `tag_output` API and produce the same `.fhetch` trace format.

Within this repository OpenFHE is vendored as a **helper**, used in two concrete and self-contained places:

- **Simulator-side polynomial math.** The bundled `fhetch_sim` executor needs a working modular-arithmetic backend to actually evaluate each `sr_*` instruction against real values (so `Compiler::replay()` can reconstruct output ciphertexts and decrypt round-trips like the ones in `tests/`). Rather than re-implement NTT, modular multiplication, `NativeInteger` / `NativeVector` wrappers, etc., the simulator calls into OpenFHE's `core/math/hal/intnat` path. This is an implementation choice for the reference simulator; it does not leak into the FHETCH IR.
- **Input / key / output serialization.** The `.input_*.bin`, `.mk.bin`, `.rk.bin`, and ciphertext-template files that host integrations produce (and that `fhetch_driver` consumes in roundtrip mode) use the **cereal** binary format vendored inside OpenFHE's third-party tree. Again, this is a pragmatic reuse — any host that speaks cereal-compatible binary can produce or consume those files. If a future host prefers a different serialization (Protocol Buffers, Arrow, raw little-endian dumps), only the helpers in `auto_facade.cpp` / `src/fhetch_parser.cpp` need to change; the on-the-wire `.fhetch` trace format and the public API stay the same.

In short: the `.fhetch` trace and the `niobium::fhetch::` API surface are the intended portable interface. OpenFHE supplies the numerical engine and the binary container we chose for the reference implementation, nothing more. Hosts that want to replace one or the other can do so without touching the IR.

## Public API

### `fhetch_api.h` — FHETCH Polynomial IR

Defines every FHETCH Polynomial IR instruction as a C++ function. These are **not called directly by end-user application code** — they are called by the host integration (e.g. OpenFHE probes in `niobium-client`). Each call records one hardware instruction in the trace.

The baseline instruction set implements the FHETCH Polynomial IR specification at [fhetch.org](https://fhetch.org); the multi-residue and decomposition gadgets, CKKS bootstrap, and optional operations below are Niobium extensions on top of that spec.

Baseline instructions (map 1:1 to hardware ISA):

| Function            | Opcode       | Description                              |
|---------------------|--------------|------------------------------------------|
| `sr_addp(a,b,q)`    | ADD          | Component-wise polynomial addition mod q |
| `sr_subp(a,b,q)`    | SUB          | Component-wise polynomial subtraction    |
| `sr_mulp(a,b,q)`    | MUL          | Component-wise polynomial multiplication |
| `sr_addps(a,s,q)`   | ADDI         | Polynomial + scalar                      |
| `sr_subps(a,s,q)`   | SUBI         | Polynomial - scalar                      |
| `sr_mulps(a,s,q)`   | MULI         | Polynomial * scalar                      |
| `sr_ntt(a,q)`       | NTT1+NTT2    | Forward negacyclic NTT                   |
| `sr_intt(a,q)`      | INTT1+INTT2  | Inverse negacyclic NTT                   |
| `sr_permute(a,...)` | MORPH1+MORPH2| General permutation with sign flips      |
| `halt()`            | STOP         | End of trace                             |

**Multi-residue gadgets** lower to per-residue baseline instructions:
`mr_addp`, `mr_subp`, `mr_mulp`, `mr_ntt`, `mr_intt`, `mr_zeros`, `mr_append_srp`, `mr_union`, `mr_subset`, `fast_base_convert`, `rescale_fbc`, `mrpa_dotproduct`, `dig_decomp`.

**Data types** (opaque, pimpl pattern): `Polynomial`, `Scalar`, `MRP`, `MRS`, `SRPArray`, `MRPArray`.

**Optional operations**: non-integer arithmetic (`_ni` suffix), Fourier transforms (`sr_ft`/`sr_ift`), TFHE gadgets (`gadget_decomp`, `gsw_rlwe_ext_prod`), automorphisms (`sr_automorph_eval`, `sr_automorph_coeff`, `sr_rot_automorph_coeff`), CKKS bootstrapping (`ckks_bootstrap`).

**File I/O**: binary and JSON serializers for `Polynomial`, `Scalar`, `MRP`, `MRS`, `SRPArray`, `MRPArray`, plus directory-based layouts for large multi-residue objects.

### `compiler.h` — Session API

```cpp
namespace niobium {
  class Compiler {
  public:
    // Session lifecycle
    void init(int& argc, char** argv);
    bool start();
    bool stop();

    // Program metadata
    void set_program_info(name, version, description);
    void set_build_info(file, line, timestamp);

    // Cache management (skip re-recording when structure is unchanged)
    typedef std::vector<std::pair<std::string, std::string>> CacheParameters;
    void cache_parameters(CacheParameters& params);
    bool is_cache_valid();

    // Crypto context and key tagging (generic over OpenFHE types)
    template<typename CryptoContextType>
    void capture_crypto_context(const CryptoContextType& cc);
    template<typename CryptoContextType>
    void tag_keys(const CryptoContextType& cc);

    // Input / output tagging
    template<typename T>
    void tag_input(const std::string& name, const T& ct, ...);
    template<typename T>
    void probe(const std::string& name, const T& ct);
    template<typename CC, typename T>
    bool result(const CC& cc, const std::string& name, T& ct);

    // Replay (local simulator)
    bool replay();

    // Recording modes
    void enable_hollow_mode(bool enabled);
    void enable_multithreaded_recording();

    // Functional epochs (split large computations)
    void start_epoch();
    bool stop_epoch();
  };

  Compiler& compiler();  // Global singleton
}
```

### `.fhetch` trace format

The trace is a human-readable text file of FHETCH Polynomial IR operations:

```
# Niobium FHETCH Trace
# Program: my_program v1.0
# Instruction Count: 24
# Modulus Count: 2

modulus_count 2
m[0] 0xFFFFFFFFFFFFFFFF    # copy sentinel
m[1] 0x3FFFFE80001

sr_addp %2, %0, %1, m=1
sr_mulp %3, %2, %1, m=1
sr_ntt  %4, %3, m=1, omega=...
halt
```

Conventions (matching `niobium-compiler`'s `ModulusTable`):

- `m[0]` is the sentinel `0xFFFFFFFFFFFFFFFF` for copy/zero-init opcodes.
- `m[1..N]` hold real Q and P moduli, sorted ascending for deterministic ordering.

### `fhetch_replay.json`

A manifest emitted alongside the trace, consumed by the local simulator and the compilation server:

- `crypto_context` — scheme, ring dimension, multiplicative depth, modulus chain + Hensel-lifted inverse chain.
- `key_start_addr_ids` — FHETCH address where each key type begins (`evalmult` at 25, `evalautomorphism` right after).
- `files` — per-input `.bin`/`.ids` pairs, key files, instruction trace, output index.

## Repository layout

```
niobium-fhetch/
  include/
    niobium/
      fhetch_api.h            # Complete FHETCH instruction set
      compiler.h              # Session API
  src/
    fhetch_api.cpp            # Instruction recording into .fhetch
    compiler.cpp              # Session lifecycle, replay orchestration
    compiler_internal.h       # Internal API between compiler/probes
    probes.cpp                # C-linkage openfhe_cprobe_* implementations
    auto_facade.cpp           # capture_crypto_context / tag_keys / tag_input
    trace_writer.{h,cpp}      # .fhetch text emitter
    cereal_io.h               # Binary I/O helpers
    fhetch_sim/               # Optional: trace parser + OpenFHE executor
  vendor/
    openfhe/                  # Niobium-instrumented OpenFHE (submodule)
    json/                     # nlohmann/json (submodule)
  CMakeLists.txt
  Makefile
  README.md
  LICENSE                     # Apache 2.0
```

## Building

```bash
git submodule update --init --recursive
make build-release           # or: make build   (Debug)
```

This builds OpenFHE (vendored submodule), then the `libnbfhetch` library and the `fhetch_sim` CLI. Options:

| CMake option       | Default | Effect                                                            |
|--------------------|---------|-------------------------------------------------------------------|
| `BUILD_SHARED_LIBS`| ON      | Shared vs static library                                          |
| `WITH_FHETCH_SIM`  | ON      | Build the standalone `fhetch_sim` CLI (the simulator library code is always linked into `libnbfhetch` because `Compiler::replay()` uses it). |

### Prerequisites

- C++17 compiler
- CMake 3.16+
- OpenFHE (Niobium-instrumented branch, vendored under `vendor/openfhe` — required because the compiler uses `cereal` serialization bundled with OpenFHE)

## Releases

Building from source (above) is one option. Tagged releases also publish a **prebuilt binary
distribution**, `niobium-runtime`, on the GitHub Releases page, so downstream language
channels — the Python wheel today, Node later — depend on a release instead of compiling
OpenFHE from source. Each release is a versioned, relocatable prefix (headers, libraries,
`fhetch_sim`, and CMake config) that a build consumes via `find_package(NiobiumFhetch)`.

```bash
make runtime        # build a distribution locally, into dist/
make test-runtime   # external-consumer acceptance (records + replays)
```

## Usage

A host integration (e.g. `niobium-client`) typically drives the session as:

```cpp
#include "niobium/compiler.h"

int main(int argc, char* argv[]) {
    niobium::compiler().init(argc, argv);
    niobium::compiler().set_program_info("my_app", "1.0", "workload description");

    niobium::Compiler::CacheParameters params;
    params.push_back({"workload", "my_workload"});
    niobium::compiler().cache_parameters(params);

    // Host-specific: load data, capture crypto context, tag inputs/keys.
    // ...

    if (!niobium::compiler().is_cache_valid()) {
        niobium::compiler().start();
        // Host computation — probes in the host's FHE backend fire FHETCH
        // recording calls automatically.
        niobium::compiler().probe("result", result);
        niobium::compiler().stop();
    }

    niobium::compiler().replay();   // optional local simulation
    // Retrieve reconstructed outputs via compiler().result(...).
    return 0;
}
```

End users do **not** call `fhetch_api.h` functions directly — they are invoked by the host integration's probe mechanism.

For OpenFHE-specific instrumentation and full end-to-end examples (client / server / decrypt splits, CKKS bootstrap, ciphertext rehydration), see [`niobium-client`](https://github.com/NiobiumInc/niobium-client).

### FHETCH-only examples

This repository ships examples that exercise the FHETCH API directly — no OpenFHE crypto context in the user code, just polynomials, scalars, MRPs, and MRPAs driven through the `fhetch::` free functions. They are built when `NIOBIUM_FHETCH_WITH_EXAMPLES=ON` (or via `make test-simple-fhetch-release`).

| Example                        | What it does |
|--------------------------------|--------------|
| `examples/fhetch/simple_fhetch.cpp` | Single-residue ops (`sr_addp`, `sr_mulp`, `sr_mulps`, `sr_negp`, `sr_ntt`/`sr_intt`), multi-residue ops (`mr_addp`, `mr_mulp`, `mr_ntt`/`mr_intt`), and an MRPA dot product. Emits a `.fhetch` trace + replay manifest + replay outputs. |

Under the hood, `fhetch::tag_input` / `tag_output` wire into the Compiler's captured-inputs / output-probes the same way the OpenFHE auto-facade (`Compiler::tag_input<Ciphertext>`) does, so `Compiler::replay()` populates the simulator's memory directly and writes the computed output values to `fhetch_replay_outputs.json`.

Run:

```bash
make test-simple-fhetch-release
```

### Test harness — `fhetch_driver`

`tests/fhetch_driver/` ships a standalone executable that reads a `.fhetch` trace from disk and re-drives it through the FHETCH API, producing a secondary trace that is replayed through the simulator.

Cooperative `Compiler::replay()` with `--target local` dispatches to the `fhetch_sim` project worker by default (located via `NBCC_FHETCH_SIM`, else `fhetch_sim` on `PATH`), which replays the recorded project directly. Setting `NBCC_FHETCH_DRIVER` opts that dispatch into this roundtrip harness instead — useful as an API-coverage check, but far heavier: the re-drive materializes every live-in input three times over and re-records the full trace.

The parser itself is a public library component — `include/niobium/fhetch_parser.h` + `src/fhetch_parser.cpp` — so other callers can consume it directly via `libnbfhetch`. Build-gated by `NIOBIUM_FHETCH_WITH_TESTS=ON`.

```bash
make build-release
make test-fhetch-driver-release TRACE=/path/to/trace.fhetch N=2048
```

**Two modes**: run without `--source-dir` for a structural sanity re-drive (live-ins zero-filled, no output probes). Run with `--source-dir <path-to-primary-workload-dir> --cc <cc.bin> --output-ct NAME:PATH` to do a value-level roundtrip: the driver deserializes the primary's captured inputs (`.input_*.bin` / keys) via OpenFHE cereal, tags the original outputs, and reconstructs a ciphertext for the specified output name so a decrypt step can verify the secondary against the expected plaintext.

### End-to-end roundtrip tests

`tests/{simple_ops,bootstrap}/` are OpenFHE `client / server / decrypt` triples ported from `niobium-client`. Built alongside `fhetch_driver` under `NIOBIUM_FHETCH_WITH_TESTS=ON`. The top-level `make test-roundtrip-simple-ops-release` target runs every `simple_ops` operation through the full pipeline:

1. `simple_ops_client` encrypts inputs.
2. `simple_ops_server` records the `.fhetch` trace and runs the **primary** simulator replay → `ct_result.bin`.
3. `simple_ops_decrypt … ct_result.bin` — primary decrypt PASS.
4. `fhetch_driver --source-dir WORKLOAD --cc cc.bin --output-ct result:ct_result_secondary.bin` — re-drives the trace with real inputs, runs the **secondary** simulator replay, reconstructs the output ciphertext.
5. `simple_ops_decrypt … ct_result_secondary.bin` — secondary decrypt PASS.

All 13 simple_ops (`ADD, SUB, NEG, ADDI, SUBI, MULI, ADD_ADD, ADD_SUB, MUL, MUL_ADD, ADD_MUL, MUL_MUL, MORPH`) currently pass both primary and secondary decrypt. The `bootstrap` example is wired the same way (`make test-roundtrip-bootstrap-release`) but has a separate primary-side numerical precision issue that also affects the secondary.

## Architecture notes

- **Thin, scheme-agnostic recording.** The library records FHETCH Polynomial IR only. Scheme-level semantics (CKKS vs BFV vs BGV, which polynomial operations a given ciphertext op lowers to) are the responsibility of the host integration and its FHE backend.
- **FHETCH-level trace.** The trace uses FHETCH operation names (`sr_addp`, `sr_ntt`, `mr_mulp`, ...) rather than internal hardware instructions. Lowering (e.g. `sr_ntt` → `ntt1`+`ntt2`, register allocation, load/store insertion) happens server-side.
- **Compiler-parity conventions.** Output artifacts follow `niobium-compiler` conventions so the two toolchains can be diffed artifact-for-artifact: sentinel at `m[0]`, ascending modulus order, inputs at ids 1..24 / `evalmult` at 25 / `evalautomorphism` following.
- **Local simulator for validation.** `fhetch_sim` replays a `.fhetch` file against its `fhetch_replay.json` using OpenFHE's `NativeVector` / `NativeInteger` math as a concrete backend for the modular arithmetic, giving a deterministic reference for what the hardware would compute. `Compiler::replay()` + `result()` exposes this round-trip to the host. OpenFHE here is purely a helper library — see **Relationship to OpenFHE** above; the FHETCH IR it's executing makes no assumptions about the backend.

## License

Apache 2.0 — see [LICENSE](LICENSE).

## Contributing

We are actively working on a contribution policy and Contributor License
Agreement (CLA). Until that process is in place we are not yet able to accept
external contributions. If you have a bug report, a feature request, or a
question, please [contact us](https://niobium.co/contact) directly.
Watch this repository to be notified when the contribution policy launches.
