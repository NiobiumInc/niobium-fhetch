# Python bindings (niobium-fhetch)

This repo builds the two native Python extensions behind the `niobium_sdk` wheel
(which is assembled downstream in `niobium-client`):

- **`openfhe`** — the vendored **openfhe-python** crypto module, rebuilt against
  Niobium's instrumented OpenFHE.
- **`niobium_session`** — pybind11 bindings for the `niobium::compiler()` record/replay
  session API (`bindings/python/niobium_session.cpp`).

Both are gated behind the CMake `WITH_PYTHON` option and land in
`build/python/{openfhe,niobium_session}.*.so`.

The Python build is kept out of the C++-focused root files: the CMake lives in
[`bindings/python/CMakeLists.txt`](bindings/python/CMakeLists.txt) (`if(WITH_PYTHON) add_subdirectory(python)`
from the root), and the dev/smoke make targets in [`make/python.mk`](make/python.mk)
(`include`d by the root Makefile, sharing its variable namespace).

## openfhe-python: vendoring, pinning, patching

openfhe-python is a **nested submodule** at `vendor/openfhe-python`, built
**unmodified from source** via its own CMake as a CMake `ExternalProject` (its
`CMakeLists` uses `CMAKE_SOURCE_DIR` + a site-packages install that don't compose under
`add_subdirectory`). Two build-time adjustments are applied — neither edits the vendored
source:

1. **`-DOPENFHE_CPROBES`** — the OpenFHE probe hooks are header-inlined and guarded by
   this macro, and OpenFHE's CMake config does *not* propagate it. Without it,
   `openfhe.so`'s inlined clone/copy + Cereal-deserialize id-assignment probes compile
   out, so recording on *deserialized* ciphertexts silently drops the input copy-ins
   (inputs replay as zero). Fresh-encrypt single-process tests don't catch this; the
   client/server split does.
2. **`Development` → `Development.Module`** — openfhe-python's
   `find_package(Python … Development)` requires `Development.Embed` (libpython), which
   manylinux Pythons omit (module-only), so it fails there. openfhe-python never links
   libpython (`pybind11_add_module` builds the module; the find result is only used for
   the interpreter version + exec_prefix), so the build patches that line to
   `Development.Module` in a **build-tree copy** of the source — the submodule stays
   pristine (no dirty tree, no committed delta). CMake emits a warning if the line to
   patch has changed upstream.

### Pinning

Pinned at **`v1.4.2.0.5`** (commit `b90edb3`) — the first openfhe-python tag carrying the
`CompressionLevel` rename, which matches OpenFHE **1.4.2** (the version this repo's
OpenFHE fork tracks). To re-vendor:

```bash
cd vendor/openfhe-python && git fetch && git checkout <new-tag> && cd -
git add vendor/openfhe-python && git commit -m "vendor openfhe-python <new-tag>"
make build-python-release …     # rebuild
make test-roundtrip-python-release …   # re-verify
```

The `OPENFHE_CPROBES` + `Development.Module` adjustments reapply automatically. pybind11
is a **build-time `find_package` item** (not vendored/pinned in source) — install the
version you want in the build environment; it's aligned to upstream, which builds on
**pybind11 3.x**. (abi3 is not achievable with pybind11 at any version.)

## Session bindings (`niobium_session`)

`bindings/python/niobium_session.cpp` binds the `niobium::compiler()` API. Bound surface:

```
init  set_program_info  set_build_info  cache_parameters  is_cache_valid
start  stop  pause  resume  is_running  is_stopped  replay  get_program_directory
capture_crypto_context  tag_input(Ciphertext|Plaintext)  tag_keys  probe  result
enable_hollow_mode
```

Conventions:
- **Pythonic names:** predicates are `is_*` (not the C++ `*_p`); `set_program_info` /
  `set_build_info` take named args with defaults (Python has no `__FILE__`/`__LINE__`/
  `__TIMESTAMP__`).
- **Runtime coupling (important):** the module must **co-load `libnbfhetch`** — probe
  globals (`g_replay_mode`, the `NB_WEAK` symbols in `src/auto_facade.cpp`) are defined
  there, not in OpenFHE, and `openfhe.so`'s inlined probes reference them. The tests load
  it `RTLD_GLOBAL` (via `bindings/python/tests/_nbcommon.py`, path from `NB_FHETCH_LIB`) *before*
  `import openfhe`; the downstream wheel does it in its package `__init__`.

### Adding or updating a bound endpoint

Add an `m.def("name", …)` in `bindings/python/niobium_session.cpp`. Endpoints templated on
OpenFHE types are instantiated for `DCRTPoly` (see `capture_crypto_context`, `tag_input`,
`tag_keys`, `probe`, `result`). Rebuild + smoke-test:

```bash
make test-session-api-python-release PYTHON=$PY
```

Several `Compiler` endpoints are intentionally **unbound** — internals, auto-invoked
hooks (e.g. `tag_bootstrap_precompute`, fired from `stop()`), and address-allocator
plumbing — bind one only if a scenario needs it. `set_ring_dimension` is deliberately
omitted: `capture_crypto_context()` already derives `N` from the CryptoContext; a manual
setter belongs to the context-less IR path, not the recorder.

## Building

Prerequisites: OpenFHE built/installed, plus **pybind11** + Python dev headers in the
Python you build against (point `PYTHON=` at a venv that has pybind11):

```bash
python3.12 -m venv .venv && .venv/bin/pip install pybind11
export PY=$PWD/.venv/bin/python

# Standalone: build OpenFHE here, then the bindings
make config-python-release build-python-release PYTHON=$PY

# Against a pre-installed OpenFHE (e.g. the parent client's vendor/lib/openfhe):
make build-python-release PYTHON=$PY \
     EXTERNAL_OPENFHE=1 OPENFHE_INSTALL_DIR=/abs/path/to/openfhe
```

`config-python-release` sets `WITH_PYTHON=ON` (and `NIOBIUM_FHETCH_WITH_TESTS=ON`, so
`fhetch_driver` builds for the roundtrip secondary). Outputs:
`build/python/openfhe.*.so` + `build/python/niobium_session.*.so`.

## Testing

Full roundtrips — each scenario runs client → server → decrypt with **primary** replay
(via `fhetch_sim`) **and secondary** replay (re-driven through the C++ `fhetch_driver`):

```bash
make test-roundtrip-simple-ops-python-release  PYTHON=$PY   # 13 CKKS ops
make test-roundtrip-plaintext-add-python-release PYTHON=$PY
make test-roundtrip-bootstrap-python-release   PYTHON=$PY
make test-roundtrip-python-release             PYTHON=$PY   # the full sweep

make test-session-api-python-release           PYTHON=$PY   # TEMP session-endpoint smoke
```

The test targets export what the scripts need — `NB_FHETCH_LIB` (for the `_nbcommon`
RTLD_GLOBAL preload), `NBCC_FHETCH_SIM`, `PYTHONPATH` (`build/python`), and
`DYLD_LIBRARY_PATH`/`LD_LIBRARY_PATH` (the OpenFHE dylibs) — so the scripts run as bare
commands. Test scenarios live in `bindings/python/tests/<scenario>/{client,server,decrypt}.py`
with the shared bootstrap `bindings/python/tests/_nbcommon.py`.
