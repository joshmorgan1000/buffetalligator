# Integrated registry growth — 2026-09-11

The allocator now reserves stable virtual space for its backing registry, commits a 1 MiB prefix initially, and extends the writable prefix in 1 MiB increments when fresh slot IDs require more storage. Existing records and live reference counters never move. The fixed encoding defaults to 24 slot bits, allowing 16,777,215 reusable slots and byte lengths below 1 TiB; CMake can override that ceiling.

## Implementation

- `ba_os_reserve` uses inaccessible virtual storage; `ba_os_commit` makes an owned page range writable, using `VirtualAlloc` on Windows and `mmap`/`mprotect` on POSIX.
- A blocking mutex serializes rare prefix extensions, and a release store publishes the resulting capacity to fresh claimants using acquire loads.
- The fresh cursor advances with a bounded compare-exchange only after storage is available; failed commitment cannot consume IDs, and exhaustion cannot wrap the cursor.
- Fresh records initialize their atomic fields before being returned; recycled records retain the existing tagged free-list protocol.
- The ordinary thread-plate claim path is unchanged and gains no capacity check or lock; retain/release resolve the stable registry pointer.
- The reservation remains for process lifetime, including after allocator shutdown, so outstanding slices can still be released.
- Commitment failure returns `BA_E_ALLOC`; reaching the encoded ceiling returns `BA_E_SLOTS` and still permits reuse of returned slots.

The safe fresh-cursor update differs from the earlier prototype, which used fetch-add; the prototype microbenchmarks are not exact timings for this implementation.

## Validation

- All 13 tests passed with 24-bit and 20-bit library configurations, built through `run_build.sh`.
- The OS test verifies extension zero-fill, preservation of existing pages, and native mutex exclusion.
- A dedicated 17-bit core test exhausts all 131,071 usable slots with 32 allocation threads while a reader retains and views an existing slice.
- The test injects five commit failures, checks unmodified capacity and unconsumed IDs, and uses a tight budget to verify charge rollback.
- It checks all issued IDs for uniqueness, repeated exhaustion, reuse after exhaustion, and final release of a retained slice after shutdown.
- The integrated growth test passed under UndefinedBehaviorSanitizer.
- An installed CMake consumer verified `ALLIGATOR_SLOT_BITS=24` propagation and ran the public Slice contract test.
- Runtime validation was on macOS; Windows and Linux implementations were not executed in this environment.
- ThreadSanitizer and AddressSanitizer remain unavailable due to the local runtime failures documented in the earlier prototype report.

## Before/after benchmark

Three paired runs used the existing `buffetalligator_bench` executable on the M4 Max. The baseline is the 21-bit fixed-registry binary saved before this change; the candidate is the 24-bit integrated implementation. Both used Release builds, the same benchmark source, and the same logger revision. Run order alternated, and no builds or sanitizer processes ran alongside the benchmark.

Values are medians of aggregate wall nanoseconds per operation, with the three-run minimum–maximum in parentheses. The existing view and novel rows report averages of individually timed operations. The harness includes per-operation clocks and statistics updates; these are regression observations, not isolated slot-allocation costs or a statistically established speedup.

| Workload | Fixed 21-bit | Growing 24-bit | Median change |
|---|---:|---:|---:|
| 1 threads claim+free | 64.95 (63.16–88.57) | 58.37 (57.90–58.92) | -10.1% |
| 4 threads claim+free | 82.51 (47.44–85.27) | 47.02 (42.16–47.90) | -43.0% |
| 8 threads claim+free | 35.16 (27.73–36.05) | 37.25 (30.97–42.33) | +5.9% |
| 16 threads claim+free | 25.79 (24.56–27.33) | 26.34 (23.30–32.45) | +2.1% |
| view+drop | 27.01 (26.41–27.10) | 29.77 (26.02–31.08) | +10.2% |
| 1 MiB novel | 1033.83 (1012.11–1361.22) | 973.17 (966.77–1551.97) | -5.9% |

The results vary by workload and overlap substantially in most multi-threaded cases. The 16-thread claim/free median changed by about +2%; view/drop changed by about +10%, or 2.8 ns. These short runs do not establish that the registry change caused the apparent gains or losses. Explicit growth and failure handling are covered by the focused correctness test rather than inferred from the claim benchmark.

## Reproduce

```sh
./run_build.sh -DALLIGATOR_SLOT_BITS=24
build/current/tests/buffetalligator_registry_growth_test
build/current/tests/buffetalligator_bench
```

Changing the slot width changes the packed Slice interpretation: rebuild the library and consumers with the same definition. Existing CMake caches retain their previous value unless overridden explicitly.

[Raw measurements](integrated_growth_2026-09-11.csv) · [Full benchmark output](integrated_growth_2026-09-11.log) · [Prototype measurements](registry_growth.md)
