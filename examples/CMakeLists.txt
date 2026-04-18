# ==============================================================================
# Helper: set RPATH for OpenFHE shared libraries (used by the simulator)
# ==============================================================================
macro(set_openfhe_rpath TARGET_NAME)
  if(APPLE)
    set_target_properties(${TARGET_NAME} PROPERTIES
        BUILD_RPATH "@loader_path;${OPENFHE_LIBRARY_PATH}")
  else()
    set_target_properties(${TARGET_NAME} PROPERTIES
        BUILD_RPATH "${OPENFHE_LIBRARY_PATH}")
  endif()
endmacro()

# ==============================================================================
# FHETCH-only examples (no OpenFHE in the user code — the library still pulls
# OpenFHE in for simulator-side modular arithmetic).
# ==============================================================================

add_executable(simple_fhetch fhetch/simple_fhetch.cpp)
target_link_libraries(simple_fhetch PRIVATE niobium_fhetch)
set_openfhe_rpath(simple_fhetch)
