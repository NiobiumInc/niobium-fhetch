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

# ==============================================================================
# Simulator liveness free schedule (last-read + dead-write frees).
# ==============================================================================

add_executable(simulator_liveness simulator_liveness/test.cpp)
target_link_libraries(simulator_liveness PRIVATE niobium_fhetch)
target_include_directories(simulator_liveness PRIVATE "${CMAKE_SOURCE_DIR}/src")
set_openfhe_rpath(simulator_liveness)

# ==============================================================================
# Exit status of fhetch_sim --project (run_local_replay_from_project).
# ==============================================================================

add_executable(local_replay_test local_replay/test.cpp)
target_link_libraries(local_replay_test PRIVATE niobium_fhetch)
target_include_directories(local_replay_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
set_openfhe_rpath(local_replay_test)

# ==============================================================================
# FHETCH IR — opcode vocabulary + instruction. Header-only, no OpenFHE.
# ==============================================================================

add_executable(fhetch_ir_test fhetch_ir/test.cpp)
target_link_libraries(fhetch_ir_test PRIVATE niobium_fhetch)

# ==============================================================================
# Binary sidecar (.fhex) round-trip. Needs no OpenFHE, so no RPATH help.
# ==============================================================================

add_executable(fhex_roundtrip fhex_roundtrip/test.cpp)
target_link_libraries(fhex_roundtrip PRIVATE niobium_fhetch)

# ==============================================================================
# Trace reader — text and binary. Needs no OpenFHE.
# ==============================================================================

add_executable(fhetch_reader_test fhetch_reader/test.cpp)
target_link_libraries(fhetch_reader_test PRIVATE niobium_fhetch)

# ==============================================================================
# Trace writer — text and binary. Needs no OpenFHE.
# ==============================================================================

add_executable(fhetch_writer_test fhetch_writer/test.cpp)
target_link_libraries(fhetch_writer_test PRIVATE niobium_fhetch)

# ==============================================================================
# Caller-chosen NTT roots: recording, validation, both serializations, the
# parser's re-drive, and the simulator's transforms against OpenFHE's.
# ==============================================================================

add_executable(ntt_omega_test ntt_omega/test.cpp)
target_link_libraries(ntt_omega_test PRIVATE niobium_fhetch)
target_include_directories(ntt_omega_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
set_openfhe_rpath(ntt_omega_test)

# ==============================================================================
# replay_project's --out scoping: nbcc_fhetch_replay's secondary output is
# pinned inside the project directory instead of the spawned process's CWD.
# ==============================================================================

add_executable(replay_out_test replay_out/test.cpp)
target_link_libraries(replay_out_test PRIVATE niobium_fhetch)
set_openfhe_rpath(replay_out_test)

# ==============================================================================
# The recorder-free core on its own: linked against nbfhetch_core only, so it
# links only if the core needs no niobium::compiler(). check_symbols.py (run by
# `make test-fhetch-core[-release]`) checks the archive's symbols as well.
# ==============================================================================

add_executable(fhetch_core_test fhetch_core/test.cpp)
target_link_libraries(fhetch_core_test PRIVATE nbfhetch_core)
set_openfhe_rpath(fhetch_core_test)
# The instrumented shared libOPENFHEcore leaves the recorder's cprobe hooks
# undefined, and this process, with no recorder, never calls them (it defines
# only the load-time hook; see fhetch_recording.h). macOS binds them lazily; GNU
# ld refuses an executable over a shared library with undefined references
# unless told they are left for run time.
if(NOT APPLE AND NOT NIOBIUM_FHETCH_OPENFHE_STATIC)
  target_link_options(fhetch_core_test PRIVATE "LINKER:--allow-shlib-undefined")
endif()

# ==============================================================================
# Binding record (.nbbr) and output metadata (.nbom) formats. C++17, no
# dependencies: always built.
# ==============================================================================

add_executable(binding_formats_test binding_record/formats_test.cpp)
target_link_libraries(binding_formats_test PRIVATE nbfhetch_formats)

# ==============================================================================
# The recorder writes the binding record and output metadata next to the trace.
# ==============================================================================

add_executable(binding_capture_test binding_capture/test.cpp)
target_link_libraries(binding_capture_test PRIVATE niobium_fhetch)
target_include_directories(binding_capture_test PRIVATE "${CMAKE_SOURCE_DIR}/src" ${JSON_INCLUDE_DIR})
set_openfhe_rpath(binding_capture_test)

# ==============================================================================
# Bind and output limbs (needs NIOBIUM_FHETCH_WITH_NBBF). C++20, no OpenFHE.
# ==============================================================================

if(TARGET nbfhetch_binding)
  add_executable(binding_bind_test binding_record/bind_test.cpp)
  target_link_libraries(binding_bind_test PRIVATE nbfhetch_binding)
endif()
