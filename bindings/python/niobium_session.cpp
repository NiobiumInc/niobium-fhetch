// pybind11 binding for the niobium::compiler() session API.
// Companion to the vendored openfhe-python crypto module: both link the single
// loaded instrumented OpenFHE + libnbfhetch, so recording can be driven from
// Python (cross-module pybind type sharing passes openfhe Ciphertext/CryptoContext).
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "openfhe.h"
#include "niobium/compiler.h"

namespace py = pybind11;
using namespace lbcrypto;

PYBIND11_MODULE(niobium_session, m) {
    m.doc() = "niobium::compiler() session bindings";

    m.def("init", [](std::vector<std::string> args) {
        static char prog[] = "niobium_session";
        static std::vector<std::string> storage;   // keep backing strings alive
        storage = std::move(args);
        std::vector<char *> argv;
        argv.push_back(prog);
        for (auto &s : storage) argv.push_back(s.data());
        int argc = static_cast<int>(argv.size());
        char **a = argv.data();
        niobium::compiler().init(argc, a);
    }, py::arg("args") = std::vector<std::string>{});

    m.def("set_program_info", [](const std::string &n, const std::string &v, const std::string &d) {
        niobium::compiler().set_program_info(n, v, d);
    }, py::arg("name") = "", py::arg("version") = "", py::arg("description") = "");
    m.def("set_build_info", [](const std::string &f, int l, const std::string &t) {
        niobium::compiler().set_build_info(f, l, t);
    }, py::arg("file") = "", py::arg("line") = 0, py::arg("timestamp") = "");
    m.def("cache_parameters", [](std::vector<std::pair<std::string, std::string>> p) {
        niobium::Compiler::CacheParameters cp(p.begin(), p.end());
        niobium::compiler().cache_parameters(cp);
    });
    m.def("is_cache_valid", []() { return niobium::compiler().is_cache_valid(); });
    m.def("start", []() { return niobium::compiler().start(); });
    m.def("stop", []() { return niobium::compiler().stop(); });
    m.def("pause", []() { return niobium::compiler().pause(); });
    m.def("resume", []() { return niobium::compiler().resume(); });
    m.def("is_running", []() { return niobium::compiler().running_p(); });
    m.def("is_stopped", []() { return niobium::compiler().stopped_p(); });
    m.def("replay", []() { return niobium::compiler().replay(); });
    // set_ring_dimension is intentionally NOT bound here: in the recorder path
    // capture_crypto_context() sets it from the CryptoContext's own ring dim
    // (OpenFHE's GetRingDimension()), so a manual setter is redundant. It belongs
    // to the direct-FHETCH-IR binding (niobium_ir), where there is no CryptoContext.
    m.def("get_program_directory", []() {
        return niobium::compiler().get_program_directory().string();
    });

    // Templated on OpenFHE types — instantiated for DCRTPoly here.
    m.def("capture_crypto_context", [](CryptoContext<DCRTPoly> cc) {
        niobium::compiler().capture_crypto_context(cc);
    });
    m.def("tag_input", [](const std::string &name, Ciphertext<DCRTPoly> ct) {
        niobium::compiler().tag_input(name, ct);
    });
    m.def("tag_input", [](const std::string &name, Plaintext pt) {
        niobium::compiler().tag_input(name, pt);
    });
    m.def("enable_hollow_mode", [](bool enabled) {
        niobium::compiler().enable_hollow_mode(enabled);
    }, py::arg("enabled") = true);
    m.def("tag_keys", [](CryptoContext<DCRTPoly> cc) {
        niobium::compiler().tag_keys(cc);
    });
    m.def("probe", [](const std::string &name, Ciphertext<DCRTPoly> ct) {
        niobium::compiler().probe(name, ct);
    });
    // Returns (ok, ciphertext); the out-ciphertext is populated on success.
    m.def("result", [](CryptoContext<DCRTPoly> cc, const std::string &name) {
        Ciphertext<DCRTPoly> out;
        bool ok = niobium::compiler().result(cc, name, out);
        return py::make_tuple(ok, out);
    });
}
