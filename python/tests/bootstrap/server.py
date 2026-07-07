#!/usr/bin/env python3
"""bootstrap server — Python port of tests/bootstrap/server.cpp.

Deserializes context/keys/ciphertext, re-runs bootstrap precompute, records
EvalBootstrap under hollow mode, replays, serializes the result.
Usage: server.py <dir> [niobium-init-flags...]
  NIOBIUM_BOOTSTRAP_HOLLOW=0 forces real-math recording (default: hollow).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from _nbcommon import o, BIN  # noqa: E402

LEVEL_BUDGET = [4, 4]


def main(argv):
    d = argv[1]
    flags = list(argv[2:])

    import niobium_session as nb
    cc, ok = o.DeserializeCryptoContext(f"{d}/cc.bin", BIN)
    if not ok:
        sys.exit("failed to load crypto context")
    if not cc.DeserializeEvalMultKey(f"{d}/mk.bin", BIN):
        sys.exit("failed to load eval mult key")
    if not cc.DeserializeEvalAutomorphismKey(f"{d}/rk.bin", BIN):
        sys.exit("failed to load eval automorphism keys")
    ciph, ok = o.DeserializeCiphertext(f"{d}/ciphertext.bin", BIN)
    assert ok, "ciphertext"

    # Re-run precompute (fires precompute probes) before capture.
    cc.EvalBootstrapSetup(LEVEL_BUDGET)

    nb.init(flags)
    nb.set_program_info("bootstrap_server", "1.0", "CKKS bootstrapping")
    nb.set_build_info(__file__, 1, "n/a")
    nb.cache_parameters([("workload", "ckks_bootstrap")])
    nb.capture_crypto_context(cc)
    nb.tag_input("input_cipher", ciph)
    nb.tag_keys(cc)

    hollow = os.environ.get("NIOBIUM_BOOTSTRAP_HOLLOW", "1") != "0"
    if not nb.is_cache_valid():
        print(f"recording bootstrap ({'hollow' if hollow else 'real'} mode)...")
        nb.enable_hollow_mode(hollow)
        nb.start()
        out = cc.EvalBootstrap(ciph)
        nb.probe("output_cipher", out)
        nb.stop()
        nb.enable_hollow_mode(False)

    if not nb.replay():
        sys.exit("replay() failed")
    ok, ct_result = nb.result(cc, "output_cipher")
    if not ok:
        sys.exit("result() failed")
    o.SerializeToFile(f"{d}/ct_result.bin", ct_result, BIN)
    print(f"server complete -> {d}/ct_result.bin")


if __name__ == "__main__":
    main(sys.argv)
