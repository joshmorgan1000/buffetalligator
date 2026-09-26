# PrioritySlice showdown

Measured on macOS arm64 with Apple clang 17 in a Release build on 2026-09-25. The baseline was Claude's existing PrioritySlice and PrioritySliceT entry before the free-slot hint. Each number is the median of three process runs; each run reports the median of seven timed samples after one warmup. Each sample performs 100,000 deterministic operations. Lower is better.

```sh
./run_build.sh --skip-tests --no-color
./build/tests/benchmarks/buffetalligator_priority_slice_benchmark
```

Raw is `PrioritySlice`; typed is `PrioritySliceT<uint32_t, uint32_t>`. Accepted pushes feed descending keys to a full queue. Rejected pushes feed increasing keys to a full queue. Pop-and-refill reinserts the same key. Pop-and-rotate inserts the next increasing key so the minimum moves across slots. The pop measurements include one push per pop and are reported in nanoseconds per pair.

| Capacity | Workload | Raw baseline → current (ns) | Typed baseline → current (ns) |
| --- | --- | ---: | ---: |
| 32 | Accepted push | 18.3 → 18.4 | 17.7 → 18.5 |
| 32 | Rejected push | 1.25 → 1.29 | 1.25 → 1.29 |
| 32 | Pop and refill | 15.4 → 13.8 | 15.2 → 13.9 |
| 32 | Pop and rotate | 20.3 → 14.9 | 19.7 → 14.7 |
| 256 | Accepted push | 63.6 → 66.9 | 63.7 → 66.9 |
| 256 | Rejected push | 1.22 → 1.27 | 1.25 → 1.29 |
| 256 | Pop and refill | 43.6 → 45.7 | 43.4 → 45.8 |
| 256 | Pop and rotate | 59.3 → 39.5 | 57.9 → 39.6 |

The last freed slot now lives in the existing 64-byte header. A push tries that slot with one CAS before scanning for another free slot. On this machine, the rotating pop/push pair improved about 33% for raw and 32% for typed at capacity 256. Full-queue pushes and rejections did not show a reliable gain; the small differences above are within the observed clock and run variation. This benchmark is single-threaded; concurrent correctness is covered by `buffetalligator_priority_slice_test`.

## Experiments

| Change | Observation | Decision |
| --- | --- | --- |
| Free-slot hint and direct CAS | Pop-and-rotate at capacity 256 fell from about 59 to 39 ns raw and 58 to 40 ns typed. | Kept |
| SIMD minimum value followed by a second index search | Same-slot refill improved, but rotating capacity 256 rose from about 39 to 51–54 ns. | Reverted |
| Alternate SIMD maximum update | No reliable improvement across the measured workloads. | Reverted |
| Separate full-queue push loop | Added code without a reliable overall improvement. | Reverted |

Run the command above to reproduce the comparison on the target hardware.
