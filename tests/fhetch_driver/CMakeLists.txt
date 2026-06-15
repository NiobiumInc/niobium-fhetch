# fhetch_driver — reads a .fhetch trace and re-drives it through the
# niobium::fhetch API via the parser in src/fhetch_parser.cpp. In
# --source-dir mode it also loads the primary workload's captured inputs
# via OpenFHE cereal, tags the original outputs, and serializes
# reconstructed ciphertexts for decrypt-verified roundtrip tests.

add_executable(fhetch_driver main.cpp)
target_link_libraries(fhetch_driver PRIVATE niobium_fhetch)
target_include_directories(fhetch_driver PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${JSON_INCLUDE_DIR}"
    "${CMAKE_SOURCE_DIR}/vendor/openfhe/third-party/cereal/include"
)

if(APPLE)
  set_target_properties(fhetch_driver PROPERTIES
      BUILD_RPATH "@loader_path;${OPENFHE_LIBRARY_PATH}")
else()
  set_target_properties(fhetch_driver PROPERTIES
      BUILD_RPATH "${OPENFHE_LIBRARY_PATH}")
endif()
