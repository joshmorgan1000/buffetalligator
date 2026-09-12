BuffetAlligator audit, September 12, 2026

The library functions, but its current allocation strategy is materially different from the shared slab-chain design. This audit changes tests and benchmark tooling only. The production sources, public header, and build script remain unchanged. The measured runtime is commit `07c8d2c92996a0483456b5dc8b1866a74a181177`.

The corrected suite ran 27 tests: 24 passed and three failed. All 19 pre-existing tests passed. The three failures demonstrate allocation calls in the plain wire codec, encrypted wire codec, and warmed TCP exchange. They remain failing tests; no expected-failure inversion or suppression was applied. Whole-slab benchmarking separately encountered `BA_E_BUDGET` in all three repetitions.

The large rewrite is [commit 9b9129e](https://github.com/joshmorgan1000/buffetalligator/commit/9b9129e850dfcdd24a431a4b15e67d3b9cd2e9a5), “Upgrades,” timestamped September 8, 2026, at 01:51:47 Eastern. It changed 29 files, adding 4,847 lines and deleting 1,432, including an 1,103-line implementation plan. It removed the original Alligator, Buffet, SliceFriend, and tracker implementations and introduced `src/core/ba_core.c`, `ba_core.h`, `ba_os.c`, `ba_os.h`, and `ba_worker.c`.

The plan explicitly prescribed thread-local plates and described their claim path as “One branch, zero atomics, one thread-local cache line.” Its P4-T4 task was titled “Delete the old core.” The accompanying results pursued a threefold single-thread latency improvement, improved parallel throughput, and lower retained footprint. These documents establish the recorded implementation rationale, not independent proof that the user authorized every architectural change. The plan called itself an approved direction; the originating discussion is not available in this audit. The plan and results were subsequently deleted in `a0d5952`, “removing plans,” at 02:38:38 that morning. They were inspected in Git, not restored.

| Aspect | Immediately before the rewrite | Current implementation |
|---|---|---|
| Arena registry | Singleton `Alligator` with `std::array<std::atomic<Buffet*>, 0x20000>` | Global table of plate records, with stable reserved addresses and progressively committed storage |
| Ordinary claim | `Buffet::claim()` atomically increments the shared slab bump offset | `ba_claim()` advances the caller's plain thread-local cursor |
| Slab advancement | Boundary-crossing path advances the current buffer and walks four successors ahead | Exclusive current-slab token, prepared-slab ring, worker replenishment, synchronous construction when the ring is empty |
| Reference counting | Buffer/slab ownership | Plate ownership, shared by many slices from that thread's region |
| Slice representation | 16 bytes: packed size/index and cached pointer | Still 16 bytes; the index identifies a plate rather than a Buffet |
| Initial prepared capacity | First slab plus one successor | Current slab plus one prepared slab in the observed fixture |

The pre-rewrite code already had a worker and explicit novel allocations. Neither the existence of a worker nor a 16-byte Slice was introduced by September 8. The important change was replacing atomic shared-slab claims and the buffer-reference table with thread-local subdivision and plate records. The later September 11 change increased the default packed slot width to 24 bits.

My initial audit asserted that `sizeof(Slice)` must be four, that different ranges must have different backing indices, and that initialization must immediately prepare four slabs. The historical implementation does not meet those assertions either. The alignment probe also deliberately violates the custom allocator's own precondition, and the registry probe exercises explicit novel allocations. These are now observations rather than regression assertions. Their initial outputs are preserved in `initial_audit_with_overstrict_assertions.txt`, so the correction is visible.

The current ordinary claim path works as follows:

1. A placement obtains aligned slab backing and publishes a current slab and prepared reserve.
2. A thread first claims a larger region of the slab, called a plate, and obtains one registry slot for that region.
3. Small Slice claims advance that thread's cursor by a 64-byte-rounded amount. Their descriptors contain the requested size, the shared plate slot, and their individual host pointer. Claiming within an existing plate does not perform an atomic bump operation.
4. Copies and views update the plate's atomic reference state. Thread exit or plate exhaustion publishes the total number of issued claims. Slab ownership is released after the plate's outstanding references are gone.
5. An empty prepared ring can cause slab construction on the caller. Oversized or explicit novel claims allocate separate backing. Fresh registry entries can commit another writable chunk. A worker recycles or releases retired backing and adjusts prepared capacity.

This explains both the fast small-claim results and the architectural mismatch. A Slice can outlive its creating thread because lifetime is shared and atomic, even though its original claim cursor was thread-local. A plate index alone cannot recover one particular Slice's byte range. The current API exposes Vulkan buffer ranges; it does not expose the original singleton's buffer-reference array as a shader-addressable arena table.

`ba_stats_t` is a copied snapshot. Its source counters in `ba_placement_t` are atomic, and `ba_stats()` loads them atomically. The snapshot is not an atomic transaction across all counters. Ordinary built-in claims are 64-byte aligned; seeing `malloc` in a metadata or wire-buffer path does not by itself establish a misaligned Slice. A custom callback that lies about its declared alignment is accepted by the current registration code; the diagnostic fixture returned an address with remainder one modulo 64.

The allocation probe produced these measurements:

| Workload | Observed library calls |
|---|---:|
| 2,046 warmed 64-byte claims within prepared capacity | Zero malloc/calloc/realloc/aligned-allocation/map/commit calls |
| 8,192 preclaimed Slice queue handoffs | Zero observed allocation calls; full descriptor and payload identity preserved |
| 16,400 explicit novel claims crossing registry capacity | Two registry commits, totaling 2 MiB |
| 1,000 plain encode/decode pairs | 1,000 calloc calls |
| 1,000 encrypted encode/decode pairs | 1,000 calloc plus 1,000 malloc calls |
| 100 warmed TCP round trips, client and server in one process | 600 calloc plus 400 malloc calls; 13,848,000 bytes requested |

The counters wrap real C allocation entry points in separately compiled copies of the current library objects. They do not replace the allocator or emulate its behavior. Counts exclude allocations inside separately linked dependencies, C++ runtime internals, and custom callback code. Requested bytes are cumulative allocation volume, not resident memory. Performance binaries use the ordinary, uninstrumented library.

Measurements were made on an Apple M4 Max with 16 CPU cores, 128 GiB RAM, 16 KiB pages, and 128-byte cache lines, running macOS 26.6.2 with Apple Clang 17 and a Release build. Three repetitions produced 103 arena rows covering approximately 106.1 million operations, 36,000 measured network round trips, and 24,000 Vulkan upload/download pairs. Warmup is additional. CPU benchmarks, network benchmarks, and GPU benchmarks ran sequentially. Other machine workloads and scheduler placement were not controlled.

| Threads | 64-byte claim/touch/drop, median million operations/s | Observed range |
|---:|---:|---:|
| 1 | 123.655 | 68.130–127.793 |
| 2 | 251.446 | 140.326–253.984 |
| 4 | 284.343 | 278.364–289.049 |
| 8 | 187.068 | 184.065–197.046 |
| 16 | 168.467 | 150.703–244.803 |
| 32 | 221.625 | 214.375–247.312 |

These are aggregate rates, not per-thread rates. Every claim touches its first and last byte and releases its Slice. The timed region excludes thread construction and warmup. A separate sampled-latency run includes timer overhead; short operations sometimes measure zero because the clock advances in approximately 42 ns increments. Zero-valued sampled latencies must not be interpreted as zero-cost operations.

| Workload | Median result across three repetitions |
|---|---:|
| 1 KiB claim/touch/drop, one thread | 60.060 ns/operation; 16.650 million/s |
| 64 KiB claim/touch/drop, one thread | 1,846.283 ns/operation; 0.542 million/s |
| Aligned allocation + full zero + touch + free, 64 bytes | 24.674 ns/operation |
| Aligned allocation + full zero + touch + free, 1 KiB | 35.358 ns/operation |
| Copy/drop on one shared Slice, one thread | 154.371 million/s |
| Copy/drop on one shared Slice, 16 threads | 8.121 million/s aggregate |
| Preclaimed queue, one producer and one consumer | 166.205 million handoffs/s |
| Preclaimed queue, four producers and four consumers | 16.808 million handoffs/s aggregate |
| Explicit novel allocation/drop, 1 KiB | Sampled p50 1.667 µs; p99 23.334 µs |

The aligned-allocation baseline reuses the system heap and explicitly zeroes the whole request. The Slice workload advances through arena memory whose zeroing may have happened earlier or through the OS. These are application-level workload comparisons, not identical memory-access traces. Queue timings include payload verification, draining, and completion joins, but exclude payload allocation and queue construction. Every queue identifier is checked for duplicates or loss after timing.

The 256 MiB whole-slab workload released each claim before requesting another. It failed with `BA_E_BUDGET` after 116, 118, and 116 successful claims. The configured placement budget was 27,858,345,984 bytes. The rows record the failures explicitly. The code path and allocation counters are consistent with callers outrunning worker reclamation and charging additional backing until the budget is exhausted; that explanation is an inference, not a completed root-cause fix. No allocator change was made to make the benchmark finish successfully.

The network benchmark used separate client/server processes on loopback, one outstanding request, a reused source Slice, and validation of every response byte. Each request goes through the current API's connection/exchange lifecycle, so the results are not persistent-connection bandwidth figures. Libfabric used `FI_PROVIDER=tcp`; these are not hardware RDMA measurements.

| Protocol | 1 KiB median round trips/s | Median p50 | Median p99 |
|---|---:|---:|---:|
| TCP | 12,675 | 68.500 µs | 128.292 µs |
| UDP | 14,335 | 69.000 µs | 79.416 µs |
| Libfabric TCP provider | 2,010 | 492.250 µs | 554.750 µs |
| Encrypted TCP | 10,826 | 76.375 µs | 133.375 µs |
| Encrypted UDP | 10,470 | 80.292 µs | 140.708 µs |
| Encrypted libfabric TCP provider | 2,009 | 495.625 µs | 559.791 µs |

At 32 KiB, median TCP throughput was 7,939 round trips/s and encrypted TCP was 2,767 round trips/s. The complete results include all protocols and both payload sizes.

The network implementation serializes a Slice's payload and placement name into an allocated frame, opens a transport exchange, and claims backing again at the receiving process. The receiver gets an equivalent placement and copied bytes, not the sender's process-local arena reference. Encryption adds authentication and a temporary decrypted buffer. Device-only Vulkan memory adds staging transfers before sending and after receiving.

Vulkan synchronization with device-local-only requested memory had median upload/download-pair p50 latency of about 199 µs for 4 KiB and 197 µs for 1 MiB. Host-visible property combinations measured near the timer's 42 ns granularity on this machine. Those mapped-memory results are API-call overhead, not GPU transfer bandwidth. A separate device-write unit test executes `vkCmdFillBuffer` against the actual Slice buffer and verifies all 1,024 words after library readback for flags 1, 2, 6, and 14. It passes. It does not establish a shader-side 32-bit arena-reference contract.

The device-write test captures the actual queue-family choice by forwarding `vkCreateDevice` unchanged, prepares all Slice buffers before submitting, and uses a separately created command pool. It follows the [Vulkan fill-buffer contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdFillBuffer.html) and verifies GPU-written data rather than relying only on host-memory echoes. No validation-layer or ThreadSanitizer success is claimed; ThreadSanitizer is unusable on this host. Earlier sanitizer results are not presented as new coverage of this audit harness.

Reproduce the complete audit with:

```sh
bash tests/audit/run_audit.sh
```

The runner builds through `run_build.sh`, reports progress every second, runs tests and benchmarks sequentially, preserves their output under `build/arena-audit`, and returns failure when a test or benchmark fails. The test failures and budget failures are unresolved; this report is evidence for correcting the implementation, not a release approval.

Recorded evidence: [tests](contracts.txt), [arena CSV](arena.csv), [arena failure details](arena_progress.txt), [network CSV](network.csv), [Vulkan CSV](vulkan.csv), [device-write test](vulkan_device_write.txt), and [environment](environment.txt). The earlier aborted benchmark is preserved in `arena_initial_failure.csv` and `arena_initial_failure.txt`.
