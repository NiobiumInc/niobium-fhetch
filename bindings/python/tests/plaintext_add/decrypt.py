#!/usr/bin/env python3
"""plaintext_add decrypt — Python port of tests/plaintext_add/decrypt.cpp.

Verifies each of the 10 slots equals 2*(i+1). Usage: decrypt.py <dir> [ct_file].
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from _nbcommon import o, BIN  # noqa: E402

EXPECTED = [2.0 * (i + 1) for i in range(10)]


def main(argv):
    d = argv[1]
    ct_file = argv[2] if len(argv) > 2 else "ct_result.bin"

    cc, ok = o.DeserializeCryptoContext(f"{d}/cc.bin", BIN)
    assert ok, "cc"
    sk, ok = o.DeserializePrivateKey(f"{d}/sk.bin", BIN)
    assert ok, "sk"
    ct, ok = o.DeserializeCiphertext(f"{d}/{ct_file}", BIN)
    assert ok, ct_file

    pt = cc.Decrypt(ct, sk)
    pt.SetLength(len(EXPECTED))
    got = pt.GetRealPackedValue()
    tol = max(0.01, 2.0 ** -(pt.GetLogPrecision() - 2))
    ok_all = True
    for i, exp in enumerate(EXPECTED):
        diff = abs(got[i] - exp)
        if abs(exp) > 1.0:
            diff /= abs(exp)
        if diff > tol:
            ok_all = False
    print(f"[{'PASS' if ok_all else 'FAIL'}] plaintext_add: "
          f"{[round(x, 3) for x in got]} (tol {tol:.4g})")
    sys.exit(0 if ok_all else 1)


if __name__ == "__main__":
    main(sys.argv)
