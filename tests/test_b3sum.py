#!/usr/bin/env python3
"""End-to-end checks for the example C b3sum, against an independent b3sum."""
import argparse
import concurrent.futures
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

OFFICIAL = os.environ.get("B3SUM_OFFICIAL") or shutil.which("b3sum")


def call(argv, data=None, env=None, timeout=45):
    assert OFFICIAL, "the independent b3sum executable is required for these tests"
    return subprocess.run(argv, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, timeout=timeout)


def good(argv, data=None, env=None):
    result = call(argv, data, env)
    assert result.returncode == 0, (argv, result.returncode, result.stderr.decode(errors="replace"))
    return result.stdout


def expect_failure(argv, data=None):
    result = call(argv, data)
    assert result.returncode != 0, (argv, result.stdout, result.stderr)
    return result


def suite(label, binary):
    root = Path(tempfile.mkdtemp(prefix=f"b3sum-c-{label}-"))
    try:
        plain = root / "plain.txt"
        plain.write_bytes(b"abc")
        spaces = root / "space name.bin"
        spaces.write_bytes(bytes(range(256)))
        empty = root / "empty"
        empty.write_bytes(b"")
        nested = root / "nested" / "deep"
        nested.mkdir(parents=True)
        payload = nested / "payload.bin"
        payload.write_bytes(bytes((i * 37 + 11) % 251 for i in range(1024 * 1024)))
        newline = root / "line\nbreak"
        newline.write_bytes(b"newline path")
        file_args = [str(plain), str(spaces), str(empty), str(payload), str(newline)]
        checks = 0

        def same(args, data=None, env=None):
            nonlocal checks
            got = good([binary, *args], data, env)
            want = good([OFFICIAL, *args], data, env)
            assert got == want, (label, args, got[:200], want[:200])
            checks += 1
            return got

        def own(args, data=None, env=None):
            nonlocal checks
            got = good([binary, *args], data, env)
            want = good([OFFICIAL, *args], data, env)
            assert got == want, (label, args)
            checks += 1
            return got

        # Basic files, directories, stdin, XOF and both output forms.
        own([str(plain)])
        own([str(spaces)])
        own([str(empty)])
        own(file_args)
        own([str(root)])
        own([], b"abc\0binary\xff")
        own(["--no-names", str(plain)])
        own(["--tag", str(spaces)])
        own(["--length", "80", "--seek", "7", str(payload)])
        own(["--length", "0", str(plain)])
        own(["--raw", "--length", "80", "--seek", "7", str(payload)])
        own(["--raw", "--length", "0", str(plain)])
        own(["--no-mmap", str(payload)])
        own(["--num-threads", "1", str(payload)])
        own(["--num-threads", "2", str(payload)])
        own(["--num-threads", "0", str(payload)], env=dict(os.environ, RAYON_NUM_THREADS="1"))

        # Keyed and derive-key modes, including reading the key from stdin.
        key = bytes(range(32))
        own(["--keyed", str(payload)], key)
        context = "b3sum-c example/test context"
        own(["--derive-key", context, str(payload)])

        # Check files in both formats, including spaces and an escaped newline.
        checks_text = own(file_args).decode()
        tagged_text = own(["--tag", *file_args]).decode()
        normal = root / "checks.txt"
        normal.write_text(checks_text)
        tagged = root / "tagged.txt"
        tagged.write_text(tagged_text)
        got = good([binary, "--check", str(normal)]).decode()
        want = good([OFFICIAL, "--check", str(normal)]).decode()
        assert got == want, (label, got, want)
        checks += 1
        got = good([binary, "--check", "--tag", str(tagged)]).decode()
        want = good([OFFICIAL, "--check", "--tag", str(tagged)]).decode()
        assert got == want, (label, got, want)
        checks += 1
        assert good([binary, "--check", "--quiet", str(normal)]) == b""
        checks += 1
        assert good([binary, "--check", "--quiet", str(normal)], env=dict(os.environ, RAYON_NUM_THREADS="1")) == b""
        checks += 1

        bad = root / "bad.txt"
        bad.write_text(checks_text.replace(checks_text.split()[0], "0" * 64, 1))
        result = expect_failure([binary, "--check", str(bad)])
        assert b"FAILED" in result.stdout + result.stderr
        checks += 1
        malformed = root / "malformed.txt"
        malformed.write_text("not a checksum\n")
        expect_failure([binary, "--check", str(malformed)])
        checks += 1

        # Command-line contract errors and useful help/version output.
        for args in [
            ["--quiet", str(plain)],
            ["--raw", str(plain), str(spaces)],
            ["--raw", "--tag", str(plain)],
            ["--keyed", "--derive-key", "x", str(plain)],
            ["--check", "--length", "64", str(normal)],
            ["--no-names", "--check", str(normal)],
            ["--length", "not-a-number", str(plain)],
            ["--num-threads", "not-a-number", str(plain)],
            ["--not-an-option", str(plain)],
        ]:
            expect_failure([binary, *args])
            checks += 1
        help_text = good([binary, "--help"]).decode()
        assert "Usage:" in help_text and "--derive-key" in help_text
        version = good([binary, "--version"]).decode()
        assert "BLAKE3-Servil" in version and "0.8.0" in version
        checks += 2
        return label, checks
    finally:
        shutil.rmtree(root)


def main():
    global OFFICIAL
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True)
    parser.add_argument("--static-bin", required=True)
    parser.add_argument("--official", default=OFFICIAL,
                        help="independent reference b3sum (default: B3SUM_OFFICIAL or PATH)")
    args = parser.parse_args()
    OFFICIAL = args.official
    binaries = [("shared", args.bin), ("static", args.static_bin)]
    for _, path in binaries:
        assert Path(path).is_file(), path
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        results = list(pool.map(lambda item: suite(*item), binaries))
    for label, count in results:
        print(f"{label}: {count} checks passed")
    print(f"all {sum(count for _, count in results)} checks passed")


if __name__ == "__main__":
    main()
