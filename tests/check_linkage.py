#!/usr/bin/env python3
"""Show from the binaries themselves which BLAKE3 library they use.

The end-to-end tests compare outputs with an official b3sum, which is a
separate program; this check confirms that the example itself links
BLAKE3-Servil's C API (libb3_servil) and nothing from official BLAKE3.
Linux/ELF only: it reads the binaries with readelf, nm and ldd.
"""
import argparse
import platform
import re
import subprocess
import sys
from pathlib import Path


def run(argv):
    return subprocess.run(argv, check=True, capture_output=True, text=True).stdout


def needed(binary):
    return re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", run(["readelf", "-d", binary]))


def defined_symbols(binary):
    names = set()
    for line in run(["nm", "--defined-only", binary]).splitlines():
        parts = line.split()
        if len(parts) >= 3:
            names.add(parts[2])
    return names


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True)
    parser.add_argument("--static-bin", required=True)
    parser.add_argument("--lib-dir", required=True)
    args = parser.parse_args()
    if platform.system() != "Linux":
        print("linkage: skipped (this check reads ELF binaries on Linux)")
        return

    lib_dir = Path(args.lib_dir).resolve()
    checks = 0

    # Shared build: needs libb3_servil, found in this build's install prefix.
    deps = needed(args.bin)
    assert any(d.startswith("libb3_servil.so") for d in deps), ("shared", deps)
    assert not any("blake3" in d.lower() for d in deps), ("shared", deps)
    resolved = re.search(r"libb3_servil\.so\S*\s+=>\s+(\S+)", run(["ldd", args.bin]))
    assert resolved, "ldd did not resolve libb3_servil"
    assert Path(resolved.group(1)).resolve().parent == lib_dir, resolved.group(1)
    checks += 3

    # Static build: the C API is inside the binary; no libb3_servil needed.
    deps = needed(args.static_bin)
    assert not any("b3_servil" in d or "blake3" in d.lower() for d in deps), ("static", deps)
    symbols = defined_symbols(args.static_bin)
    for name in ("b3_hasher_init", "b3_hasher_update", "b3_hasher_finalize", "b3_version"):
        assert name in symbols, ("static", name)
    # Official BLAKE3's C library uses blake3_hasher_* names; none may appear.
    official = sorted(s for s in symbols if s.startswith("blake3_hasher_"))
    assert not official, ("static", official)
    checks += 3

    for label, binary in (("shared", args.bin), ("static", args.static_bin)):
        version = run([binary, "--version"])
        assert "BLAKE3-Servil 0.8.0" in version, (label, version)
        checks += 1

    print(f"linkage: {checks} checks passed (both binaries use libb3_servil, BLAKE3-Servil 0.8.0)")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as error:
        sys.exit(f"linkage check failed: {error}")
