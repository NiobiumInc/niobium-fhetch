#!/usr/bin/env python3
"""TEMPORARY bespoke smoke for session endpoints the recorder scenarios don't reach:
pause/resume, is_running/is_stopped, get_program_directory.

These have no recorder-channel (ep-2) C++ scenario to mirror — their faithful tests
belong elsewhere: get_program_directory to the IR-emission channel's simple_fhetch
port, and pause/resume to a ScopedPause utility test. This file intentionally
diverges from the replicate-only convention; REMOVE it once the IR channel provides
that coverage.

Run: session_api_smoke.py     Env: NB_FHETCH_LIB, PYTHONPATH (build/python).
"""
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _nbcommon import o, BIN  # noqa: E402


def main():
    import niobium_session as nb

    # Minimal CKKS context + two ciphertexts (relin key for the paused EvalMult).
    p = o.CCParamsCKKSRNS()
    p.SetSecurityLevel(o.SecurityLevel.HEStd_NotSet)
    p.SetRingDim(2048)
    p.SetMultiplicativeDepth(2)
    p.SetScalingModSize(42)
    cc = o.GenCryptoContext(p)
    for feat in (o.PKE, o.KEYSWITCH, o.LEVELEDSHE):
        cc.Enable(feat)
    kp = cc.KeyGen()
    cc.EvalMultKeyGen(kp.secretKey)
    a = cc.Encrypt(kp.publicKey, cc.MakeCKKSPackedPlaintext([1.0, 2.0]))
    b = cc.Encrypt(kp.publicKey, cc.MakeCKKSPackedPlaintext([3.0, 4.0]))

    nb.init(["--no-ring-dim-check"])
    nb.set_program_info("session_api_smoke", "1.0", "session endpoint smoke")
    nb.set_build_info(__file__)  # line/timestamp default; see wheel facade for auto-capture
    nb.cache_parameters([("workload", "session_api")])
    nb.capture_crypto_context(cc)
    nb.tag_input("a", a)
    nb.tag_input("b", b)
    nb.tag_keys(cc)

    checks = []
    checks.append(("is_running() == False before start", nb.is_running() is False))
    nb.start()
    checks.append(("is_running() == True after start", nb.is_running() is True))

    r = cc.EvalAdd(a, b)      # recorded
    nb.pause()
    _ = cc.EvalMult(a, b)     # NOT recorded — paused
    nb.resume()
    nb.probe("result", r)
    nb.stop()

    checks.append(("is_running() == False after stop", nb.is_running() is False))
    checks.append(("is_stopped() == True after stop", nb.is_stopped() is True))

    # get_program_directory() points at where the trace was written.
    pdir = nb.get_program_directory()
    traces = glob.glob(os.path.join(pdir, "*.fhetch"))
    checks.append((f"get_program_directory() holds the trace ({pdir})", len(traces) == 1))

    # pause() worked: the paused EvalMult's sr_mulp must be absent; the EvalAdd present.
    trace = open(traces[0]).read() if traces else ""
    checks.append(("trace has sr_addp (recorded EvalAdd)", "sr_addp" in trace))
    checks.append(("trace has NO sr_mulp (paused EvalMult excluded)", "sr_mulp" not in trace))

    ok = True
    for name, passed in checks:
        print(f"  [{'PASS' if passed else 'FAIL'}] {name}")
        ok = ok and passed
    print("session_api smoke:", "ALL PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
