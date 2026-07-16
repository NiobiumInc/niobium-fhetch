# make/runtime.mk — build and package the niobium-runtime binary artifact.
#
# niobium-runtime is the shared, language-neutral C++ core: instrumented OpenFHE +
# libnbfhetch + fhetch_sim + headers + CMake config + a manifest. It is built once per
# platform and published as a versioned binary artifact that downstream language
# channels (e.g. niobium-sdk-python) consume instead of building OpenFHE from source.
#
# Included by the root Makefile; shares its variable namespace (VENDOR_DIR,
# VENDOR_LIB_DIR, OPENFHE_DIR, OPENFHE_INSTALL_DIR, FHETCH_INSTALL_DIR, NUM_CPUS,
# CURDIR, CMAKE_JSON_INCLUDE_DIR_FLAG, EXTERNAL_OPENFHE).

# Version tracks the fhetch library (project(NiobiumFhetch VERSION ...)); the OpenFHE
# version, pins, and interop tag live in the manifest, not the version number.
RUNTIME_VERSION  := $(shell grep -oE 'NiobiumFhetch VERSION [0-9]+\.[0-9]+\.[0-9]+' $(CURDIR)/CMakeLists.txt | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
RUNTIME_OS       := $(shell uname -s | tr '[:upper:]' '[:lower:]')
RUNTIME_ARCH     := $(shell uname -m)
RUNTIME_ID       := niobium-runtime-$(RUNTIME_VERSION)-$(RUNTIME_OS)-$(RUNTIME_ARCH)
# Bundle only this platform's shared libs, so a vendor/lib left over from a different
# platform (e.g. building macOS then Linux in one tree) can't contaminate the artifact.
ifeq ($(RUNTIME_OS),darwin)
  RUNTIME_LIB_EXT   := dylib
  RUNTIME_WRONG_EXT := so
else
  RUNTIME_LIB_EXT   := so
  RUNTIME_WRONG_EXT := dylib
endif
RUNTIME_BUILD    := $(CURDIR)/build-runtime
RUNTIME_STAGE    := $(RUNTIME_BUILD)/$(RUNTIME_ID)
RUNTIME_TARBALL  := $(CURDIR)/dist/$(RUNTIME_ID).tar.gz
RUNTIME_TESTS    := $(RUNTIME_BUILD)/tests
# Template assets for this fragment: manifest.json.in + NiobiumFhetchConfig.cmake.
RUNTIME_TEMPLATES := $(CURDIR)/make/runtime

RUNTIME_OPENFHE_VERSION := $(shell grep -oE 'OPENFHE_VERSION_(MAJOR|MINOR|PATCH) [0-9]+' $(OPENFHE_DIR)/CMakeLists.txt | grep -oE '[0-9]+' | paste -sd. -)
RUNTIME_OPENFHE_PIN     := $(shell git -C $(OPENFHE_DIR) rev-parse HEAD 2>/dev/null)
RUNTIME_FHETCH_PIN      := $(shell git -C $(CURDIR) rev-parse HEAD 2>/dev/null)

# Loader path so the acceptance binaries + the fhetch_sim subprocess resolve the libs.
RUNTIME_RUN_ENV = DYLD_LIBRARY_PATH=$(RUNTIME_STAGE)/lib:$$DYLD_LIBRARY_PATH \
                  LD_LIBRARY_PATH=$(RUNTIME_STAGE)/lib:$$LD_LIBRARY_PATH \
                  NBCC_FHETCH_SIM=$(RUNTIME_STAGE)/bin/fhetch_sim

.PHONY: runtime build-runtime assemble-runtime package-runtime build-runtime-tests \
        test-runtime test-runtime-simple-fhetch test-runtime-simple-ops clean-runtime

##@ Runtime artifact (niobium-runtime)

runtime: build-runtime assemble-runtime package-runtime ## Build, assemble, and package niobium-runtime

# Build the runtime's C++ pieces: instrumented OpenFHE (reusing the repo's own
# config/build-openfhe targets) + a lean libnbfhetch/fhetch_sim (no examples/tests),
# each installed to its vendor/lib prefix. EXTERNAL_OPENFHE=1 skips the OpenFHE build.
build-runtime: ## Build + install OpenFHE and a lean libnbfhetch/fhetch_sim
ifneq ($(EXTERNAL_OPENFHE),1)
	$(MAKE) config-openfhe-release
	$(MAKE) build-openfhe-release
endif
	cmake -S $(CURDIR) -B $(RUNTIME_BUILD)/fhetch \
		-DCMAKE_BUILD_TYPE=Release \
		-DOPENFHE_INSTALL_DIR=$(OPENFHE_INSTALL_DIR) \
		$(CMAKE_JSON_INCLUDE_DIR_FLAG) \
		-DNIOBIUM_FHETCH_WITH_EXAMPLES=OFF \
		-DNIOBIUM_FHETCH_WITH_TESTS=OFF \
		-DWITH_FHETCH_SIM=ON \
		-DCMAKE_INSTALL_LIBDIR=lib \
		-DCMAKE_INSTALL_PREFIX=$(FHETCH_INSTALL_DIR)
	cmake --build $(RUNTIME_BUILD)/fhetch -j $(NUM_CPUS) --config Release
	cmake --install $(RUNTIME_BUILD)/fhetch

# Merge the two install prefixes (OpenFHE + fhetch) into one staged prefix, preserving
# the standard install layout (lib/, lib/OpenFHE, lib/cmake/NiobiumFhetch, include/,
# bin/) so the exported CMake configs' relative paths resolve to the prefix root, then
# make it relocatable: the dylibs already use @rpath/$$ORIGIN install-names + deps, but
# OpenFHEConfig ships absolute paths and fhetch_sim has no self-rpath.
assemble-runtime: ## Stage the (relocatable) niobium-runtime prefix
	rm -rf $(RUNTIME_STAGE)
	mkdir -p $(RUNTIME_STAGE)/lib $(RUNTIME_STAGE)/bin $(RUNTIME_STAGE)/include \
	         $(RUNTIME_STAGE)/share/licenses
	cp -a $(OPENFHE_INSTALL_DIR)/lib/libOPENFHE*.$(RUNTIME_LIB_EXT)*  $(RUNTIME_STAGE)/lib/
	cp -a $(FHETCH_INSTALL_DIR)/lib/libnbfhetch*.$(RUNTIME_LIB_EXT)*  $(RUNTIME_STAGE)/lib/
	cp -a $(OPENFHE_INSTALL_DIR)/lib/OpenFHE                $(RUNTIME_STAGE)/lib/
	cp -a $(FHETCH_INSTALL_DIR)/lib/cmake                   $(RUNTIME_STAGE)/lib/
	cp    $(FHETCH_INSTALL_DIR)/bin/fhetch_sim              $(RUNTIME_STAGE)/bin/
	cp -a $(OPENFHE_INSTALL_DIR)/include/openfhe            $(RUNTIME_STAGE)/include/
	cp -a $(FHETCH_INSTALL_DIR)/include/niobium             $(RUNTIME_STAGE)/include/
	cp $(RUNTIME_TEMPLATES)/NiobiumFhetchConfig.cmake \
		$(RUNTIME_STAGE)/lib/cmake/NiobiumFhetch/NiobiumFhetchConfig.cmake
	# Relocatability: rewrite OpenFHEConfig's absolute OpenFHE_INCLUDE/LIBDIR relative to
	# the config file, and give bin/fhetch_sim a self-rpath to the bundled libs.
	sed -i.bak \
		-e 's|^set(OpenFHE_INCLUDE .*|set(OpenFHE_INCLUDE "$${OpenFHE_CMAKE_DIR}/../../include/openfhe")|' \
		-e 's|^set(OpenFHE_LIBDIR .*|set(OpenFHE_LIBDIR "$${OpenFHE_CMAKE_DIR}/../../lib")|' \
		$(RUNTIME_STAGE)/lib/OpenFHE/OpenFHEConfig.cmake
	rm -f $(RUNTIME_STAGE)/lib/OpenFHE/OpenFHEConfig.cmake.bak
ifeq ($(RUNTIME_OS),darwin)
	install_name_tool -add_rpath @loader_path/../lib $(RUNTIME_STAGE)/bin/fhetch_sim
else
	patchelf --set-rpath '$$ORIGIN/../lib' $(RUNTIME_STAGE)/bin/fhetch_sim
endif
	cp $(CURDIR)/LICENSE      $(RUNTIME_STAGE)/share/licenses/LICENSE.niobium-fhetch
	cp $(OPENFHE_DIR)/LICENSE $(RUNTIME_STAGE)/share/licenses/LICENSE.OpenFHE
	sed -e 's|@RUNTIME_VERSION@|$(RUNTIME_VERSION)|g' \
	    -e 's|@RUNTIME_PLATFORM@|$(RUNTIME_OS)-$(RUNTIME_ARCH)|g' \
	    -e 's|@RUNTIME_OPENFHE_VERSION@|$(RUNTIME_OPENFHE_VERSION)|g' \
	    -e 's|@RUNTIME_OPENFHE_PIN@|$(RUNTIME_OPENFHE_PIN)|g' \
	    -e 's|@RUNTIME_FHETCH_PIN@|$(RUNTIME_FHETCH_PIN)|g' \
	    $(RUNTIME_TEMPLATES)/manifest.json.in > $(RUNTIME_STAGE)/manifest.json
	@if ls $(RUNTIME_STAGE)/lib/*.$(RUNTIME_WRONG_EXT) >/dev/null 2>&1; then \
		echo "ERROR: .$(RUNTIME_WRONG_EXT) libs in a $(RUNTIME_OS) runtime — cross-platform contamination (run 'make clean-all' between platforms)"; \
		exit 1; fi
	@echo "assembled: $(RUNTIME_STAGE)"

package-runtime: ## Tar the assembled prefix into dist/
	mkdir -p $(CURDIR)/dist
	tar -czf $(RUNTIME_TARBALL) -C $(RUNTIME_BUILD) $(RUNTIME_ID)
	@echo "packaged: $(RUNTIME_TARBALL)"

# Acceptance tests: an EXTERNAL project find_package()s the staged prefix (fhetch source
# tree NOT on the include/link path) and exercises both artifact surfaces. Target names
# mirror the fhetch build-tree tests one-to-one (test-simple-fhetch, test-simple-ops), so
# the artifact-level coverage maps directly to the library-level tests.
build-runtime-tests: ## Build the external acceptance consumers against the staged prefix
	cmake -S $(CURDIR)/tests/runtime -B $(RUNTIME_TESTS) \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_PREFIX_PATH=$(RUNTIME_STAGE) \
		-DNIOBIUM_RUNTIME_PREFIX=$(RUNTIME_STAGE) \
		-DSIMPLE_FHETCH_SRC=$(CURDIR)/examples/fhetch/simple_fhetch.cpp \
		-DSIMPLE_OPS_DIR=$(CURDIR)/tests/simple_ops
	cmake --build $(RUNTIME_TESTS) -j $(NUM_CPUS)

test-runtime: test-runtime-simple-fhetch test-runtime-simple-ops ## Artifact acceptance: IR + crypto paths

# FHETCH IR + simulator path (record -> replay via fhetch_sim). Mirrors test-simple-fhetch.
test-runtime-simple-fhetch: build-runtime-tests ## Artifact acceptance: the FHETCH-IR recorder path
	cd $(RUNTIME_TESTS) && $(RUNTIME_RUN_ENV) ./runtime_simple_fhetch --no-ring-dim-check
	@echo "test-runtime-simple-fhetch: PASS ($(RUNTIME_ID))"

# Instrumented-OpenFHE crypto recording path: client encrypts+serializes, server
# deserializes+records+replays via fhetch_sim, decrypt checks the result. Mirrors
# test-simple-ops (primary path only; the fhetch_driver secondary is a library test —
# the artifact ships fhetch_sim, not fhetch_driver).
test-runtime-simple-ops: build-runtime-tests ## Artifact acceptance: the instrumented-OpenFHE crypto path
	cd $(RUNTIME_TESTS) && rm -rf simple_ops_keys simple_ops_server_workload_* && \
		$(RUNTIME_RUN_ENV) ./runtime_simple_ops_client  simple_ops_keys 5 6 >/dev/null && \
		$(RUNTIME_RUN_ENV) ./runtime_simple_ops_server  simple_ops_keys MUL --no-ring-dim-check >/dev/null && \
		$(RUNTIME_RUN_ENV) ./runtime_simple_ops_decrypt simple_ops_keys MUL ct_result.bin | grep -qE 'PASS'
	@echo "test-runtime-simple-ops: PASS (MUL 30, $(RUNTIME_ID))"

clean-runtime: ## Remove the runtime build + packaged artifacts
	-rm -rf $(RUNTIME_BUILD)
	-rm -f  $(CURDIR)/dist/niobium-runtime-*.tar.gz
