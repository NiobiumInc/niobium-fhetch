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

set(OPENFHE_LIBRARIES
    OPENFHEbinfhe
    OPENFHEcore
    OPENFHEpke
)

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
    if(APPLE)
      set_target_properties(fhetch_sim PROPERTIES
          BUILD_RPATH "@loader_path;${OPENFHE_LIBRARY_PATH}")
    else()
      set_target_properties(fhetch_sim PROPERTIES
          BUILD_RPATH "${OPENFHE_LIBRARY_PATH}")
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
