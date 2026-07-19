cmake_minimum_required(VERSION 3.16)
project(niobium_runtime_acceptance CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Acceptance consumers for the assembled niobium-runtime prefix. The fhetch source tree
# is NOT on the include/link path — everything comes from the artifact via
# find_package(NiobiumFhetch) alone: it brings Niobium::niobium_fhetch with OpenFHE
# (headers + libs, via the config's find_dependency(OpenFHE) + imported targets) and the
# PUBLIC -DOPENFHE_CPROBES define (without which recording on deserialized ciphertexts
# silently drops the input copy-ins). This proves the packaged runtime is a
# self-contained SDK an external project builds against with find_package and nothing else.

find_package(NiobiumFhetch REQUIRED)

if(NOT NIOBIUM_RUNTIME_PREFIX)
  message(FATAL_ERROR "pass -DNIOBIUM_RUNTIME_PREFIX=<staged prefix>")
endif()

# --- simple_fhetch: FHETCH IR + simulator path (no OpenFHE crypto in user code) -------
# Mirrors the fhetch `test-simple-fhetch` target at the artifact level.
if(SIMPLE_FHETCH_SRC)
  add_executable(runtime_simple_fhetch ${SIMPLE_FHETCH_SRC})
  target_link_libraries(runtime_simple_fhetch PRIVATE Niobium::niobium_fhetch)
  # Runtime rpath so the bundled libs load without LD_LIBRARY_PATH.
  set_target_properties(runtime_simple_fhetch PROPERTIES BUILD_RPATH ${NIOBIUM_RUNTIME_PREFIX}/lib)
endif()

# --- simple_ops: instrumented-OpenFHE crypto recording path (client/server/decrypt) ---
# Mirrors the fhetch `test-simple-ops` roundtrip at the artifact level (primary
# fhetch_sim replay only; the artifact ships no fhetch_driver for the secondary).
if(SIMPLE_OPS_DIR)
  foreach(_stage client server decrypt)
    add_executable(runtime_simple_ops_${_stage} ${SIMPLE_OPS_DIR}/${_stage}.cpp)
    target_link_libraries(runtime_simple_ops_${_stage} PRIVATE Niobium::niobium_fhetch)
    set_target_properties(runtime_simple_ops_${_stage} PROPERTIES
        BUILD_RPATH ${NIOBIUM_RUNTIME_PREFIX}/lib)
  endforeach()
endif()
