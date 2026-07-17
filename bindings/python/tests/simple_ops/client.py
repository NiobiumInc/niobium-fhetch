#!/usr/bin/env python3
"""simple_ops client — Python port of tests/simple_ops/client.cpp.

Generates a CKKS context + keys, encrypts two values, serializes everything for
the server. Pure OpenFHE (no niobium session). Usage: client.py <dir> [a] [b].
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from _nbcommon import o, BIN  # noqa: E402  (loads libnbfhetch, imports openfhe)


def main(argv):
    d = argv[1] if len(argv) > 1 else "simple_ops_keys"
    a = float(argv[2]) if len(argv) > 2 else 5.0
    b = float(argv[3]) if len(argv) > 3 else 6.0
    os.makedirs(d, exist_ok=True)

    # CKKS params identical to client.cpp (compiler TOY defaults).
    p = o.CCParamsCKKSRNS()
    p.SetSecurityLevel(o.SecurityLevel.HEStd_NotSet)
    p.SetRingDim(2048)
    p.SetMultiplicativeDepth(3)
    p.SetScalingModSize(42)
    p.SetFirstModSize(57)
    p.SetScalingTechnique(o.FLEXIBLEAUTO)
    cc = o.GenCryptoContext(p)
    for feat in (o.PKE, o.KEYSWITCH, o.LEVELEDSHE, o.ADVANCEDSHE):
        cc.Enable(feat)

    kp = cc.KeyGen()
    cc.EvalMultKeyGen(kp.secretKey)
    cc.EvalRotateKeyGen(kp.secretKey, [1, -1])   # for MORPH (EvalRotate ±1)

    o.SerializeToFile(f"{d}/cc.bin", cc, BIN)
    o.SerializeToFile(f"{d}/pk.bin", kp.publicKey, BIN)
    o.SerializeToFile(f"{d}/sk.bin", kp.secretKey, BIN)
    cc.SerializeEvalMultKey(f"{d}/mk.bin", BIN, "")
    cc.SerializeEvalAutomorphismKey(f"{d}/rk.bin", BIN, "")

    # ct_a packs (a, b) so EvalRotate(ct_a, 1)[0] == b; ct_b = (b).
    ct_a = cc.Encrypt(kp.publicKey, cc.MakeCKKSPackedPlaintext([a, b]))
    ct_b = cc.Encrypt(kp.publicKey, cc.MakeCKKSPackedPlaintext([b]))
    o.SerializeToFile(f"{d}/ct_a.bin", ct_a, BIN)
    o.SerializeToFile(f"{d}/ct_b.bin", ct_b, BIN)
    with open(f"{d}/values.txt", "w") as fh:
        fh.write(f"{a} {b}\n")
    print(f"client complete -> {d}/")


if __name__ == "__main__":
    main(sys.argv)
