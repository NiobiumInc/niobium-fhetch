cmake_minimum_required(VERSION 3.16)

# VERSION is the single source of truth for this repo's version. make/runtime.mk
# previously scraped the number back out of the project() line below with a
# regex; both now read the same file.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" NIOBIUM_FHETCH_VERSION)
string(STRIP "${NIOBIUM_FHETCH_VERSION}" NIOBIUM_FHETCH_VERSION)
if(NOT NIOBIUM_FHETCH_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR
    "VERSION must be a bare X.Y.Z semver, got '${NIOBIUM_FHETCH_VERSION}'.")
endif()
set_property(DIRECTORY APPEND PROPERTY
  CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

project(NiobiumFhetch VERSION ${NIOBIUM_FHETCH_VERSION} LANGUAGES CXX)

# Stamped into every bundle this library writes, so the consuming compiler can
# tell what produced it. NIOBIUM_CLIENT_VERSION is left to the distribution
# embedding this library -- it defaults to "unknown", which is correct for a
# standalone build. See COMPATIBILITY.md.
add_compile_definitions(NIOBIUM_FHETCH_VERSION="${NIOBIUM_FHETCH_VERSION}")

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
    src/fhetch_encoding.cpp
    src/fhetch_parser.cpp
    src/fhetch_reader.cpp
    src/fhetch_writer.cpp
    src/fhetch_sim/simulator.cpp
    src/binding_capture.cpp
    src/local_replay.cpp
    src/probes.cpp
    src/trace_writer.cpp
)

# ==============================================================================
# Binding record (.nbbr) and output metadata (.nbom) formats
# (docs/BINDING_RECORD.md). C++17 and dependency-free, so the recorder always
# writes them; bind (nbfhetch_binding, below) is the opt-in C++20/nbbf part.
# ==============================================================================

# Static (PIC): it is folded into libnbfhetch, so whatever ships libnbfhetch
# ships no extra shared library.
add_library(nbfhetch_formats STATIC
    src/formats/binding_record.cpp
    src/formats/file_io.cpp
    src/formats/output_metadata.cpp
)
add_library(Niobium::fhetch_formats ALIAS nbfhetch_formats)
set_target_properties(nbfhetch_formats PROPERTIES
    OUTPUT_NAME "nbfhetch_formats"
    EXPORT_NAME "fhetch_formats"
    POSITION_INDEPENDENT_CODE ON
)
target_compile_features(nbfhetch_formats PUBLIC cxx_std_17)
target_include_directories(nbfhetch_formats PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

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
target_link_libraries(niobium_fhetch PUBLIC nbfhetch_formats)

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
# The vendored openfhe-python crypto module + the niobium::compiler() session
# module. Self-contained under bindings/python/ (both land in ${CMAKE_BINARY_DIR}/python).
# The niobium-client wheel build flips WITH_PYTHON the same way; here it backs the
# `*-python-release` dev/smoke make targets. See bindings/python/CMakeLists.txt.

if(WITH_PYTHON)
    add_subdirectory(bindings/python)
endif()

# ==============================================================================
# Install
# ==============================================================================

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Installs the targets into export set <package>Targets, plus that package's
# Targets file, config (from cmake/<package>Config.cmake.in) and version file,
# so consumers can find_package(<package>) and use Niobium::<export name>.
# Package configs are what find_package locates; the Targets file alone is not.
function(niobium_install_package package)
  install(TARGETS ${ARGN}
      EXPORT ${package}Targets
      LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
      RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR})
  set(_dest ${CMAKE_INSTALL_LIBDIR}/cmake/${package})
  install(EXPORT ${package}Targets
      FILE        ${package}Targets.cmake
      NAMESPACE   Niobium::
      DESTINATION ${_dest})
  configure_package_config_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/${package}Config.cmake.in
      ${CMAKE_CURRENT_BINARY_DIR}/${package}Config.cmake
      INSTALL_DESTINATION ${_dest})
  write_basic_package_version_file(
      ${CMAKE_CURRENT_BINARY_DIR}/${package}ConfigVersion.cmake
      VERSION       ${PROJECT_VERSION}
      COMPATIBILITY SameMajorVersion)
  install(FILES
      ${CMAKE_CURRENT_BINARY_DIR}/${package}Config.cmake
      ${CMAKE_CURRENT_BINARY_DIR}/${package}ConfigVersion.cmake
      DESTINATION ${_dest})
endfunction()

# find_package(NiobiumFhetchFormats): Niobium::fhetch_formats, C++17, no deps.
# NiobiumFhetch depends on it (niobium_fhetch links it PUBLIC).
niobium_install_package(NiobiumFhetchFormats nbfhetch_formats)
niobium_install_package(NiobiumFhetch niobium_fhetch)

# Every public header but binding.h, which needs nbbf and is installed with
# nbfhetch_binding.
install(DIRECTORY include/niobium
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
    PATTERN "binding.h" EXCLUDE
)

if(WITH_FHETCH_SIM)
    install(TARGETS fhetch_sim
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ==============================================================================
# Examples (optional)
# ==============================================================================

# ==============================================================================
# Bind (nbfhetch_binding): joins a labelled NBBF upload with a binding record.
# C++20 and nbbf, so opt-in: pass -DNIOBIUM_FHETCH_WITH_NBBF=ON and
# -Dnbbf_DIR=<prefix>/lib/cmake/nbbf (the Makefile's NBBF_DIR does both).
# find_package(NiobiumFhetchBinding): Niobium::fhetch_binding.
# ==============================================================================

option(NIOBIUM_FHETCH_WITH_NBBF "Build bind (nbfhetch_binding); needs nbbf" OFF)
if(NIOBIUM_FHETCH_WITH_NBBF)
  find_package(nbbf 1.0 REQUIRED)
  add_library(nbfhetch_binding src/binding/bind.cpp)
  add_library(Niobium::fhetch_binding ALIAS nbfhetch_binding)
  set_target_properties(nbfhetch_binding PROPERTIES
      OUTPUT_NAME "nbfhetch_binding"
      EXPORT_NAME "fhetch_binding"
      VERSION     ${PROJECT_VERSION}
      SOVERSION   1
      POSITION_INDEPENDENT_CODE ON)
  target_compile_features(nbfhetch_binding PUBLIC cxx_std_20)
  target_link_libraries(nbfhetch_binding PUBLIC nbfhetch_formats nbbf::nbbf)
  niobium_install_package(NiobiumFhetchBinding nbfhetch_binding)
  install(FILES include/niobium/binding.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/niobium)
endif()

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
