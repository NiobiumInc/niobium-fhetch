# Python bindings — dev/smoke targets for the vendored openfhe-python crypto
# module + the niobium::compiler() session module. Included by the root Makefile,
# whose variable namespace it shares: NUM_CPUS, BUILD_DIR, UNAME_S, CURDIR,
# OPENFHE_INSTALL_DIR, FHETCH_INSTALL_DIR, CMAKE_JSON_INCLUDE_DIR_FLAG,
# OPENFHE_BUILD_DEP_RELEASE, and the set-build-config macro all come from there.
#
# The native build lives in python/CMakeLists.txt (add_subdirectory under
# WITH_PYTHON); these targets just drive config/build + the roundtrip/smoke loop.
# `make help` greps $(MAKEFILE_LIST), so these targets still list.

.PHONY: config-python-release build-python-release \
        test-roundtrip-simple-ops-python-release test-roundtrip-plaintext-add-python-release \
        test-roundtrip-bootstrap-python-release test-roundtrip-python-release \
        test-session-api-python-release

# Discover pybind11's CMake package and the interpreter from PYTHON (default
# python3). Override with `make PYTHON=/path/to/python …`. pybind11 is a build
# dependency: `pip install pybind11`.
PYTHON       ?= python3
PYBIND11_DIR := $(shell $(PYTHON) -m pybind11 --cmakedir 2>/dev/null)
PY_EXE       := $(shell command -v $(PYTHON))

ifeq ($(UNAME_S), Darwin)
    LIBNBFHETCH := libnbfhetch.dylib
else
    LIBNBFHETCH := libnbfhetch.so
endif

# Run the Python bindings out of the build tree. Mirrors the root Makefile's
# DYLD_LIBRARY_PATH export so the recipes below are bare commands (like the C++
# ones): libnbfhetch is loaded RTLD_GLOBAL by the tests (NB_FHETCH_LIB) so the
# crypto module's probe globals resolve; build/ carries libnbfhetch for dyld/ld.
export NB_FHETCH_LIB   := $(CURDIR)/build/$(LIBNBFHETCH)
export NBCC_FHETCH_SIM := $(CURDIR)/build/fhetch_sim
export PYTHONPATH      := $(CURDIR)/build/python:$(PYTHONPATH)
ifeq ($(UNAME_S), Darwin)
    export DYLD_LIBRARY_PATH := $(CURDIR)/build:$(DYLD_LIBRARY_PATH)
else
    export LD_LIBRARY_PATH   := $(CURDIR)/build:$(LD_LIBRARY_PATH)
endif

##@ Python bindings (optional; needs pybind11 + Python dev headers)

config-python-release: ## Configure with the Python bindings (Release; requires OpenFHE built + pybind11)
	$(call set-build-config,Release,build)
	@if [ -z "$(PYBIND11_DIR)" ]; then \
		echo "pybind11 CMake dir not found for '$(PYTHON)'. Install it: $(PYTHON) -m pip install pybind11"; \
		exit 1; \
	fi
	cmake -S $(CURDIR) -B $(CURDIR)/build \
		-DCMAKE_BUILD_TYPE=Release \
		-DOPENFHE_INSTALL_DIR=$(OPENFHE_INSTALL_DIR) \
		$(CMAKE_JSON_INCLUDE_DIR_FLAG) \
		-DWITH_PYTHON=ON \
		-DNIOBIUM_FHETCH_WITH_TESTS=ON \
		-Dpybind11_DIR=$(PYBIND11_DIR) \
		-DPython_EXECUTABLE=$(PY_EXE) \
		-DCMAKE_INSTALL_PREFIX=$(FHETCH_INSTALL_DIR)

build-python-release: $(OPENFHE_BUILD_DEP_RELEASE) config-python-release ## Build the Python bindings (Release)
	$(call set-build-config,Release,build)
	cmake --build build -j $(NUM_CPUS) --config Release

# ==============================================================================
# Python End-to-end roundtrip tests (primary + secondary via fhetch_driver)
# ==============================================================================

# Python analog of `define roundtrip-simple-op`: for a given op ($1), drive
# client -> server (primary replay) -> decrypt -> fhetch_driver (secondary
# replay) -> decrypt. Mirrors the C++ helper; only the binary/python-script
# invocation differs (ring dim is fixed at 2048 in the client scripts).
define roundtrip-simple-op-python
	@echo ""
	@echo "=== Roundtrip $(1) (python) ==="
	@rm -rf simple_ops_keys simple_ops_server_workload_*
	@$(PY_EXE) python/tests/simple_ops/client.py simple_ops_keys $(2) $(3) 2>&1 | tail -1
	@$(PY_EXE) python/tests/simple_ops/server.py simple_ops_keys $(1) --no-ring-dim-check 2>&1 | grep -E "Complete:|ERROR" | head -3 || true
	@echo "  -- primary decrypt --"
	@$(PY_EXE) python/tests/simple_ops/decrypt.py simple_ops_keys $(1) ct_result.bin 2>&1 | grep -E "PASS|FAIL"
	@echo "  -- fhetch_driver (secondary) --"
	@WORKLOAD_DIR=$$(ls -d simple_ops_server_workload_simple_ops_op_$(1) 2>/dev/null || true); \
	 if [ -z "$$WORKLOAD_DIR" ]; then echo "  [SKIP] no workload dir"; exit 0; fi; \
	 $(BUILD_DIR)/tests/fhetch_driver/fhetch_driver \
	     $$WORKLOAD_DIR/$$WORKLOAD_DIR.fhetch --ring-dim 2048 --no-ring-dim-check \
	     --source-dir $$WORKLOAD_DIR \
	     --cc simple_ops_keys/cc.bin \
	     --output-ct result:simple_ops_keys/ct_result_secondary.bin 2>&1 \
	     | grep -E "replayed:|Complete:|wrote|ERROR" | head -5 || true
	@echo "  -- secondary decrypt --"
	@$(PY_EXE) python/tests/simple_ops/decrypt.py simple_ops_keys $(1) ct_result_secondary.bin 2>&1 | grep -E "PASS|FAIL"
endef

test-roundtrip-simple-ops-python-release: build-python-release ## Full python roundtrip for all simple_ops (primary + secondary decrypt)
	$(call set-build-config,Release,build)
	$(call roundtrip-simple-op-python,ADD,5,6)
	$(call roundtrip-simple-op-python,SUB,5,6)
	$(call roundtrip-simple-op-python,NEG,5,6)
	$(call roundtrip-simple-op-python,ADDI,5,6)
	$(call roundtrip-simple-op-python,SUBI,5,6)
	$(call roundtrip-simple-op-python,MULI,5,6)
	$(call roundtrip-simple-op-python,ADD_ADD,5,6)
	$(call roundtrip-simple-op-python,ADD_SUB,5,6)
	$(call roundtrip-simple-op-python,MUL,5,6)
	$(call roundtrip-simple-op-python,MUL_ADD,5,6)
	$(call roundtrip-simple-op-python,ADD_MUL,5,6)
	$(call roundtrip-simple-op-python,MUL_MUL,5,6)
	$(call roundtrip-simple-op-python,MORPH,5,6)

test-roundtrip-plaintext-add-python-release: build-python-release ## Full python roundtrip for plaintext-add (primary + secondary decrypt)
	$(call set-build-config,Release,build)
	@rm -rf plaintext_add_keys plaintext_add_server_workload_*
	@echo "=== Plaintext-Add client (python) ==="
	$(PY_EXE) python/tests/plaintext_add/client.py plaintext_add_keys
	@echo "=== Plaintext-Add server (python) ==="
	$(PY_EXE) python/tests/plaintext_add/server.py plaintext_add_keys --no-ring-dim-check
	@echo "=== Plaintext-Add primary decrypt ==="
	$(PY_EXE) python/tests/plaintext_add/decrypt.py plaintext_add_keys ct_result.bin
	@echo "=== Plaintext-Add fhetch_driver (secondary) ==="
	@WORKLOAD_DIR=$$(ls -d plaintext_add_server_workload_* 2>/dev/null); \
	 $(BUILD_DIR)/tests/fhetch_driver/fhetch_driver \
	     $$WORKLOAD_DIR/$$WORKLOAD_DIR.fhetch --ring-dim 2048 --no-ring-dim-check \
	     --source-dir $$WORKLOAD_DIR \
	     --cc plaintext_add_keys/cc.bin \
	     --output-ct output_cipher:plaintext_add_keys/ct_result_secondary.bin
	@echo "=== Plaintext-Add secondary decrypt ==="
	$(PY_EXE) python/tests/plaintext_add/decrypt.py plaintext_add_keys ct_result_secondary.bin

test-roundtrip-bootstrap-python-release: build-python-release ## Full python roundtrip for bootstrap (primary + secondary decrypt)
	$(call set-build-config,Release,build)
	@rm -rf bootstrap_keys bootstrap_server_workload_*
	@echo "=== Bootstrap client (python) ==="
	$(PY_EXE) python/tests/bootstrap/client.py bootstrap_keys
	@echo "=== Bootstrap server (python) ==="
	$(PY_EXE) python/tests/bootstrap/server.py bootstrap_keys --no-ring-dim-check
	@echo "=== Bootstrap primary decrypt ==="
	$(PY_EXE) python/tests/bootstrap/decrypt.py bootstrap_keys ct_result.bin
	@echo "=== Bootstrap fhetch_driver (secondary) ==="
	@WORKLOAD_DIR=$$(ls -d bootstrap_server_workload_* 2>/dev/null); \
	 $(BUILD_DIR)/tests/fhetch_driver/fhetch_driver \
	     $$WORKLOAD_DIR/$$WORKLOAD_DIR.fhetch --ring-dim 2048 --no-ring-dim-check \
	     --source-dir $$WORKLOAD_DIR \
	     --cc bootstrap_keys/cc.bin \
	     --output-ct output_cipher:bootstrap_keys/ct_result_secondary.bin
	@echo "=== Bootstrap secondary decrypt ==="
	$(PY_EXE) python/tests/bootstrap/decrypt.py bootstrap_keys ct_result_secondary.bin

test-roundtrip-python-release: test-roundtrip-simple-ops-python-release test-roundtrip-plaintext-add-python-release test-roundtrip-bootstrap-python-release ## Full python roundtrip sweep: simple_ops + plaintext-add + bootstrap

# TEMPORARY (remove when the IR channel covers these): smoke for session endpoints
# not exercised by the recorder scenarios — pause/resume, is_running/is_stopped,
# get_program_directory.
test-session-api-python-release: build-python-release ## TEMP: session-endpoint smoke (pause/resume, is_running, get_program_directory)
	$(call set-build-config,Release,build)
	@rm -rf session_api_smoke_workload_*
	$(PY_EXE) python/tests/session_api_smoke.py
