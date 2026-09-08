# Implementation results

Validated on September 8, 2026, on the same Apple M4 Max machine used for the baseline.

## Status

All implementation tasks from P0 through P5 are implemented, and P6 cleanup checks pass. The user accepted close benchmark misses for this release because the machine is busy with other workloads, while retaining crashes as a release blocker. The original strict benchmark limits remain documented below and are not reported as passing. Final release and ASan/UBSan runs each pass all nine tests without crashes, warnings, or sanitizer diagnostics.

The private arena is now pure C11 under `src/core`, with the public C++ API wrapping it. The old allocator, buffet, tracker, and SliceFriend implementations are removed. `Slice` remains 16 bytes with unchanged inline bodies and packed layout. Existing public signatures remain available, including the legacy hazard-limit constant; the actual hazard pool now uses runtime sizing.

Implemented behavior includes biased plate reference counting, TLS sealing, worker reclamation, runway replenishment, zeroed novel caching, per-placement and shared OS budgets, runtime geometry and pressure probes, synchronous trim, explicit shutdown, the placement descriptor overload, and the public Memory API. Header regressions cover WeakSlice copy offsets, typed lengths/views, concurrent map publication, reset readers, and hazard-row reuse.

## Validation

- `./run_build.sh`: all 9 tests pass, with no compiler warnings.
- Homebrew LLVM AddressSanitizer plus UndefinedBehaviorSanitizer: all 9 tests pass with `UBSAN_OPTIONS=halt_on_error=1`, and the full test log contains no sanitizer diagnostics or warnings.
- ThreadSanitizer: **not validated**; both installed runtimes crash before application initialization, with the saved Homebrew backtrace entering `__tsan::SlotLock` from intercepted `dispatch_once` during `InitializePlatform`.
- Apple AddressSanitizer deadlocks in its own startup; the Homebrew runtime was used successfully instead.
- The three original contract tests are unchanged apart from the two permitted private-header include removals in `memory_test.cpp`.
- `run_build.sh` is unchanged; `git diff --check` passes.
- Cleanup scans find no TODO, FIXME, or phase scaffolding in `src`, `include`, or `tests`; old-core references are limited to the required forward declarations and friends.
- macOS was built and exercised; Linux and Windows branches have not been executed on their target systems.

Reproduce the successful sanitizer configuration with:

```sh
CC=/opt/homebrew/opt/llvm/bin/clang CXX=/opt/homebrew/opt/llvm/bin/clang++ \
CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
CXXFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
LDFLAGS='-fsanitize=address,undefined' UBSAN_OPTIONS=halt_on_error=1 \
./run_build.sh --build-dir /tmp/buffetalligator-llvm-asan \
    --install-dir /tmp/buffetalligator-llvm-asan/install
```

## Final release validation

After LLDB inspection, slab-refill and plate-retirement functions were kept out of line so their register saves no longer burden ordinary claims and releases. `ba_claim` now needs a 16-byte stack frame instead of 96 bytes, and `ba_release` needs no stack frame. Reference-counting memory orders and the public API remain unchanged.

The final `./run_build.sh` and Homebrew LLVM ASan/UBSan build both pass all nine tests. A final benchmark completed without crashing: single-thread mean 28.76 ns, 16-thread wall 9.51 ns/op, worst claim 1.922208 ms, and settled footprint 1,851,704 bytes. Timing misses are accepted for this release under the user's busy-machine allowance; they have not been filtered out or relabeled as strict passes. Output is saved in `build/implementation-results/release-final-bench.txt`, with LLDB disassembly in `release-fast-path.txt`. ThreadSanitizer remains unavailable due to its previously diagnosed runtime startup crash.

## Benchmark gate

Each parallel worker performs one million claim/release operations; all measurements retain the original wall-clock timing. The latest three runs are `build/implementation-results/verified-benchmark-{1,2,3}.txt`.

| Row | Baseline mean ns | Median mean ns | Median wall ns/op | Worst max µs | Claims above 1 ms across three runs |
|---|---:|---:|---:|---:|---:|
| 1 threads claim+free | 81.0 | 29.54 | 54.47 | 264.833 | 0 |
| 4 threads claim+free | 440.59 | 55.27 | 24.71 | 35.625 | 0 |
| 8 threads claim+free | 1027.99 | 63.07 | 14.29 | 52.75 | 0 |
| 16 threads claim+free | 2790.99 | 89.88 | 9.46 | 1686.917 | 34 |
| view+drop | 19.24 | 18.88 | 18.88 | 23.792 | 0 |
| 1 MiB novel | 5545.57 | 1027.03 | 1027.03 | 15.375 | 0 |

The single-thread target is 81.00 / 3 = **27.00 ns**; the observed median is **29.54 ns**, so it fails. The 16-thread wall target is below the 1-thread baseline; **9.46 ns/op** passes against both its 81.00 ns mean and 99.29 ns wall time. The no-claim-above-1-ms condition fails with a worst observation of **1.686917 ms**.

After the three-second settle, each built-in slab is 268,435,456 bytes. Heap holds 536,870,912 bytes at runway target 1, below its 805,306,368-byte bound; aligned_heap holds 805,306,368 bytes at target 2, below its 1,073,741,824-byte bound. Measured process footprint is approximately 1.8 MiB, versus roughly 28 GiB in the initial baseline. The footprint gate passes.

Profiles were taken before performance changes. The Time Profiler recorded 1,259 of 3,600 leaf samples in `ba_release`, versus 5 inclusive samples in `ba_carve`. Hardware reports 128-byte cache lines while plate records are 64 bytes; fresh slot assignment now spaces records using the probed cache-line size. The System Trace also recorded benchmark threads preempted for more than 15 ms and substantial unrelated compiler/linker activity. This establishes scheduler interference but does not waive the failed benchmark limits or prove that every outlier has the same cause.

Raw profiles, trace exports, baseline output, and scheduler summaries are retained under `build/implementation-results/`.

## Correctness changes from the design pseudocode

- Current-slab access is serialized until a newly carved plate owns its slab reference; loading the pointer and incrementing its reference later permits reclamation in between.
- The worker announces a pending runway publication under the same current-slab token; a claimant waits for budget-charged prepared capacity instead of falsely failing its second slab allocation.
- Header-pool access is synchronized because a synchronous runway miss can build on a caller as well as on the worker.
- macOS thread cleanup uses `_tlv_atexit` so plates are sealed before compiler TLS storage is destroyed.
- C++ placement callbacks use statically generated typed bridges; the plan's function-pointer casts triggered UndefinedBehaviorSanitizer despite matching handle layouts.
- Worker publication and sleep checks use a common sequentially consistent order to prevent missed wakeups; SliceMap's hazard publication and row replacement similarly require a total order across separate atomics.
- Callback failure paths release both substrate and handle, background re-zero failure records a placement fault, and policy updates synchronize with worker reads.
- Concurrent ID lookup snapshots use atomic loads before the existing SIMD search, and Apple SIMD broadcasts use splat constructors rather than scalar zero-extension constructors.

## README traceability

| README behavior | Implementing functions |
|---|---|
| Stable built-in identifiers and default selection | `ba_init`, `ba_placement_register`, `BuffetMenu::ensure_builtins_slow` |
| 16-byte claims, thread-local bump path, shared views | `ba_claim`, `ba_view`, `SliceLayout` assertions |
| Plate reference counting and thread-exit sealing | `ba_retain`, `ba_release`, `ba_seal`, `ba_thread_exit`, `ba_os_tls_set` |
| Direct and novel routing | `ba_claim_slow`, `ba_carve`, `ba_claim_novel` |
| Runtime geometry and honored explicit slab size | `ba_os_probe`, `ba_plate_size`, `ba_placement_register` |
| Worker startup preallocation and runway reuse | `ba_initialize_placement`, `ba_replenish`, `ba_runway_pop` |
| Guaranteed fresh zeroing and recycled zeroing | `ba_os_map`, custom allocator contract, `ba_rezero`, `ba_recycle_slab` |
| Framework-owned C++ handles and exact callback types | `PlacementBridge`, `destroy_handle`, `ba_slab_release`, `ba_novel_dispose` |
| Shared OS ceiling and per-placement budgets | `ba_init`, `ba_charge`, `ba_charge_counter`, `ba_budget_set` |
| Caller-side novel allocation and optional zeroed caches | `ba_claim_novel`, `ba_retire_novel`, `ba_novel_cache_pop` |
| Pressure policies and trim | `ba_os_pressure`, `ba_worker_main`, `ba_trim_placement`, `ba_trim_cache` |
| Explicit quiescent shutdown | `BuffetMenu::shutdown`, `ba_shutdown`, `ba_worker_stop` |
| Memory counters and resource queries | `ba_stats`, `ba_stats_total`, `ba_sysinfo`, `Memory` definitions in `slice.cpp` |
| Descriptor overload and alignment errors | `BuffetMenu::register_type`, `ba_placement_register` |

The existing Build and Install README sections were preserved as requested.
