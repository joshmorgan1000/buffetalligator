# TLS Slice queue versus moodycamel — 2026-09-11

The replacement C prototype moves 256-Slice blocks through thread-local buffers, sharded mailboxes, and semaphore wakeups. It removes the old prototype's per-Slice atomics. At eight producers and eight consumers making individual-Slice calls, it delivered **560.69 million Slices/sec versus 400.88 million for equally buffered moodycamel: a 39.9% gain**. The seven observed ranges did not overlap for that case.

Keep this as an experiment for individual-Slice handoffs among fixed workers. Moodycamel remains faster at four producers/four consumers and when callers already supply large bulk batches. The C prototype also has higher median delivery latency than buffered moodycamel. These measurements support a workload-specific throughput gain, not a general replacement.

The [SliceMap follow-up](queue_slicemap_comparison.md) measures the existing map with atomic message sequences, including direct-slot consumption and an audit of its lookup costs.

## Throughput

Million delivered Slices/sec, median of seven repetitions with minimum–maximum in parentheses. One delivered Slice includes both enqueue and dequeue and is counted once. “API batch” is the caller's requested batch size; both buffered implementations stage up to 256 items internally regardless of that value.

| Producers / consumers | API batch | C TLS + semaphores | Moodycamel direct | Moodycamel + TLS |
|---|---:|---:|---:|---:|
| 4 / 4 | 1 | 394.98 (326.68–462.46) | 63.30 (27.77–72.48) | **472.64 (448.61–538.09)** |
| 4 / 4 | 32 | 444.84 (412.56–460.18) | 302.44 (293.29–327.13) | **564.27 (524.18–641.68)** |
| 4 / 4 | 256 | 459.26 (422.87–512.94) | **743.62 (697.10–765.12)** | 543.25 (496.30–609.08) |
| 8 / 8 | 1 | **560.69 (545.34–575.85)** | 89.06 (70.52–121.46) | 400.88 (345.80–427.18) |
| 8 / 8 | 32 | **565.21 (538.47–585.64)** | 429.98 (400.26–488.59) | 471.01 (403.13–557.50) |
| 8 / 8 | 256 | 558.33 (509.47–567.78) | **912.63 (849.55–975.97)** | 498.34 (414.17–555.22) |

Giving moodycamel the same buffering is essential: comparing only with its direct single-item API would suggest a 6.3× gain at 8/8, mostly attributable to batching. The useful comparison for individual calls is the 1.40× gain over its TLS adapter. For caller-supplied batches of 256, native moodycamel is 1.63× faster than the C prototype at 8/8.

[4/4 raw results](queue_tls_native_4p4c_2026-09-11.csv) · [8/8 raw results](queue_tls_native_8p8c_2026-09-11.csv)

## Delivery latency

A separate instrumented pass used eight producers/eight consumers and individual calls. Values are the median of seven per-run percentiles, in microseconds; samples include local staging, enqueue backpressure, and residence through dequeue. These are delivery times, not queue-call costs.

| Implementation | p50 µs | p99 µs | p99.9 µs | Instrumented million Slices/sec |
|---|---:|---:|---:|---:|
| C TLS + semaphores | 10.04 | 73.96 | 217.17 | 506.52 |
| Moodycamel direct | 107.00 | 516.42 | 1245.67 | 89.18 |
| Moodycamel + TLS | 2.58 | 61.50 | 691.33 | 311.88 |

The C prototype trades higher p50 and p99 delivery latency for throughput compared with buffered moodycamel; its observed p99.9 was lower. This does not establish a latency or starvation bound. Partial blocks can remain private indefinitely unless callers flush them, so low-rate traffic needs an explicit burst-boundary flush policy.

[Latency raw results](queue_tls_native_latency_8p8c_2026-09-11.csv)

## C design and contract

The measured prototype has since moved into the library as C11 [ba_queue.c](../src/core/ba_queue.c), with a private C interface in [ba_queue.h](../src/core/ba_queue.h) and a public `SliceQueue` wrapper in `<alligator.hpp>`. Producer and consumer state use actual `_Thread_local` storage; binding caches its address once per worker to avoid repeated TLS resolution on individual calls.

Each producer owns a preallocated pool of sixteen blocks, each holding 256 real 16-byte `ba_slice_t` descriptors. Individual pushes copy into an exclusively owned block. Full blocks publish automatically; `ba_queue_flush` publishes a partial block. The producer distributes completed blocks round-robin over consumer mailboxes. A consumer detaches an entire ready list under the mailbox mutex and drains it privately, stealing from other mailboxes when needed. After copying a block out, it returns the block to its producer's free list and signals the available-space semaphore.

Each mailbox has a pthread mutex, linked ready list, and sleeping-consumer notification semaphore. Empty consumers register their wait under the same mutex that protects publication, so publication cannot lose a wakeup between the final empty check and sleep. Producers signal only a registered sleeper. Closing publishes one atomic flag and wakes registered consumers; consumers acquire that flag at block-list boundaries and repeat the scan if closure raced their earlier scan. There are no per-Slice atomic operations in the C queue; mutexes and semaphores still synchronize block ownership. Apple builds use libdispatch's C semaphore API, and the other Unix branch uses POSIX semaphores.

The mailbox mutex publishes initialized descriptors and payload writes to consumers; the free-list mutex orders completed descriptor copying before block reuse. Storage remains allocated until workers stop, so this bounded experiment does not require hazard-pointer reclamation.

The prototype's limits are deliberate and affect applicability:

- Worker counts are fixed, producer lanes and consumer home mailboxes are uniquely assigned, and all configured consumers must participate.
- Each thread can bind one producer role and one consumer role; rebinding while holding a block is unsupported.
- Push may block for capacity, and pop may sleep for work; this is not a lock-free queue.
- Callers must flush partial producer blocks and close only after every producer has finished and flushed.
- Ordering holds within each published block; blocks can reorder even from the same producer, with no global FIFO guarantee.
- Reset requires a fully drained queue and quiescent workers; destruction requires all workers to have stopped.
- Dynamic registration, growth, cancellation, timed flushes, and concurrent destruction are outside this prototype.
- Descriptor ownership transfers logically; queue operations do not retain or release Slice references.

## Baselines and measurement method

[bench_queue.cpp](../tests/bench_queue.cpp) compares three adapters in the same harness:

- `c_tls_semaphore`: the C implementation described above.
- `moodycamel_direct`: `ConcurrentQueue<ba_slice_t>` with explicit producer/consumer tokens, native single-item APIs for batch 1, and native bulk APIs for larger batches.
- `moodycamel_tls`: `BlockingConcurrentQueue<ba_slice_t>` with actual C++ `thread_local` producer/consumer buffers of 256 descriptors, native bulk transfers, and semaphore-based consumer waits; partial buffers require flush just as in the C queue.

Moodycamel uses `MAX_SUBQUEUE_SIZE=4096`, `EXPLICIT_INITIAL_INDEX_SIZE=256`, and its default block size of 32. Each producer's capacity is prefilled and drained before starting workers, so nonallocating enqueue does not depend on a producer obtaining its first blocks while competing with other producers. Every measured round asserts zero moodycamel allocations. Capacity limits are comparable, but total storage and treatment of privately staged or consumed descriptors differ.

The buffered moodycamel consumer waits with a 50 µs timeout to observe harness completion. Its producer retries full bulk enqueues with yielding; the C producer sleeps on its free-block semaphore. Direct moodycamel polls, yielding after 64 unsuccessful calls. This comparison includes those different backpressure and waiting policies rather than claiming to isolate the cost of atomics alone.

The dependency is pinned to [upstream revision 683b9e31ea15eb69f1b81cc1defc7850d5f20b71](https://github.com/cameron314/concurrentqueue/tree/683b9e31ea15eb69f1b81cc1defc7850d5f20b71). The `concurrentqueue.h` SHA-256 is `399efd015a5971bc547d211fb2986825b84c68a1f465650bcfa970dda55c2664`; `blockingconcurrentqueue.h`, `lightweightsemaphore.h`, and the license come from that same revision. Moodycamel's own producer subqueues already relax global FIFO; see its [upstream design documentation](https://github.com/cameron314/concurrentqueue#high-level-design).

- Apple M4 Max, 16 CPU cores, 128 GiB RAM, macOS 26.6.2 (25G83), Apple Clang 17.0.0.
- Release C11/C++20, `-O3 -DNDEBUG`, without LTO or CPU pinning.
- Moodycamel is allowed normal compiler inlining; the C queue uses ordinary external function calls.
- Each repetition uses one warm-up drain followed by sixteen timed drains, each with 1,048,576 items per producer and persistent workers.
- Seven repetitions rotate the order of the three engines for each worker shape and API batch.
- Payload allocation, Slice view creation, worker creation/join, retain/release, and validation are outside timing.
- Timed work includes start/finish barriers, queue operations, descriptor copying, waiting/polling, payload ID reads, and private count/sum/XOR accumulation.
- There is no shared per-item measurement counter; consumers publish a padded completion count only when idle after every producer finishes.
- Latency sampling timestamps one item in 1,024 before its first enqueue attempt and after dequeue; it is absent from throughput runs.

The machine was an active desktop, not isolated. Samples taken every two seconds during the final throughput passes show other processes using a median of 251.5% CPU and a maximum of 328.9% (roughly 2.5 and 3.3 cores). Scheduler placement, competing work, and thermal state remain uncontrolled. The latency pass did not capture a separate background-load log. These results should not be generalized to other machines or sparse/bursty workloads.

[Background CPU samples](queue_tls_native_load_2026-09-11.csv)

## Validation and provenance

The final Release build passed all 14 CTest tests with the optional queue benchmark enabled.

The final queue stress suite passed ten consecutive Release executions and a separate UndefinedBehaviorSanitizer execution. It checks partial flushes, empty close/wakeup races, repeated block reuse and complete drains, producer-written payload visibility, descriptor metadata, and exactly-once delivery using an atomic bitmap. Shapes include 1/1, 1/8, 8/1, 8/8, and 24/24 workers; each uses 8,195 items per producer with API batches 1, 32, and 256. The C queue is restricted to one block per producer in stress runs to force frequent reuse and backpressure.

All 147 final throughput/latency measurements validated counts and sum/XOR checksums after every drain, covering 15,502,147,584 timed deliveries plus warmups, with zero timed moodycamel allocations. The expensive atomic bitmap is used only in correctness runs, not in those performance measurements. UBSan and stress checks do not establish race freedom; ThreadSanitizer and AddressSanitizer were not rerun because of the host runtime failures recorded in the [registry experiment](registry_growth.md).

The [earlier per-cell-atomic experiment](queue_comparison.md) is historical and its implementation was discarded. Files named `queue_tls_pilot*`, `queue_tls_sharded_pilot*`, and `queue_tls_call_boundary*` preserve development measurements and are excluded from the tables here: they respectively used a global ready list, short sharded pilots, or artificial non-inlined moodycamel call boundaries. The first pilot is incomplete. Only the four `queue_tls_native_*` files linked above support this report.

## Reproduce

Fetch all headers and the license from the same pinned revision, then use the repository build script:

```sh
mkdir -p deps/src/concurrentqueue-benchmark
queue_revision=683b9e31ea15eb69f1b81cc1defc7850d5f20b71
for queue_file in concurrentqueue.h blockingconcurrentqueue.h lightweightsemaphore.h LICENSE.md; do
    curl --fail --location "https://raw.githubusercontent.com/cameron314/concurrentqueue/$queue_revision/$queue_file" --output "deps/src/concurrentqueue-benchmark/$queue_file"
done
./run_build.sh -DBUFFETALLIGATOR_QUEUE_BENCHMARK=ON -DMOODYCAMEL_BENCHMARK_INCLUDE_DIR="$PWD/deps/src/concurrentqueue-benchmark"
build/current/tests/buffetalligator_queue_bench --verify
build/current/tests/buffetalligator_queue_bench --producers 4 --consumers 4 --items 1048576 --rounds 16 --repeats 7 > queue_tls_4p4c.csv
build/current/tests/buffetalligator_queue_bench --producers 8 --consumers 8 --items 1048576 --rounds 16 --repeats 7 > queue_tls_8p8c.csv
build/current/tests/buffetalligator_queue_bench --latency --producers 8 --consumers 8 --batch 1 --items 1048576 --rounds 16 --repeats 7 > queue_tls_latency.csv
```

The benchmark and moodycamel dependency remain optional; the C queue is now part of the library. Omitting worker selection exercises 1/1, 2/2, 4/4, 8/8, 1/8, 8/1, and 16/16; omitting batch selection exercises 1, 32, and 256. Shorter item/round/repetition counts are useful for smoke checks, but do not reproduce the measurements above.
