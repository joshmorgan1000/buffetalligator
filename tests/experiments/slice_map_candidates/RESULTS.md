# SliceMap experiment results — September 30, 2026

None of these candidates establishes a consistent single-thread SliceMap improvement.
Combining the publication-counter updates is promising for concurrent insertion,
but no candidate has been promoted to production.

## Candidate decisions

Numbers are wall-clock nanoseconds per operation, using the median of three independent
process medians; each process contains 15 measured repetitions after two warmups.
Throughput gains compare with unmodified SliceMap, not with `std::unordered_map`.

| Candidate | Measured result | Decision |
| --- | --- | --- |
| Combine publication-counter updates | Eight-writer insertion: 197.67 → 164.82 ns/op, 1.199x throughput; single-thread insertion has no established gain at any tested size | Keep as a concurrent-insertion candidate; no production promotion |
| Defer payload hazard creation until a hit | No established single-thread miss benefit; eight-reader misses: 27.94 → 32.93 ns/op, 17.9% more time | Do not promote |
| Both map changes together | Eight-writer insertion: 197.67 → 159.89 ns/op, 1.236x throughput; 65,536-row single-thread insertion: 27.83 → 29.90 ns/op, 7.4% more time | Do not promote as a general improvement |
| Resolve Slice copy metadata once | SliceMap hit medians improve slightly at 4,096/65,536 rows but worsen at 1,048,576; std retained-hit medians improve about 4–6% on one thread | Shared Slice-copy hypothesis remains interesting; no consistent SliceMap gain |

For eight-writer insertion, baseline process medians span 188.69–203.02 ns/op,
the combined-counter candidate 164.06–168.87, and both map changes 153.84–161.58.
Every candidate process median beats every baseline process median in this case.
Individual threaded samples have wide ranges, so this is a repeatable local signal,
not a statistical significance claim or supported-platform qualification.

A subsequent FlatSliceMap experiment review found that this harness's reset policy can
retain previous sample tables until a reclamation batch runs, increasing SliceMap's
resident memory across samples. Its std adapter also uses `.slice()` rather than the
ordinary Slice copy used by SliceMap. These are additional comparison limits; the
[FlatSliceMap harness](../flat_slice_map/README.md) explicitly collects retired tables
between samples and matches retained-copy operations across containers. Its numbers
must be compared within that new experiment rather than directly with this sweep.

The apparent million-row improvements from both map changes are unresolved: baseline
insertion varies from 31.95 to 53.64 ns/op across processes, while the candidate spans
32.55–32.90. The candidate is slightly slower in the first series and faster in the
later two. Hits and misses also show substantial baseline drift. Those aggregate
speedups are insufficient evidence of a general single-thread improvement.

The fused-copy candidate changes Slice itself, which both maps use. Its std hit-time
improvements therefore cannot be credited to SliceMap's indexing. A direct Slice-copy
experiment would help isolate this effect; this matrix measures copies through map hits.

## Current production baseline against std

Lower is better. The threaded std baseline uses one mutex; single-thread std uses no lock.

| Rows | Active workers | Operation | SliceMap ns/op | std ns/op |
| ---: | --- | --- | ---: | ---: |
| 4,096 | 1 | Insert | 28.91 | 11.59 |
| 4,096 | 1 | Hit | 12.70 | 11.78 |
| 4,096 | 1 | Miss | 2.84 | 1.60 |
| 65,536 | 1 | Insert | 27.83 | 11.08 |
| 65,536 | 1 | Hit | 13.79 | 15.01 |
| 65,536 | 1 | Miss | 3.46 | 1.68 |
| 1,048,576 | 1 | Insert | 37.21 | 10.92 |
| 1,048,576 | 1 | Hit | 23.44 | 13.76 |
| 1,048,576 | 1 | Miss | 7.32 | 2.04 |
| 65,536 | 8 writers | Insert | 197.67 | 99.31 |
| 65,536 | 8 readers | Hit | 153.81 | 52.07 |
| 65,536 | 8 readers | Miss | 27.94 | 19.46 |

This sweep does not support a blanket SliceMap win over std, including concurrent
insertion. Even the faster insertion candidates remain behind their own std+mutex
controls: 164.82 versus 105.35 ns/op for the combined counter, and 159.89 versus
115.90 for both changes. Earlier results with different sizes or noisy runs should
not be generalized to this workload.

## Measurement and correctness evidence

- Apple M4 Max, arm64, macOS 26.6.2, AC power; completed 21:02–21:05 EDT.
- Release system-Clang build with `-O3 -DNDEBUG -std=c++20 -flto=thin -arch arm64 -fPIC`
  for production and candidate source objects, built through `run_build.sh`.
- 42/42 registered checks passed, none skipped, including every candidate's map,
  concurrent growth, Slice ownership/lifetime, Vulkan, and native/translated Metal checks.
- 60 sequential benchmark processes: five candidates × four configurations × three series.
  Candidate/configuration launch order rotates; container order alternates within each process.
- Each lookup phase uses at least 262,144 queries, increasing to 1,048,576 at the largest size.
  Every measured phase validates results outside its timer.
- Generated-source and production-source hashes match the recorded inputs; production files
  remained unchanged throughout the sweep; the build/test/benchmark activity monitor reported
  no competing processes.

This is the existing deterministic integer-ID workload: consecutive insertions, cycling
successful lookups, and negative missing IDs, with both containers sized in advance.
Payload setup and result destruction are outside timing; insertion includes container
allocation, and hits include creating and retaining owned Slice handles. Writers finish
before readers start. The results do not cover mixed reads/writes, replacement-heavy
workloads, different key distributions, Linux/x86, or isolated CPU scheduling/thermals.
Observed ranges are not confidence intervals.

## Artifacts and remaining hypotheses

- [Full numerical summary](results-2026-09-30.csv), including per-process ranges and std controls.
- [Measurement manifest](run-2026-09-30.json), including commands, source hashes, and executable hashes.
- [Full local analysis](../../../build/map-candidates/results-final/RESULTS.md), with all tables.
- Raw CSVs, process logs, compile commands, correctness logs, and generated-source manifests
  are under `build/map-candidates/results-final/`; `build/map-candidates/results/` is an
  aborted pilot and is excluded from every number above.

Pool allocation, batched Slice-ID allocation, publication-directory changes, and compact
SIMD indexing remain unmeasured hypotheses. Any implementation of these belongs in a
separate experiment before a production change. See [README.md](README.md) for reproduction.
