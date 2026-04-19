# fhetch_driver — reads a .fhetch trace and re-drives it through the
# niobium::fhetch API via the parser in src/fhetch_parser.cpp.

add_executable(fhetch_driver main.cpp)
target_link_libraries(fhetch_driver PRIVATE niobium_fhetch)

if(APPLE)
  set_target_properties(fhetch_driver PROPERTIES
      BUILD_RPATH "@loader_path;${OPENFHE_LIBRARY_PATH}")
else()
  set_target_properties(fhetch_driver PROPERTIES
      BUILD_RPATH "${OPENFHE_LIBRARY_PATH}")
endif()
