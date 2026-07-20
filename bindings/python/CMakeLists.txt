# niobium-fhetch Python bindings — the two extension modules that share the
# single loaded instrumented OpenFHE + libnbfhetch (so probes fire into the
# active recording session):
#   * openfhe         — vendored openfhe-python, built UNMODIFIED via its own CMake
#                       (ExternalProject; its CMakeLists uses CMAKE_SOURCE_DIR and a
#                       site-packages install that don't compose under add_subdirectory)
#   * niobium_session — the niobium::compiler() session bindings (our source)
#
# Dual-mode:
#   * Component (add_subdirectory(bindings/python) from the top-level CMakeLists under
#     WITH_PYTHON): the from-source co-dev build. The parent defines the niobium_fhetch
#     target + the OPENFHE_* vars, and OpenFHE is built from the vendored fork.
#   * Standalone / runtime (cmake -S bindings/python -DNIOBIUM_RUNTIME_PREFIX=<extracted
#     niobium-runtime>): builds the SAME sources against the prebuilt runtime — no OpenFHE
#     compile. This is the path the niobium-client wheel drives for a release build.
# Both land the two modules in ${CMAKE_BINARY_DIR}/python — where the wheel picks them up.
cmake_minimum_required(VERSION 3.16)

# A standalone invocation needs its own project(); under add_subdirectory the parent set it.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(niobium_fhetch_python CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

# Runtime mode is selected by NIOBIUM_RUNTIME_PREFIX (-D or env), so it works whether the
# recipe is invoked standalone or added by the client wheel via add_subdirectory. Without
# it, component/from-source mode: the parent defines niobium_fhetch + the OPENFHE_* vars.
if(NOT NIOBIUM_RUNTIME_PREFIX)
    set(NIOBIUM_RUNTIME_PREFIX "$ENV{NIOBIUM_RUNTIME_PREFIX}")
endif()
if(NIOBIUM_RUNTIME_PREFIX)
    # --- Runtime mode: consume a prebuilt niobium-runtime prefix (no OpenFHE compile) ---
    set(NB_PY_RUNTIME_MODE ON)
    if(NOT EXISTS "${NIOBIUM_RUNTIME_PREFIX}/lib/cmake/NiobiumFhetch")
        message(FATAL_ERROR
            "NIOBIUM_RUNTIME_PREFIX does not point at an extracted niobium-runtime prefix")
    endif()
    set(RT "${NIOBIUM_RUNTIME_PREFIX}")
    list(APPEND CMAKE_PREFIX_PATH "${RT}")
    find_package(NiobiumFhetch REQUIRED)   # imports Niobium::niobium_fhetch + OPENFHE* targets
    set(NB_OPENFHE_DIR     "${RT}/lib/OpenFHE")
    set(NB_FHETCH_TARGET   Niobium::niobium_fhetch)
    set(NB_OPENFHE_LIBPATH "${RT}/lib")
    # The imported runtime target exposes the OpenFHE includes BUILD_INTERFACE-only, so
    # niobium_session wires them (and the OPENFHE libs) explicitly in this mode (below).
    set(NB_OPENFHE_INC
        ${RT}/include/openfhe ${RT}/include/openfhe/core
        ${RT}/include/openfhe/pke ${RT}/include/openfhe/binfhe)
else()
    # --- Component mode: from-source co-dev build; the parent provides the deps ---
    set(NB_PY_RUNTIME_MODE OFF)
    set(NB_OPENFHE_DIR     "${OPENFHE_INSTALL_DIR}/lib/OpenFHE")
    set(NB_FHETCH_TARGET   niobium_fhetch)
    set(NB_OPENFHE_LIBPATH "${OPENFHE_LIBRARY_PATH}")
endif()

# The fhetch source root (two levels up from bindings/python/): openfhe-python is vendored there.
get_filename_component(NB_FHETCH_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
find_package(pybind11 CONFIG REQUIRED)

set(NB_PY_OUTDIR "${CMAKE_BINARY_DIR}/python")
file(MAKE_DIRECTORY "${NB_PY_OUTDIR}")

# --- Crypto module: vendored openfhe-python, built via its own CMake ---
# Build from a PATCHED COPY, not the submodule in place. openfhe-python's
# CMakeLists requests full Python "Development" (which needs Development.Embed =
# libpython). manylinux Pythons are module-only (no libpython), so that find
# fails there — yet openfhe-python never links libpython: pybind11_add_module
# builds the extension, and the find_package(Python) result is used only for the
# interpreter version + exec_prefix. Rewrite it to Development.Module (the correct
# component for an extension module) in a build-tree copy, leaving the vendored
# submodule pristine (no dirty working tree, no committed delta). Upstream ships
# Ubuntu-specific wheels (system libpython present), so this path was never hit there.
set(NB_OFHE_PY_COPY "${CMAKE_BINARY_DIR}/openfhe-python-src")
file(REMOVE_RECURSE "${NB_OFHE_PY_COPY}")
file(COPY "${NB_FHETCH_ROOT}/vendor/openfhe-python/"
     DESTINATION "${NB_OFHE_PY_COPY}")
file(READ "${NB_OFHE_PY_COPY}/CMakeLists.txt" _nb_ofhe_cml)
string(REPLACE "COMPONENTS Interpreter Development)"
               "COMPONENTS Interpreter Development.Module)"
               _nb_ofhe_patched "${_nb_ofhe_cml}")
if(_nb_ofhe_cml STREQUAL _nb_ofhe_patched)
    message(WARNING "openfhe-python Development->Development.Module patch did not "
                    "apply (upstream CMakeLists changed?); manylinux builds may fail.")
endif()
file(WRITE "${NB_OFHE_PY_COPY}/CMakeLists.txt" "${_nb_ofhe_patched}")

include(ExternalProject)
ExternalProject_Add(openfhe_python
    SOURCE_DIR       "${NB_OFHE_PY_COPY}"
    DOWNLOAD_COMMAND ""
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        # Match libnbfhetch: the OpenFHE probe hooks are header-inlined and
        # guarded by OPENFHE_CPROBES, and the OpenFHE config does NOT
        # propagate the define. Without it, openfhe.so's inlined clone/copy
        # and Cereal-deserialize id-assignment probes compile out, so
        # recording on *deserialized* ciphertexts drops the input copy-in
        # ops (inputs replay as zero). WITH_CPROBES in the OpenFHE build is
        # exactly `add_definitions(-DOPENFHE_CPROBES)`.
        -DCMAKE_CXX_FLAGS=-DOPENFHE_CPROBES
        -DOpenFHE_DIR=${NB_OPENFHE_DIR}
        -DOPENFHE_REQUIRED_VERSION=1.4.2
        -Dpybind11_DIR=${pybind11_DIR}
        -DPython_EXECUTABLE=${Python_EXECUTABLE}
        -DPYTHON_EXECUTABLE_PATH=${Python_EXECUTABLE}
        -DCMAKE_INSTALL_PREFIX=${NB_PY_OUTDIR}
    BUILD_ALWAYS ON
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config ${CMAKE_BUILD_TYPE}
)

# --- Session module: niobium::compiler() bindings (our source) ---
# Component mode: niobium_fhetch PUBLIC-propagates the OpenFHE includes/libs and the
# OPENFHE_CPROBES define, so the headers compile identically to the library. Runtime
# mode: the imported target propagates less, so wire OpenFHE explicitly (see above).
pybind11_add_module(niobium_session niobium_session.cpp)
target_link_libraries(niobium_session PRIVATE ${NB_FHETCH_TARGET})
if(NB_PY_RUNTIME_MODE)
    target_include_directories(niobium_session PRIVATE ${NB_OPENFHE_INC})
    target_link_directories(niobium_session PRIVATE ${RT}/lib)
    target_link_libraries(niobium_session PRIVATE OPENFHEpke OPENFHEbinfhe OPENFHEcore)
endif()
set_target_properties(niobium_session PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${NB_PY_OUTDIR}")
if(APPLE)
    set_target_properties(niobium_session PROPERTIES
        BUILD_RPATH "@loader_path;${NB_OPENFHE_LIBPATH}")
else()
    set_target_properties(niobium_session PROPERTIES
        BUILD_RPATH "$ORIGIN;${NB_OPENFHE_LIBPATH}")
endif()
