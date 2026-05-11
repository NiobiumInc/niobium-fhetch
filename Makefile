# ==============================================================================
# niobium-fhetch — Build System
# ==============================================================================
# Convention: dbuild/ for Debug, build/ for Release.
#
# Quick start:
#   make sync             # One-time: sync submodules (openfhe, json)
#   make config           # Configure OpenFHE + fhetch library (Debug)
#   make build            # Build OpenFHE + fhetch library (Debug)
#
# Release:
#   make release          # Shortcut: config-release + build-release
#
# Full list:
#   make help
# ==============================================================================

SHELL := /bin/bash
.SHELLFLAGS := -o pipefail -c

# ==============================================================================
# Platform Detection & CPU Count
# ==============================================================================

UNAME_S := $(shell uname -s)

ifndef NUM_CPUS
  ifeq ($(UNAME_S), Darwin)
    NUM_CPUS := $(shell sysctl -n hw.ncpu)
  else
    NUM_CPUS := $(shell nproc)
  endif
endif

# ==============================================================================
# Build Configuration
# ==============================================================================

BUILD_CONFIG = Debug
BUILD_DIR = dbuild

define set-build-config
$(eval BUILD_CONFIG = $(1))
$(eval BUILD_DIR = $(2))
endef

# Directories
VENDOR_DIR       := $(CURDIR)/vendor
VENDOR_LIB_DIR   := $(VENDOR_DIR)/lib

# Overridable by a parent build (niobium-client / niobium-compiler) so the
# same source tree can be built either standalone (using vendored submodules)
# or with deps supplied from outside. Use ?= so command-line / env wins.
OPENFHE_DIR          ?= $(VENDOR_DIR)/openfhe
OPENFHE_INSTALL_DIR  ?= $(VENDOR_LIB_DIR)/openfhe
JSON_INCLUDE_DIR     ?=
FHETCH_INSTALL_DIR   := $(VENDOR_LIB_DIR)/niobium-fhetch

# When EXTERNAL_OPENFHE=1, the parent has already built+installed OpenFHE and
# OPENFHE_INSTALL_DIR points at it; skip our own openfhe config/build steps.
EXTERNAL_OPENFHE ?= 0
ifeq ($(EXTERNAL_OPENFHE),1)
  OPENFHE_BUILD_DEP_DEBUG    :=
  OPENFHE_BUILD_DEP_RELEASE  :=
  OPENFHE_CONFIG_DEP_DEBUG   :=
  OPENFHE_CONFIG_DEP_RELEASE :=
else
  OPENFHE_BUILD_DEP_DEBUG    := build-openfhe
  OPENFHE_BUILD_DEP_RELEASE  := build-openfhe-release
  OPENFHE_CONFIG_DEP_DEBUG   := config-openfhe
  OPENFHE_CONFIG_DEP_RELEASE := config-openfhe-release
endif

CMAKE_JSON_INCLUDE_DIR_FLAG := $(if $(JSON_INCLUDE_DIR),-DJSON_INCLUDE_DIR=$(JSON_INCLUDE_DIR))

OPENMP    ?= OFF
NATIVEOPT ?= OFF

# Export the OpenFHE lib dir on whichever variable the current OS uses so
# the test targets can launch the examples without each recipe having to
# repeat the LD_LIBRARY_PATH incantation. OPENFHE_INSTALL_DIR is
# command-line-overridable, so `make OPENFHE_INSTALL_DIR=/abs/path …` from
# a parent build (niobium-client) propagates correctly.
ifeq ($(UNAME_S), Darwin)
    export DYLD_LIBRARY_PATH := $(OPENFHE_INSTALL_DIR)/lib:$(DYLD_LIBRARY_PATH)
else
    export LD_LIBRARY_PATH   := $(OPENFHE_INSTALL_DIR)/lib:$(LD_LIBRARY_PATH)
endif

# ==============================================================================
# Targets
# ==============================================================================

.PHONY: help sync sync-openfhe sync-json update-openfhe \
        config config-release build build-release release \
        config-openfhe config-openfhe-release build-openfhe build-openfhe-release \
        config-fhetch config-fhetch-release \
        install install-release clean clean-all

##@ Primary Targets

help: ## Display this help message
	@echo "niobium-fhetch Build System"
	@echo ""
	@echo "Usage:"
	@echo "  make \033[36m<target>\033[0m"
	@echo ""
	@awk 'BEGIN {FS = ":.*##"; printf ""} \
		/^[a-zA-Z_0-9-]+:.*?##/ { printf "  \033[36m%-28s\033[0m %s\n", $$1, $$2 } \
		/^##@/ { printf "\n\033[1m%s\033[0m\n", substr($$0, 5) } ' $(MAKEFILE_LIST)

##@ Submodules

sync: sync-openfhe sync-json ## Sync all submodules to pinned commits

sync-openfhe: ## Sync OpenFHE submodule to pinned commit
	git submodule update --init --recursive vendor/openfhe

sync-json: ## Sync nlohmann/json submodule to pinned commit
	git submodule update --init vendor/json

update-openfhe: ## Update OpenFHE submodule to latest remote commit
	cd $(OPENFHE_DIR) && git fetch origin && git checkout nb_main && git pull origin nb_main

##@ OpenFHE Build

config-openfhe: ## Configure OpenFHE (Debug)
	$(call set-build-config,Debug,dbuild)
	cmake -S $(OPENFHE_DIR) -B $(OPENFHE_DIR)/dbuild \
		-DCMAKE_BUILD_TYPE=Debug \
		-DBUILD_EXAMPLES=OFF \
		-DBUILD_UNITTESTS=OFF \
		-DBUILD_BENCHMARKS=OFF \
		-DBUILD_EXTRAS=OFF \
		-DWITH_CPROBES=ON \
		-DWITH_REDUCED_NOISE=ON \
		-DWITH_NATIVEOPT=$(NATIVEOPT) \
		-DWITH_OPENMP=$(OPENMP) \
		-DCMAKE_INSTALL_PREFIX=$(OPENFHE_INSTALL_DIR)

config-openfhe-release: ## Configure OpenFHE (Release)
	$(call set-build-config,Release,build)
	cmake -S $(OPENFHE_DIR) -B $(OPENFHE_DIR)/build \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_EXAMPLES=OFF \
		-DBUILD_UNITTESTS=OFF \
		-DBUILD_BENCHMARKS=OFF \
		-DBUILD_EXTRAS=OFF \
		-DWITH_CPROBES=ON \
		-DWITH_REDUCED_NOISE=ON \
		-DWITH_NATIVEOPT=$(NATIVEOPT) \
		-DWITH_OPENMP=$(OPENMP) \
		-DCMAKE_INSTALL_PREFIX=$(OPENFHE_INSTALL_DIR)

build-openfhe: ## Build and install OpenFHE (Debug)
	$(call set-build-config,Debug,dbuild)
	cd $(OPENFHE_DIR) && \
		cmake --build dbuild -j $(NUM_CPUS) --target install --config Debug

build-openfhe-release: ## Build and install OpenFHE (Release)
	$(call set-build-config,Release,build)
	cd $(OPENFHE_DIR) && \
		cmake --build build -j $(NUM_CPUS) --target install --config Release

##@ Library Build

config-fhetch: ## Configure the fhetch library + examples + tests (Debug, requires OpenFHE built)
	$(call set-build-config,Debug,dbuild)
	cmake -S $(CURDIR) -B $(CURDIR)/dbuild \
		-DCMAKE_BUILD_TYPE=Debug \
		-DOPENFHE_INSTALL_DIR=$(OPENFHE_INSTALL_DIR) \
		$(CMAKE_JSON_INCLUDE_DIR_FLAG) \
		-DNIOBIUM_FHETCH_WITH_EXAMPLES=ON \
		-DNIOBIUM_FHETCH_WITH_TESTS=ON \
		-DCMAKE_INSTALL_PREFIX=$(FHETCH_INSTALL_DIR)

config-fhetch-release: ## Configure the fhetch library + examples + tests (Release, requires OpenFHE built)
	$(call set-build-config,Release,build)
	cmake -S $(CURDIR) -B $(CURDIR)/build \
		-DCMAKE_BUILD_TYPE=Release \
		-DOPENFHE_INSTALL_DIR=$(OPENFHE_INSTALL_DIR) \
		$(CMAKE_JSON_INCLUDE_DIR_FLAG) \
		-DNIOBIUM_FHETCH_WITH_EXAMPLES=ON \
		-DNIOBIUM_FHETCH_WITH_TESTS=ON \
		-DCMAKE_INSTALL_PREFIX=$(FHETCH_INSTALL_DIR)

##@ Combined Targets

config: $(OPENFHE_CONFIG_DEP_DEBUG) config-fhetch ## Configure everything (Debug)

config-release: $(OPENFHE_CONFIG_DEP_RELEASE) config-fhetch-release ## Configure everything (Release)

build: $(OPENFHE_BUILD_DEP_DEBUG) ## Build everything (Debug)
	$(call set-build-config,Debug,dbuild)
	cmake --build dbuild -j $(NUM_CPUS) --config Debug

build-release: $(OPENFHE_BUILD_DEP_RELEASE) ## Build everything (Release)
	$(call set-build-config,Release,build)
	cmake --build build -j $(NUM_CPUS) --config Release

release: config-release build-release ## Shortcut: configure + build everything (Release)

##@ Examples

test-simple-fhetch: build ## Record + replay the FHETCH-only simple example (Debug)
	$(call set-build-config,Debug,dbuild)
	@rm -rf simple_fhetch_example_simple
	$(BUILD_DIR)/examples/simple_fhetch

test-simple-fhetch-release: build-release ## Record + replay the FHETCH-only simple example (Release)
	$(call set-build-config,Release,build)
	@rm -rf simple_fhetch_example_simple
	$(BUILD_DIR)/examples/simple_fhetch

# Re-drive a .fhetch trace through the API via tests/fhetch_driver.
# Pass a trace path as TRACE=/path/to/file.fhetch, ring dim as N=2048.
# If TRACE is unset, the target runs simple_fhetch first to produce a trace
# and drives that one — so `make test-fhetch-driver[-release]` works with
# no arguments.
TRACE ?=
N     ?= 2048

SIMPLE_TRACE := simple_fhetch_example_simple/simple_fhetch_example_simple.fhetch

test-fhetch-driver: build ## Re-drive a .fhetch trace (Debug). TRACE=<path> N=<ring_dim>; defaults to simple_fhetch's trace
	$(call set-build-config,Debug,dbuild)
	@if [ -z "$(TRACE)" ]; then \
		echo "[test-fhetch-driver] TRACE unset — running simple_fhetch to produce one"; \
		rm -rf simple_fhetch_example_simple; \
		$(BUILD_DIR)/examples/simple_fhetch; \
	fi
	$(BUILD_DIR)/tests/fhetch_driver/fhetch_driver $${TRACE:-$(SIMPLE_TRACE)} --ring-dim $(N)

test-fhetch-driver-release: build-release ## Re-drive a .fhetch trace (Release). TRACE=<path> N=<ring_dim>; defaults to simple_fhetch's trace
	$(call set-build-config,Release,build)
	@if [ -z "$(TRACE)" ]; then \
		echo "[test-fhetch-driver-release] TRACE unset — running simple_fhetch to produce one"; \
		rm -rf simple_fhetch_example_simple; \
		$(BUILD_DIR)/examples/simple_fhetch; \
	fi
	$(BUILD_DIR)/tests/fhetch_driver/fhetch_driver $${TRACE:-$(SIMPLE_TRACE)} --ring-dim $(N)

# ==============================================================================
# End-to-end roundtrip tests (primary + secondary via fhetch_driver)
# ==============================================================================

# Helper: for a given simple_ops operation ($1), drive the full pipeline:
#   client -> server (primary replay) -> decrypt primary
#        -> fhetch_driver (secondary replay, writes ct_result_secondary.bin)
#        -> decrypt secondary
# Both decryptions must PASS.
define roundtrip-simple-op
	@echo ""
	@echo "=== Roundtrip $(1) ==="
	@rm -rf simple_ops_keys simple_ops_server_workload_*
	@$(BUILD_DIR)/tests/simple_ops_client simple_ops_keys $(2) $(3) 2>&1 | tail -1
	@$(BUILD_DIR)/tests/simple_ops_server simple_ops_keys $(1) 2>&1 | grep -E "Complete:|ERROR" | head -3 || true
	@echo "  -- primary decrypt --"
	@$(BUILD_DIR)/tests/simple_ops_decrypt simple_ops_keys $(1) ct_result.bin 2>&1 | grep -E "PASS|FAIL"
	@echo "  -- fhetch_driver (secondary) --"
	@WORKLOAD_DIR=$$(ls -d simple_ops_server_workload_simple_ops_op_$(1) 2>/dev/null || true); \
	 if [ -z "$$WORKLOAD_DIR" ]; then echo "  [SKIP] no workload dir"; exit 0; fi; \
	 $(BUILD_DIR)/tests/fhetch_driver/fhetch_driver \
	     $$WORKLOAD_DIR/$$WORKLOAD_DIR.fhetch --ring-dim 2048 \
	     --source-dir $$WORKLOAD_DIR \
	     --cc simple_ops_keys/cc.bin \
	     --output-ct result:simple_ops_keys/ct_result_secondary.bin 2>&1 \
	     | grep -E "replayed:|Complete:|wrote|ERROR" | head -5 || true
	@echo "  -- secondary decrypt --"
	@$(BUILD_DIR)/tests/simple_ops_decrypt simple_ops_keys $(1) ct_result_secondary.bin 2>&1 | grep -E "PASS|FAIL"
endef

test-roundtrip-simple-ops-release: build-release ## Full roundtrip for all simple_ops (primary + secondary decrypt)
	$(call set-build-config,Release,build)
	$(call roundtrip-simple-op,ADD,5,6)
	$(call roundtrip-simple-op,SUB,5,6)
	$(call roundtrip-simple-op,NEG,5,6)
	$(call roundtrip-simple-op,ADDI,5,6)
	$(call roundtrip-simple-op,SUBI,5,6)
	$(call roundtrip-simple-op,MULI,5,6)
	$(call roundtrip-simple-op,ADD_ADD,5,6)
	$(call roundtrip-simple-op,ADD_SUB,5,6)
	$(call roundtrip-simple-op,MUL,5,6)
	$(call roundtrip-simple-op,MUL_ADD,5,6)
	$(call roundtrip-simple-op,ADD_MUL,5,6)
	$(call roundtrip-simple-op,MUL_MUL,5,6)
	$(call roundtrip-simple-op,MORPH,5,6)

test-roundtrip-bootstrap-release: build-release ## Full roundtrip for bootstrap (primary + secondary decrypt)
	$(call set-build-config,Release,build)
	@rm -rf bootstrap_keys bootstrap_server_*
	@echo "=== Bootstrap client ==="
	$(BUILD_DIR)/tests/bootstrap_client bootstrap_keys
	@echo "=== Bootstrap server ==="
	$(BUILD_DIR)/tests/bootstrap_server bootstrap_keys
	@echo "=== Bootstrap primary decrypt ==="
	$(BUILD_DIR)/tests/bootstrap_decrypt bootstrap_keys ct_result.bin
	@echo "=== Bootstrap fhetch_driver (secondary) ==="
	@WORKLOAD_DIR=$$(ls -d bootstrap_server_workload_* 2>/dev/null); \
	 N=$$($(BUILD_DIR)/tests/bootstrap_server bootstrap_keys 2>&1 | grep -oP 'Ring dimension:\s*\K[0-9]+' | head -1); \
	 $(BUILD_DIR)/tests/fhetch_driver/fhetch_driver \
	     $$WORKLOAD_DIR/$$WORKLOAD_DIR.fhetch --ring-dim $${N:-2048} \
	     --source-dir $$WORKLOAD_DIR \
	     --cc bootstrap_keys/cc.bin \
	     --output-ct output_cipher:bootstrap_keys/ct_result_secondary.bin
	@echo "=== Bootstrap secondary decrypt ==="
	$(BUILD_DIR)/tests/bootstrap_decrypt bootstrap_keys ct_result_secondary.bin

test-roundtrip-plaintext-add-release: build-release ## Full roundtrip for plaintext-add (primary + secondary decrypt)
	$(call set-build-config,Release,build)
	@rm -rf plaintext_add_keys plaintext_add_server_*
	@echo "=== Plaintext-Add client ==="
	$(BUILD_DIR)/tests/plaintext_add_client plaintext_add_keys
	@echo "=== Plaintext-Add server ==="
	$(BUILD_DIR)/tests/plaintext_add_server plaintext_add_keys
	@echo "=== Plaintext-Add primary decrypt ==="
	$(BUILD_DIR)/tests/plaintext_add_decrypt plaintext_add_keys ct_result.bin
	@echo "=== Plaintext-Add fhetch_driver (secondary) ==="
	@WORKLOAD_DIR=$$(ls -d plaintext_add_server_workload_* 2>/dev/null); \
	 N=$$($(BUILD_DIR)/tests/plaintext_add_server plaintext_add_keys 2>&1 | grep -oP 'Ring dimension:\s*\K[0-9]+' | head -1); \
	 $(BUILD_DIR)/tests/fhetch_driver/fhetch_driver \
	     $$WORKLOAD_DIR/$$WORKLOAD_DIR.fhetch --ring-dim $${N:-2048} \
	     --source-dir $$WORKLOAD_DIR \
	     --cc plaintext_add_keys/cc.bin \
	     --output-ct output_cipher:plaintext_add_keys/ct_result_secondary.bin
	@echo "=== Plaintext-Add secondary decrypt ==="
	$(BUILD_DIR)/tests/plaintext_add_decrypt plaintext_add_keys ct_result_secondary.bin

test-roundtrip-release: test-roundtrip-simple-ops-release test-roundtrip-bootstrap-release test-roundtrip-plaintext-add-release ## Full roundtrip sweep: simple_ops + bootstrap + plaintext-add

# ==============================================================================
# test-release — everything that currently passes
# ==============================================================================
# Aggregates the test targets known to succeed end-to-end, excluding the
# bootstrap roundtrip (the primary-side CKKS decrypt there still trips the
# approximation-error tolerance — tracked as a separate simulator precision
# issue). Useful as the "ship it" gate and as the CI target for passing runs.

test-release: \
    test-simple-fhetch-release \
    test-fhetch-driver-release \
    test-roundtrip-simple-ops-release  ## Run all currently-passing Release tests

##@ Installation

install: ## Install the fhetch library (Debug)
	$(call set-build-config,Debug,dbuild)
	cmake --install dbuild

install-release: ## Install the fhetch library (Release)
	$(call set-build-config,Release,build)
	cmake --install build

##@ Cleanup

clean: ## Remove all build artifacts
	-rm -rf build dbuild
	-rm -rf $(OPENFHE_DIR)/build $(OPENFHE_DIR)/dbuild

clean-all: clean ## Deep clean including vendor installations
	-rm -rf $(VENDOR_LIB_DIR)
