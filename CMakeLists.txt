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

# The recorder-free core: the trace format, the IR builder and the simulator.
# No niobium::compiler(), no OpenFHE scheme code, no cprobe hooks (the
# simulator uses OpenFHE's math library only).
set(CORE_SOURCES
    src/fhetch_api.cpp
    src/fhetch_encoding.cpp
    src/fhetch_parser.cpp
    src/fhetch_reader.cpp
    src/fhetch_writer.cpp
    src/fhetch_sim/simulator.cpp
    src/trace_writer.cpp
)

# The recorder: the session API niobium::compiler(), the OpenFHE auto-facade,
# the cprobe hooks, the binding record capture and local replay.
set(RECORDER_SOURCES
    src/auto_facade.cpp
    src/binding_capture.cpp
    src/compiler.cpp
    src/fhetch_sync.cpp
    src/local_replay.cpp
    src/probes.cpp
)

# OpenFHE's math library alone (what the simulator uses).
if(NIOBIUM_FHETCH_OPENFHE_STATIC)
  set(OPENFHE_CORE_LIBRARY OPENFHEcore_static)
else()
  set(OPENFHE_CORE_LIBRARY OPENFHEcore)
endif()

# ==============================================================================
# Binding record (.nbbr) and output metadata (.nbom) formats
# (docs/BINDING_RECORD.md). C++17 and dependency-free, so the recorder always
# writes them; bind (nbfhetch_binding, below) is the opt-in C++20/nbbf part.
# ==============================================================================

# niobium_object_archive(<name> <export_name> SOURCES ...): the sources compiled
# once, as the OBJECT library <name>_objects, and the static archive <name> of
# those objects, exported as Niobium::<export_name>. libnbfhetch compiles in the
# same objects, so it (shared or static, and a consumer that links its archive
# by path) needs no second library, and holds one copy of any process-wide state.
function(niobium_object_archive name export_name)
  cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
  add_library(${name}_objects OBJECT ${ARG_SOURCES})
  set_target_properties(${name}_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_compile_features(${name}_objects PUBLIC cxx_std_17)
  target_include_directories(${name}_objects PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

  add_library(${name} STATIC $<TARGET_OBJECTS:${name}_objects>)
  add_library(Niobium::${export_name} ALIAS ${name})
  set_target_properties(${name} PROPERTIES
      OUTPUT_NAME "${name}"
      EXPORT_NAME "${export_name}"
      POSITION_INDEPENDENT_CODE ON)
  target_compile_features(${name} PUBLIC cxx_std_17)
  target_include_directories(${name} PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>)
endfunction()

niobium_object_archive(nbfhetch_formats fhetch_formats SOURCES
    src/formats/binding_record.cpp
    src/formats/file_io.cpp
    src/formats/output_metadata.cpp
)

# ==============================================================================
# Recorder-free core (nbfhetch_core)
# ==============================================================================
#
# The static nbfhetch_core is installed for find_package(NiobiumFhetchCore), so
# a recorder elsewhere can link the IR builder without a second
# niobium::compiler(); libnbfhetch therefore holds exactly what it held before.

niobium_object_archive(nbfhetch_core fhetch_core SOURCES ${CORE_SOURCES})
target_include_directories(nbfhetch_core_objects PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fhetch_sim
    ${JSON_INCLUDE_DIR}
    ${OPENFHE_INCLUDE_DIRS})
# The same definition libnbfhetch is compiled with, so the OpenFHE headers the
# simulator includes expand identically in both; it adds no hook reference
# there (tests/fhetch_core checks the core's undefined symbols).
target_compile_definitions(nbfhetch_core_objects PRIVATE OPENFHE_CPROBES)
target_link_libraries(nbfhetch_core_objects PUBLIC nbfhetch_formats)

find_package(Threads REQUIRED)
target_link_directories(nbfhetch_core PUBLIC $<BUILD_INTERFACE:${OPENFHE_LIBRARY_PATH}>)
target_link_libraries(nbfhetch_core PUBLIC nbfhetch_formats ${OPENFHE_CORE_LIBRARY} Threads::Threads)

# ==============================================================================
# Library target
# ==============================================================================

# The core's and the formats' objects plus the recorder (see above for why not
# their archives).
add_library(niobium_fhetch ${RECORDER_SOURCES} $<TARGET_OBJECTS:nbfhetch_core_objects>
    $<TARGET_OBJECTS:nbfhetch_formats_objects>)
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
# find_package(NiobiumFhetchCore): Niobium::fhetch_core, the recorder-free core.
niobium_install_package(NiobiumFhetchCore nbfhetch_core)
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
