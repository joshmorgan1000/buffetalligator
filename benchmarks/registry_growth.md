# Registry growth benchmark — 2026-09-11

Registry growth is now integrated; see the [implementation and allocator benchmark report](integrated_growth.md).

The standalone prototype supports keeping the capacity check on fresh-slot acquisition: the measured single-thread median rose from 5.93 to 7.04 ns per fresh slot, and the 16-thread aggregate cost rose from 66.09 to 67.89 ns per slot (2.7%). These estimates have overlapping sample ranges; they are not a precise bound on the overhead.

Shared locking cost more at one thread but improved throughput substantially in the deliberately saturated 16-thread free-list test. Reduced CAS contention is a plausible explanation, not a measured causal result. This experiment does not justify putting a shared mutex around every production Slice operation.

## Environment

- Apple M4 Max, 16 logical/physical CPU cores, 128 GiB RAM, 128-byte cache lines.
- macOS 26.6.2 (25G83); Apple Clang 17.0.0 (clang-1700.6.3.2), C++20, Release (`-O3 -DNDEBUG`).
- One benchmark process, unpinned workers, ordinary OS scheduling; this is a shared development machine.
- Each thread-count group reuses its workers for all modes and trials.

## Steady-state results

Values are median **aggregate wall nanoseconds per operation**, followed by the minimum–maximum of nine trials. Multi-thread rows describe throughput, not individual request latency.

| Workload | Threads | Fixed capacity | Atomic growth check | Shared reader guard |
|---|---:|---:|---:|---:|
| Fresh slot | 1 | 5.92 (5.18–17.92) | 7.04 (5.47–17.62) | 16.14 (14.15–45.87) |
| Fresh slot | 2 | 25.70 (20.20–31.16) | 29.43 (25.79–30.72) | 28.67 (26.55–31.13) |
| Fresh slot | 16 | 66.09 (63.74–71.24) | 67.89 (62.09–70.94) | 47.35 (40.28–56.12) |
| Pop + push pair | 1 | 13.64 (11.91–21.14) | 13.75 (8.97–41.38) | 22.88 (20.96–34.97) |
| Pop + push pair | 2 | 36.86 (34.19–40.95) | 34.23 (33.24–37.46) | 37.69 (33.97–41.90) |
| Pop + push pair | 16 | 833.06 (613.40–888.72) | 740.76 (621.20–843.41) | 59.12 (53.09–62.83) |

The atomic-growth mode loads capacity only when the free list is empty and it issues a fresh slot. The recycled path contains no capacity check; differences between fixed and atomic-growth recycled results cannot be attributed to that check. Scheduling, code layout, and contention remain sources of variation.

## Growth costs

Each cold trial starts with a fresh mapping and performs 262,144 fresh claims. The growing mode extends page permissions 32 times in 1 MiB chunks. The event timer starts after obtaining the growth mutex and ends after release-publication of capacity; it excludes mutex wait time and later page faults. The full cold-trial costs include those waits and first-touch faults.

| Threads | Median trial event p50 (µs) | Median trial event p99 (µs) | Worst event (µs) | Fixed cold ns/slot | Growing cold ns/slot |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.458 | 3.375 | 43.417 | 24.48 (8.77–59.36) | 37.26 (14.61–44.09) |
| 2 | 0.916 | 1.833 | 5.500 | 13.78 (12.97–21.88) | 14.43 (12.94–21.94) |
| 16 | 1.541 | 2.375 | 5.167 | 55.86 (24.85–81.51) | 56.69 (18.94–64.51) |

The cold-trial ranges are wide, especially at one and 16 threads. The permission-extension event measurements are more consistent, but they do not represent the complete worst-case delay experienced by a claimant.

## What was measured

- A C++ replica of the core tagged free list, fresh-slot counter, 64-byte record layout, and stride-two slot permutation, using a 64 MiB virtual reservation.
- Fixed capacity: the whole reservation is writable from the start.
- Atomic growth: the first 1 MiB is writable; a fresh claimant acquire-loads capacity and calls a mutex-serialized `mprotect` extension if needed; a release store publishes usable capacity.
- Shared guard: the fixed-capacity algorithm with a `std::shared_lock` around every pop and every push; no writer or relocation is included, so this measures reader-guard overhead only.
- All modes use identical stable storage and separate the hot head, fresh counter, and capacity onto 128-byte cache lines; the production global-variable layout is not reproduced.
- Fresh trials include construction of each record, use prefaulted storage, and sum 16 batches of 262,144 claims; quiescent resets between batches are outside timing.
- Recycled trials perform 4,194,304 pop/push pairs, seeded with one available record per worker, with no application work between operations; this intentionally maximizes contention.
- Two warmup trials precede nine measured trials for each scenario; mode order rotates, and timestamps surround bulk batches rather than individual operations.
- Trial checksums keep claimed indices observable; a separate verifier checks the complete issued-index set, exclusive recycled ownership, and reference updates to an existing record during growth.

This is a registry prototype, not an integrated allocator benchmark. It excludes slab allocation, the recycling worker, placement callbacks, packed Slice encoding, and normal claim/retain/release work. It does not establish an end-to-end allocation regression or speedup, and it does not remove the handle’s fixed slot-ID ceiling.

## Validation

- `./run_build.sh`: all 12 existing tests passed.
- Prototype `--verify`: passed at 1, 2, and 32 threads, with eight growth events in every case.
- UndefinedBehaviorSanitizer: the same 1/2/32-thread verification passed.
- ThreadSanitizer could not validate races: the local Apple runtime crashed in `__tsan::SlotLock`; an independent minimal thread-and-join program also crashed.
- AddressSanitizer stalled in its shadow-memory initialization before `main`, confirmed by a process stack sample; the stalled process was stopped before timing.
- All 216 final timing rows completed and their embedded checks passed.

## Reproduce

```sh
./run_build.sh
build/current/tests/buffetalligator_registry_bench --verify
build/current/tests/buffetalligator_registry_bench > registry.csv
```

The benchmark target is enabled with the project tests on macOS and Linux. Run it without concurrent builds or sanitizer processes. Sanitizer verification on this machine required execution outside the agent sandbox.

```sh
c++ -std=c++20 -O1 -g -fsanitize=undefined -fno-sanitize-recover=all -pthread tests/bench_registry.cpp -o /tmp/registry-ubsan
/tmp/registry-ubsan --verify
```

[Benchmark source](../tests/bench_registry.cpp) · [Raw measurements](registry_growth_2026-09-11.csv)
