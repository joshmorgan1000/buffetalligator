# PrioritySlice showdown, round two

Measured on macOS arm64 with Apple clang 17 (Apple simd backend) and on Ubuntu x86-64 with GCC 13.3
built with `-march=native` (AVX-512 backend) on 2026-09-25. Each number is the median of seven timed
samples after one warmup, each sample 100,000 deterministic operations. Lower is better.

```sh
./run_build.sh --skip-tests --no-color
./build/tests/benchmarks/buffetalligator_priority_slice_benchmark   # the round-one scoreboard
./build/tests/benchmarks/buffetalligator_priority_slice_showdown    # this round, against std::priority_queue
```

## What changed

Round one cached the last freed slot so a push after a pop lands with one CAS, but every pop and
every eviction still scanned the whole array. This round adds a block index for the natural order:
two caches behind the 64-byte header hold each 32-slot block's largest and smallest word. A pop
scans the block caches, then one block, then one CAS; an eviction does the same with the maxima.
The block scan reports the block's runner-up, so a pop refreshes its block's cache without a
second scan, and an eviction re-arms the rejection threshold from the pick's runner-up without
re-reducing the caches. The caches are hints: every scan re-checks the block it picked and fixes
a stale entry before acting. Capacities of one block skip the caches and run the flat algorithm.

The free slot count is the only thing that gates the constant-time rejection, so pops no longer
touch the threshold at all, and the fill that takes the last free slot arms it once. Two SIMD
kernels gained a runner-up form of the minimum scan, and the Apple backend reduces lanes with
vector minimum and maximum instead of a scalar merge.

## Round-one scoreboard, Apple M4

Raw `PrioritySlice`, same workloads and harness as round one. Round one's numbers are the
"current" column of its results page.

| Capacity | Workload | Round one (ns) | This round (ns) |
| --- | --- | ---: | ---: |
| 32 | Accepted push | 18.4 | 16.0 |
| 32 | Rejected push | 1.29 | 1.25 |
| 32 | Pop and refill | 13.8 | 12.4 |
| 32 | Pop and rotate | 14.9 | 16.8 |
| 256 | Accepted push | 66.9 | 26.0 |
| 256 | Rejected push | 1.27 | 1.25 |
| 256 | Pop and refill | 45.7 | 19.9 |
| 256 | Pop and rotate | 39.5 | 24.6 |

## Against std::priority_queue, Apple M4

`PrioritySliceT<uint32_t, uint32_t>` against a bounded `std::priority_queue`. The heap gets
whichever order suits the workload: a max-heap for the push workloads so a full heap evicts its
top, a min-heap for the pop workloads so it pops the best. A real replacement would need a
min-max heap to do both, so the heap column is a floor, not a competitor.

| Capacity | Workload | PrioritySliceT (ns) | std::priority_queue (ns) |
| --- | --- | ---: | ---: |
| 32 | Accepted push | 15.8 | 8.7 |
| 32 | Rejected push | 1.5 | 0.4 |
| 32 | Pop and refill | 12.1 | 20.5 |
| 32 | Pop and rotate | 16.4 | 9.6 |
| 256 | Accepted push | 24.5 | 12.0 |
| 256 | Rejected push | 1.7 | 0.4 |
| 256 | Pop and refill | 20.9 | 30.3 |
| 256 | Pop and rotate | 23.9 | 13.1 |
| 1024 | Accepted push | 28.0 | 15.5 |
| 1024 | Rejected push | 1.2 | 0.4 |
| 1024 | Pop and refill | 27.1 | 37.1 |
| 1024 | Pop and rotate | 27.8 | 18.2 |

## Ubuntu x86-64, AVX-512, GCC 13

| Capacity | Workload | Round-one harness (ns) | PrioritySliceT (ns) | std::priority_queue (ns) |
| --- | --- | ---: | ---: | ---: |
| 32 | Accepted push | 17.3 | 17.1 | 5.8 |
| 32 | Pop and refill | 28.1 | 28.0 | 8.7 |
| 32 | Pop and rotate | 25.8 | 25.7 | 6.3 |
| 256 | Accepted push | 20.0 | 19.7 | 17.5 |
| 256 | Pop and refill | 28.6 | 28.6 | 12.7 |
| 256 | Pop and rotate | 31.4 | 31.9 | 18.3 |
| 1024 | Accepted push | | 32.6 | 24.3 |
| 1024 | Pop and refill | | 45.0 | 15.7 |
| 1024 | Pop and rotate | | 57.4 | 26.7 |

The AVX-512 backend still merges its eight lanes with a scalar loop after each scan; the vector
reduction used on the Apple backend has not been ported there yet, which is most of the gap
between the two machines on the pop workloads.

## Threads, Apple M4, capacity 256

| Workload | Threads | PrioritySliceT (M ops/s) | heap + mutex (M ops/s) |
| --- | --- | ---: | ---: |
| Mostly rejected pushes | 1 | 764 | 231 |
| | 4 | 2300 | 43 |
| | 8 | 1610 | 62 |
| | 16 | 1385 | 69 |
| Beam, pushers and poppers | 1 + 1 | 22 | 86 |
| | 2 + 2 | 18 | 16 |
| | 4 + 4 | 8 | 17 |
| | 8 + 8 | 6 | 20 |

The rejection path is read-only, so a shared result set scales with the number of pushers. A
shared beam does not: every pop hits the header line and the same best slot, and a locked heap
wins there from four pairs up. Use this queue as the shared top-K filter; give a beam per-thread
heaps or a sharded design.

## Experiments

| Change | Observation | Decision |
| --- | --- | --- |
| Per-block minimum and maximum caches | Capacity 256 accepted push 67 to 26 ns, pop and refill 46 to 20 ns, pop and rotate 40 to 25 ns. | Kept |
| Runner-up form of the minimum scan | Removed the second block scan from every pop. | Kept |
| Re-arm from the pick's runner-up | Removed a cache reduce from every eviction. | Kept |
| Vector lane reduction on the Apple backend | About 4 ns off every scan of 16 words or more. | Kept |
| Pops no longer reset the threshold | Removed a store and a store-load round trip per pop; the free count already gates rejection. | Kept |
| Single-block capacities skip the caches | Capacity 32 back to the flat algorithm's cost; without it the caches cost 15 ns per pair there. | Kept |
| Sequentially consistent header protocol | No measurable cost on Apple silicon; removed anyway because the free count made it redundant. | Removed |

## HeapSlice: the heap on a Slice

Josh's follow-up: if a heap wins the single-owner rows, put the heap on a Slice. `HeapSlice` and
`HeapSliceT` are a bounded min-max heap over the same packed words behind the same 64-byte
header, so one structure pops the best and evicts the worst in logarithmic time with no scans and
no vector. It is single-owner, not lock-free; `PrioritySliceT` and `HeapSliceT` share the typed
layer, so the key encodings, Slice ownership, and API are identical.

The `std::priority_queue` column is still the ideal-order heap for each workload: a max-heap for
the push rows, a min-heap for the pop rows. A single standard heap cannot do both, so the fair
reading is HeapSliceT against whichever one-directional heap the row favours.

Apple M4:

| Capacity | Workload | PrioritySliceT (ns) | HeapSliceT (ns) | std::priority_queue (ns) |
| --- | --- | ---: | ---: | ---: |
| 32 | Accepted push | 16.6 | 14.5 | 8.8 |
| 32 | Rejected push | 1.3 | 1.9 | 0.4 |
| 32 | Pop and refill | 12.4 | 11.0 | 20.4 |
| 32 | Pop and rotate | 15.9 | 18.5 | 9.5 |
| 256 | Accepted push | 23.9 | 16.1 | 11.7 |
| 256 | Rejected push | 1.3 | 2.0 | 0.4 |
| 256 | Pop and refill | 21.9 | 17.7 | 30.6 |
| 256 | Pop and rotate | 24.5 | 21.3 | 13.5 |
| 1024 | Accepted push | 27.9 | 20.2 | 15.1 |
| 1024 | Rejected push | 1.2 | 2.0 | 0.4 |
| 1024 | Pop and refill | 27.0 | 24.5 | 37.9 |
| 1024 | Pop and rotate | 29.4 | 26.9 | 19.2 |

Ubuntu x86-64, AVX-512, GCC 13:

| Capacity | Workload | PrioritySliceT (ns) | HeapSliceT (ns) | std::priority_queue (ns) |
| --- | --- | ---: | ---: | ---: |
| 32 | Accepted push | 16.9 | 11.2 | 5.8 |
| 32 | Pop and refill | 28.4 | 8.2 | 7.3 |
| 32 | Pop and rotate | 25.8 | 10.2 | 5.7 |
| 256 | Accepted push | 19.9 | 18.3 | 17.0 |
| 256 | Pop and refill | 30.1 | 13.4 | 11.2 |
| 256 | Pop and rotate | 32.5 | 18.6 | 16.6 |
| 1024 | Accepted push | 32.7 | 19.7 | 24.3 |
| 1024 | Pop and refill | 54.7 | 16.9 | 14.2 |
| 1024 | Pop and rotate | 57.8 | 22.3 | 24.0 |

A min-max heap compares three candidates per level where a one-directional binary heap compares
two, which is the 1.4x on the rows where the standard heap gets its ideal order. Where the
standard heap would have to be the other kind, HeapSliceT wins by 1.5x to 1.8x. An interval heap
would bring the two-directional cost down to about the one-directional cost if that last factor
matters.

## Against Folly's RelaxedConcurrentPriorityQueue

Josh asked whether Folly has a lock-free priority queue. It does not. `folly::RelaxedConcurrentPriorityQueue`
is a Mound (Liu and Spear, ICPP 2012): a heap of sorted linked lists with a `folly::SpinLock` on every
node, hazard pointers for optimistic reads, heap-allocated nodes, unbounded size, and a "relaxed" pop
that hands out a shared batch of popped nodes. `folly::FlatCombiningPriorityQueue` combines under a
lock. Neither can trim to a capacity. The vendored `deps/folly` build from 2026-09-20 still links, so
the comparison below is measured, not argued. Folly's queue orders by value, so the packed word was
pushed with its key bits inverted to pop the smallest key first.

Apple M4, pushers and poppers in pairs, random keys, million operations per second:

| Pairs | PrioritySliceT capacity 256 | PrioritySliceT capacity 4096 | Folly relaxed |
| --- | ---: | ---: | ---: |
| 1 + 1 | 22 | 20 | 26 |
| 2 + 2 | 18 | 17 | 21 |
| 4 + 4 | 8 | 9 | 14 |
| 8 + 8 | 6 | 7 | 2.3 |

Pushers only, random keys, million pushes per second. PrioritySliceT trims to 256 and rejects in
constant time; Folly must allocate and insert every element because it cannot trim:

| Pushers | PrioritySliceT capacity 256 | Folly relaxed |
| --- | ---: | ---: |
| 1 | 785 | 0.7 |
| 4 | 2060 | 3.1 |
| 8 | 1600 | 5.5 |

Two defects surfaced while measuring this and are fixed: the free-slot search started at slot zero,
which made any bulk fill quadratic (a 4096-slot fill fell from 201 ns to 6 ns per push once the
search starts at the free hint and wraps), and a pop on an empty queue rescanned every slot before
believing the free count. Freed slots now also land in a four-entry ring in the header so a burst of
pops does not leave pushes hunting for slots.
