# C Slice queue versus moodycamel — 2026-09-11

**Historical experiment, superseded by the [TLS/semaphore comparison](queue_tls_comparison.md).** The per-cell-atomic prototype measured here was discarded; `src/core/ba_queue.c` now contains the replacement, so these reproduction commands no longer reproduce this implementation.

This experiment compared the earlier benchmark-only C implementation with upstream moodycamel `ConcurrentQueue<ba_slice_t>`, using the same real 16-byte Slice views and harness.

## Results and interpretation

**These are loaded-system observations, not an isolated performance ranking.** The background CPU log shows `test_seeded_search` using a median of 13.84 CPU cores during the throughput pass, alongside desktop and VM activity. The initially quiet-looking interval did not persist. Five alternating pairs cannot remove that interference; several ranges span factors of four or more. A quiet-machine rerun is required before selecting an implementation on performance grounds.

The C prototype completed every correctness and delivery check, and its observed throughput was in the same broad range as moodycamel in the balanced workloads. The 8-producer/8-consumer batched medians were 43.19 versus 42.16 million delivered Slices/sec, respectively. This near-tie is not evidence that the C implementation is faster. Moodycamel also already relaxes global FIFO through producer subqueues, so relaxing order alone does not provide a new advantage over this baseline.

### Throughput

Millions of delivered Slices per second; median of five runs, with minimum–maximum in parentheses. Higher is better. Each item includes both enqueue and dequeue.

| Producers / consumers | Batch | C prototype | Moodycamel |
|---|---:|---:|---:|
| 1 / 1 | 1 | 5.86 (3.67–13.61) | 5.00 (2.26–12.58) |
| 1 / 1 | 32 | 11.16 (8.51–39.24) | 5.32 (4.06–22.06) |
| 2 / 2 | 1 | 8.16 (4.57–18.97) | 6.95 (4.89–11.90) |
| 2 / 2 | 32 | 24.41 (9.05–44.51) | 13.29 (7.17–27.46) |
| 4 / 4 | 1 | 11.55 (6.45–22.66) | 13.88 (12.02–29.04) |
| 4 / 4 | 32 | 44.52 (37.89–177.93) | 54.10 (18.45–105.75) |
| 8 / 8 | 1 | 25.35 (20.48–42.80) | 27.70 (19.80–36.57) |
| 8 / 8 | 32 | 43.19 (27.66–54.35) | 42.16 (25.00–59.03) |
| 1 / 8 | 1 | 21.22 (14.46–52.63) | 20.13 (4.72–22.23) |
| 1 / 8 | 32 | 26.48 (11.68–40.11) | 73.38 (29.16–92.83) |
| 8 / 1 | 1 | 12.42 (11.37–38.90) | 7.85 (5.63–10.38) |
| 8 / 1 | 32 | 16.47 (8.35–21.86) | 10.41 (9.30–16.07) |
| 16 / 16 | 1 | 12.75 (10.07–14.09) | 9.33 (7.43–12.32) |
| 16 / 16 | 32 | 13.01 (9.48–47.96) | 11.89 (10.17–13.26) |

The asymmetry matters: the observed 1-producer/8-consumer bulk median favored moodycamel, while the 8-producer/1-consumer median favored the C prototype. These are workload-specific observations under changing load, not established causal advantages.

### Sampled delivery latency

Microseconds, shown as the median of five per-run p50 / p99 values. Lower is better. This separate instrumented pass includes enqueue backpressure and queue residence; these values are not individual queue-call costs. The background test occupied a median of 14.89 cores during this pass.

| Producers / consumers | Batch | C p50 / p99 | Moodycamel p50 / p99 |
|---|---:|---:|---:|
| 1 / 1 | 1 | 15.17 / 412.54 | 18.75 / 179.21 |
| 1 / 1 | 32 | 10.12 / 1037.08 | 9.83 / 81.38 |
| 2 / 2 | 1 | 27.96 / 1101.79 | 34.08 / 447.46 |
| 2 / 2 | 32 | 17.62 / 1342.25 | 18.04 / 2131.29 |
| 4 / 4 | 1 | 42.00 / 2768.25 | 50.33 / 8464.88 |
| 4 / 4 | 32 | 24.88 / 788.29 | 21.79 / 1597.17 |
| 8 / 8 | 1 | 50.88 / 3879.75 | 75.12 / 5607.88 |
| 8 / 8 | 32 | 40.38 / 10053.42 | 26.83 / 3272.04 |

Both queues experienced millisecond tail latency under CPU contention. High aggregate throughput does not establish low delivery latency or fairness, and neither these samples nor the consumer item counts establish a starvation bound.

### Uneven producer scheduling

All producers except producer zero yield every 256 items; there is still the same number of items per producer. Values are median million Slices/sec with minimum–maximum. The 1/1 control has no additional producer yielding. Background test load had a median of 14.62 cores.

| Producers / consumers | Batch | C prototype | Moodycamel |
|---|---:|---:|---:|
| 1 / 1 | 1 | 35.18 (17.99–62.33) | 18.59 (8.78–49.74) |
| 1 / 1 | 32 | 40.89 (19.18–202.98) | 56.16 (11.98–121.63) |
| 2 / 2 | 1 | 9.85 (5.70–16.27) | 10.79 (5.43–12.85) |
| 2 / 2 | 32 | 8.16 (5.92–23.40) | 11.94 (7.13–32.52) |
| 4 / 4 | 1 | 6.42 (3.28–9.71) | 8.82 (0.60–15.48) |
| 4 / 4 | 32 | 11.83 (2.06–16.29) | 12.76 (3.12–16.46) |
| 8 / 8 | 1 | 13.41 (9.72–14.10) | 12.58 (8.64–17.82) |
| 8 / 8 | 32 | 11.47 (9.17–20.43) | 8.17 (6.18–17.23) |

All 300 complete measurements validated delivery counts and checksums after every round, covering 4,299,161,600 timed handoffs plus warm-ups; all reported zero timed moodycamel allocations.

[Throughput CSV](queue_throughput_2026-09-11.csv) · [Latency CSV](queue_latency_2026-09-11.csv) · [Uneven-producer CSV](queue_skew_2026-09-11.csv) · [Background CPU log](queue_background_load_2026-09-11.csv). The [interrupted first pass](queue_throughput_loaded_partial_2026-09-11.csv) is preserved for transparency and excluded from these tables.

## Implementations

The C prototype assigns one fixed ring to each producer. Consumers acquire the published tail and use a relaxed compare-exchange on the lane's head to reserve an exclusive batch. A release store of each cell's reusable ticket orders the completed descriptor copy before the producer can overwrite it. The producer acquires those tickets and release-publishes initialized descriptors. Lane cursors and consumer tokens occupy separate 128-byte cache lines. Consumers retain lane affinity for up to 256 items before rotating, and skip empty or contended lanes.

Each lane holds 4,096 entries. Storage remains allocated until all workers join, so there are no hazard-pointer scans or node reclamation in this experiment. A preempted consumer can delay reuse when its lane wraps; this prototype does not claim lock-free progress. It is neither a production queue nor an implementation of dynamic block growth, thread registration, blocking waits, or concurrent destruction.

Moodycamel uses explicit producer and consumer tokens, `try_enqueue` / `try_dequeue` for single items, and its native bulk APIs for batches of 32. Traits set `MAX_SUBQUEUE_SIZE=4096` and `EXPLICIT_INITIAL_INDEX_SIZE=256`; block size remains the upstream default of 32. The initial block pool contains `(4096 + 64) * producer_count` entries. This is a comparable per-producer occupancy limit, not byte-identical storage or identical handling of in-flight dequeues. An allocation counter verifies zero moodycamel allocations in every measured round.

Both implementations retain per-producer reservation order and offer no global FIFO ordering between producers. Consumers may finish overlapping dequeues out of order. Neither measurement establishes a starvation bound.

Upstream revision: [`683b9e31ea15eb69f1b81cc1defc7850d5f20b71`](https://github.com/cameron314/concurrentqueue/tree/683b9e31ea15eb69f1b81cc1defc7850d5f20b71), fetched on 2026-09-11. Header SHA-256: `399efd015a5971bc547d211fb2986825b84c68a1f465650bcfa970dda55c2664`. Its license is retained beside the downloaded header in the ignored dependency directory. See the [upstream design and API documentation](https://github.com/cameron314/concurrentqueue#high-level-design).

## Measurement method

- Apple M4 Max, 16 CPU cores, 128 GiB RAM, 128-byte cache lines; macOS 26.6.2 (25G83), Apple Clang 17.0.0.
- Release C11 and C++20 builds with `-O3 -DNDEBUG`, without LTO or CPU pinning.
- The queue prototype is compiled as C; C++ supplies the benchmark harness and moodycamel adapter.
- Both implementations cross a non-inlined queue call boundary, so moodycamel is not inlined into the driver loop while the C implementation remains external.
- Five paired repetitions alternate which implementation runs first.
- Each throughput repetition creates workers before timing, runs one complete warm-up drain, then reuses those workers for four measured drains of 1,048,576 items per producer.
- The driver creates Slice views and initializes payloads before starting workers; all references remain live until complete drains return ownership to the driver, which releases them after the comparisons.
- Timed work includes enqueue/dequeue, polling, descriptor copying, payload ID reads, private count/sum/XOR accumulation, and start/finish barriers; it excludes payload allocation, retain/release, thread creation/join, and post-round validation.
- Throughput counts each delivered item once, including both its enqueue and dequeue; it is not the sum of enqueue and dequeue operation rates.
- Consumers have private measurement storage; there is no global per-item statistics counter.
- Empty/full polling yields after 64 unsuccessful attempts for both queues, with a watchdog for stalled tests.
- The separate latency mode timestamps one item in 1,024 before its first enqueue attempt and after dequeue, including enqueue backpressure and queue residence; instrumentation is absent from the throughput pass.
- Latency runs use 262,144 items per producer; skew runs use 524,288, with four measured drains and five paired repetitions in both modes.
- The skew mode additionally yields every 256 items on all producers except producer zero.

## Validation

The regular build passed all 14 tests with the optional benchmark enabled. The queue test checks empty/full boundaries, failed-push recovery, wrapped ordering with one consumer, exact-once delivery with an atomic bitmap, descriptor metadata, and producer-write visibility. It repeatedly reuses 64-entry C rings with batches of 1 and 32, including asymmetric workloads and 24 producers plus 24 consumers. Timed runs separately validate delivered counts and sum/XOR checksums after every drain.

The final stress test also passed under UndefinedBehaviorSanitizer. ThreadSanitizer and AddressSanitizer were not rerun because of the same host's runtime failures documented in [the earlier registry experiment](registry_growth.md). The publication/reuse protocol was reviewed directly, but stress tests and UBSan do not replace a successful race-sanitizer run or a formal concurrency proof.

## Reproduce

Download the pinned header and license into the ignored dependency directory:

```sh
mkdir -p deps/src/concurrentqueue-benchmark
curl --fail --location https://raw.githubusercontent.com/cameron314/concurrentqueue/683b9e31ea15eb69f1b81cc1defc7850d5f20b71/concurrentqueue.h --output deps/src/concurrentqueue-benchmark/concurrentqueue.h
curl --fail --location https://raw.githubusercontent.com/cameron314/concurrentqueue/683b9e31ea15eb69f1b81cc1defc7850d5f20b71/LICENSE.md --output deps/src/concurrentqueue-benchmark/LICENSE.md
./run_build.sh -DBUFFETALLIGATOR_QUEUE_BENCHMARK=ON -DMOODYCAMEL_BENCHMARK_INCLUDE_DIR="$PWD/deps/src/concurrentqueue-benchmark"
build/current/tests/buffetalligator_queue_bench --verify
build/current/tests/buffetalligator_queue_bench > queue_throughput.csv
build/current/tests/buffetalligator_queue_bench --latency --items 262144 > queue_latency.csv
build/current/tests/buffetalligator_queue_bench --skew --items 524288 > queue_skew.csv
```

The benchmark dependency is opt-in and is not linked into or installed with the allocator library. `--items`, `--rounds`, and `--repeats` allow shorter or longer runs.
