#!/usr/bin/env python3
# Copyright 2024-present Niobium Microsystems, Inc.
# Licensed under the Apache License, Version 2.0.
"""Checks that the recorder-free core is recorder-free, from its symbols.

Usage: check_symbols.py <libnbfhetch_core.a> [<libnbfhetch.dylib|.so>]

The core (first argument) must neither define nor reference niobium::Compiler or
niobium::compiler(), and must not reference an OpenFHE cprobe hook
(openfhe_cprobe_*, and OpenFHE's misspelt openfhe_cporbe_with_openmp) or a
niobium_auto:: hook. The optional second argument, the
recorder library, is a control: it must define niobium::compiler(), so a scan
that finds nothing there means the scan itself is broken.

Exits 0 when every check holds, 1 when one fails, 2 on a usage error.
"""
import re
import subprocess
import sys

FORBIDDEN_REFERENCES = {
    # OpenFHE spells one hook openfhe_cporbe_with_openmp.
    "OpenFHE cprobe hook": re.compile(r"openfhe_cp(?:ro|or)be"),
    "niobium_auto hook": re.compile(r"niobium_auto::"),
    "Compiler symbol": re.compile(r"niobium::Compiler\b|niobium::compiler\(\)"),
}
COMPILER = FORBIDDEN_REFERENCES["Compiler symbol"]


def symbols(path, undefined):
    """Demangled symbol names of `path`: its undefined ones, or its defined external ones."""
    flags = ["-u"] if undefined else ["-g", "--defined-only"]
    listing = subprocess.run(["nm", *flags, path], capture_output=True, text=True, check=True).stdout
    names = [line.split()[-1] for line in listing.splitlines() if line.strip() and not line.endswith(":")]
    demangled = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True, check=True)
    return set(demangled.stdout.splitlines())


def report(ok, what):
    print(("  ok   " if ok else "  FAIL ") + what)
    return ok


def main(argv):
    if len(argv) not in (2, 3):
        print(__doc__, file=sys.stderr)
        return 2
    core = argv[1]
    undefined = symbols(core, undefined=True)
    defined = symbols(core, undefined=False)
    ok = report(bool(defined), f"{core}: {len(defined)} defined, {len(undefined)} undefined symbols")
    for what, rx in FORBIDDEN_REFERENCES.items():
        hits = sorted(s for s in undefined if rx.search(s))
        ok &= report(not hits, f"references no {what}" + (f": {hits[:5]}" if hits else ""))
    defined_compiler = sorted(s for s in defined if COMPILER.search(s))
    ok &= report(not defined_compiler, "defines no Compiler symbol" + (f": {defined_compiler[:5]}" if defined_compiler else ""))
    if len(argv) == 3:
        recorder_defined = symbols(argv[2], undefined=False)
        ok &= report(any(COMPILER.search(s) for s in recorder_defined),
                     f"control: {argv[2]} defines the Compiler, so the scan sees such symbols")
    print("all checks passed" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
