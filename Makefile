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
    export DYLD_LIBRARY_PATH := $(CURDIR)/vendor/lib/openfhe/lib:$(DYLD_LIBRARY_PATH)
  else
    NUM_CPUS := $(shell nproc)
    export LD_LIBRARY_PATH := $(CURDIR)/vendor/lib/openfhe/lib:$(LD_LIBRARY_PATH)
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

OPENFHE_DIR          := $(VENDOR_DIR)/openfhe
OPENFHE_INSTALL_DIR  := $(VENDOR_LIB_DIR)/openfhe
FHETCH_INSTALL_DIR   := $(VENDOR_LIB_DIR)/niobium-fhetch

OPENMP    ?= OFF
NATIVEOPT ?= OFF

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
		-DNIOBIUM_FHETCH_WITH_EXAMPLES=ON \
		-DNIOBIUM_FHETCH_WITH_TESTS=ON \
		-DCMAKE_INSTALL_PREFIX=$(FHETCH_INSTALL_DIR)

config-fhetch-release: ## Configure the fhetch library + examples + tests (Release, requires OpenFHE built)
	$(call set-build-config,Release,build)
	cmake -S $(CURDIR) -B $(CURDIR)/build \
		-DCMAKE_BUILD_TYPE=Release \
		-DOPENFHE_INSTALL_DIR=$(OPENFHE_INSTALL_DIR) \
		-DNIOBIUM_FHETCH_WITH_EXAMPLES=ON \
		-DNIOBIUM_FHETCH_WITH_TESTS=ON \
		-DCMAKE_INSTALL_PREFIX=$(FHETCH_INSTALL_DIR)

##@ Combined Targets

config: config-openfhe config-fhetch ## Configure everything (Debug)

config-release: config-openfhe-release config-fhetch-release ## Configure everything (Release)

build: build-openfhe ## Build everything (Debug)
	$(call set-build-config,Debug,dbuild)
	cmake --build dbuild -j $(NUM_CPUS) --config Debug

build-release: build-openfhe-release ## Build everything (Release)
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
