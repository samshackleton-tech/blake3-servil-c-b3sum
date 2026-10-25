#!/usr/bin/env python3
"""Benchmark the example with bench-hashes' existing b3sum workload."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", required=True)
    parser.add_argument("--bench-dir", required=True, help="a bench-hashes checkout")
    parser.add_argument("--official", default=shutil.which("b3sum"))
    parser.add_argument("--rounds", type=int, default=4)
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--out", default="build/benchmark-results")
    args = parser.parse_args()
    binary = Path(args.bin).resolve()
    bench = Path(args.bench_dir).resolve()
    official = shutil.which(args.official) if args.official else None
    assert binary.is_file(), binary
    assert (bench / "Cargo.toml").is_file(), bench
    assert official and Path(official).is_file(), "an independent b3sum is required"
    assert args.rounds >= 2, "bench-hashes needs at least two rounds"
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    # The example uses the default library pool here; no experimental selectors.
    env.pop("BLAKE3_DIAGNOSTIC_GPR", None)
    env.pop("BLAKE3_DIAGNOSTIC_LINE_OFFSET", None)
    command = [
        "cargo", "run", "--release", "--manifest-path", str(bench / "Cargo.toml"), "--",
        "b3sum", "--rounds", str(args.rounds),
    ]
    if args.quick:
        command.append("--quick")
    command += [f"example={binary}", f"official={official}"]
    result = subprocess.run(command, cwd=out, env=env)
    if result.returncode:
        raise SystemExit(result.returncode)
    provenance = {
        "example": str(binary),
        "example_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "official": official,
        "bench_checkout": str(bench),
        "bench_commit": subprocess.check_output(["git", "-C", str(bench), "rev-parse", "HEAD"], text=True).strip(),
        "command": command,
        "quick": args.quick,
        "rounds": args.rounds,
        "note": "Timing and statistics are owned by bench-hashes b3sum, not this wrapper.",
    }
    (out / "example-provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    print(f"benchmark results and provenance: {out}")


if __name__ == "__main__":
    main()
