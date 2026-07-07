#!/usr/bin/env python3
"""plaintext_add client — Python port of tests/plaintext_add/client.cpp.

Generates a CKKS context + keys, encrypts [1..10], serializes for the server.
Pure OpenFHE. Usage: client.py <dir>.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from _nbcommon import o, BIN  # noqa: E402

INPUT = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0]


def main(argv):
    d = argv[1] if len(argv) > 1 else "plaintext_add_keys"
    os.makedirs(d, exist_ok=True)

    p = o.CCParamsCKKSRNS()
    p.SetSecretKeyDist(o.UNIFORM_TERNARY)
    p.SetSecurityLevel(o.SecurityLevel.HEStd_NotSet)
    p.SetRingDim(2048)
    p.SetScalingModSize(59)
    p.SetScalingTechnique(o.FLEXIBLEAUTO)
    p.SetFirstModSize(60)
    p.SetMultiplicativeDepth(2)
    cc = o.GenCryptoContext(p)
    for feat in (o.PKE, o.KEYSWITCH, o.LEVELEDSHE):
        cc.Enable(feat)

    kp = cc.KeyGen()
    o.SerializeToFile(f"{d}/cc.bin", cc, BIN)
    o.SerializeToFile(f"{d}/pk.bin", kp.publicKey, BIN)
    o.SerializeToFile(f"{d}/sk.bin", kp.secretKey, BIN)
    ct = cc.Encrypt(kp.publicKey, cc.MakeCKKSPackedPlaintext(INPUT))
    o.SerializeToFile(f"{d}/ciphertext.bin", ct, BIN)
    print(f"client complete -> {d}/")


if __name__ == "__main__":
    main(sys.argv)
