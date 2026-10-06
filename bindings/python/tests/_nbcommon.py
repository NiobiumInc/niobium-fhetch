"""Shared bootstrap for the niobium-fhetch Python tests.

openfhe.so references probe globals defined in libnbfhetch (e.g. g_replay_mode);
load libnbfhetch into the global namespace before importing the crypto module so
those symbols resolve. Import this first, then use `o` (openfhe) and `BIN`.

Env: NB_FHETCH_LIB = path to libnbfhetch.{dylib,so}.
"""
import ctypes
import os
import sys

_lib = os.environ.get("NB_FHETCH_LIB")
if not _lib or not os.path.exists(_lib):
    sys.exit(f"NB_FHETCH_LIB not set or missing: {_lib!r}")
ctypes.CDLL(_lib, mode=ctypes.RTLD_GLOBAL)

import openfhe as o  # noqa: E402

BIN = o.BINARY

# Niobium hardware runs exactly one ring dimension, 2^16. The clients default
# to it; the Makefile test targets pass 2^11 (TEST_RING_DIM) with
# --no-ring-dim-check, for speed.
HW_RING_DIM = 1 << 16


def ring_dim_arg(argv, i):
    """Ring dimension from argv[i], else the hardware's 2^16."""
    return int(argv[i]) if len(argv) > i else HW_RING_DIM
