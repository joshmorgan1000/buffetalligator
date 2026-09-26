# Map comparison and container benchmarks

The map experiment is [slicemapvsvecset.cpp](slicemapvsvecset.cpp). Build and run it
from the repository root:

```sh
./run_build.sh
./build/tests/benchmarks/buffetalligator_slicemapvsvecset --csv build/map-comparison.csv
```

See [the measured results and remaining qualification work](MAP_RESULTS.md).

The PrioritySlice showdown uses [priority_slice_benchmark.cpp](priority_slice_benchmark.cpp),
with [baseline and current measurements](PRIORITY_SLICE_RESULTS.md). Round two adds
[priority_slice_showdown.cpp](priority_slice_showdown.cpp) against `std::priority_queue`, with
[its measurements](PRIORITY_SLICE_SHOWDOWN.md), including `HeapSliceT`, the same words as a
min-max heap for a single owner.

The script and VS Code build tasks both use `build/`; there is no automatic
`current/` subdirectory. `--build-dir DIR` uses exactly DIR. Debug and Release
share that directory, so rebuild Release before interpreting timings.

The comparison runs ownership, capacity, duplicate, merge, and drain checks before
every timed selection. `--case contracts` runs only correctness checks; CTest
registers this mode when tests and benchmarks are enabled. Performance sweeps are
manual and have no timing thresholds.

## Map contenders and contracts

| Contender | Storage and lookup | Duplicate policy | Concurrent use in this experiment |
| --- | --- | --- | --- |
| VecSet indexed | Fixed distinct-key capacity; dense Slice handles plus atomic hash index | Replaces existing row without consuming capacity | First publication uses acquire/release; blocking per-row gates protect replacement and retained reads |
| VecSet SIMD | Same VecSet and payload ownership; existing `SIMDMisc::find_id` kernel | Same as indexed VecSet | ID scans run only after publishers finish |
| SliceMap | Growing hash buckets with immutable eight-ID SIMD leaves and stable positions | Replaces at the existing position | Insert/read/update overlap; old payloads reclaimed through hazards |
| HazardMap | Original scalar bucket-chain traversal with hazard publication | Appends rows; lookup finds newest | Insert/read overlap; nodes are reclaimed only after workers stop |
| unordered_map+mutex | `std::unordered_map<int64_t, Slice>` under one mutex | `insert_or_assign` | Insert/read/update overlap |
| VersionMap | Experimental bucket chains with atomically published immutable Slice versions | One key node; newest published version wins | Insert/read/update overlap; obsolete versions collected only when all workers stop |

SliceMap hashes to a bucket and scans its immutable ID block with the existing
SIMD kernel. Full buckets split and the position directory grows beyond the
constructor's reservation. Replacements keep the same position and release the
old Slice after its hazard readers finish; independently retained Slice handles
preserve the backing allocation. VersionMap is a separate experiment using
scalar bucket traversal and delayed collection, with neither SIMD lookup nor
hazard-pointer reclamation.

HazardMap is not evidence of safe concurrent hazard reclamation: it clears its
hazard before returning the borrowed handle and this experiment keeps nodes alive
through the reader phase. Its scalar traversal is reported honestly; the explicit
SIMD comparison is VecSet's contiguous ID scan.

VersionMap grows key nodes beyond its initial bucket hint but does not rehash.
Its publication uses pointer CAS; allocation is not promised to be lock-free.
It retains every superseded Slice until a quiescent collection, so storage grows
with writes between collections. Duplicate timings **include** that collection.
It is an experimental candidate, not a production replacement.

VecSet's cached pointer list, score sorting, drains, and merges require quiescent
workers. All candidates return a retained Slice in comparable lookup tests.
SliceMap's `data()` shares a contiguous borrowed pointer view between concurrent
readers while writers and reset are quiescent;
the first access after mutation includes rebuilding it, and later accesses use
the cache. The view workloads report both costs. `as()` borrows a payload while
writers are quiescent; concurrent readers use `get_slice()` or `slice_at()`.
Reset may overlap retained readers but requires quiescent writers and waiters;
merge and move require quiescent users. There is no individual-key erase API.
Publication and lookup use atomic protocols without a container mutex; allocation,
Slice teardown, and arbitrary typed finalizers have no lock-free guarantee.

## Map workloads and measurement

Defaults: 64, 256, 512, 2,048, 16,384, and 65,536 rows; 65,536 total queries;
two warmups and five measured rounds; teams of 1, 2, and the hardware-reported
worker count. Stream shapes include 1/1, 1/many, many/1, and balanced teams.

- Unique fill and concurrent gather reserve destination storage before timing.
- Lookup covers scattered hits, sequential hits, misses, 50% misses, eight hot
  keys, and quarter-full storage.
- Streaming overlaps unique-key producers with polling readers.
- Growth exceeds the initial size hint by eight times; fixed-capacity candidates
  are explicitly ineligible.
- Views measure cached pointer access versus rebuilding a full list; these are
  different operations and do not receive a shared winner.
- Merges include uneven partitions and validate every source and destination.
- Duplicate replacement includes sequential updates and concurrent shared-key
  writers/readers; append-only candidates are explicitly ineligible.

Candidate order rotates each round. Persistent worker teams exclude thread
creation. Payload setup, output allocation, verification, and final container
destruction are outside timing; insertion includes candidate node allocation.
Every sample validates payload identity and ownership after timing.
The scalar loops in the harness generate/control operations; SIMD ID scanning
uses the existing kernel.

Tables use the logging library's `print_table`, including first and second place
with their medians for the largest row count per section in the final summary.
Detailed tables report median [min,max]; costs are aggregate, not individual
request latency, and the benchmark adds no mutex around SliceMap.
The CSV contains all case/candidate medians and ranges. Small or overlapping
differences do not establish a winner.

```sh
# Correctness, including twice the requested worker count for stress.
./build/tests/benchmarks/buffetalligator_slicemapvsvecset --case contracts --workers 32

# Uneven partitions and a complete diagnostic sweep.
./build/tests/benchmarks/buffetalligator_slicemapvsvecset \
    --rows 131 --workers 3 --rounds 2 --lookups 4096

# Repeat a particular workload with more samples.
./build/tests/benchmarks/buffetalligator_slicemapvsvecset \
    --case duplicates --rows 65536 --workers 16 --rounds 15 --csv build/duplicates.csv
```

Use `--help` for all selections. The 64-byte payloads carry deterministic identity
tags, not embedding data; no embedding computation is benchmarked here.

## Separate production-container benchmarks

Build with `./run_build.sh`, then run from the repository root:

```sh
./build/tests/benchmarks/buffetalligator_slice_map_benchmark
./build/tests/benchmarks/buffetalligator_slice_queue_benchmark
```

`BUFFETALLIGATOR_BUILD_BENCHMARKS` defaults on for a standalone build and off when
the library is included as a subproject. Benchmarks are separate executables and
are not registered with CTest, so ordinary correctness runs do not depend on
machine speed or run the performance sweep. No additional dependencies are needed.

## Workloads

Both executables run an unlocked standard-container baseline on one thread,
then compare concurrent worker teams against a standard container protected by
one `std::mutex`. The default sweep includes one producer/one consumer, balanced
teams at powers of two up to the reported logical CPU count, and asymmetric
one-producer/many-consumer and many-producer/one-consumer teams. Explicit worker
counts replace the sweep and can oversubscribe the machine for stress testing.

| Benchmark | Work measured | Default size |
| --- | --- | --- |
| SliceMap / unordered_map | Insert unique IDs, then successful lookups, then missing lookups | 4,096 rows; 65,536 lookups per phase |
| SliceQueue / deque | Move each message from its producer to exactly one consumer | 1,048,576 messages |

The standalone map benchmark synchronizes producers before starting readers so
insertion, hit, and miss costs have separate timings. Consumers obtain shared
Slice claims without removing map entries. The `stream` and `duplicates` cases
in `slicemapvsvecset.cpp` exercise overlapping producers, readers, and replacement
with the growing SIMD-bucket SliceMap.

SliceQueue currently uses mutexes and semaphores for block handoffs; it is not
reported as lock-free. The deque comparison blocks on condition variables and
has a global bound of `producers * capacity`, matching SliceQueue's total message
capacity. SliceQueue partitions that capacity into per-producer pools, while
deque shares it across producers. All consumers remain active until closure and
drain. Queue throughput counts delivered messages; each message requires a push
and a pop. It does not count those as two independent delivered messages.

For the one-thread queue case, storage is sized to fit the entire input rounded
up to a whole SliceQueue block. That thread fills and then drains both queues,
so it cannot block waiting for itself. Concurrent cases use `--capacity` per
producer and exercise bounded storage reuse.

## Measurement

- Deterministic unique identifiers use independent 64-byte Slice claims;
  both implementations receive the same payloads and use the same worker loops.
- Payload allocation and initialization, input sharing, container reset,
  correctness validation, and thread creation/join are outside the timed region.
- Map slot arrays and hash buckets are reserved before timing; row/node
  allocations during insertion remain included for both maps. Deque's own
  internal allocations also remain included.
- Successful map lookups retain their returned Slice in preallocated output
  slots; those claims are released outside timing. Queue consumption includes
  reading the identifier, recording it in a pre-reserved per-consumer buffer,
  and releasing delivered Slice claims. There is no shared per-message test counter.
- Worker teams persist across warmups and measured samples. Timing spans the
  earliest worker entry through the latest worker completion, including worker
  scheduling skew and queue binding, flush, closure, and drain.
- One warmup and five measured repetitions run by default. Implementation order
  alternates between samples. Results include median, min/max time, aggregate
  throughput, and the ratio of median throughputs. Aggregate ns/op is not
  individual message latency.
- Every sample, including warmups, verifies all map outputs or exact-once queue
  delivery after timing. Failures produce a nonzero exit status. No performance
  threshold determines success.
- Long cases report progress every second. `--timeout` limits each complete
  topology comparison, including setup, warmups, validation, and repetitions.

Run on an otherwise idle machine using a Release build. Sanitizer timings are
for correctness checks only. Vary map size, occupancy, and worker count to expose
differences between lookup approaches; no universal winner is assumed.

## Examples

```sh
# Explicit contention and raw per-sample CSV output.
./build/tests/benchmarks/buffetalligator_slice_queue_benchmark \
    --producers 8 --consumers 8 --csv build/queue.csv

# Give both implementations a batch of 256 messages per public call.
./build/tests/benchmarks/buffetalligator_slice_queue_benchmark \
    --producers 8 --consumers 8 --batch 256 --capacity 4096

# Exercise partial batches, uneven partitions, and minimal queue capacity.
./build/tests/benchmarks/buffetalligator_slice_queue_benchmark \
    --producers 3 --consumers 5 --items 100003 --batch 37 --capacity 256

# Compare a larger map with concurrent producers followed by concurrent readers.
./build/tests/benchmarks/buffetalligator_slice_map_benchmark \
    --items 65536 --lookups 65536 --producers 8 --consumers 8 --csv build/map.csv

# Unlocked baseline on a single worker thread.
./build/tests/benchmarks/buffetalligator_slice_map_benchmark --single-thread
```

Use `--help` for options. `--items` is the total work, not work per producer;
`--lookups` is the total work in each map lookup phase. SliceMap has 256 reusable
hazard records for simultaneously registered threads; the benchmark reserves
one for its coordinator and rejects larger producer/reader teams. The comparison
contract stress doubles `--workers`, limiting that option to 127. Queue capacity must
be a positive multiple of 256, and batch
sizes range from 1 to 256. Repetitions, timeout, sizes, and explicit worker counts
must be positive; `--warmup 0` is supported for diagnostic runs.
