# FlatSliceMap experiment

This experiment evaluates a separate map for exclusive mutation and shared immutable
read phases using `absl::flat_hash_map<int64_t, Slice>`. Production sources, public
headers, and normal CMake targets are unchanged. The Abseil dependency is the version
already pinned by `run_build.sh` (20260107.1), compiled from those sources inside this
experiment build with the same Release settings and probed LTO support.

See [PRELIMINARY.md](PRELIMINARY.md) for the current validation, interrupted measurement
status, and explicitly preliminary observations.

## Contract

The opaque `experiments::FlatSliceMap` owns its table in a private implementation.
`add_slice` inserts or replaces by moving ownership; `get_slice` returns an ordinary,
independently owned Slice copy; `reset` releases map ownership while preserving table
capacity. Returned Slices retain backing across rehash, replacement, reset, and map
destruction. No payload bytes are copied. Keys can store null Slices, so a null lookup
does not distinguish a missing key from a stored null value.

Every mutation requires exclusive access. Multiple readers may use the unchanged map
after external publication, with no writer, reset, or destruction overlapping them.
This does not synchronize mutations to payload bytes through returned Slices.

Abseil stores entries inline and invalidates element pointers during rehash, making
independently retained Slice handles a useful boundary; see the
[official container guide](https://abseil.io/docs/cpp/guides/container).
This prototype does not implement SliceMap's stable positions, contiguous pointer view,
publication hooks, wait thresholds, typed finalizers, or concurrent mutation contracts.
It is a separate type, not a drop-in replacement.

## Build and correctness

From the repository root:

```sh
./run_build.sh --no-color --build-dir build/flat-map-experiment \
    --install-dir build/flat-map-experiment/install -DCMAKE_BUILD_TYPE=Release \
    -DBUFFETALLIGATOR_BUILD_TESTS=OFF -DBUFFETALLIGATOR_BUILD_ALLOCATOR_TESTS=OFF \
    -DBUFFETALLIGATOR_BUILD_BENCHMARKS=OFF -DBUFFETALLIGATOR_ENABLE_METAL=ON \
    -DCMAKE_PROJECT_INCLUDE="$PWD/tests/experiments/flat_slice_map/register.cmake"
```

The hook registers the FlatSliceMap ownership test and the existing SliceMap test.
The ownership test checks independent IDs and metadata, exact backing release counts,
growth, replacement, null/missing/extreme keys, reset/destruction, and concurrent readers
after exclusive construction. It exercises the actual opaque candidate and public Slice
ABI. The hook also records source hashes at configuration for provenance checks.

## Measurement

```sh
ruby tests/experiments/flat_slice_map/run.rb \
    build/flat-map-experiment build/flat-map-experiment/results-next
ruby tests/experiments/flat_slice_map/analyze.rb build/flat-map-experiment/results-next
```

Use a fresh results directory with other builds and benchmarks idle. The runner waits
through competing work and retries affected processes, keeping discarded attempts
separate from the 42 valid processes in three series. Each
process compares all three containers in rotated order, with two warmups and eleven
measured repetitions. Payload preparation, map construction/initial reservation, thread
creation, result destruction, and correctness checks are outside the timer.

The single-thread matrix covers 4,096, 65,536, and 1,048,576 rows; full reservation versus
growth from 16; and two deterministic key/access models. Dense keys increase by one with
cyclic queries. Sparse keys increase by 256 with an odd-stride query permutation covering
every row at these power-of-two sizes. These are models, not recorded production traces.
Misses use the corresponding negative keys. Lookup counts are at least 262,144 and at
least the table size.

Two additional configurations use 65,536 and 1,048,576 fully reserved dense rows with one
builder followed by eight readers. std and Abseil need no container mutex in this frozen
phase. Reported ns/op measures aggregate wall time divided by operations, not individual
request latency. Writes and reads never overlap.

Insertion and replacement transfer prepared Slice claims. All three containers use
ordinary Slice copies for retained hits, including their arena allocation/reference
costs. The initial fixture retains backing through replacement, so replacement measures
releasing the map's claim rather than freeing sole-owned backing. Growth costs are timed
only in the growth configuration. Every sample starts with a fresh map; old SliceMap
states are explicitly collected on the coordinator while workers are quiescent, outside
timing. Final row capacities, std bucket counts, and Abseil slot counts are logged.

These controls differ from the previous candidate sweep: that harness used `.slice()`
for std hits and deferred table reclamation across resets. Compare the three containers
within this experiment; do not attribute differences between reports solely to Abseil.
Default hashing policies also differ, so this compares complete containers, not only
their layouts. Repeat ranges are descriptive, not confidence intervals. Results on this
host do not qualify other operating systems or CPU architectures.
