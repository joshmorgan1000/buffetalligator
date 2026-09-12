# SliceMap as a sequence queue — 2026-09-11

**Historical baseline:** this report measures SliceMap before the [indexed lookup and TLS row-allocation changes](slicemap_performance.md); its original measurements and audit are retained below.

The existing SliceMap was tested with one shared atomic producer sequence and one shared atomic consumer sequence, with the producer sequence stored as the map's row ID. A second variant consumes known slots directly. Both use the existing public API without changing SliceMap.

At eight producers/eight consumers and a 65,536-message window, the final median rates were **209.51 million messages/sec for the C TLS queue, 4.18 million for direct-slot SliceMap, and 0.384 million for ID-based SliceMap**. Avoiding ID lookup and its hazard window improved SliceMap by 10.9× in this workload, but the C queue remained 50.2× faster than the indexed variant.

## Final comparison

Million delivered messages/sec, median of seven repetitions, with minimum–maximum in parentheses. Every caller pushes or pops one Slice at a time. A delivered message includes both enqueue and dequeue, counted once.

| Implementation | 8,192-message window | 65,536-message window |
|---|---:|---:|
| C TLS + semaphores | **42.96 (29.66–46.04)** | **209.51 (178.25–225.40)** |
| Moodycamel direct | 28.23 (13.07–37.40) | 68.56 (58.03–76.48) |
| Moodycamel + TLS | 24.48 (20.95–24.87) | 110.96 (108.31–136.50) |
| SliceMap, message-ID lookup | 2.325 (1.060–2.433) | 0.384 (0.371–0.402) |
| SliceMap, direct-slot consumption | 3.845 (3.024–4.188) | 4.175 (3.904–4.615) |

These are repeated finite-window drains, not the long streams in the [earlier TLS comparison](queue_tls_comparison.md). Barriers, completion, wakeups, and imbalance cost substantially more per item in the small window. The same windows and harness are used for all five implementations in this table.

[Final raw measurements](queue_slicemap_final_2026-09-11.csv) · [CPU load samples](queue_slicemap_final_load_2026-09-11.csv) · [Commands and source hashes](queue_slicemap_final_run_2026-09-11.json)

## How the two SliceMap adapters work

The adapters are in [bench_queue.cpp](../tests/bench_queue.cpp). Both producer and consumer counters are separately aligned to 128 bytes and use `fetch_add(1, memory_order_relaxed)` for ticket allocation; the map's publication protocol provides payload visibility. Tickets are reserved individually even when the benchmark's bulk-call option is selected.

In `slicemap_sequence`, producers call `add_slice(producer_ticket, slice)`. A consumer reserves its next ticket and repeatedly calls `get_slice(consumer_ticket)` until that exact message is published. A failed lookup retains the pending ticket rather than skipping it. Consumers stop reserving tickets at the known window size. Message sequence and the payload's immutable test identifier are distinct: the former is the map key, and the latter checks delivery integrity.

In `slicemap_indexed`, producers are identical, while consumers reserve physical slot numbers and call `published(slot)` followed by `slice_at(slot)`. Publication order can differ from producer sequence order, so this variant does not consume messages in sequence-ID order. Its correctness check verifies the stored message sequence against the payload. Slot access is safe here because rows are immutable throughout a drain and reset happens only after all participants reach the finish barrier; this variant does not exercise concurrent row reclamation or hazard-protected copy-out.

The map has capacity for the entire window and keeps all rows until reset. It has no consuming erase operation in the current API. Before timing, the harness stages owned views of the same real 16-byte Slice descriptors used by the queues. Producers move those views into the map. Consumers use the real Slice copy-out API, copy its descriptor for validation, and release the temporary reference. The map and fixture keep the payload alive through the drain.

Per-row `new Row` allocation and consumer retain/release occur inside the timed region. Map reset, row reclamation, and replenishing the staged input references happen outside timing. Thus these rates exclude a cost that a reusable streaming adapter would still need to pay. The CSV's `timed_moody_allocations` column measures only the moodycamel allocator: zero there does not mean SliceMap avoids allocation.

All payload views share one backing allocation, as in the earlier queue fixture. Consequently, SliceMap's copy-out contends on that backing's reference counter; the descriptor-moving queues do not perform retain/release per item. Different payload ownership layouts could change that cost. This comparison measures the existing API's work, not a pure comparison of two ticket counters.

## Why this SliceMap path became expensive

The current implementation is a fixed-capacity append/result channel. `get_slice(id)` searches the ID array from the beginning, rather than looking up a hash bucket or directly indexing by ID. Consuming every distinct message through that API requires roughly quadratic search work across a window. At 65,536 rows, a successful lookup examines about 32,000 IDs on average, before counting unsuccessful polls.

Read-only history inspection identified commit **`9b9129e`, September 8, 2026, “Upgrades”**, as the change that introduced `find_atomic_ids` in [slice.cpp](../src/memory/slice.cpp). It replaced direct `SIMDMisc::find_id(ids(), capacity, id)` calls with an eight-ID snapshot loop. Each snapshot element is loaded through `atomic_ref<int64_t>` with acquire ordering, then copied into a temporary array for SIMD search.

The current Release object confirms eight scalar acquire loads (`LDAPR`) and stack stores per full snapshot, followed by an out-of-line call to `SIMDMisc::find_id` for those eight values. Thus the live lookup path pays snapshot construction and a function call repeatedly while traversing the array. This is a concrete code-generation cost, beyond the source-level label “atomic.”

The same commit also strengthened row-pointer publication, verification, removal, and hazard-pointer accesses to `seq_cst`, and changed `id()` and reset's ID accesses to atomics. These changes predate this queue benchmark; the benchmark work did not alter SliceMap's production implementation. The commit metadata does not establish which person or agent authored each change.

Some distinctions matter when deciding what to optimize:

- Concurrent publication/reset writes the ID array, so replacing atomic reads with unsynchronized SIMD reads would reintroduce data races under the current API contract.
- Acquiring every rejected candidate is a candidate for reduction: an atomic relaxed screening pass could acquire and revalidate a matching ID before accessing its row, preserving the publication dependency.
- The hazard publication, pointer recheck, unlink, and reclamation scan form a separate safety protocol; weakening all of them together is not justified by this benchmark.
- `seq_cst` exchanges during quiescent destruction/merge appear stronger than their documented usage needs, but those operations are outside this timed handoff path and cannot explain its measured slowdown.
- The two external counters sit on top of SliceMap's existing `claimed_` and `published_` RMW counters; the public `add_slice` API still reserves and publishes a slot internally.

Direct-slot consumption removes the repeated ID search and hazard copy-out window and provides the measured 10.9× gain at the larger window. It does not isolate the cost of acquire versus relaxed loads, nor is it an old-versus-new SliceMap benchmark. The historical throughput regression has not been quantified against the old implementation under identical conditions.

## Other worker counts and measurement limits

An earlier four-engine pass tested the requested ID-based adapter with a fixed 8,192-message window at 1/1, 2/2, 4/4, and 8/8. Its SliceMap medians were 0.399, 0.893, 1.458, and 2.401 million messages/sec respectively. Its extra 65,536-message 8/8 case measured 0.395 million/sec. These observations show that more consumers help the scan, while a larger ID array sharply increases its work.

[Initial worker-scaling results](queue_slicemap_comparison_2026-09-11.csv) · [Initial CPU log](queue_slicemap_load_2026-09-11.csv) · [Initial run provenance](queue_slicemap_run_2026-09-11.json)

The final five-engine table uses an Apple M4 Max with 16 CPU cores and 128 GiB RAM, macOS 26.6.2, Apple Clang 17, C11/C++20 Release `-O3 -DNDEBUG`, normal moodycamel inlining, and no LTO or affinity pinning. Each repetition uses persistent workers, one warmup drain, and either 64 timed drains of 8,192 total messages or 16 timed drains of 65,536. The five engines rotate execution order. Payload ID reads and private count/sum/XOR accumulation are timed; allocation of payload backing, thread creation/join, and validation are not.

The host remained an active desktop. Final background CPU samples had median/maximum 224.3%/892.3% during the small-window pass and 169.4%/274.4% during the larger-window pass. The small-window pass had a transient competing burst and correspondingly wider ranges. The larger window is the stronger evidence for the main comparison. No unrelated processes were stopped.

An [earlier five-engine pass](queue_slicemap_indexed_comparison_2026-09-11.csv) coincided with a competing build using roughly 13 cores during the small-window case; its [load log](queue_slicemap_indexed_load_2026-09-11.csv) and [provenance](queue_slicemap_indexed_run_2026-09-11.json) are retained, but those results are excluded from the final table. The [short pilot](queue_slicemap_pilot_2026-09-11.csv) is also excluded.

## Validation and reproduction

The repository build script passed all 15 CTest tests after adding the SliceMap adapter. Both final SliceMap modes then passed ten consecutive stress executions and UndefinedBehaviorSanitizer. Checks cover exact-once delivery, message-order/payload association, metadata, payload visibility, partial batches, reset/reuse, asymmetric workers, and 24 producers plus 24 consumers. Verification deliberately yields after some producer sequence reservations to exercise out-of-order publication. Atomic bitmaps are used only in correctness runs.

All 70 final performance measurements validated counts and sum/XOR checksums after every drain, covering 55,050,240 timed deliveries plus warmups. Every row reports zero timed moodycamel allocations. The earlier four-engine pass separately covered 88,080,384 timed deliveries. ThreadSanitizer was not rerun because of the host runtime failure documented in the [registry experiment](registry_growth.md); UBSan does not establish race freedom.

Use the dependency setup from the [TLS report](queue_tls_comparison.md), then:

```sh
./run_build.sh -DBUFFETALLIGATOR_QUEUE_BENCHMARK=ON -DMOODYCAMEL_BENCHMARK_INCLUDE_DIR="$PWD/deps/src/concurrentqueue-benchmark"
build/current/tests/buffetalligator_queue_bench --verify --slicemap
build/current/tests/buffetalligator_queue_bench --slicemap --producers 8 --consumers 8 --batch 1 --items 1024 --rounds 64 --repeats 7 > slicemap_8192.csv
build/current/tests/buffetalligator_queue_bench --slicemap --producers 8 --consumers 8 --batch 1 --items 8192 --rounds 16 --repeats 7 > slicemap_65536.csv
```

`--items` is per producer, while the window is the total across producers. Without `--slicemap`, the benchmark retains its original three queue engines. SliceMap is append-only within each measured window; this benchmark does not turn it into a production streaming queue.
