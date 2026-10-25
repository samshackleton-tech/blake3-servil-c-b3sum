# b3sum-c: a complete C example for BLAKE3-Servil

This repository shows how to use **BLAKE3-Servil through its C API** in a real
command-line program. It is not a binding generator or a one-call hash demo:
`src/b3sum.c` hashes files, directories and standard input, offers keyed and
key-derivation modes, extended output, check files and the usual b3sum output
formats. It has end-to-end tests and a benchmark using the project's existing
`bench-hashes b3sum` workload.

## What it does

| Feature | Command |
|---|---|
| Hash one file or many files | `build/b3sum-c FILE...` |
| Recursively hash a directory | `build/b3sum-c DIRECTORY` |
| Read standard input | `build/b3sum-c` or `build/b3sum-c -` |
| Keyed hashing (32 key bytes on stdin) | `build/b3sum-c --keyed FILE` |
| Key derivation | `build/b3sum-c --derive-key CONTEXT FILE` |
| Extended output and seeking | `build/b3sum-c --length 80 --seek 7 FILE` |
| Raw bytes | `build/b3sum-c --raw --length 80 --seek 7 FILE` |
| BSD-style output | `build/b3sum-c --tag FILE` |
| No filenames | `build/b3sum-c --no-names FILE` |
| Read, not mmap | `build/b3sum-c --no-mmap FILE` |
| Limit file workers | `build/b3sum-c --num-threads 2 FILE...` |
| Check normal or tagged sums | `build/b3sum-c --check CHECKS.txt` |
| Quiet checking | `build/b3sum-c --check --quiet CHECKS.txt` |
| Help and version | `build/b3sum-c --help`, `build/b3sum-c --version` |

The program is POSIX C11. It uses `mmap` for regular files when enabled,
falling back to streamed reads. `--no-mmap` uses one worker, as the b3sum
contract describes. It preserves argument order and sorts directory entries.
Check lines support the normal and `BLAKE3 (file) = hash` forms, including
b3sum's escaped newline, carriage-return and backslash path forms.

## Build

Requirements:

- a C11 compiler and POSIX platform (tested on Linux);
- Rust and Cargo;
- `cargo-c` 0.10.26 or later and `pkg-config`;
- Python 3 for the tests and benchmark wrapper.

Clone with the pinned BLAKE3-Servil submodule, then build both shared- and
static-linked examples:

```sh
git clone --recurse-submodules https://github.com/samshackleton-tech/blake3-servil-c-b3sum
cd blake3-servil-c-b3sum
make
```

`make servil` runs `cargo cinstall` for the submodule's `capi/`, installing
into `build/servil`. The example is compiled with the header and linker flags
from `pkg-config`, as the C API README recommends. The shared binary gets a
local rpath for this build tree; an installed program should use the normal
system loader configuration instead.

The submodule is pinned to **`2ae28f20449bd84ab18b0852f036c4ad21dbe9fb`**.
The C header and library report version `0.8.0`. The source is not bundled
into the example: users can inspect the exact upstream API it was tested
against.

For an existing installation, the same compile shape is:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic src/b3sum.c \
    $(pkg-config --cflags --libs b3_servil) -pthread -o b3sum-c
```

## Tests

```sh
make test
# or, if the independent b3sum is elsewhere:
make test OFFICIAL_B3SUM=/path/to/b3sum
```

The test script runs the shared and static executables independently and
compares their behavior with the independent `b3sum` on PATH. It covers:

- empty, binary, large, nested and unusual filenames;
- directories, multiple inputs, standard input, mmap and no-mmap;
- plain, keyed and derive-key modes;
- XOF length/seek, raw and hex output, tags and no-names;
- thread counts 0, 1 and 2, plus `RAYON_NUM_THREADS`;
- normal/tagged checks, quiet checks, mismatches and malformed lines;
- option errors, help and version output.

No benchmark process is started by `make test`.

## Benchmark

Benchmark the example against the independent `b3sum` with the existing
`bench-hashes b3sum` command, not a custom timing loop:

```sh
python3 benchmark/benchmark.py \
    --bin build/b3sum-c \
    --bench-dir /path/to/bench-hashes \
    --quick --rounds 4
```

The wrapper records the command, exact binary SHA-256, bench checkout and
commit. `bench-hashes` writes the samples, statistics, map and result files.
`--quick` selects its smaller workload; omit it for the full workload. The
initial public result is a quick diagnostic screen, not a release or
cross-machine claim: [EPYC9255 quick benchmark and correctness record](results/epyc-20261025-quick/RESULT.md).
It shows the current CLI/file-worker design is slower than official in most
cells, with one warm many-small-files exception. The benchmark includes
process startup, filesystem traversal and the C wrapper; it is not a
kernel-only comparison. Run native benchmarks on a quiet EPYC9255 or
another explicitly chosen host, not on a shared development laptop.

## C API lessons

The example intentionally uses the API's actual contracts:

- `b3_hasher` is caller-owned and **64-byte aligned**. Use the type from
  `b3.h`, not an untyped byte array. Heap storage must satisfy that alignment.
- Initialization happens through the C API entry points. `b3_hasher_init`
  and the other calls take the `b3_mode` value returned by `b3_plain`,
  `b3_keyed` or `b3_derive_key`.
- `b3_hasher_output` supplies arbitrary XOF offsets and lengths; ordinary
  32-byte output can use `b3_hasher_finalize`.
- `b3_hasher_update` and `b3_hasher_update_multithreaded` both accept streamed
  input. The default case uses the library's pool. The public C API has no
  thread-count setter, so non-default `--num-threads` values are enforced by
  this example's own file-worker count instead of being silently ignored.
- The C API's pointer, lifetime, alignment and non-overlap obligations are
  real preconditions. Simple local guards are useful, but a C library cannot
  validate arbitrary allocation ownership or lifetime.

These are documented in [`docs/C-API-NOTES.md`](docs/C-API-NOTES.md), with the
choices that make this example useful as an API/documentation test.

## Scope and provenance

This is an example program, not a replacement for the upstream `b3sum`
package or a new BLAKE3 implementation. It is not yet a supported Windows
example: its directory and file handling is POSIX-specific. Its benchmark is
separate from the correctness tests, and a speed result on one host is not a
promise on another. Read the source pins and result provenance before using
any numbers here.

Released into the public domain under CC0-1.0; see `LICENSE`.
