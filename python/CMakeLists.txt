# niobium-fhetch Python bindings — the two extension modules that share the
# single loaded instrumented OpenFHE + libnbfhetch (so probes fire into the
# active recording session):
#   * openfhe         — vendored openfhe-python, built UNMODIFIED via its own CMake
#                       (ExternalProject; its CMakeLists uses CMAKE_SOURCE_DIR and a
#                       site-packages install that don't compose under add_subdirectory)
#   * niobium_session — the niobium::compiler() session bindings (our source)
#
# Component-only: add_subdirectory(python) from the top-level CMakeLists under
# WITH_PYTHON. Unlike a standalone build, this needs the niobium_fhetch target
# (niobium_session links it) and the OPENFHE_* vars, all defined by the parent.
# Both modules land in ${CMAKE_BINARY_DIR}/python — the location the niobium-client
# wheel build (which flips WITH_PYTHON the same way) picks them up from.

# The fhetch source root (parent of python/): openfhe-python is vendored there.
get_filename_component(NB_FHETCH_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)

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
pybind11_add_module(niobium_session niobium_session.cpp)
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
