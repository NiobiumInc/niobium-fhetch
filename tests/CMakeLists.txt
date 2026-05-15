# FHETCH test harnesses
add_subdirectory(fhetch_driver)

# Helper: set RPATH for OpenFHE shared libraries (used by the example binaries
# that link OpenFHE directly).
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
# Simple-ops example (client / server / decrypt)
# ==============================================================================

add_executable(simple_ops_client  simple_ops/client.cpp)
add_executable(simple_ops_server  simple_ops/server.cpp)
add_executable(simple_ops_decrypt simple_ops/decrypt.cpp)

target_link_libraries(simple_ops_client  PRIVATE niobium_fhetch)
target_link_libraries(simple_ops_server  PRIVATE niobium_fhetch)
target_link_libraries(simple_ops_decrypt PRIVATE niobium_fhetch)

set_openfhe_rpath(simple_ops_client)
set_openfhe_rpath(simple_ops_server)
set_openfhe_rpath(simple_ops_decrypt)

# ==============================================================================
# Bootstrap example (client / server / decrypt)
# ==============================================================================

add_executable(bootstrap_client  bootstrap/client.cpp)
add_executable(bootstrap_server  bootstrap/server.cpp)
add_executable(bootstrap_decrypt bootstrap/decrypt.cpp)

target_link_libraries(bootstrap_client  PRIVATE niobium_fhetch)
target_link_libraries(bootstrap_server  PRIVATE niobium_fhetch)
target_link_libraries(bootstrap_decrypt PRIVATE niobium_fhetch)

set_openfhe_rpath(bootstrap_client)
set_openfhe_rpath(bootstrap_server)
set_openfhe_rpath(bootstrap_decrypt)

# ==============================================================================
# Plaintext-add example (client / server / decrypt)
# ==============================================================================

add_executable(plaintext_add_client  plaintext_add/client.cpp)
add_executable(plaintext_add_server  plaintext_add/server.cpp)
add_executable(plaintext_add_decrypt plaintext_add/decrypt.cpp)

target_link_libraries(plaintext_add_client  PRIVATE niobium_fhetch)
target_link_libraries(plaintext_add_server  PRIVATE niobium_fhetch)
target_link_libraries(plaintext_add_decrypt PRIVATE niobium_fhetch)

set_openfhe_rpath(plaintext_add_client)
set_openfhe_rpath(plaintext_add_server)
set_openfhe_rpath(plaintext_add_decrypt)

# ==============================================================================
# Regression: dest==src1 for automorph kernels.
# ==============================================================================

add_executable(automorph_alias_test automorph_alias_test/test.cpp)
target_link_libraries(automorph_alias_test PRIVATE niobium_fhetch)
set_openfhe_rpath(automorph_alias_test)
