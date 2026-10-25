# FlatSliceMap prototype status — September 30, 2026

The opaque Abseil-backed prototype is implemented and its correctness checks pass.
Production sources and public headers are unchanged. The full performance matrix is
unfinished because unrelated acsimd builds repeatedly overlapped measurements.

## Completed validation

`run_build.sh` completed the Release build and all four registered tests passed:
the new FlatSliceMap ownership/published-reader test, the existing SliceMap test,
and two logger tests. No tests were skipped. The prototype checks independent Slice
IDs and metadata, zero-copy backing retention, replacement, growth, reset/destruction,
exact backing release counts, null/extreme keys, and published concurrent readers.

The production map, candidate implementation, and repository-pinned Abseil 20260107.1
were compiled with `-O3 -DNDEBUG -std=c++20 -flto=thin -arch arm64 -fPIC` on Apple
M4 Max, macOS 26.6.2, AppleClang 17. This does not qualify other supported platforms.

## First-series observations, not final benchmark conclusions

The latest sweep completed 11 of its planned 42 processes before another unrelated
build overlapped the next process. The affected attempt was stopped and archived under
`discarded/`; it contributes no numbers below. No competing build/test process was
observed during the 11 accepted processes, but thermals and all background load were
not controlled. There are no completed repeat series yet.

These are medians of eleven samples in one process, after two warmups, with fully
reserved tables and sequential positive integer keys. Units are aggregate ns/op;
lower is better. They demonstrate why insertion alone cannot select a general winner.

| Rows | Operation | SliceMap | std::unordered_map | FlatSliceMap |
| ---: | --- | ---: | ---: | ---: |
| 65,536 | Insert | 31.47 | 12.41 | 8.34 |
| 65,536 | Retained hit | 18.14 | 12.02 | 15.52 |
| 65,536 | Miss | 4.41 | 1.64 | 3.47 |
| 65,536 | Replace | 29.20 | 6.59 | 10.91 |
| 1,048,576 | Insert | 30.41 | 9.28 | 12.99 |
| 1,048,576 | Retained hit | 18.02 | 8.60 | 29.17 |
| 1,048,576 | Miss | 6.80 | 1.64 | 3.54 |
| 1,048,576 | Replace | 32.07 | 5.33 | 25.39 |

FlatSliceMap improves insertion against SliceMap in these samples, but its million-row
retained hit is slower, and std leads all four million-row dense-key operations.
Abseil is not established as a universal performance winner.

The aligned-key stress model strongly penalizes this machine's default std hashing:
logs report power-of-two bucket counts equal to the row count, while libc++'s integer
hash preserves key bits and reduces those hashes with a mask. Keys separated by 256
therefore form chains of 256 entries; the negative aligned misses traverse those
occupied buckets too. This measures sensitivity to that hashing/bucket policy as well
as container layout. Dense and aligned-key results must stay separate.

## What remains

Complete the repeated matrix during a quiet window before selecting a production
implementation. The million-row aligned-key process took about three minutes, so the
full current matrix needs approximately twenty minutes of clean measurement time,
plus any waits or retries. The runner will wait through builds, reject contaminated
attempts, and refuse to report an incomplete matrix as complete.

The intended type has exclusive mutations and shared immutable reads after external
publication. It does not provide SliceMap's stable positional access, pointer views,
publication hooks/waits, typed finalizers, or concurrent mutation contracts. A production
API decision still needs to account for which of those contracts callers require.

Local evidence is under `build/flat-map-experiment/results-clean/`: `run.json`, accepted
CSV/log pairs, `correctness.log`, compile commands, source hashes, and discarded-attempt
records. `results/` and `results-final/` are earlier interrupted sweeps and must not be
merged into a claimed complete run. See [README.md](README.md) for reproduction.
