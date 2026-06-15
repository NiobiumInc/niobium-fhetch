// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.

#include "niobium/compiler.h"
#include "niobium/fhetch_api.h"
#include "niobium/fhetch_sim/simulator.h"
#include "compiler_internal.h"
#include "trace_writer.h"
#include "niobium/checks.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <cstdlib>
#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <nlohmann/json.hpp>

#include <spawn.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <vector>

// `environ` is POSIX-required globally but not declared by <unistd.h>
// uniformly across platforms (Darwin needs the explicit extern).
extern char** environ;

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>

// OpenFHE hollow mode global — defined in libOPENFHEcore.
// When true, polynomial operations skip expensive math but preserve structure.
namespace lbcrypto { extern bool g_hollow_mode; }

// Cooperative auto-tagging opt-in. Weak no-op stub lives in auto_facade.cpp;
// a strong override in libnbclient_autofacade switches the facade into
// cooperative (tag-only, host-driven-lifecycle) mode.
namespace niobium_auto { void enable_auto_tagging(); }

namespace niobium {

// ============================================================================
// Compiler::Impl — hidden implementation
// ============================================================================

struct Compiler::Impl {
    TraceWriter trace_writer;

    // Program metadata
    std::string program_name;
    std::string program_version;
    std::string program_description;
    std::string source_file;
    int source_line = 0;
    std::string build_timestamp;

    // Cache
    CacheParameters cache_params;
    std::string cache_suffix;

    // State
    bool running = false;
    bool stopped = false;
    bool hollow_mode = false;
    bool multithreaded = false;
    bool fhetch_mode = false;

    // Epochs
    uint32_t epoch_id = 0;

    // Overrides for the crypto context checks
    // These checks verify that primes and ring dimension are compatible with
    // the Niobium Hardware
    bool check_primes = true;
    bool check_ring_dim = true;

    // Crypto context info (populated by capture_crypto_context)
    uint64_t ring_dimension = 0;
    uint32_t multiplicative_depth = 0;
    uint32_t scaling_mod_size = 0;
    std::string scheme_name;
    std::string security_level;
    std::vector<uint64_t> modulus_chain;
    std::vector<uint64_t> inverse_modulus_chain;

    // Hardware data-format toggles, selected via --montgomery /
    // --bit_reversal / --niobium_hw (= both) on the CLI consumed by init().
    // Client recordings stay ordinary-form regardless; the toggles select
    // the *replay* data format: dispatch_to_compiler_target() forwards
    // --niobium_hw to nbcc_fhetch_replay when both are on, and the driver
    // hardware-izes inputs/immediates/switchmodulus blocks itself. The
    // Niobium hardware convention is both together; partial combinations
    // are rejected at replay().
    bool montgomery_mode = false;
    bool bitrev_mode = false;

    // Cooperative auto-tagging (set by enable_auto_tagging()). When true,
    // replay() refreshes stale inputs from the manifest and dispatches a
    // disk-based replay (local fhetch_driver / remote forwarder) instead of
    // the in-process, in-memory-captured_inputs simulator path.
    bool auto_tagging = false;

    // Key start addr_ids (first addr_id recorded for each key type)
    uint64_t evalmult_start_addr_id = 0;
    uint64_t evalautomorphism_start_addr_id = 0;

    // Replay target. "local" runs the in-process FHETCH simulator; any other
    // value (e.g. "FUNC_SIM", "fpga5.2") hands the recorded project off to
    // the compiler's nbcc_fhetch_replay executable instead. Selected via
    // --target=<value> on the CLI consumed by init().
    std::string target = "local";

    // Optimization level for the compiler-side replay, normalized to "O0".."O3".
    // Defaults to O0 (conservative); overridable via --opt-level=<v> on the CLI.
    // Forwarded to the replay driver (and on to the transport server) so the
    // chosen level reaches the compiler's nbcc_fhetch_replay.
    std::string opt_level = "O0";

    // Last written trace path (set by stop())
    std::filesystem::path last_trace_path;

    // Simulator instance (created by replay())
    std::unique_ptr<fhetch_sim::Simulator> simulator;

    // Input polynomial data captured by tag_input().
    // Each entry: {name, [{addr_id, modulus, values}]}
    struct PolyElement {
        uint64_t addr_id;
        uint64_t modulus;
        std::vector<uint64_t> values;
    };
    struct InputRecord {
        std::string name;
        CapturedKind kind;
        // On-disk source file this input was loaded from + its mtime at tag
        // time (set via set_input_source). Persisted to inputs.json so a later
        // replay run can detect changed input files by mtime and refresh only
        // those against the recorded addresses. Empty source_path → not a
        // file-backed input (e.g. a made plaintext); skip the staleness check.
        std::string source_path;
        int64_t source_mtime = 0;
        std::vector<PolyElement> elements;
        // Array-element boundaries for SRPArray / MRPArray. element_starts[i]
        // is the index into `elements` where array position i begins. For
        // SRP / MRP, element_starts is empty (the whole record is one
        // logical element). For SRPArray, every entry in `elements` is its own
        // array position. For MRPArray, element_starts has K entries marking
        // the M_k-residue boundaries.
        std::vector<size_t> element_starts;
    };
    std::vector<InputRecord> captured_inputs;
    // name → index into captured_inputs, so store_input_* doesn't
    // linear-scan when re-tagging an existing record. Kept in sync
    // wherever captured_inputs is mutated.
    std::unordered_map<std::string, size_t> captured_input_index;

    // Output probe addresses captured by probe().
    struct OutputRecord {
        std::string name;
        CapturedKind kind;
        std::vector<uint64_t> addr_ids;
        std::vector<uint64_t> moduli;
        std::vector<size_t> element_starts;
    };
    std::vector<OutputRecord> captured_outputs;
    std::unordered_map<std::string, size_t> captured_output_index;

    // Hook invoked by stop() right before fhetch_replay.json is written.
    // Set by capture_crypto_context<Ciphertext<DCRTPoly>> to auto-capture
    // CC-derived precomputed data (e.g. FHECKKSRNS bootstrap precompute)
    // without forcing the user to call a specific API for it.
    std::function<void()> auto_capture_at_stop;

    // Gate for capture_crypto_context()'s install of the bootstrap-precompute
    // auto-capture hook. Default true preserves legacy behavior.
    bool auto_capture_bootstrap_precompute_enabled = true;

    // Hook invoked by stop() after trace_writer.stop_recording() but before
    // write_replay_json. Cleared by reset(); not by stop().
    std::function<void()> post_recording_hook;

    // Derived program directory
    std::filesystem::path program_dir;

    std::string full_program_name() const {
        std::string name = program_name;
        if (!cache_suffix.empty())
            name += "_" + cache_suffix;
        return name;
    }

    // Logging verbosity
    bool verbose =  false;
};

// ============================================================================
// Singleton
// ============================================================================

static Compiler* g_compiler = nullptr;

Compiler& compiler() {
    if (!g_compiler) {
        g_compiler = new Compiler();
    }
    return *g_compiler;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

Compiler::Compiler() : impl_(std::make_unique<Impl>()) {}
Compiler::~Compiler() = default;

// ============================================================================
// Session lifecycle
// ============================================================================

// Normalize a user-supplied optimization level ("3", "O3", "o3") to the
// canonical "O0".."O3". Returns "" if the value is not a recognized level.
static std::string normalize_opt_level(const std::string& v) {
    std::string s = v;
    if (!s.empty() && (s[0] == 'O' || s[0] == 'o')) s = s.substr(1);
    if (s.size() == 1 && s[0] >= '0' && s[0] <= '3') return std::string("O") + s[0];
    return "";
}

void Compiler::init(int& argc, char** argv) {
    // Parse and consume Niobium-specific flags from argv.
    // Recognized flags: --hollow, --multithreaded, --target=<value>,
    // --opt-level=<O0..O3>, -v, --no-prime-check, --no-ring-dim-check, --montgomery, --bit_reversal,
    // --niobium_hw (= montgomery + bit_reversal, the Niobium hardware
    // convention; matches the compiler's flag of the same name).
    int write_pos = 1;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--hollow") == 0) {
            impl_->hollow_mode = true;
        } else if (std::strcmp(a, "--multithreaded") == 0) {
            impl_->multithreaded = true;
        } else if (std::strncmp(a, "--target=", 9) == 0) {
            impl_->target = a + 9;
        } else if (std::strcmp(a, "--target") == 0 && i + 1 < argc) {
            impl_->target = argv[++i];
        } else if (std::strcmp(a, "--montgomery") == 0) {
            impl_->montgomery_mode = true;
        } else if (std::strcmp(a, "--bit_reversal") == 0) {
            impl_->bitrev_mode = true;
        } else if (std::strcmp(a, "--niobium_hw") == 0) {
            impl_->montgomery_mode = true;
            impl_->bitrev_mode = true;
        } else if (std::strncmp(a, "--opt-level=", 12) == 0) {
            std::string lvl = normalize_opt_level(a + 12);
            if (!lvl.empty()) impl_->opt_level = lvl;
        } else if (std::strcmp(a, "--opt-level") == 0 && i + 1 < argc) {
            std::string lvl = normalize_opt_level(argv[++i]);
            if (!lvl.empty()) impl_->opt_level = lvl;
        } else if (std::strcmp(a, "-v") == 0) {
            impl_->verbose = true;
        } else if (std::strcmp(a, "--no-prime-check") == 0) {
            impl_->check_primes = false;
        } else if (std::strcmp(a, "--no-ring-dim-check") == 0) {
            impl_->check_ring_dim = false;
        } else {
            argv[write_pos++] = argv[i];
        }
    }
    argc = write_pos;

    if (impl_->target != "local") {
        std::cout << "[NIOBIUM] Target: " << impl_->target
                  << " (replay will hand off to nbcc_fhetch_replay)" << std::endl;
    }
    if (impl_->montgomery_mode || impl_->bitrev_mode) {
        std::cout << "[NIOBIUM] Hardware data format:"
                  << (impl_->montgomery_mode ? " montgomery" : "")
                  << (impl_->bitrev_mode ? " bit_reversal" : "") << std::endl;
    }
}

void Compiler::enable_auto_tagging() {
    impl_->auto_tagging = true;
    // Delegate to the auto-facade. Weak stub is a no-op; the strong override
    // in libnbclient_autofacade switches the facade into cooperative mode.
    niobium_auto::enable_auto_tagging();
}

bool Compiler::start() {
    if (impl_->running) return false;

    // Start each cycle from a clean trace + clean fhetch registries so
    // multi-cycle drivers don't leak prior recordings forward.
    impl_->trace_writer.clear();
    niobium::fhetch::reset_for_epoch();

    impl_->running = true;
    impl_->stopped = false;
    impl_->trace_writer.start_recording();
    std::cout << "[NIOBIUM] Recording started" << std::endl;
    return true;
}

bool Compiler::stop() {
    if (!impl_->running) return false;
    // Pull fhetch::tag_input / tag_output state into captured_inputs /
    // captured_outputs so write_replay_json picks them up. No-op for the
    // OpenFHE auto-facade flow (it populates captured_outputs directly).
    niobium::detail::sync_fhetch_state_to_compiler();
    impl_->trace_writer.emit("halt");
    impl_->trace_writer.stop_recording();
    impl_->running = false;
    impl_->stopped = true;

    // Write the FHETCH trace file
    auto dir = get_program_directory();
    impl_->last_trace_path = impl_->trace_writer.write(dir, impl_->full_program_name());

    // Auto-capture any CC-derived precomputed data (e.g. CKKS bootstrap
    // precompute) before writing the replay index, mirroring the compiler's
    // create_replay_index(): user code only calls start()/stop(), never a
    // "tag bootstrap precompute" API.
    if (impl_->auto_capture_at_stop) {
        impl_->auto_capture_at_stop();
        impl_->auto_capture_at_stop = nullptr;  // don't run twice
    }

    if (impl_->post_recording_hook) {
        impl_->post_recording_hook();
    }

    // Write fhetch_replay.json with inputs, outputs, and modulus table
    write_replay_json();

    std::cout << "[NIOBIUM] Recording stopped ("
              << impl_->trace_writer.instruction_count()
              << " instructions)" << std::endl;
    return true;
}

bool Compiler::pause() {
    if (!impl_->running) return false;
    impl_->trace_writer.pause_recording();
    return true;
}

bool Compiler::resume() {
    if (!impl_->running) return false;
    impl_->trace_writer.resume_recording();
    return true;
}

// ============================================================================
// Program metadata
// ============================================================================

void Compiler::set_program_info(const std::string& name,
                                const std::string& version,
                                const std::string& description) {
    impl_->program_name = name;
    impl_->program_version = version;
    impl_->program_description = description;
    impl_->trace_writer.set_program_info(name, version, description);
}

void Compiler::set_build_info(const std::string& file, int line,
                              const std::string& timestamp) {
    impl_->source_file = file;
    impl_->source_line = line;
    impl_->build_timestamp = timestamp;
    impl_->trace_writer.set_source_info(file, line, timestamp);
}

// ============================================================================
// Cache management
// ============================================================================

void Compiler::cache_parameters(CacheParameters& params) {
    impl_->cache_params = params;

    // Build cache suffix from parameters
    std::string suffix;
    for (const auto& [key, value] : params) {
        if (!suffix.empty()) suffix += "_";
        suffix += key + "_" + value;
    }
    impl_->cache_suffix = suffix;
}

bool Compiler::is_cache_valid() {
    // Check if a trace file already exists for this program configuration
    auto dir = get_program_directory();
    auto trace_path = dir / (impl_->full_program_name() + ".fhetch");
    return std::filesystem::exists(trace_path);
}

// ============================================================================
// Recording modes
// ============================================================================

void Compiler::set_ring_dimension(uint64_t N) {
    if(impl_->check_ring_dim && !is_compatible_ring_dim(N)) {
        throw std::runtime_error("Ring dimension " + std::to_string(N) + " is not compatible with Niobium Hardware.");
    }
    impl_->ring_dimension = N;
}

void Compiler::set_crypto_context_info(const std::string& scheme_name,
                                       uint32_t multiplicative_depth,
                                       uint32_t scaling_mod_size,
                                       const std::string& security_level,
                                       const std::vector<uint64_t>& modulus_chain) {
    impl_->scheme_name = scheme_name;
    impl_->multiplicative_depth = multiplicative_depth;
    impl_->scaling_mod_size = scaling_mod_size;
    impl_->security_level = security_level;
    impl_->modulus_chain = modulus_chain;

    // Compute inverse modulus chain (Hensel lifting)
    impl_->inverse_modulus_chain.clear();
    for (uint64_t q : modulus_chain) {
        if(impl_->check_primes && !is_compatible_prime(q)) {
            throw std::runtime_error("Modulus " + std::to_string(q) + " is not compatible with Niobium Hardware.");
        }
        uint64_t ninv = 1;
        for (int i = 1; i < 64; i++) {
            if (((q * ninv) >> i) & 1)
                ninv |= (1ULL << i);
        }
        impl_->inverse_modulus_chain.push_back(ninv);
    }
}

void Compiler::set_key_start_addr_id(const std::string& key_type, uint64_t addr_id) {
    if (key_type == "evalmult")
        impl_->evalmult_start_addr_id = addr_id;
    else if (key_type == "evalautomorphism")
        impl_->evalautomorphism_start_addr_id = addr_id;
}

void Compiler::reserve_addresses(uint64_t next_addr) {
    detail::reserve_fhetch_addresses(next_addr);
}

void Compiler::clear_captured_inputs() {
    impl_->captured_inputs.clear();
    impl_->captured_input_index.clear();
}

void Compiler::clear_captured_outputs() {
    impl_->captured_outputs.clear();
    impl_->captured_output_index.clear();
}

void Compiler::clear_captured() {
    clear_captured_inputs();
    clear_captured_outputs();
}

void Compiler::set_auto_capture_at_stop(std::function<void()> fn) {
    impl_->auto_capture_at_stop = std::move(fn);
}

void Compiler::set_auto_capture_bootstrap_precompute(bool enabled) {
    impl_->auto_capture_bootstrap_precompute_enabled = enabled;
}

bool Compiler::auto_capture_bootstrap_precompute_enabled() const {
    return impl_->auto_capture_bootstrap_precompute_enabled;
}

void Compiler::set_post_recording_hook(std::function<void()> fn) {
    impl_->post_recording_hook = std::move(fn);
}

namespace detail {
// Defined in fhetch_api.cpp; clears TU-static recording state.
void clear_recording_registries() noexcept;
}  // namespace detail

void Compiler::reset() {
    detail::clear_recording_registries();
    impl_->captured_inputs.clear();
    impl_->captured_input_index.clear();
    impl_->captured_outputs.clear();
    impl_->captured_output_index.clear();
    impl_->trace_writer.clear();
    impl_->program_dir.clear();
    impl_->last_trace_path.clear();
    impl_->running = false;
    impl_->stopped = false;
    impl_->fhetch_mode = false;
    impl_->epoch_id = 0;
    impl_->modulus_chain.clear();
    impl_->inverse_modulus_chain.clear();
    impl_->check_primes = true;
    impl_->scheme_name.clear();
    impl_->security_level.clear();
    impl_->ring_dimension = 0;
    impl_->check_ring_dim = true;
    impl_->multiplicative_depth = 0;
    impl_->scaling_mod_size = 0;
    impl_->evalmult_start_addr_id = 0;
    impl_->evalautomorphism_start_addr_id = 0;
    impl_->cache_params.clear();
    impl_->cache_suffix.clear();
    impl_->program_name.clear();
    impl_->program_version.clear();
    impl_->program_description.clear();
    impl_->source_file.clear();
    impl_->source_line = 0;
    impl_->build_timestamp.clear();
    impl_->auto_capture_at_stop = nullptr;
    impl_->post_recording_hook = nullptr;
    impl_->montgomery_mode = false;
    impl_->bitrev_mode = false;
}

namespace {

// Find by name, or push a fresh entry whose kind defaults to
// `default_kind`. Templated to avoid naming Compiler::Impl's private
// nested record types. The static_asserts spell out the concept that
// concepts would express directly in C++20+.
template <typename RecordVec, typename IndexMap>
auto& find_or_create_record(RecordVec& records, IndexMap& index, const std::string& name,
                            CapturedKind default_kind) {
    using R = typename RecordVec::value_type;
    static_assert(std::is_assignable_v<decltype(std::declval<R&>().name)&, std::string>,
                  "find_or_create_record: record must have an assignable `name`");
    static_assert(std::is_assignable_v<decltype(std::declval<R&>().kind)&, CapturedKind>,
                  "find_or_create_record: record must have an assignable `kind`");
    auto it = index.find(name);
    if (it != index.end())
        return records[it->second];
    R fresh;
    fresh.name = name;
    fresh.kind = default_kind;
    const size_t idx = records.size();
    records.push_back(std::move(fresh));
    index.emplace(name, idx);
    return records[idx];
}

}  // namespace

// Re-tagging an existing name with a different kind is a programming
// error; we drop the residue and report rather than silently corrupting
// the record. assert() for debug-build fast detection; the cerr + return
// covers release builds where NDEBUG strips the assert.
void Compiler::store_input_element(const std::string& input_name,
                                   CapturedKind kind, bool starts_new_element,
                                   uint64_t addr_id, uint64_t modulus,
                                   const std::vector<uint64_t>& values) {
    auto& rec = find_or_create_record(impl_->captured_inputs, impl_->captured_input_index,
                                      input_name, kind);
    if (rec.kind != kind) {
        std::cerr << "[NIOBIUM] store_input_element: kind mismatch on '" << input_name
                  << "' (existing=" << static_cast<int>(rec.kind)
                  << ", new=" << static_cast<int>(kind) << "); residue dropped" << std::endl;
        assert(false && "store_input_element: kind mismatch on existing input record");
        return;
    }
    if (starts_new_element)
        rec.element_starts.push_back(rec.elements.size());
    rec.elements.push_back({addr_id, modulus, values});
}

void Compiler::set_input_source(const std::string& input_name,
                                const std::string& source_path) {
    auto it = impl_->captured_input_index.find(input_name);
    if (it == impl_->captured_input_index.end())
        return;  // no record yet — nothing to annotate
    auto& rec = impl_->captured_inputs[it->second];
    rec.source_path = source_path;
    // mtime as a raw clock count: implementation-defined units/epoch, but
    // stable on the same machine/filesystem across runs, so equality
    // comparison between the record run and a later replay run is valid.
    rec.source_mtime = 0;
    try {
        std::error_code ec;
        auto t = std::filesystem::last_write_time(source_path, ec);
        if (!ec)
            rec.source_mtime =
                static_cast<int64_t>(t.time_since_epoch().count());
    } catch (...) {
        // leave source_mtime = 0 (treated as "unknown" → always refresh)
    }
}

void Compiler::store_output_probe(const std::string& output_name,
                                  CapturedKind kind, bool starts_new_element,
                                  uint64_t addr_id, uint64_t modulus) {
    auto& rec = find_or_create_record(impl_->captured_outputs, impl_->captured_output_index,
                                      output_name, kind);
    if (rec.kind != kind) {
        std::cerr << "[NIOBIUM] store_output_probe: kind mismatch on '" << output_name
                  << "' (existing=" << static_cast<int>(rec.kind)
                  << ", new=" << static_cast<int>(kind) << "); residue dropped" << std::endl;
        assert(false && "store_output_probe: kind mismatch on existing output record");
        return;
    }
    if (starts_new_element)
        rec.element_starts.push_back(rec.addr_ids.size());
    rec.addr_ids.push_back(addr_id);
    rec.moduli.push_back(modulus);
}

void Compiler::enable_hollow_mode(bool enabled) {
    impl_->hollow_mode = enabled;
    lbcrypto::g_hollow_mode = enabled;
    if (enabled) {
        std::cout << "[NIOBIUM] Hollow mode ENABLED — skipping polynomial math" << std::endl;
    } else {
        std::cout << "[NIOBIUM] Hollow mode DISABLED — using real math" << std::endl;
    }
}

bool Compiler::is_hollow_mode() const {
    return impl_->hollow_mode;
}

void Compiler::enable_multithreaded_recording() {
    impl_->multithreaded = true;
}

bool Compiler::is_multithreaded() const {
    return impl_->multithreaded;
}

// ============================================================================
// FHETCH mode
// ============================================================================

bool Compiler::is_fhetch_mode() const {
    return impl_->fhetch_mode;
}

void Compiler::set_fhetch_mode() {
    impl_->fhetch_mode = true;
}

// ============================================================================
// Functional epochs
// ============================================================================

void Compiler::start_epoch() {
    // Nothing to memorize in the client — epochs are a recording-phase concept.
    // The trace writer handles the reset.
}

bool Compiler::stop_epoch() {
    if (!impl_->running) return false;

    // Finalize the current epoch's trace
    impl_->trace_writer.emit("halt");
    impl_->trace_writer.stop_recording();

    auto epoch_dir = get_program_directory() / ("epoch_" + std::to_string(impl_->epoch_id));
    std::string epoch_name = impl_->full_program_name() + "_epoch_" + std::to_string(impl_->epoch_id);
    impl_->trace_writer.write(epoch_dir, epoch_name);

    // Reset for next epoch
    impl_->trace_writer.clear();
    impl_->epoch_id++;
    impl_->trace_writer.start_recording();
    return true;
}

uint32_t Compiler::epoch_id() const {
    return impl_->epoch_id;
}

// ============================================================================
// Replay — run the FHETCH simulator
// ============================================================================

bool Compiler::replay() {
    // Hardware-format guard rails. The in-process simulator executes in
    // ordinary form only, and the transport replay engine couples Montgomery
    // and bit-reversal behind the single niobium_hw flag — so local replay
    // rejects either toggle, and transport replay rejects partial toggles.
    if (impl_->montgomery_mode || impl_->bitrev_mode) {
        if (impl_->target == "local") {
            std::cerr << "[NIOBIUM] montgomery/bit_reversal modes are not supported by the "
                         "in-process simulator; use a transport target (e.g. FUNC_SIM)"
                      << std::endl;
            return false;
        }
        if (impl_->montgomery_mode != impl_->bitrev_mode) {
            std::cerr << "[NIOBIUM] transport replay supports ordinary form or the full "
                         "hardware format (montgomery + bit_reversal) only; enable both "
                         "(--niobium_hw) or neither" << std::endl;
            return false;
        }
    }

    if (impl_->last_trace_path.empty()) {
        // Look for an existing trace (cached)
        auto dir = get_program_directory();
        auto path = dir / (impl_->full_program_name() + ".fhetch");
        if (std::filesystem::exists(path)) {
            impl_->last_trace_path = path;
        } else {
            std::cerr << "[NIOBIUM] No trace file found for replay" << std::endl;
            return false;
        }
    }

    if (impl_->ring_dimension == 0) {
        std::cerr << "[NIOBIUM] Ring dimension not set — call capture_crypto_context() before replay()" << std::endl;
        return false;
    }

    // ----- Cooperative (host-driven) replay -----------------------------------
    // The host program ran zero ops this pass (gated behind is_cache_valid), so
    // the in-memory captured_inputs are incomplete. Instead: refresh any input
    // files that changed since record (mtime) onto their recorded addresses,
    // then dispatch a DISK-based replay that reads the refreshed project —
    // local via the standalone fhetch_driver, remote via the compiler forwarder.
    // result() then reads serialized_probes/<name>.ct for both.
    if (impl_->auto_tagging) {
        refresh_stale_inputs();
        if (impl_->target != "local")
            return dispatch_to_compiler_target();
        return run_local_fhetch_driver();
    }

    // ----- Target != local: hand the fhetch project off to the compiler -----
    // For any non-"local" target we skip the in-process FHETCH simulator and
    // invoke the compiler-side driver (nbcc_fhetch_replay) which runs the full
    // Niobium optimization pipeline, produces replay.json + artifacts, executes
    // replay against the selected target device, and writes the resulting
    // probes back into <program_dir>/serialized_probes/ so result() picks them
    // up transparently.
    if (impl_->target != "local") {
        return dispatch_to_compiler_target();
    }

    // Cache-hit replay: the recording pass populated captured_outputs in
    // memory AND wrote <program>.outputs.json to disk. On a fresh process
    // start (e.g. the second run of an auto-facade workflow) the in-memory
    // map is empty, so write_replay_outputs / reconstruct_probes would
    // skip silently. Rehydrate from the on-disk outputs.json so the
    // downstream reconstruction stage has something to work with.
    if (impl_->captured_outputs.empty()) {
        auto dir = get_program_directory();
        auto outputs_path = dir / (impl_->full_program_name() + ".outputs.json");
        if (std::filesystem::exists(outputs_path)) {
            std::ifstream ifs(outputs_path);
            nlohmann::json j = nlohmann::json::parse(ifs, nullptr, false);
            if (!j.is_discarded() && j.contains("outputs")) {
                for (const auto& out : j["outputs"]) {
                    std::string name = out.value("name", "");
                    if (name.empty() || !out.contains("ciphertext_data")) continue;
                    for (const auto& poly : out["ciphertext_data"]) {
                        if (!poly.contains("elements")) continue;
                        for (const auto& elem : poly["elements"]) {
                            // Modulus is unknown here; set to 0. The simulator
                            // uses addr_id only for output-extraction, not
                            // modulus semantics, so the placeholder is safe.
                            store_output_probe(name, CapturedKind::SRP,
                                               /*starts_new_element=*/false,
                                               elem.get<uint64_t>(), 0);
                        }
                    }
                }
            }
        }
    }

    std::cout << "[NIOBIUM] Replaying trace: " << impl_->last_trace_path << std::endl;

    impl_->simulator = std::make_unique<fhetch_sim::Simulator>();
    impl_->simulator->set_ring_dimension(impl_->ring_dimension);

    if (!impl_->simulator->load_trace(impl_->last_trace_path)) {
        std::cerr << "[NIOBIUM] Failed to load trace for replay" << std::endl;
        return false;
    }

    // Pull in any state registered via the FHETCH-only tag_input / tag_output
    // path (fhetch_api.cpp). For OpenFHE flows this is a no-op — the
    // auto-facade has already populated captured_inputs / captured_outputs.
    niobium::detail::sync_fhetch_state_to_compiler();

    // Compute the "live-in" set: addresses read before being written in the trace.
    // Only these addresses need input data; addresses that are written first
    // get their values from the simulation itself.
    auto rbw_addrs = impl_->simulator->get_read_before_write_addresses();
    std::set<uint64_t> rbw_set(rbw_addrs.begin(), rbw_addrs.end());

    // Populate simulator memory from captured input data.
    //
    // Load captured polys unconditionally (not only live-in addresses).
    // Rationale: inputs like bootstrap-precompute plaintexts are typically
    // *cloned* before being consumed by the trace; the clone's address is
    // the one that ends up live-in, and the data-parent chain carries data
    // from the captured original to the clone. If we skip non-live-in
    // captures here, the parent is never materialized and propagation
    // cannot reach the clone.
    size_t direct_count = 0;
    for (const auto& input : impl_->captured_inputs) {
        size_t live = 0;
        size_t aux = 0;
        for (const auto& elem : input.elements) {
            impl_->simulator->store_polynomial(elem.addr_id, elem.values, elem.modulus);
            direct_count++;
            if (rbw_set.count(elem.addr_id)) ++live;
            else ++aux;
        }
        if (impl_->verbose) {
            std::cout << "[NIOBIUM]   " << input.name << ": "
                    << input.elements.size() << " elements, "
                    << live << " live-in, " << aux << " aux (for parent chain)"
                    << std::endl;
        }
    }

    // Propagate data through the copy/move lineage. Both directions:
    //
    //  1. Forward  (parent -> child): after `child = copy(parent)` the
    //     child holds the parent's data, so if the parent is captured,
    //     the child inherits.
    //
    //  2. Reverse (child -> parent): if a captured poly is the end of a
    //     copy chain (e.g. an object stored in a map that was built by
    //     copying an earlier temporary), the earlier poly still holds
    //     the same data unless it was modified after the copy. For
    //     pre-start captures (inputs, keys, bootstrap precompute) the
    //     chain is stable, so reverse propagation is safe and fills in
    //     the addresses the trace actually reads from.
    //
    // We iterate to a fixed point so multi-hop chains get filled in.
    const auto& parent_map = detail::get_data_parent_map();
    size_t propagated = 0;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [child, parent] : parent_map) {
            auto copy_one = [&](uint64_t dst, uint64_t src) {
                if (!impl_->simulator->is_initialized(dst) &&
                     impl_->simulator->is_initialized(src)) {
                    impl_->simulator->store_polynomial(
                        dst,
                        impl_->simulator->get_polynomial(src),
                        impl_->simulator->get_modulus(src));
                    propagated++;
                    changed = true;
                }
            };
            // forward
            if (rbw_set.count(child)) copy_one(child, parent);
            // reverse
            if (rbw_set.count(parent)) copy_one(parent, child);
        }
    }

    // Report unloaded live-in addresses
    std::vector<uint64_t> unloaded;
    for (uint64_t a : rbw_set) {
        if (!impl_->simulator->is_initialized(a))
            unloaded.push_back(a);
    }

    // Debug: dump a few sample values for every loaded input so we can
    // visually confirm (a) the data is non-zero, and (b) it lives at the
    // address the trace actually reads from. Enable with NIOBIUM_DEBUG_LIVEIN=1
    // or automatically if at least one input is all-zero.
    {
        const char* env = std::getenv("NIOBIUM_DEBUG_LIVEIN");
        bool force_dump = env && *env && std::string(env) != "0";
        size_t zero_addrs = 0;
        for (uint64_t a : rbw_set) {
            if (!impl_->simulator->is_initialized(a)) continue;
            const auto& v = impl_->simulator->get_polynomial(a);
            bool all_zero = true;
            for (uint64_t x : v) if (x != 0) { all_zero = false; break; }
            if (all_zero) ++zero_addrs;
        }
        if (force_dump || zero_addrs > 0) {
            std::cout << "[NIOBIUM-DBG] live-in data check ("
                      << zero_addrs << " all-zero of " << rbw_set.size() << "):" << std::endl;
            std::vector<uint64_t> sorted(rbw_set.begin(), rbw_set.end());
            std::sort(sorted.begin(), sorted.end());
            for (uint64_t a : sorted) {
                if (!impl_->simulator->is_initialized(a)) continue;
                const auto& v = impl_->simulator->get_polynomial(a);
                uint64_t q = impl_->simulator->get_modulus(a);
                bool all_zero = true;
                for (uint64_t x : v) if (x != 0) { all_zero = false; break; }
                if (impl_->verbose) {
                    std::cout << "[NIOBIUM-DBG]   %" << a
                            << " q=0x" << std::hex << q << std::dec
                            << " v[0..3]=" << (!v.empty()?v[0]:0)
                            << "," << (v.size()>1?v[1]:0)
                            << "," << (v.size()>2?v[2]:0)
                            << "," << (v.size()>3?v[3]:0)
                            << (all_zero ? "  [ALL-ZERO]" : "")
                            << std::endl;
                }
            }
        }
    }

    // Debug: dump unloaded live-in addresses + a sample of captured non-live-in.
    if (std::getenv("NIOBIUM_DEBUG_ADDRS") && !unloaded.empty()) {
        std::sort(unloaded.begin(), unloaded.end());
        std::set<uint64_t> captured_addrs;
        for (const auto& input : impl_->captured_inputs)
            for (const auto& e : input.elements)
                captured_addrs.insert(e.addr_id);
        std::cout << "[NIOBIUM-DBG] unloaded live-in: min=" << unloaded.front()
                  << " max=" << unloaded.back() << " count=" << unloaded.size()
                  << std::endl;
        std::cout << "[NIOBIUM-DBG] captured addrs: ";
        std::vector<uint64_t> sorted_cap(captured_addrs.begin(), captured_addrs.end());
        std::sort(sorted_cap.begin(), sorted_cap.end());
        if (!sorted_cap.empty())
            std::cout << "min=" << sorted_cap.front() << " max=" << sorted_cap.back()
                      << " count=" << sorted_cap.size();
        std::cout << std::endl;
        size_t overlap = 0;
        for (uint64_t a : unloaded) if (captured_addrs.count(a)) ++overlap;
        std::cout << "[NIOBIUM-DBG] unloaded ∩ captured = " << overlap << std::endl;
    }

    std::cout << "[NIOBIUM] Live-in: " << rbw_set.size()
              << ", loaded: " << direct_count << " direct + "
              << propagated << " propagated, unloaded: " << unloaded.size();
    if (!unloaded.empty() && unloaded.size() <= 100) {
        std::cout << " [";
        for (size_t i = 0; i < unloaded.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << "%" << unloaded[i];
        }
        std::cout << "]";
    }
    std::cout << std::endl;

    // Collect probe/output addresses so liveness leaves them in memory for
    // write_replay_outputs() to read after run() returns.
    std::vector<uint64_t> live_out;
    for (const auto& output : impl_->captured_outputs) {
        live_out.insert(live_out.end(),
                        output.addr_ids.begin(), output.addr_ids.end());
    }
    impl_->simulator->set_live_out_addresses(live_out);
    impl_->simulator->compute_liveness();

    auto result = impl_->simulator->run();

    if (result.errors > 0) {
        std::cerr << "[NIOBIUM] Replay failed: " << result.errors << " errors" << std::endl;
        return false;
    }

    std::cout << "[NIOBIUM] Replay complete: " << result.instructions_executed
              << " instructions, " << result.elapsed_seconds << "s" << std::endl;

    // Write output polynomial values for probe addresses
    write_replay_outputs();

    // Reconstruct ciphertext probes from simulator output
    // (reads fhetch_replay_outputs.json written above)
    reconstruct_probes();

    return true;
}

// ============================================================================
// dispatch_to_compiler_target — invoke nbcc_fhetch_replay for non-local targets
// ============================================================================
//
// When the user passes --target=<value> (value != "local") to the client, we
// skip the in-process FHETCH simulator and spawn the compiler-side driver.
// The driver reads the recorded fhetch project, re-records it through the
// full Niobium optimization pipeline, executes replay against the selected
// target device, and writes ciphertext probes into <program_dir>/serialized_probes/.
// result() on the client side reads from that same directory, so no further
// plumbing is needed.
//
// The executable is located via:
//   1. NBCC_FHETCH_REPLAY env var (absolute path, wins if set)
//   2. PATH lookup for "nbcc_fhetch_replay"
// Spawned-executable locator: the env var if set and non-empty, else the
// PATH-resolved default name.
static std::string env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? std::string(v) : std::string(fallback);
}

// niobium_hw is project-intrinsic (recorded into fhetch_replay.json), so
// replay_project reads it from the on-disk project rather than impl_ state.
static bool read_project_niobium_hw(const std::filesystem::path& dir) {
    std::ifstream ifs(dir / "fhetch_replay.json");
    if (!ifs.is_open()) return false;
    auto j = nlohmann::json::parse(ifs, nullptr, /*allow_exceptions=*/false);
    return !j.is_discarded() && j.value("niobium_hw", false);
}

bool Compiler::replay_project(const std::string& target, const std::filesystem::path& dir,
                              const std::string& opt_level) {
    // Build the worker command. "local" runs the bundled, open fhetch_sim (no
    // compiler dependency); any other target forwards the project to the
    // compiler-side nbcc_fhetch_replay. posix_spawnp with explicit argv (rather
    // than a shell string) avoids quoting hazards while preserving PATH lookup.
    std::string exec;
    std::vector<std::string> args;
    if (target == "local") {
        exec = env_or("NBCC_FHETCH_SIM", "fhetch_sim");
        args = {exec, "--project=" + dir.string()};
    } else {
        exec = env_or("NBCC_FHETCH_REPLAY", "nbcc_fhetch_replay");
        args = {exec, "--project=" + dir.string(), "--target=" + target};
        // Hardware format is project-intrinsic; opt-level is a replay-time
        // choice forwarded only when non-default (so the O0 argv is unchanged).
        if (read_project_niobium_hw(dir)) args.emplace_back("--niobium_hw");
        if (opt_level != "O0") args.push_back("--opt-level=" + opt_level);
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    std::cout << "[NIOBIUM] replay_project target '" << target << "' via:";
    for (const auto& a : args) std::cout << ' ' << a;
    std::cout << std::endl;

    pid_t pid = 0;
    int spawn_err = ::posix_spawnp(&pid, exec.c_str(), nullptr, nullptr,
                                   argv.data(), environ);
    int rc = -1;
    if (spawn_err == 0) {
        int status = 0;
        if (::waitpid(pid, &status, 0) == pid) {
            if (WIFEXITED(status)) {
                rc = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                rc = 128 + WTERMSIG(status);
            }
        }
    } else {
        std::cerr << "[NIOBIUM] posix_spawnp failed: errno=" << spawn_err
                  << std::endl;
    }
    if (rc != 0) {
        std::cerr << "[NIOBIUM] " << exec << " failed (exit " << rc
                  << "). Is the binary on PATH, or its locator env var set?"
                  << std::endl;
        return false;
    }

    // <dir>/serialized_probes/<name>.ct should now exist for every probe;
    // result()/result_from() read those directly.
    auto probes_dir = dir / "serialized_probes";
    if (!std::filesystem::exists(probes_dir) ||
        std::filesystem::is_empty(probes_dir)) {
        std::cerr << "[NIOBIUM] " << exec << " reported success but produced no "
                     "serialized probes in " << probes_dir << std::endl;
        return false;
    }

    std::cout << "[NIOBIUM] replay_project complete. Probes in "
              << probes_dir << std::endl;
    return true;
}

bool Compiler::dispatch_to_compiler_target() {
    return replay_project(impl_->target, get_program_directory(), impl_->opt_level);
}

bool Compiler::run_local_fhetch_driver() {
    auto dir = get_program_directory();
    std::string prog = impl_->full_program_name();

    std::string exec = env_or("NBCC_FHETCH_DRIVER", "fhetch_driver");

    auto probes_dir = dir / "serialized_probes";
    std::filesystem::create_directories(probes_dir);

    // Recorded probe names come from <prog>.outputs.json (written at record
    // time). On a replay pass the host calls no probe(), so memory is empty —
    // read the names back from disk.
    std::vector<std::string> output_names;
    {
        std::ifstream ifs(dir / (prog + ".outputs.json"));
        if (ifs.is_open()) {
            auto j = nlohmann::json::parse(ifs, nullptr, /*allow_exceptions=*/false);
            if (!j.is_discarded() && j.contains("outputs"))
                for (const auto& o : j["outputs"]) {
                    auto n = o.value("name", std::string());
                    if (!n.empty()) output_names.push_back(n);
                }
        }
    }
    if (output_names.empty()) {
        std::cerr << "[NIOBIUM] run_local_fhetch_driver: no probe names in "
                  << (dir / (prog + ".outputs.json")) << std::endl;
        return false;
    }

    // driver <trace> --ring-dim N --source-dir <dir> --cc <cc>
    //        --output-ct <name>:<serialized_probes/<name>.ct> ...
    std::string ring_dim_str = std::to_string(impl_->ring_dimension);
    std::string cc_path = (dir / "cryptocontext.dat").string();
    std::vector<std::string> args = {
        exec, impl_->last_trace_path.string(),
        "--ring-dim", ring_dim_str,
        "--source-dir", dir.string(),
        "--cc", cc_path,
    };
    // Propagate the hardware-compatibility check opt-outs to the spawned
    // driver, which runs its own init()/set_ring_dimension().
    if (!impl_->check_ring_dim) args.push_back("--no-ring-dim-check");
    if (!impl_->check_primes)   args.push_back("--no-prime-check");
    for (const auto& name : output_names) {
        args.push_back("--output-ct");
        args.push_back(name + ":" + (probes_dir / (name + ".ct")).string());
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    std::cout << "[NIOBIUM] Cooperative local replay via " << exec << " ("
              << output_names.size() << " probe(s))" << std::endl;

    pid_t pid = 0;
    int spawn_err = ::posix_spawnp(&pid, exec.c_str(), nullptr, nullptr,
                                   argv.data(), environ);
    int rc = -1;
    if (spawn_err == 0) {
        int status = 0;
        if (::waitpid(pid, &status, 0) == pid) {
            if (WIFEXITED(status)) rc = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) rc = 128 + WTERMSIG(status);
        }
    } else {
        std::cerr << "[NIOBIUM] posix_spawnp(fhetch_driver) failed: errno="
                  << spawn_err << std::endl;
    }
    if (rc != 0) {
        std::cerr << "[NIOBIUM] fhetch_driver failed (exit " << rc
                  << "). Is it on PATH, or NBCC_FHETCH_DRIVER set?" << std::endl;
        return false;
    }
    if (!std::filesystem::exists(probes_dir) || std::filesystem::is_empty(probes_dir)) {
        std::cerr << "[NIOBIUM] fhetch_driver produced no serialized probes in "
                  << probes_dir << std::endl;
        return false;
    }
    std::cout << "[NIOBIUM] Cooperative local replay complete. Probes in "
              << probes_dir << std::endl;
    return true;
}

// ============================================================================
// write_replay_json — serialize inputs, outputs, and metadata for replay
// ============================================================================

void Compiler::write_replay_json() {
    using json = nlohmann::json;
    auto dir = get_program_directory();
    auto path = dir / "fhetch_replay.json";
    std::string prog = impl_->full_program_name();

    json replay;

    // ---- program_name / program_info ----
    replay["program_name"] = prog + ".fhetch";
    replay["program_info"] = {
        {"name", impl_->program_name},
        {"version", impl_->program_version},
        {"description", impl_->program_description}
    };

    // ---- crypto_context (matches compiler's schema) ----
    json cc;
    cc["scheme_name"] = impl_->scheme_name;
    cc["ring_dimension"] = impl_->ring_dimension;
    cc["multiplicative_depth"] = impl_->multiplicative_depth;
    cc["scaling_modulus_size"] = impl_->scaling_mod_size;
    cc["security_level"] = impl_->security_level;
    // Use the trace writer's modulus table as the authoritative source —
    // it includes all moduli encountered during recording (base chain +
    // key-switching moduli), matching the .fhetch file's modulus_count.
    const auto& trace_moduli = impl_->trace_writer.modulus_table();
    if (!trace_moduli.empty()) {
        cc["modulus_chain"] = trace_moduli;
        cc["modulus_chain_length"] = trace_moduli.size();
        // Recompute inverse chain for the complete set
        std::vector<uint64_t> inv_chain;
        for (uint64_t q : trace_moduli) {
            uint64_t ninv = 1;
            for (int i = 1; i < 64; i++) {
                if (((q * ninv) >> i) & 1)
                    ninv |= (1ULL << i);
            }
            inv_chain.push_back(ninv);
        }
        cc["inverse_modulus_chain"] = inv_chain;
    } else {
        cc["modulus_chain"] = impl_->modulus_chain;
        cc["modulus_chain_length"] = impl_->modulus_chain.size();
        cc["inverse_modulus_chain"] = impl_->inverse_modulus_chain;
    }
    cc["is_valid"] = true;
    replay["crypto_context"] = cc;

    // Hardware format is project-intrinsic — record it so a singleton-free
    // replay_project() can decide whether to forward --niobium_hw.
    replay["niobium_hw"] = impl_->montgomery_mode && impl_->bitrev_mode;

    // ---- files ----
    json files;
    files["instructions"] = impl_->last_trace_path.filename().string();

    // Inputs (master index referencing per-input .bin + .ids)
    std::string inputs_index_file = prog + ".inputs.json";
    files["inputs"] = inputs_index_file;

    // Write the inputs index file (same format as compiler's inputs.cbor)
    {
        json inputs_index;
        inputs_index["program_name"] = prog + ".fhetch";
        // input_count is re-set below after filtering out key-named captures.
        inputs_index["input_format"] = "cereal_binary";
        inputs_index["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        json inputs_arr = json::array();
        for (const auto& input : impl_->captured_inputs) {
            // Eval keys are captured in m_captured_inputs (for data capture) but
            // serialized separately to .mk.bin / .rk.bin via the bespoke cereal
            // format in serialize_eval_keys (see auto_facade.cpp). Skip them
            // here so the compiler reads them through its evalmult_keys /
            // evalautomorphism_keys paths instead of treating them as user
            // inputs.
            if (input.name == "evalmult_key" ||
                input.name == "automorphism_key") {
                continue;
            }
            json idx;
            idx["name"] = input.name;
            idx["ids_file"] = prog + ".input_" + input.name + ".ids";
            idx["bin_file"] = prog + ".input_" + input.name + ".bin";
            idx["instances_count"] = 1;
            // Staleness metadata for cooperative replay: the on-disk file this
            // input was loaded from and its mtime at record time. A later
            // replay run compares the current mtime to refresh only changed
            // inputs (see Compiler::set_input_source / replay refresh).
            if (!input.source_path.empty()) {
                idx["source_path"] = input.source_path;
                idx["source_mtime"] = input.source_mtime;
            }
            inputs_arr.push_back(idx);
        }
        inputs_index["inputs"] = inputs_arr;
        inputs_index["input_count"] = inputs_arr.size();
        std::ofstream inp_out(dir / inputs_index_file);
        if (inp_out.is_open()) {
            inp_out << inputs_index.dump(2) << std::endl;
            inp_out.close();
        }
    }

    // Outputs
    std::string outputs_file = prog + ".outputs.json";
    files["outputs"] = outputs_file;

    // Write the outputs file (same format as compiler's outputs.cbor)
    {
        json outputs_data;
        outputs_data["program_name"] = prog + ".fhetch";
        outputs_data["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        json outputs_arr = json::array();
        for (const auto& output : impl_->captured_outputs) {
            json out_entry;
            out_entry["name"] = output.name;
            out_entry["payload_type"] = "ciphertext";
            json ct_data = json::array();
            for (size_t j = 0; j < output.addr_ids.size(); ++j) {
                json poly;
                poly["poly_index"] = j;
                poly["elements"] = json::array({output.addr_ids[j]});
                ct_data.push_back(poly);
            }
            out_entry["ciphertext_data"] = ct_data;
            outputs_arr.push_back(out_entry);
        }
        outputs_data["outputs"] = outputs_arr;
        std::ofstream out_out(dir / outputs_file);
        if (out_out.is_open()) {
            out_out << outputs_data.dump(2) << std::endl;
            out_out.close();
        }
    }

    // Key file references
    auto mk_bin = dir / (prog + ".mk.bin");
    if (std::filesystem::exists(mk_bin)) {
        files["evalmult_keys"] = (dir / (prog + ".mk.bin")).string();
        files["evalmult_ids"] = (dir / (prog + ".mk.ids")).string();
    }
    auto rk_bin = dir / (prog + ".rk.bin");
    if (std::filesystem::exists(rk_bin)) {
        files["evalautomorphism_keys"] = (dir / (prog + ".rk.bin")).string();
        files["evalautomorphism_ids"] = (dir / (prog + ".rk.ids")).string();
    }
    auto bp_bin = dir / (prog + ".bp.bin");
    if (std::filesystem::exists(bp_bin)) {
        files["bootstrap_precomp"] = (prog + ".bp.bin");
        files["bootstrap_precomp_ids"] = (prog + ".bp.ids");
    }

    replay["files"] = files;

    // ---- Top-level fields matching compiler's replay.json ----
    replay["input_format"] = "cereal_binary";
    // Keys are written via serialize_eval_keys in auto_facade.cpp using a
    // stable bespoke cereal layout: uint32 num_keys, then per-key
    // (uint32 av_size, av DCRTPolys, uint32 bv_size, bv DCRTPolys). Both
    // tests/fhetch_driver/main.cpp's load_key_bin and niobium-compiler's
    // cereal_binary key reader consume this format.
    replay["evalmult_format"] = "cereal_binary";
    replay["evalautomorphism_format"] = "cereal_binary";
    // Client recordings are always ordinary-form (standard residues, natural
    // order, ordinary immediates); this flag describes the recording, not the
    // replay target. Hardware-izing (input data, immediates, switchmodulus
    // blocks) happens driver-side when dispatch passes --niobium_hw.
    replay["niobium_hw"] = false;
    replay["num_registers"] = 16;
    replay["config_sectors"] = 1;
    replay["hbm_mode"] = "interleaved";
    replay["max_memory_id"] = 0;

    // Key start addr_ids
    json key_start;
    if (impl_->evalmult_start_addr_id > 0)
        key_start["evalmult"] = impl_->evalmult_start_addr_id;
    if (impl_->evalautomorphism_start_addr_id > 0)
        key_start["evalautomorphism"] = impl_->evalautomorphism_start_addr_id;
    if (!key_start.empty())
        replay["key_start_addr_ids"] = key_start;

    replay["generated_timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::ofstream out(path);
    if (out.is_open()) {
        out << replay.dump(2) << std::endl;
        out.close();
        std::cout << "[NIOBIUM] Replay JSON written: " << path << std::endl;
    }
}

// ============================================================================
// write_replay_outputs — after simulation, write computed probe values
// ============================================================================

void Compiler::write_replay_outputs() {
    using json = nlohmann::json;
    if (!impl_->simulator || impl_->captured_outputs.empty()) return;

    auto dir = get_program_directory();
    auto path = dir / "fhetch_replay_outputs.json";

    // Ops without a modulus operand (sr_automorph_eval, sr_ft, ...) record
    // modulus=0 in mod_map; emit COPY_MODULUS so the reader falls back to
    // the template's tower modulus instead of trying to build params with 0.
    constexpr uint64_t COPY_MODULUS = 0xFFFFFFFFFFFFFFFFULL;
    json root;
    json outputs_arr = json::array();
    for (const auto& output : impl_->captured_outputs) {
        json out_entry;
        out_entry["name"] = output.name;
        json elems_arr = json::array();
        for (size_t j = 0; j < output.addr_ids.size(); ++j) {
            uint64_t addr = output.addr_ids[j];
            auto values = impl_->simulator->get_polynomial(addr);
            json elem;
            elem["addr_id"] = addr;
            elem["modulus"] = (output.moduli[j] == 0) ? COPY_MODULUS : output.moduli[j];
            elem["status"] = values.empty() ? "missing" : "computed";
            elem["values"] = values;
            elems_arr.push_back(elem);
        }
        out_entry["elements"] = elems_arr;
        outputs_arr.push_back(out_entry);
    }
    root["outputs"] = outputs_arr;

    std::ofstream out(path);
    if (out.is_open()) {
        out << root.dump(2) << std::endl;
        out.close();
        std::cout << "[NIOBIUM] Replay outputs written: " << path << std::endl;
    }
}

// ============================================================================
// State queries
// ============================================================================

bool Compiler::running_p() {
    return impl_->running;
}

bool Compiler::stopped_p() {
    return impl_->stopped;
}

std::string Compiler::program_name() {
    return impl_->full_program_name();
}

std::filesystem::path Compiler::get_program_directory() const {
    if (!impl_->program_dir.empty())
        return impl_->program_dir;

    // Default: create directory next to the executable or in cwd
    auto name = impl_->full_program_name();
    if (name.empty()) name = "niobium_trace";
    auto dir = std::filesystem::current_path() / name;
    return dir;
}

void Compiler::set_program_directory(const std::filesystem::path& dir) {
    impl_->program_dir = dir;
}

// Friend function declared in compiler.h — provides internal access to Impl.
Compiler::Impl& compiler_impl(Compiler& c) {
    return *c.impl_;
}

}  // namespace niobium

namespace niobium::detail {

// Iterate the Compiler's captured_inputs / captured_outputs records.
// The local Impl reference is const so the iteration is structurally
// read-only: the helpers themselves never mutate captured_inputs /
// captured_outputs. A misbehaving callback could still mutate Compiler
// state by reaching back through the niobium::compiler() singleton —
// callers must not do that mid-iteration (it would invalidate the
// vector iterator).
namespace {

// Build a CapturedShape's per_element_moduli from a flat moduli vector
// and array-element start offsets. Shared between the input and output
// iterators below.
std::vector<std::vector<uint64_t>>
slice_per_element_moduli(niobium::CapturedKind kind, const std::vector<uint64_t>& flat,
                         const std::vector<size_t>& element_starts) {
    using namespace niobium;
    std::vector<std::vector<uint64_t>> out;
    if (flat.empty()) {
        // Edge case: a record with no residues yet. Fall back to one empty
        // element so callers get a non-empty per_element_moduli they can
        // size-check without a second branch.
        out.emplace_back();
        return out;
    }
    switch (kind) {
        case CapturedKind::SRP:
        case CapturedKind::MRP: {
            out.emplace_back(flat.begin(), flat.end());
            break;
        }
        case CapturedKind::SRPArray: {
            // Every residue is its own array position.
            out.reserve(flat.size());
            for (auto q : flat) out.push_back({q});
            break;
        }
        case CapturedKind::MRPArray: {
            // element_starts delineates per-element residue ranges.
            // Defensive: if element_starts is empty (e.g. legacy records
            // rehydrated via the cache-hit path), treat the whole record
            // as a single MRP element.
            if (element_starts.empty()) {
                out.emplace_back(flat.begin(), flat.end());
                break;
            }
            const size_t k = element_starts.size();
            out.reserve(k);
            for (size_t i = 0; i < k; ++i) {
                const size_t lo = element_starts[i];
                const size_t hi = (i + 1 < k) ? element_starts[i + 1] : flat.size();
                out.emplace_back(flat.begin() + lo, flat.begin() + hi);
            }
            break;
        }
    }
    return out;
}

}  // namespace

void for_each_captured_input(const std::function<void(const CapturedInputRecord&)>& cb) {
    const auto& impl = niobium::compiler_impl(niobium::compiler());
    for (const auto& input : impl.captured_inputs) {
        CapturedInputRecord rec;
        rec.name = input.name;
        rec.addr_ids.reserve(input.elements.size());
        rec.per_residue_values.reserve(input.elements.size());
        std::vector<uint64_t> flat_moduli;
        flat_moduli.reserve(input.elements.size());
        for (const auto& e : input.elements) {
            rec.addr_ids.push_back(e.addr_id);
            rec.per_residue_values.push_back(e.values);
            flat_moduli.push_back(e.modulus);
        }
        rec.shape.kind = input.kind;
        rec.shape.per_element_moduli =
            slice_per_element_moduli(input.kind, flat_moduli, input.element_starts);
        cb(rec);
    }
}

void for_each_captured_output(
    const std::function<void(const std::string&, const CapturedShape&)>& cb) {
    const auto& impl = niobium::compiler_impl(niobium::compiler());
    for (const auto& output : impl.captured_outputs) {
        CapturedShape shape;
        shape.kind = output.kind;
        shape.per_element_moduli =
            slice_per_element_moduli(output.kind, output.moduli, output.element_starts);
        cb(output.name, shape);
    }
}

}  // namespace niobium::detail

// ============================================================================
// Internal accessor for TraceWriter — used by fhetch_api.cpp and probes.cpp.
// ============================================================================

namespace niobium::detail {

TraceWriter& trace_writer() {
    auto& impl = niobium::compiler_impl(niobium::compiler());
    return impl.trace_writer;
}

bool montgomery_mode() {
    return niobium::compiler_impl(niobium::compiler()).montgomery_mode;
}

bool bit_reversal_mode() {
    return niobium::compiler_impl(niobium::compiler()).bitrev_mode;
}

}  // namespace niobium::detail
