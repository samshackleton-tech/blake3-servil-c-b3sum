# EPYC quick benchmark and correctness record

Example source: `4e822bfacf7fac182bcb86d78caf8134c8640ae6`.
BLAKE3-Servil submodule: `2ae28f20449bd84ab18b0852f036c4ad21dbe9fb`.
Reference: official b3sum 1.8.2 at
`/root/results/released-cli-v7/bin/b3sum-official-1.8.2-L0`.
Benchmark: `bench-hashes b3sum` from `d6ce88f6537a7fbd11ab9533853818b4adaf4824`,
`--quick --rounds 4`, on EPYC9255. All timing and statistics are from the
existing tool; this wrapper supplies no timing loop.

## Correctness

A clean `make clean && make test` passed **76 checks**: 38 shared and 38
static. Tests cover files, directories, stdin, plain/keyed/derive-key modes,
XOF, raw/tagged output, checking, thread options, malformed inputs and
formatting. The static binary has no `libb3_servil.so` dependency. Logs are
in `tests/`.

## Quiet quick benchmark

The run was marked quiet: other programs used 0.33 CPUs on average and 0.58
in the busiest window. This is a small diagnostic workload, not a release or
cross-machine performance claim.

### Warm files

| Input | Example | Official | Ratio (example / official) |
|---|---:|---:|---:|
| 4 KiB | 610 us | 1.71 ms | 2.802x slower |
| 1 MiB | 757 us | 1.86 ms | 2.465x slower |
| 16 MiB | 1.72 ms | 2.67 ms | 1.582x slower |
| 100 x 16 KiB | 2.82 ms | 2.46 ms | **0.875x faster** |
| 100 x 16 KiB, process each | 50.5 ms | 161 ms | 3.195x slower |
| mixed tree, 100 files, 5 MiB | 2.89 ms | 3.96 ms | 1.369x slower |
| 4 x 16 MiB | 3.57 ms | 5.32 ms | 1.482x slower |

### Cold files

| Input | Example | Official | Ratio (example / official) |
|---|---:|---:|---:|
| 4 KiB | 720 us | 1.80 ms | 2.523x slower |
| 1 MiB | 1.24 ms | 2.25 ms | 1.816x slower |
| 16 MiB | 4.78 ms | 5.94 ms | 1.240x slower |
| 100 x 16 KiB | 3.38 ms | 9.69 ms | 2.850x slower |
| 100 x 16 KiB, process each | 59.2 ms | 167 ms | 2.822x slower |
| mixed tree, 100 files, 5 MiB | 4.02 ms | 9.07 ms | 2.224x slower |
| 4 x 16 MiB | 12.2 ms | 20.3 ms | 1.664x slower |

The benchmark includes process startup, directory expansion, file workers,
I/O and the C wrapper. It is not a kernel-only comparison. The example is a
correct API/CLI example, not a performance-equivalent replacement for the
upstream CLI. No tuning claim is made from the one warm faster cell.

## Busy first attempt

The first benchmark attempt reported 0.67 CPUs average and 1.37 in the
busiest window. Those figures are **not evidence of speed** and are kept
only as a diagnostic under `diagnostic-busy/`. The quiet result above is
kept separately in `quiet/`.

Raw samples, result text, map, binary provenance and logs are retained.
Generated input files are omitted; `bench-hashes b3sum` can regenerate them.
