# C API notes discovered while writing the example

The example is intended to exercise the C API as a user would meet it. These
choices are part of the test, not implementation trivia hidden in a binding.

## Initialization and modes

`b3_plain()`, `b3_keyed()` and `b3_derive_key()` return the `b3_mode` value.
That value is passed by address to `b3_hasher_init` and other calls. The
startup self-test is reached through the API's initialization calls; there is
no separate setup in this example. A keyed invocation reads exactly 32 bytes
from standard input and refuses `-` as an input, because the key and message
would otherwise compete for stdin.

## Storage and alignment

A `b3_hasher` is caller-owned and has `B3_HASHER_SIZE` bytes plus a 64-byte
alignment requirement. The example declares the type from `b3.h` directly,
which makes the C compiler apply `B3_ALIGN64`. A caller using heap storage
must use an allocator that provides the same alignment; ordinary `malloc`
is not an over-alignment promise. The C header and the linked library should
be the same version (`B3_VERSION` and `b3_version()`).

## Output

The default 32-byte digest is obtained with `b3_hasher_finalize`. For a
length other than 32 or a nonzero output offset, `b3_hasher_output` supplies
XOF bytes. Output buffers are owned by the caller. Raw output is deliberately
restricted to one input, while hex output can be tagged or omit filenames.

## Threads

The default path uses `b3_hasher_update_multithreaded` and therefore the
library's pool. The C header documents initialization of that pool but does
not expose a public function for setting its thread count. This example
therefore uses its own bounded file-worker set for non-default
`--num-threads` values and uses serial updates for those workers. `--no-mmap`
uses one worker, and `--num-threads 1` uses one worker. This is preferable to
accepting an option and silently using a different number of library threads.

The worker implementation joins every thread it creates, including the
thread-creation failure path. Output is printed only after all results are
available, preserving argument order. Each worker owns its `b3_hasher`; a
`b3_mode` is read-only and shared as the API allows.

## Check files

Normal `<hash>  <path>` and tagged `BLAKE3 (<path>) = <hash>` forms are
accepted. The path escaping matches b3sum's `\\`, `\n` and `\r` forms. The
checker rejects malformed hex, wrong-length hashes, empty paths, embedded
NULs and failed reads. It never treats a failed read as a passing checksum.
`--quiet` affects only successful `OK` lines; failures are still printed.

## Platform boundary

This example uses POSIX `open`, `fstat`, `mmap`, `fdopen`, `opendir` and
pthreads. Those are deliberate example-platform choices, not claims about
BLAKE3-Servil's supported Windows builds. The C API itself has separate
MSVC CI and installed-consumer tests. A Windows example would need its own
file/path and thread handling rather than pretending this POSIX code is
portable.
