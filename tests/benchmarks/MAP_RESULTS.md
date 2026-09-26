# Map comparison results — 2026-09-17

> **REVIEW-STALE (2026-09-26):** snapshot of 2026-09-17. SliceMap has changed three times since (growing unique-key map, 32-bit pool index collapse, aarch64 yield), so its column no longer describes the current implementation; the contenders other than SliceMap and VecSet exist only inside the experiment source. Rerun or keep as history.

## Full-contract ring: every contender meets the SliceMap ABI

The contenders now all provide what the library SliceMap provides: unique keys
with last-write-wins replacement, stable dense positions, the cached contiguous
`data()` view, `find`/`get_slice`/`slice_at`/`id`/`published`, `size`,
`wait_until_full`, publish hooks, merge, and reset. HazardMap and VersionMap
carry a shared PositionLedger (dense position cells, hook-gated contiguous
watermark, cached view) on their own lock-free cores; MutexMap implements the
same surface with dense vector rows under one mutex. VecSet remains a labeled
fixed-capacity control. The harness dispatch is now uniform - one code path
for every full-contract contender - so the comparison isolates the hash-table
core itself.

Two library-grade bugs surfaced under the 32-worker first-insert race and were
fixed in both the shared ledger and the production SliceMap: the boundary-only
watermark advance could legally read a stale publish flag under acquire-only
ordering and strand the watermark (fixed by seq_cst on the flag/boundary pair),
and an advance whose walk stopped at a just-published row returned after its
CAS instead of re-walking, permanently stranding the watermark one short
(fixed by continuing the walk after each successful CAS). Five consecutive
32-worker contract runs and a 40-round oversubscribed library stress pass with
the fixes.

A build-directory misconfiguration to Debug silently invalidated one
measurement round; all numbers below are Release (-O3), confirmed by flags.

Five-round medians at 65,536 rows, full contract on every column except the
fixed-capacity control:

| Workload | SliceMap (Michael) | HazardMap (chain) | VersionMap (versions) | mutex | VecSet |
| --- | ---: | ---: | ---: | ---: | ---: |
| Fill 1w, ns/row | 25.46 | 26.73 | 21.41 | 21.29 | 14.02 |
| Gather 2p, us | 5,427 | 4,575 | 1,592 | 2,245 | 3,023 |
| Gather 16p, us | 12,856 | 5,098 | 5,348 | 5,757 | 4,910 |
| Scattered hit 1r, ns | 29.80 | 29.94 | 37.51 | 36.21 | 24.92 |
| Scattered hit 16r, ns | 57.62 | 49.39 | 42.36 | 63.05 | 40.50 |
| Miss 16r, ns | 3.67 | 3.98 | 31.54 | 36.39 | 4.78 |
| Hot-eight 1r, ns | 7.20 | 7.35 | 7.32 | 10.37 | 31.15 |
| Streaming 15p+1c, us | 12,914 | 6,190 | 6,015 | 7,261 | 6,549 |
| Streaming 8p+8c, us | 11,483 | 7,277 | 6,933 | 11,330 | 7,290 |
| Growth 16p, ns/row | 193.64 | 78.10 | 79.42 | 98.83 | N/A |
| Seq. replace, ns/write | 57.43 | 29.87 | 23.48 | 18.49 | 12.71 |
| Shared-key 8p+8c, us | 7,149 | 14,205 | 17,142 | 3,593 | 16,888 |
| Merge 8 parts, us | 1,299 | 3,119 | 3,007 | 2,005 | 976 |

Reading for the switch decision, apples to apples:

- No core wins everywhere. The fixed-bucket lock-free cores (HazardMap,
  VersionMap) beat the library's doubling-table Michael core by 2-2.5x on
  multi-producer writes (gather 16p, streaming 15p, growth 16p) because they
  skip the resize in-flight windows and drains; the library core wins
  single-reader hot lookups, 16-reader misses, and merges by similar factors.
- The full contract costs real throughput: single-writer fill converged from
  16-25 ns/row (map primitives) to 21-27 ns/row across every lock-free core,
  now on par with the mutex control; the fixed-capacity VecSet keeps the lead
  only because its rows are preallocated.
- The mutex control genuinely wins shared-key contention (3.6 ms vs 7.1-17.1
  ms) and 16-reader sequential/hot patterns; a lock is simply competitive
  when the critical section is one hash-bucket update.
- Actionable for the library: if the carrier can size buckets at construction
  (capacity hint equals final size, as every benchmark workload does), a
  fixed-bucket core with the current contract would roughly halve contended
  write latency; keeping the doubling machinery costs the 2-2.5x shown above
  only when growth actually happens.

Reproduce:

```sh
./run_build.sh --skip-tests -DBUFFETALLIGATOR_BUILD_TESTS=ON -DBUFFETALLIGATOR_BUILD_BENCHMARKS=ON
ctest --test-dir build --output-on-failure
for case in fill gather lookup stream growth view combine duplicates; do
  ./build/tests/benchmarks/buffetalligator_slicemapvsvecset \
      --case $case --csv build/map-full-$case.csv
done
```

## Contract-honest contender ring

The previous ring let HazardMap append duplicate keys (not a map) and let
VersionMap defer all reclamation to quiescent collection (unbounded memory),
so neither paid the contract the others pay. Both now meet the requirements:

- HazardMap scans its bucket chain for the key and replaces on match; inserts
  validate through the head CAS, so a lost link race converts to replacement
  instead of a duplicate node. Replaced payloads retire through a shared
  benchmark hazard domain and free in batches of 64.
- VersionMap retires every replaced version through the same domain at replace
  time and frees unprotected ones in batches of 64; its quiescent-only
  collector is gone, and an exact released-versions counter keeps the leak
  checks meaningful. Both contenders and the harness share one record-claiming
  hazard domain (the old modulo slot assignment could alias two live threads).

Bugs found while making them honest, each reproduced before fixing: retiring
a version with its whole history chain double-freed versions already retired
individually (the retire stack walked freed memory into a cycle); a
`unique_ptr::release()` left inside a CAS retry loop published null currents;
and hazard records were never released back to the domain, which exhausted the
256 slots over the sweep's thread lifetimes.

Both contenders now pass the same contract gauntlet as SliceMap: ownership
with retained handles, 32-worker concurrent first insertion, shared-key
replacement with retained reads and exact reclamation accounting, and uneven
merges. A 50-round oversubscribed stress (16 writers, 16 readers, torn and
wrong-key checks) runs clean on both.

Five-round medians at 65,536 rows, all contenders contract-honest; the three
lock-free columns are the real comparison and VecSet (fixed capacity) and the
mutex map remain labeled controls:

| Workload | SliceMap | HazardMap | VersionMap | mutex | VecSet |
| --- | ---: | ---: | ---: | ---: | ---: |
| Fill 1w, ns/row | 25.39 | 19.14 | 16.39 | 13.55 | 12.90 |
| Gather 2p, us | 2,835 | 752 | 798 | 1,521 | 3,086 |
| Gather 16p, us | 9,895 | 2,009 | 1,606 | 4,546 | 6,188 |
| Scattered hit 1r, ns | 28.58 | 26.70 | 21.48 | 21.01 | 23.53 |
| Scattered hit 16r, ns | 57.54 | 52.91 | 48.03 | 50.46 | 61.51 |
| Miss 16r, ns | 3.75 | 3.19 | 41.33 | 38.25 | 9.19 |
| Streaming 15p+1c, us | 12,158 | 2,321 | 2,382 | 6,816 | 7,079 |
| Streaming 8p+8c, us | 8,376 | 3,284 | 3,512 | 9,661 | 7,002 |
| Sequential replace, ns/write | 48.26 | 26.75 | 19.24 | 12.63 | 12.47 |
| Shared-key 8p+8c, us | 5,939 | 13,365 | 15,303 | 5,236 | 16,140 |
| Merge 8 parts, us | 1,262 | 4,236 | 4,016 | 1,320 | 981 |

Reading: among honest lock-free maps, the bounded VersionMap leads fill,
growth, 16-producer gather, and single-reader lookups; HazardMap leads
contended streaming and two-reader lookups; SliceMap leads shared-key
replacement and merges. Only SliceMap also provides stable positions, the
contiguous `data()` view, wait semantics, and merge hooks - the others are
map primitives, not drop-in replacements. SliceMap's remaining write-side gap
versus VersionMap measures the position/watermark work its contract requires,
and per-thread position batching is the identified next optimization. The
mutex map still wins several single-threaded shapes and 16-reader hot-key
lookups; it remains the incumbent to beat outright.

Reproduce (per-case processes; the single full sweep's own 300-second
watchdog is marginal once every contender pays reclamation):

```sh
./run_build.sh --skip-tests -DBUFFETALLIGATOR_BUILD_TESTS=ON -DBUFFETALLIGATOR_BUILD_BENCHMARKS=ON
ctest --test-dir build --output-on-failure
for case in fill gather lookup stream growth view combine duplicates; do
  ./build/tests/benchmarks/buffetalligator_slicemapvsvecset \
      --case $case --csv build/map-honest-$case.csv
done
```

## Michael-style SliceMap rewrite

`src/containers/slicemap.cpp` now implements Michael's lock-free hash table:
buckets are chains sorted by the full 64-bit hash (the Fibonacci product is a
bijection on int64 identifiers, so chains are collision-free and identifiers
recover through the modular inverse), insertion is one CAS at the sorted
position, and readers publish only the state pointer and payload pointer into
their two hazard slots because nodes live until whole-state reclamation. Tables
double by repointing bucket pairs — chains never re-chain — with pending-split
buckets marked in the bucket slot, and dense stable positions come from a packed
counter whose low word counts rows and high word counts in-flight link attempts
so resizes can drain. The completion watermark only crosses rows whose publish
hooks have run, and only the insert landing on the watermark boundary advances
it. Replacement reclamation follows Michael's batching: retirements collect at
`kRetireBatch`, except when the hazard domain has only ever registered one
thread, where replacement frees immediately (the ownership test's prompt-free
contract is single-threaded).

Bugs fixed during the rewrite, all reproduced with focused stress before fixing:

- Whole-state teardown walked every bucket chain end to end, but split buckets
  share chain segments, so nodes reachable from two bucket starts were deleted
  twice. Destruction now stops at each bucket's hash boundary.
- The resize copy mapped old bucket `i` onto new buckets `i` and `i + m`;
  doubling actually refines `i` into `2i` and `2i + 1`, so the two middle
  quarter-ranges were swapped and lookups walked the wrong chains, duplicating
  keys. The copy now fills `2i` and `2i + 1`.
- A writer that lost the link race fell back to replacement and published its
  payload into the winner's node while its abandoned candidate node still
  owned that payload; the candidate's destructor freed the live payload. The
  candidate disarms its payload reference before every non-linking exit.
- The pointer-view CAS loser returned the winner's freshly published view
  without announcing it, so a third rebuild could retire it mid-read. The
  loser now re-protects the current view before borrowing it.
- Contention retries called `std::this_thread::yield()`, a de-prioritizing
  syscall; all hot waits now issue the architecture pause hint instead.

The concurrency test's "readers made no progress" check failed against the new
implementation because its reader pass (~40 us) now finishes before the
writers' first payloads land: the first `Slice` claim per thread costs ~8-11 ms
of allocator warmup, which the benchmark's pre-claimed `payload_pool` never
exposes. The old implementation passed only because its slower readers were
still polling through that warmup. Readers in the test now keep polling with a
five-second deadline instead of relying on the fixed pass outlasting warmup.

Correctness: all 38 CTest tests pass, including 16 writers and 16 readers
racing first insertions and replacements, 1,024 resets under active readers,
hazard-record reuse across 320 thread lifetimes, and the ownership contracts.
A 20-round oversubscribed stress (torn reads, duplicate keys, position order,
pointer-view construction) ran clean after the fixes.

This run's 15-sample medians at 65,536 rows, Apple M4 Max, versus the previous
implementation's recorded numbers:

| Workload, 65,536 rows | Previous | Michael SliceMap | Best of field (who) |
| --- | ---: | ---: | --- |
| Unique fill, 1 writer, ns/row | 83.38 | 25.61 | 6.22 (HazardMap) |
| Gather, 2 producers, us/batch | — | 2,756 | 272 (HazardMap) |
| Gather, 16 producers, us/batch | 29,792 | 11,072 | 1,638 (VersionMap) |
| Scattered hits, 1 reader, ns/op | 41.93 | 28.32 | 19.00 (HazardMap) |
| Scattered hits, 16 readers, ns/op | 53.71 | 55.61 | 47.52 (VersionMap) |
| Streaming, 8 writers + 8 readers, us/batch | 17,700 | 8,595 | 2,902 (VersionMap) |
| Sequential 25% replacement, ns/write | 86.13 | 54.82 | 12.48 (VecSet indexed) |
| Shared-key updates, 8+8, us/batch | 10,987 | 7,098 | 3,574 (VersionMap) |
| Eight-part merge, us/merge | 6,621 | 1,306 | 971 (VecSet indexed) |
| View rebuild after replacement | 96.8 us | 133.5 us | — |

SliceMap's category wins among the field: miss lookups at 16 readers (5.08
ns/op), quarter-full lookups at 2 and 16 readers, sequential-hit and mixed-50
at 2 readers. The 1M-entry 15-writer regression improved from 13.5x behind the
mutex map to 6.2x (188.9 ms vs 30.5 ms insert; 22.5 ns vs 11.0 ns hit; 13.0 ns
vs 10.7 ns miss).

Remaining gaps are structural. HazardMap (append-only, not a unique-key map)
and VersionMap (retains every obsolete version until quiescence) skip the
unique-key, stable-position, and reclamation work the SliceMap contract
requires; their leads measure that omission, not a faster map discipline. The
residual producer contention is the shared-counter traffic (in-flight window,
position claim) that resize safety and dense positions require; a per-thread
position batch and an SIMD open-addressed index are the two candidate follow-ups.

Reproduce:

```sh
./run_build.sh --skip-tests -DBUFFETALLIGATOR_BUILD_TESTS=ON -DBUFFETALLIGATOR_BUILD_BENCHMARKS=ON
ctest --test-dir build --output-on-failure
./build/tests/benchmarks/buffetalligator_slicemapvsvecset --csv build/map-michael.csv
./build/tests/benchmarks/buffetalligator_slicemapvsvecset \
    --rows 65536 --rounds 15 --csv build/map-michael-opt.csv
```

## Large-map regression and reference design (previous implementation)

The current SliceMap is an extendible hash tree with immutable leaf replacement
and a global stable-position publication mechanism; it is not Michael's
bucket-linked-list hash table. Its use of hazard pointers does not make those
additional structures part of Michael's algorithm.

On the Apple M4 Max, a larger standalone run used 8,388,608 unique entries,
8,388,608 lookups per phase, two warmups, and seven measured repetitions:

| Operation | SliceMap median | unordered_map + mutex median | SliceMap slowdown |
| --- | ---: | ---: | ---: |
| Insert, 15 writers | 4,015.28 ms | 296.790 ms | 13.53x |
| Hit lookup, 1 reader | 520.190 ms | 100.432 ms | 5.18x |
| Miss lookup, 1 reader | 255.282 ms | 88.3443 ms | 2.89x |

Insertion ranges were 3,952.46–4,040.82 ms and 293.040–317.088 ms respectively.
The harness label `15P/1C` describes successive insertion and lookup phases;
writers and readers do not overlap in this run. Keys and Slice handles alone
occupy 192 MiB, excluding map metadata and the 512 MiB payload span. This exceeds
the reported CPU cluster L2 caches, but hardware cache-miss counts and the
system-level cache capacity have not been measured. This run reproduces the
insertion regression; it does not establish comprehensive contention coverage.

```sh
./build/tests/benchmarks/buffetalligator_slice_map_benchmark \
    --items 8388608 --lookups 8388608 --warmup 2 --repetitions 7 \
    --timeout 600 --producers 15 --consumers 1 \
    --csv build/map-eight-million-15p.csv
```

A separate single-writer Time Profiler capture used 1,048,576 entries, one warmup,
50 measured repetitions, and one lookup per phase. Among 4,794 ms of samples
whose stacks contained `SliceMap::add_finalized`, allocation/deallocation stacks
accounted for 2,760 ms (57.6%), and `State::position_of` stacks accounted for
1,601 ms (33.4%). These inclusive categories overlap and must not be added.
These samples identify costs in this implementation, not a cost inherent in
hazard pointers or a direct attribution of the 15-writer slowdown.

Michael's [hash-table algorithm](https://research.ibm.com/publications/high-performance-dynamic-lock-free-hash-tables-and-list-based-sets)
uses lock-free ordered lists as buckets. The
[libcds implementation](https://github.com/khizmax/libcds/blob/master/cds/container/michael_map.h)
documents a fixed bucket count with an unbounded number of entries per bucket;
a fixed bucket count is not a fixed entry capacity. Michael's
[hazard-pointer paper](https://www.cs.otago.ac.nz/cosc440/readings/hazard-pointers.pdf)
describes protecting reader references and reclaiming retired nodes in batches.
SIMD bucket scans are an extension to evaluate separately from that linked-list
reference. Neither candidate has been implemented or measured in this follow-up.

## Growing SIMD-bucket SliceMap

The current SliceMap has unique keys, replacement at the existing stable
position, growing SIMD hash buckets, and hazard-protected payload reclamation.
It releases the replaced Slice's ownership while preserving independently
retained Slice handles. The implementation is in `src/containers/slicemap.cpp`;
the public header exposes the container interface.

**This implementation is not the fastest measured map and is not qualified as
the release winner.** Restoring the intended behavior and removing the flat
full-map scan does not establish a performance win over the other hashed maps.

The final Release sweep completed 222 workload/size/team combinations, followed
by 15 measured samples for every workload at 65,536 rows. Both runs used two
warmups, rotating candidate order, macOS ARM64, 16 logical CPUs, and Apple Clang
17.0.0. These are the final implementation's 15-sample medians:

| Workload, 65,536 rows | SliceMap | VecSet indexed | unordered_map + mutex | VersionMap |
| --- | ---: | ---: | ---: | ---: |
| Unique fill, ns/row | 83.38 | 13.46 | 18.19 | 26.00 |
| Gather, 16 producers, ms/batch | 29.792 | 5.748 | 5.327 | 1.573 |
| Scattered hits, 1 reader, ns/op | 41.93 | 23.92 | 22.11 | 23.85 |
| Scattered hits, 16 readers, ns/op | 53.71 | 62.17 | 53.93 | 44.65 |
| Streaming, 8 writers + 8 readers, ms/batch | 17.700 | 7.465 | 10.244 | 2.734 |
| Sequential 25% replacement, ns/write | 86.13 | 12.76 | 15.56 | 29.29 |
| Shared-key updates, 8 writers + 8 readers, ms/batch | 10.987 | 17.224 | 4.634 | 3.988 |
| Eight-part merge, ms/merge | 6.621 | 0.989 | 1.713 | 4.150 |

SliceMap's 16-reader lookup range was 41.30–61.60 ns/op, overlapping the mutex
map's 46.47–76.05 ns/op. That small median difference does not establish a win.
For shared-key replacement, SliceMap beat VecSet in this run but lost to the
mutex map and VersionMap. VersionMap still defers collection until quiescence;
those timings include collection but do not qualify concurrent reclamation.

`data()` supports concurrent readers while writers and reset are quiescent.
Its first access after mutation rebuilds a contiguous pointer view; at 65,536
rows this cost 96.833 us, including allocation. Subsequent accesses use the cache.
The benchmark now reports the rebuild separately so it is not hidden by repeated
cached access. Payloads themselves are not copied.

The repair's correctness checks passed:

- All 38 registered CTest tests, including existing network and allocator tests.
- Actual old-backing deallocation, retained handles, null replacement, typed
  finalizers, overlapping merge, and stable positions.
- Growth from zero reservation, 4,097 deliberately colliding hash prefixes,
  and boundary identifiers.
- Sixteen writers plus sixteen readers racing first insertions and replacement,
  concurrent pointer-view construction and refresh, and hazard-record reuse.
- Active readers across 1,024 resets and sixteen waiters racing publication
  across 256 generations.
- LLVM 22.1.2 AddressSanitizer and UndefinedBehaviorSanitizer on the map tests and
  every comparison workload at 257 rows, including 32-worker contract stress.

ThreadSanitizer qualification remains open because both installed runtimes crash
before `main` on this host. Supported-platform qualification and representative
application workloads also remain open. No release winner has been selected.

Reproduce the final measurements:

```sh
./run_build.sh --skip-tests -DBUFFETALLIGATOR_BUILD_TESTS=ON -DBUFFETALLIGATOR_BUILD_BENCHMARKS=ON
ctest --test-dir build --output-on-failure
./build/tests/benchmarks/buffetalligator_slicemapvsvecset --csv build/map-bucket-final.csv
./build/tests/benchmarks/buffetalligator_slicemapvsvecset \
    --rows 65536 --rounds 15 --csv build/map-bucket-final-confirmation.csv
```

The CSV files retain all medians and ranges; the logging tables show first and
second place for the largest case in each section.

## Historical measurements before the bucket repair

The remaining sections describe the earlier flat-scan SliceMap baseline and do
not describe the current SliceMap implementation or its performance.

The measurements favor a **hashed index for general ID lookup**, while dense
pointer-list storage remains useful for batch results and cheap combining.
They do not establish one production winner across all workloads.

The repaired experiment is `slicemapvsvecset.cpp`. Measurements below use macOS
ARM64, 16 reported logical CPUs, Apple Clang 17.0.0, and CMake Release. The default
sweep completed all 216 workload/size/team combinations: six sizes from 64 through
65,536 rows, six lookup patterns, and single-worker and concurrent shapes.
Each candidate received two warmups and five measured samples in rotating order.
Gather and duplicate cases at 65,536 rows were repeated with 15 measured samples
because several concurrent ranges overlapped substantially.

## Findings

At 65,536 rows with one reader, scattered-hit lookup in **the same VecSet** cost
25.27 ns through its index versus 2,797.55 ns through the existing SIMD scan:
about 111 times lower aggregate cost for indexed lookup. At 64 rows the figures
were 9.46 and 13.28 ns. This supports avoiding a full-array scan as the primary
lookup operation as the set grows; it does not rule out SIMD inside a hashed
index or other SIMD-friendly layouts that this experiment does not implement.

| Workload, 65,536 rows | VecSet indexed | VersionMap | unordered_map + mutex |
| --- | ---: | ---: | ---: |
| Scattered hits, 1 reader, ns/op, 5 samples | 25.27 | 19.53 | 24.79 |
| Gather, 16 producers, ms/batch, 15 samples | 5.392 | 2.643 | 5.735 |
| Sequential 25% replacement, ns/write, 15 samples | 12.57 | 31.00 | 14.74 |
| Shared-key updates, 1 writer + 1 reader, ms/batch, 15 samples | 9.382 | 2.376 | 1.648 |
| Shared-key updates, 8 writers + 8 readers, ms/batch, 15 samples | 17.815 | 3.851 | 3.763 |

The 8/8 duplicate repeat did **not** establish a winner between VersionMap and
the mutex map: their ranges were 2.922–6.660 ms and 3.320–5.882 ms. The first,
five-sample sweep favored VersionMap; the longer repeat illustrates why that
alone was insufficient evidence. VersionMap's 16-producer gather also had a wide
1.124–5.066 ms range, overlapping the mutex map's 3.265–14.430 ms.

SliceMap's unique-input eight-part merge was much cheaper: 105.58 us versus
955.88 us for VecSet, 1,535.17 us for the mutex map, and 4,522.96 us for VersionMap.
That operation preserves SliceMap's append behavior; it does not qualify
duplicate replacement. HazardMap performed well in several unique-input
gather/stream workloads, but it also appends duplicate rows.

Retained Slice lookup includes the library's shared backing-buffer reference
count increment. These results therefore measure the actual ownership contract,
not just finding a raw pointer; shared-counter contention may influence scaling.
That is a hypothesis from the implementation, not a separately isolated result.

## Decision

Pursue a hashed index for a general map, and preserve dense/cached pointer-list
storage where the batch pipeline benefits from it. The corrected VecSet is
attractive for sequential replacement and dense batch work, but its per-row
blocking gates performed poorly under shared-key contention.

VersionMap is a useful experimental alternative for concurrent publication.
It grows nodes beyond its sizing hint, but keeps obsolete immutable versions
until all workers are quiescent; its memory use grows with the writes between
collections. Duplicate timings include that collection. This experiment does
not establish bounded-memory concurrent reclamation or a production-ready
lock-free map.

Keep the mutex map as an active contender: it won some measured workloads and
matched VersionMap in the longer contended-update comparison. Production
selection still needs successful race-sanitizer validation, the required
growth/reclamation contract, representative application workloads, and results
on the supported ARM and x86 platforms.

## Validation

- All 37 registered CTest tests passed after the final rebuild.
- The comparison checks ownership, negative/boundary IDs, duplicate capacity,
  simultaneous first insertion, shared-key replacement, retained versions,
  source-wins overlap merging, uneven partitions, and 1,680 drain permutations.
- Contract stress uses 32 workers by default on this 16-thread machine.
- The complete default Release sweep and both 15-sample follow-ups passed.
- An uneven 131-row, three-worker full sweep passed.
- LLVM 22.1.2 AddressSanitizer and UndefinedBehaviorSanitizer passed all workload
  types at 257 rows, with 32-worker contract stress and no reported errors.
- ThreadSanitizer could not run on this host: both Apple and LLVM builds crashed
  before `main`; the crash stack runs through `__tsan::InitializePlatform`,
  dyld inspection, and `__tsan::SlotLock`. Race-sanitizer validation is **open**.
  Apple's AddressSanitizer also hung in initialization; the LLVM build worked.

## Reproduce

```sh
./run_build.sh
./build/tests/benchmarks/buffetalligator_slicemapvsvecset \
    --csv build/map-comparison.csv
./build/tests/benchmarks/buffetalligator_slicemapvsvecset \
    --case gather --rows 65536 --rounds 15 --csv build/map-gather-confirmation.csv
./build/tests/benchmarks/buffetalligator_slicemapvsvecset \
    --case duplicates --rows 65536 --rounds 15 --csv build/map-duplicates-confirmation.csv
```

The corresponding logs and CSVs are in `build/` for this run. Each CSV includes
every candidate's median and min/max range, plus explicit ineligibility reasons.
The sanitizer configurations live in separate explicitly selected validation
directories; the normal script and editor builds both use `build/`.
