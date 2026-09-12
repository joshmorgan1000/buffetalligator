# SliceMap performance fix — 2026-09-11

SliceMap now has a preallocated ID index and arena-allocated rows. The eight-worker, 65,536-row measurements show **35% higher append throughput, 26× faster Slice copy-out by ID, and 16.6× higher throughput in the original ID-based handoff benchmark**. Successful `find()` improves by roughly 1,344× because it no longer scans the array.

There is a measurable tradeoff: **single-worker append falls from 29.08 to 20.70 million rows/sec (29%)** at 65,536 rows, while maintaining the index. At 8,192 rows the reduction is 9%. Concurrent append improves in every measured worker/capacity combination. This is a lookup and concurrent-throughput improvement, not a claim that every operation became faster.

## Separate operations

Five alternating before/after pairs, medians, Apple M4 Max, Apple Clang 17, Release `-O3 -DNDEBUG`. The baseline executables were saved from the working tree before this change; no implementation was recovered from Git history.

### 65,536 rows

| Operation | Workers | Before Mops/s | After Mops/s | Ratio |
|---|---:|---:|---:|---:|
| Append | 1 | 29.083 | 20.700 | 0.71× |
| Append | 2 | 10.241 | 12.313 | 1.20× |
| Append | 8 | 6.354 | 8.557 | 1.35× |
| Find existing ID | 1 | 0.049 | 55.476 | 1128.69× |
| Find existing ID | 2 | 0.100 | 98.761 | 984.58× |
| Find existing ID | 8 | 0.230 | 308.647 | 1343.72× |
| Copy Slice by ID | 1 | 0.052 | 27.062 | 517.02× |
| Copy Slice by ID | 2 | 0.103 | 20.613 | 200.55× |
| Copy Slice by ID | 8 | 0.368 | 9.530 | 25.87× |

### 8,192 rows

| Operation | Workers | Before Mops/s | After Mops/s | Ratio |
|---|---:|---:|---:|---:|
| Append | 1 | 24.918 | 22.575 | 0.91× |
| Append | 2 | 9.600 | 10.411 | 1.08× |
| Append | 8 | 6.519 | 7.496 | 1.15× |
| Find existing ID | 1 | 0.270 | 117.063 | 433.07× |
| Find existing ID | 2 | 0.408 | 214.227 | 524.69× |
| Find existing ID | 8 | 1.030 | 693.657 | 673.58× |
| Copy Slice by ID | 1 | 0.309 | 46.974 | 152.10× |
| Copy Slice by ID | 2 | 0.482 | 35.540 | 73.68× |
| Copy Slice by ID | 8 | 1.597 | 18.166 | 11.37× |

The benchmark reuses workers for one untimed warmup and eight measured rounds. Append moves pre-staged Slices with nonsequential IDs; payload allocation, staging, reset, validation, and thread startup are outside timing. Row allocation and index maintenance remain inside append timing. Each lookup worker executes 2,048 deterministic probes per round. `get_slice()` includes retaining and releasing an owning Slice; the payloads can share arena reference counters, which becomes visible once scanning is eliminated.

The CSV also includes repeated lookups of one absent key. Those are a cache-hot microbenchmark, not a general random-miss workload, so they are omitted from the headline tables. Short lookup cases and OS scheduling produce outliers; all samples are retained rather than selectively filtered. At 65,536 rows, final eight-worker `find()` samples span 283–340 Mops/s, copy-out 7.80–9.86 Mops/s, and append 7.48–9.21 Mops/s.

## Original queue-shaped workload

Eight producers and eight consumers, 65,536 messages per window, individual calls, sixteen measured windows, five alternating pairs, same benchmark source in both binaries:

| Engine | Before M Slices/s | After M Slices/s | Ratio |
|---|---:|---:|---:|
| SliceMap, ID lookup | 0.392 | 6.523 | 16.64× |
| SliceMap, direct slot | 4.333 | 6.127 | 1.41× |
| C TLS queue | 191.547 | 210.256 | 1.10× |
| moodycamel, direct | 59.828 | 64.118 | 1.07× |
| moodycamel, TLS buffered | 118.824 | 116.789 | 0.98× |

ID-based and direct-slot SliceMap now have similar handoff throughput. The C TLS queue remains much faster for this workload: SliceMap still owns rows, provides keyed access, retains returned Slices, and uses per-message producer/consumer counters. It remains a fixed-capacity result channel, not a streaming queue. The unchanged queue controls vary with scheduling and system load; their differences are not attributed to SliceMap.

## Implementation and synchronization

- An open-addressed index stores the earliest slot for each ID, with at least two buckets per capacity slot. Unused high bits in each bucket carry a hash fingerprint, avoiding ID-array reads for unrelated collisions without another allocation or atomic field. Full IDs are checked before a hit is accepted. Duplicate rows retain their slot order and resolve to the lowest published slot.
- Each row uses the existing TLS Slice allocator instead of general-purpose `new`. A 16-byte owning Slice immediately precedes the row and keeps its backing alive through hazard retirement; deallocation moves that owner off the row before releasing it. The public `Row` layout and placement construction remain supported.
- Claim, completion, and waiter state occupy separate cache lines. Each thread's two hazard pointers occupy a separate 128-byte region. There are still two global counter RMWs per append; the index adds bucket CAS publication, rather than a new global lock or counter.
- A row pointer and its ID are release-published before the index, callback, and release increment of the completion count. Index acquisition and candidate validation establish visibility before payload access. Screening an ID uses a relaxed atomic load; it does not authorize dereferencing a row.
- Hazard publication, row-pointer revalidation, concurrent removal, and hazard scanning retain sequential consistency. Validation also checks the protected row's own immutable ID to reject an address reused for another key. Payload copy-out finishes before its hazard clears. Quiescent merge and destruction no longer pay sequentially consistent exchanges.
- Waiter notifications claim an armed threshold once and coalesce posted or in-flight semaphore credits. Completion-count acquisition still establishes visibility; the notification flag only controls wakeup accounting.

Reset clears the index atomically because readers may overlap reset. It still requires producers to be quiescent. Move, merge, and destruction retain their existing quiescence requirements. Partial merge now reserves the number of live rows rather than the source capacity, preserving contiguous destination slots. Reset and merge also handle an empty moved-from source.

## Costs and compatibility

The public map operations are unchanged. `SliceMap`'s object layout/alignment changed, so applications must rebuild against the matching header and library. `Slice` stays 16 bytes with unchanged ownership and backing metadata.

On a 64-bit target the index adds 16–32 bytes per capacity slot, depending on power-of-two rounding; it is 1 MiB for 65,536 rows. Row storage additionally carries the owning Slice header and follows arena allocation alignment. Index maintenance adds append work and index clearing adds reset work. Expected lookup is constant time with well-distributed hashes; deliberately colliding keys can still require linear probing. Neither benchmark times reset, and no claim is made about improved reset throughput.

## Validation and reproduction

The repository build script was used, followed by incremental rebuilds during optimization. The final Release suite passes all 17 tests. The dedicated map tests, existing hazard tests, and SliceMap queue tests each pass twenty consecutive executions, plus UndefinedBehaviorSanitizer. Tests cover 48 workers, publication visibility, callbacks/waits, duplicate IDs, wrapped probe clusters, matching fingerprints with different full IDs, negative and 32-bit IDs, move/merge/reset, and readers retaining Slices across 2,000 reset/republication generations. A separate installed `find_package(alligator)` consumer also passes.

ThreadSanitizer could not validate this change: the independent thread-and-join smoke program exits 139 on this host before any SliceMap code runs. Windows and Linux were not executed in this session.

```sh
./run_build.sh -DBUFFETALLIGATOR_QUEUE_BENCHMARK=ON \
  -DMOODYCAMEL_BENCHMARK_INCLUDE_DIR="$PWD/deps/src/concurrentqueue-benchmark"
build/current/tests/buffetalligator_slicemap_bench 8 65536 8
build/current/tests/buffetalligator_queue_bench --slicemap \
  --producers 8 --consumers 8 --batch 1 --items 8192 --rounds 16 --repeats 5
```

No builds or sanitizer runs overlapped the recorded benchmarks. One-minute host load ranged from 3.20 to 5.99 during the final run; workers were not pinned. See [operation samples](slicemap_paired_2026-09-11.csv), [handoff samples](slicemap_queue_paired_2026-09-11.csv), [commands and source/binary hashes](slicemap_paired_run_2026-09-11.json), and [validation output](slicemap_validation_2026-09-11.txt). The benchmark is in [bench_slicemap.cpp](../tests/bench_slicemap.cpp), and the regression coverage is in [slicemap_test.cpp](../tests/slicemap_test.cpp).
