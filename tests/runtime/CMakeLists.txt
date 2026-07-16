cmake_minimum_required(VERSION 3.16)
project(niobium_runtime_acceptance CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Acceptance consumers for the assembled niobium-runtime prefix. The fhetch source tree
# is NOT on the include/link path; everything comes from the artifact via find_package
# plus the prefix's own headers/libs. This proves the packaged runtime is a
# self-contained, SDK-less runtime an external project can build and run against.
#
# find_package(NiobiumFhetch) brings the imported target Niobium::niobium_fhetch and,
# critically, the PUBLIC -DOPENFHE_CPROBES it carries (without which recording on
# deserialized ciphertexts silently drops the input copy-ins). OpenFHE include/lib dirs
# are taken from NIOBIUM_RUNTIME_PREFIX directly (niobium_fhetch exposes OpenFHE
# BUILD_INTERFACE-only, and the installed OpenFHEConfig is not yet relocatable).

find_package(NiobiumFhetch REQUIRED)

if(NOT NIOBIUM_RUNTIME_PREFIX)
  message(FATAL_ERROR "pass -DNIOBIUM_RUNTIME_PREFIX=<staged prefix>")
endif()

set(_openfhe_inc
    ${NIOBIUM_RUNTIME_PREFIX}/include/openfhe
    ${NIOBIUM_RUNTIME_PREFIX}/include/openfhe/core
    ${NIOBIUM_RUNTIME_PREFIX}/include/openfhe/pke
    ${NIOBIUM_RUNTIME_PREFIX}/include/openfhe/binfhe)

# --- simple_fhetch: FHETCH IR + simulator path (no OpenFHE crypto in user code) -------
# Mirrors the fhetch `test-simple-fhetch` target at the artifact level.
if(SIMPLE_FHETCH_SRC)
  add_executable(runtime_simple_fhetch ${SIMPLE_FHETCH_SRC})
  target_include_directories(runtime_simple_fhetch PRIVATE ${_openfhe_inc})
  # niobium_fhetch's exported interface pulls in the OpenFHE libs by bare name (no path);
  # give the linker the prefix lib dir so they resolve (on Linux they are not absolute).
  target_link_directories(runtime_simple_fhetch PRIVATE ${NIOBIUM_RUNTIME_PREFIX}/lib)
  target_link_libraries(runtime_simple_fhetch PRIVATE Niobium::niobium_fhetch)
  set_target_properties(runtime_simple_fhetch PROPERTIES BUILD_RPATH ${NIOBIUM_RUNTIME_PREFIX}/lib)
endif()

# --- simple_ops: instrumented-OpenFHE crypto recording path (client/server/decrypt) ---
# Mirrors the fhetch `test-simple-ops` roundtrip at the artifact level (primary
# fhetch_sim replay only; the artifact ships no fhetch_driver for the secondary).
if(SIMPLE_OPS_DIR)
  foreach(_stage client server decrypt)
    add_executable(runtime_simple_ops_${_stage} ${SIMPLE_OPS_DIR}/${_stage}.cpp)
    target_include_directories(runtime_simple_ops_${_stage} PRIVATE ${_openfhe_inc})
    target_link_directories(runtime_simple_ops_${_stage} PRIVATE ${NIOBIUM_RUNTIME_PREFIX}/lib)
    target_link_libraries(runtime_simple_ops_${_stage} PRIVATE
        Niobium::niobium_fhetch OPENFHEpke OPENFHEbinfhe OPENFHEcore)
    set_target_properties(runtime_simple_ops_${_stage} PROPERTIES
        BUILD_RPATH ${NIOBIUM_RUNTIME_PREFIX}/lib)
  endforeach()
endif()
