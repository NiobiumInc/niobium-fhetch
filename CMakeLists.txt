cmake_minimum_required(VERSION 3.16)

project(NiobiumFhetch VERSION 1.0.0 LANGUAGES CXX)

# Use ccache if available
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
endif()

# C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default build type
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Debug, Release, RelWithDebInfo or MinSizeRel" FORCE)
endif()

# Compiler flags
# -Wno-ignored-qualifiers and -Wno-missing-field-initializers are required for
# OpenFHE headers which trigger these warnings under -Werror.
# -Wunused-but-set-variable is in clang's -Wall but not in GCC's, so add it
# explicitly so both toolchains catch the same class of dead-code bugs locally
# before CI (macOS runners use clang; Linux dev machines often use GCC).
set(CMAKE_CXX_FLAGS_DEBUG   "-Og -g -Wall -Wextra -Wunused-but-set-variable -Wno-unknown-pragmas -Wno-unused-parameter -Wno-ignored-qualifiers -Wno-missing-field-initializers -Werror"
    CACHE STRING "C++ flags for Debug builds" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -Wall -Wextra -Wunused-but-set-variable -Wno-unknown-pragmas -Wno-unused-parameter -Wno-ignored-qualifiers -Wno-missing-field-initializers -Werror"
    CACHE STRING "C++ flags for Release builds" FORCE)

option(BUILD_SHARED_LIBS   "Build shared libraries instead of static ones" ON)
option(WITH_FHETCH_SIM     "Build the standalone FHETCH simulator CLI (fhetch_sim). The simulator is always linked into the library because Compiler::replay() uses it." ON)
option(WITH_PYTHON         "Build the Python bindings: the vendored openfhe-python crypto module + the niobium::compiler() session module. Requires pybind11 + Python dev headers (pass -Dpybind11_DIR and -DPython_EXECUTABLE)." OFF)

# Link OpenFHE's *_static.a archives instead of the shared OPENFHE* libraries.
# Default OFF preserves the historical shared link exactly. Turned ON by parents
# (e.g. haze) that absorb OpenFHE into a single symbol-isolated shared object;
# requires an OpenFHE install built with -DBUILD_STATIC=ON under OPENFHE_LIBRARY_PATH.
option(NIOBIUM_FHETCH_OPENFHE_STATIC
       "Link OpenFHE's *_static.a archives instead of the shared OPENFHE* libraries." OFF)

# ==============================================================================
# OpenFHE dependency (built and installed separately via Makefile targets)
# ==============================================================================

set(OPENFHE_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/lib/openfhe"
    CACHE PATH "Path to installed OpenFHE (set by Makefile)")

set(OPENFHE_INCLUDE_DIRS
    "${OPENFHE_INSTALL_DIR}/include/openfhe"
    "${OPENFHE_INSTALL_DIR}/include/openfhe/core"
    "${OPENFHE_INSTALL_DIR}/include/openfhe/pke"
    "${OPENFHE_INSTALL_DIR}/include/openfhe/binfhe"
)

set(OPENFHE_LIBRARY_PATH "${OPENFHE_INSTALL_DIR}/lib")

if(NIOBIUM_FHETCH_OPENFHE_STATIC)
  # Topological order (dependents first): pke depends on binfhe + core, binfhe
  # on core. Required when an executable (e.g. fhetch_sim --project) pulls in
  # pke's CryptoContext-deserialize/scheme-switching back-references to binfhe.
  set(OPENFHE_LIBRARIES
      OPENFHEpke_static
      OPENFHEbinfhe_static
      OPENFHEcore_static
  )
else()
  set(OPENFHE_LIBRARIES
      OPENFHEbinfhe
      OPENFHEcore
      OPENFHEpke
  )
endif()

# nlohmann/json (header-only, single_include).
# Cache var so a parent project (e.g. niobium-compiler) can inject its own
# json submodule path via -DJSON_INCLUDE_DIR=... and we don't have to sync
# our own vendor/json submodule. Empty cache value falls back to the local
# vendored submodule for standalone builds.
set(JSON_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/json/single_include"
    CACHE PATH "Path to nlohmann/json single_include directory")

# ==============================================================================
# Library sources
# ==============================================================================

set(LIB_SOURCES
    src/auto_facade.cpp
    src/compiler.cpp
    src/fhetch_api.cpp
    src/fhetch_parser.cpp
    src/fhetch_sim/instruction.cpp
    src/fhetch_sim/simulator.cpp
    src/local_replay.cpp
    src/probes.cpp
    src/trace_writer.cpp
)

# ==============================================================================
# Library target
# ==============================================================================

add_library(niobium_fhetch ${LIB_SOURCES})
add_library(Niobium::fhetch ALIAS niobium_fhetch)

# Output name: libnbfhetch.so / libnbfhetch.a
set_target_properties(niobium_fhetch PROPERTIES
    OUTPUT_NAME "nbfhetch"
    VERSION     ${PROJECT_VERSION}
    SOVERSION   1
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(niobium_fhetch
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/fhetch_sim
        ${JSON_INCLUDE_DIR}
)

# OpenFHE include paths — BUILD_INTERFACE only (consumers set their own).
foreach(_dir ${OPENFHE_INCLUDE_DIRS})
  target_include_directories(niobium_fhetch
      PUBLIC $<BUILD_INTERFACE:${_dir}>)
endforeach()

# Enable OPENFHE_CPROBES so the instrumented branch activates probes
target_compile_definitions(niobium_fhetch PUBLIC OPENFHE_CPROBES)

find_package(Threads REQUIRED)
target_link_libraries(niobium_fhetch PRIVATE Threads::Threads)

target_link_directories(niobium_fhetch
    PUBLIC $<BUILD_INTERFACE:${OPENFHE_LIBRARY_PATH}>)
target_link_libraries(niobium_fhetch PUBLIC ${OPENFHE_LIBRARIES})

if(APPLE)
  set_target_properties(niobium_fhetch PROPERTIES
      BUILD_RPATH   "@loader_path;${OPENFHE_LIBRARY_PATH}"
      INSTALL_RPATH "@loader_path/../lib;${OPENFHE_LIBRARY_PATH}"
  )
else()
  set_target_properties(niobium_fhetch PROPERTIES
      BUILD_RPATH   "${OPENFHE_LIBRARY_PATH}"
      INSTALL_RPATH "$ORIGIN;${OPENFHE_LIBRARY_PATH}"
  )
endif()

# ==============================================================================
# FHETCH Simulator CLI (optional)
# ==============================================================================

if(WITH_FHETCH_SIM)
    add_executable(fhetch_sim src/fhetch_sim/main.cpp)
    target_link_libraries(fhetch_sim PRIVATE niobium_fhetch)
    # local_replay.h (the --project worker entry point) lives under src/.
    target_include_directories(fhetch_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(APPLE)
      set_target_properties(fhetch_sim PROPERTIES
          BUILD_RPATH "@loader_path;${OPENFHE_LIBRARY_PATH}")
    else()
      set_target_properties(fhetch_sim PROPERTIES
          BUILD_RPATH "${OPENFHE_LIBRARY_PATH}")
    endif()
endif()

# ==============================================================================
# Python bindings (optional)
# ==============================================================================
# Two extension modules that share the single loaded instrumented OpenFHE +
# libnbfhetch (so probes fire into the active recording session):
#   * openfhe        — vendored openfhe-python, built UNMODIFIED via its own CMake
#                      (ExternalProject; its CMakeLists uses CMAKE_SOURCE_DIR and a
#                      site-packages install that don't compose under add_subdirectory)
#   * niobium_session — the niobium::compiler() session bindings (our source)
# Both land in ${CMAKE_BINARY_DIR}/python. The niobium-client wheel build flips
# this same option; here it backs the `*-python-release` dev/smoke make targets.

if(WITH_PYTHON)
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
    # Ubuntu-specific wheels (system libpython present), so this path was never hit
    # there; see STATE / PYTHON_WHEEL_PLAN.
    set(NB_OFHE_PY_COPY "${CMAKE_BINARY_DIR}/openfhe-python-src")
    file(REMOVE_RECURSE "${NB_OFHE_PY_COPY}")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/vendor/openfhe-python/"
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
            -DOpenFHE_DIR=${OPENFHE_INSTALL_DIR}/lib/OpenFHE
            -DOPENFHE_REQUIRED_VERSION=1.4.2
            -Dpybind11_DIR=${pybind11_DIR}
            -DPython_EXECUTABLE=${Python_EXECUTABLE}
            -DPYTHON_EXECUTABLE_PATH=${Python_EXECUTABLE}
            -DCMAKE_INSTALL_PREFIX=${NB_PY_OUTDIR}
        BUILD_ALWAYS ON
        INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config ${CMAKE_BUILD_TYPE}
    )

    # --- Session module: niobium::compiler() bindings (our source) ---
    # Links niobium_fhetch, which PUBLIC-propagates the OpenFHE includes/libs and
    # the OPENFHE_CPROBES define, so the headers compile identically to the library.
    pybind11_add_module(niobium_session python/niobium_session.cpp)
    target_link_libraries(niobium_session PRIVATE niobium_fhetch)
    set_target_properties(niobium_session PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${NB_PY_OUTDIR}")
    if(APPLE)
        set_target_properties(niobium_session PROPERTIES
            BUILD_RPATH "@loader_path;${OPENFHE_LIBRARY_PATH}")
    else()
        set_target_properties(niobium_session PROPERTIES
            BUILD_RPATH "$ORIGIN;${OPENFHE_LIBRARY_PATH}")
    endif()
endif()

# ==============================================================================
# Install
# ==============================================================================

include(GNUInstallDirs)

install(TARGETS niobium_fhetch
    EXPORT NiobiumFhetchTargets
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/niobium
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

install(EXPORT NiobiumFhetchTargets
    FILE        NiobiumFhetchTargets.cmake
    NAMESPACE   Niobium::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NiobiumFhetch
)

if(WITH_FHETCH_SIM)
    install(TARGETS fhetch_sim
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ==============================================================================
# Examples (optional)
# ==============================================================================

option(NIOBIUM_FHETCH_WITH_EXAMPLES "Build FHETCH examples" OFF)

if(NIOBIUM_FHETCH_WITH_EXAMPLES)
    add_subdirectory(examples)
endif()

# ==============================================================================
# Tests (optional)
# ==============================================================================

option(NIOBIUM_FHETCH_WITH_TESTS "Build FHETCH test harnesses" OFF)

if(NIOBIUM_FHETCH_WITH_TESTS)
    add_subdirectory(tests)
endif()
