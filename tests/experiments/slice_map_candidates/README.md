# SliceMap candidate measurements

These experiments compare current production SliceMap with isolated source variants.
Production `src/`, `include/`, the normal build configuration, and installed library
are unchanged. Generated copies live only in the selected experiment build directory.
The experiment links their object files before the unchanged production static archive,
so each executable calls the candidate through the existing public ABI.

The completed September 30 measurements and decisions are in [RESULTS.md](RESULTS.md).

## Candidates

- `baseline`: unchanged SliceMap and Slice sources.
- `deferred_payload_hazard`: avoid constructing and clearing the payload hazard on a miss.
- `combined_publication_counter`: combine successful linking's counter decrement and position claim.
- `map_bookkeeping`: both map changes together.
- `fused_metadata_clone`: resolve each Slice copy's source and destination Region once.

The candidate arguments are in [MAP_CANDIDATES.md](MAP_CANDIDATES.md) and
[SLICE_CANDIDATES.md](SLICE_CANDIDATES.md). Pool allocation, batched ID allocation,
publication-directory changes, and a compact SIMD index are not implemented or measured
by this experiment. Their production adoption also requires separate measurements.

## Build and verify

From the repository root:

```sh
./run_build.sh --no-color --build-dir build/map-candidates \
    --install-dir build/map-candidates/install -DCMAKE_BUILD_TYPE=Release \
    -DBUFFETALLIGATOR_BUILD_TESTS=OFF -DBUFFETALLIGATOR_BUILD_ALLOCATOR_TESTS=OFF \
    -DBUFFETALLIGATOR_BUILD_BENCHMARKS=OFF \
    -DBUFFETALLIGATOR_ENABLE_METAL=ON \
    -DCMAKE_PROJECT_INCLUDE="$PWD/tests/experiments/slice_map_candidates/register.cmake"
```

The CMake hook registers its own tests despite disabling the normal test targets.
Each candidate runs the existing map ownership, concurrency, growth, Slice core,
global lifetime, and backend shader checks. The growth test includes the candidate
map source at its existing instrumentation point. Compiler flags, public definitions,
dependencies, and benchmark object code match across candidates; Release retains
the project's toolchain-probed LTO. On macOS, GPU checks include native and translated
Metal with API and shader validation. Actual hardware coverage is recorded in CTest.

## Measure

Run while builds and other heavy workloads are idle:

```sh
ruby tests/experiments/slice_map_candidates/run.rb \
    build/map-candidates build/map-candidates/results-next
ruby tests/experiments/slice_map_candidates/analyze.rb build/map-candidates/results-next
```

The runner requires a fresh results directory, completed registered checks, and
matching production source hashes. It runs 60 processes sequentially: five candidates,
four configurations, and three series with rotated candidate and configuration order.
Configurations cover 4,096, 65,536, and 1,048,576 rows on one thread, plus 65,536 rows
with eight writers followed by eight readers. Every process uses two warmups, fifteen
measured repetitions, and at least 262,144 queries per lookup phase.

The existing benchmark validates every measured result. Inserts include metadata/node
allocation; successful lookups create retained Slice owners, released outside timing.
Payload setup, reset, thread creation, and verification are outside timing. Both
containers pay the Slice-copy cost; the fused-copy candidate can improve either column.
The multi-threaded std baseline uses a single mutex even during its separate reader
phase. This is not a comparison against an unlocked read-only std map.

Raw CSVs, complete per-process output, commands, source/binary hashes, compiler cache,
compile commands, correctness output, and host metadata are saved with the results.
The analyzer checks the complete measurement matrix and reports median-of-process-medians
with observed ranges, not confidence intervals. A range overlap or a small difference
does not establish a winner. The benchmark's existing "SIMD buckets" startup label is
stale: current key lookup walks sorted chains; SIMD is used in hazard reclamation.

The runner checks for competing builds/tests and benchmarks before each process and
at its one-second progress heartbeat, rejecting a contaminated sweep after its current
process finishes; it does not stop unrelated work or control all host applications,
thermals, scheduling, or work too short for that polling interval. Supported-platform
qualification and concurrent read/write performance need separate evidence. No experiment automatically changes
production or declares a performance acceptance threshold met.
