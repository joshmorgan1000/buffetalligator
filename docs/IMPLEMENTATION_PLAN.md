# BuffetAlligator: C Core Implementation Plan

Status: approved direction, 2026-09-07. This document is the single source of truth for the
work. Read all of section 1 before starting any task. Every task lists its files, exact
interfaces, invariants, acceptance checks, and pitfalls. Do the phases in order; do the tasks
within a phase in order. Do not start a task until the previous task's acceptance checks pass.

---

## 1. Ground rules for every task

### 1.1 What is being built

The arena core (placement registry, slabs, plates, claim, retain, release, view, novel buffers,
the worker thread, recycling, budgets, statistics, system probes) moves to pure C11 under
`src/core/`. The existing C++ public header `include/alligator.hpp` stays the only public API.
Its classes become thin wrappers over the C core. The library remains a static library.

### 1.2 Contracts that must not change

- `buffetalligator::Slice` stays exactly 16 bytes: `uint64_t meta_` then `void* cached_`.
  The bit layout of `meta_` stays: bits 0..16 registry slot, bits 17..63 byte size, all ones
  is the null slice. Every inline method on `Slice` keeps its body.
- Every public class, method, template, concept, and static function in `include/alligator.hpp`
  keeps its name and signature. Additions are allowed. Removals are not.
- `BuffetMenu::register_type(name, default_slab_size, bump_alignment, alligator, deallocate,
  get_host_ptr, get_context, set_as_default)` keeps its signature and keeps returning stable
  identifiers: `heap` is 0, `aligned_heap` is 1, the first custom placement is 2.
- `Placemat::Handle` stays `{ void* substrate_handle; void* context; }`.
- Two hard guarantees on every claimed `Slice`: the host pointer is at least 64-byte aligned,
  and every byte in the claim reads as zero at the moment the claim is returned.
- The three existing contract tests under `tests/` must pass unmodified except for the two
  include-line edits called out in Phase 4.

### 1.3 Behaviors that change on purpose (record these in the header NOTES block in P4-T6)

- Registering a placement whose base alignment is below 64 throws `AlligatorException`.
- A requested slab size is honored as given (rounded up to the OS granule). The old silent
  floor of 64 MiB is gone. A requested size of 0 means "derive from the machine".
- A claim throws `AlligatorException` when the placement's budget is exhausted. It never
  returns a null `Slice` for an internal failure.
- The `AtomicRegistry` global key `stop_signal` no longer exists. Shutdown is explicit through
  `BuffetMenu::shutdown()` and is never run from a static destructor.

### 1.4 House rules that apply to the C code exactly as to the C++ code

- 4-space indentation. No blank lines anywhere except one blank line after the include block.
- Every file, struct, function, and member gets the javadoc-style Doxygen banner with the
  dashes ending at column 110 for file and top-level scope, 100 for members of a struct, 90
  for anything nested deeper. Copy the existing banners in `src/memory/buffet.cpp` for shape.
- Comments are one sentence. They are visual markers, not documentation.
- No fallbacks. If a condition cannot be handled, return a status code and let the wrapper
  throw. Never quietly substitute a smaller size, a different placement, or a null result.
- No validation on the claim, retain, release, or view fast paths beyond what section 4
  explicitly lists. Pointers the core owns are trusted.
- No `std::function`, no lambdas, no C++ in `src/core/`. The C core includes no C++ header.
- The C core never logs and never prints. It returns `ba_status_t`. The C++ wrapper logs with
  `LOG_ERROR_STREAM` only on the path that throws.
- Do not restore deleted files from git. If a call site references something removed, fix the
  call site.
- Do not edit `run_build.sh`. Build with `./run_build.sh`. It runs ctest; every task ends with
  it green.
- No `memcpy` or `memset` outside the places section 4 names. Zeroing of recycled OS-page
  slabs is done by the kernel through remapping, never by a loop in our code.
- Numbers derived from this machine are samples, not targets. Sizes and depths come from the
  formulas in section 3 over values probed at runtime.

### 1.5 Definition of done for any task

1. `./run_build.sh` prints the green success line.
2. No new compiler warnings in `build/build_buffetalligator.log`.
3. The acceptance checks listed in the task pass.
4. Nothing outside the task's listed files changed, except tests the task adds.

---

## 2. Vocabulary

| Term | Meaning |
|---|---|
| Placement | One registered memory source (heap pages, a Vulkan memory type, a test allocator). Identified by a `uint16_t` type. Max 256 per process. |
| Slab | One allocation obtained from a placement. Carved into plates. Owned by the core. Recycled through the free list when every plate on it has died. |
| Plate | A contiguous region of a slab owned by one thread for bump allocation. Also the unit of reference counting. One registry slot per plate. |
| Direct plate | A plate created for a single claim too large for a thread plate but smaller than a slab. Sealed at creation with one issued claim. |
| Novel plate | A plate whose memory is its own placement allocation. Created for claims at least as large as the slab size, or when the caller asks for a novel buffer. |
| Slot | Index into the static plate table. 17 bits. Carried in `meta` bits 0..16 of every slice. |
| Seal | The owner thread publishing a plate's final issued count and giving up bumping on it. |
| Retire | The moment a plate's outstanding references reach zero. Slab plates return their slab reference; novel plates go to the worker for caching or deallocation. |
| Runway | Prepared slabs for a placement waiting to become current. Built by the worker. |
| Free list | Slabs that have been fully retired and re-zeroed, held by the worker for reuse. |
| Worker | The single background thread that builds runway slabs, re-zeroes retired slabs, tears down novel plates, trims caches, and polls memory pressure. |
| Granule | The alignment and size multiple for slabs: the large page size when the OS reports one, else the OS page size. |

---

## 3. Formulas (all inputs probed at runtime, see `ba_sysinfo_t`)

Shape ratios below live in one struct `ba_tunables_t` with these defaults. They are not
hardware constants; they are the policy knobs, kept in one place so they can be tuned later.

| Symbol | Meaning | Default |
|---|---|---|
| `headroom_ratio` | Fraction of headroom an OS-page placement may hold | 3/4 |
| `claims_per_plate` | Target claims per thread plate | 256 |
| `plates_per_thread` | Slab must hold this many plates per hardware thread | 4 |
| `slab_budget_divisor` | A slab is never larger than budget divided by this | 16 |
| `runway_floor` | Minimum prepared slabs per placement | 1 |
| `pressure_poll_ms` | Worker poll period for pressure and trims | 50 |
| `ewma_shift` | EWMA weight is 1/2^shift | 3 |

Definitions:

```
page        = OS page size
large_page  = transparent/large page size when the OS reports one, else 0
granule     = large_page ? large_page : page
hw_threads  = online processors limited by the process affinity mask when the OS exposes one
physical    = physical RAM bytes
available   = OS-reported available bytes at probe time
limit       = min over present values of { physical, cgroup memory limit, RLIMIT_AS,
              RLIMIT_DATA, job object process memory limit }; absent values are skipped
rss         = resident set size at probe time
headroom    = min(available, limit - rss)
```

Per placement, when the descriptor field is 0:

```
budget           = headroom * headroom_ratio                       (OS-page placements)
                 = query_available(context) if provided, else unlimited   (custom placements)
claim_ewma       = EWMA of rounded claim size, seeded with page, updated at every plate seal
                   from (bytes used on the plate) / (claims issued on the plate)
plate_bytes      = clamp(pow2_ceil(claim_ewma * claims_per_plate),
                         granule,
                         pow2_floor(budget / (hw_threads * 64)))
slab_bytes       = clamp(pow2_ceil(plate_bytes * hw_threads * plates_per_thread),
                         16 * granule,
                         pow2_floor(budget / slab_budget_divisor))
                   (a nonzero descriptor slab size wins, rounded up to a granule multiple;
                    plates on such a placement are additionally capped at slab_bytes / 8)
consume_rate     = EWMA of slab_bytes / (ns between the slab becoming current and the next
                   advance), updated at every advance
build_ns         = EWMA of the wall time the worker spends building one slab
runway_target    = clamp(ceil(consume_rate * build_ns / slab_bytes) + runway_floor + misses,
                         runway_floor,
                         (budget - live) / slab_bytes)
                   where misses is the count of runway misses in the last poll period
```

A global cap also applies to the sum over all OS-page placements: `os_budget = headroom *
headroom_ratio`. A slab build fails with `BA_E_BUDGET` when either the placement or the OS
sum would exceed its budget after the build.

Pressure response, evaluated by the worker every `pressure_poll_ms`:

| Level | Source | Action |
|---|---|---|
| NONE | default | none |
| WARN | Linux PSI `some avg10 > 10`; macOS dispatch WARN; Windows low-memory notification | trim every free list to one slab, trim novel caches to half their budget |
| CRITICAL | Linux PSI `full avg10 > 5`; macOS dispatch CRITICAL | trim every free list and novel cache to zero, runway_target forced to runway_floor until the level drops |

---

## 4. Core design reference

Agents implementing Phases 2 and 3 follow this section literally.

### 4.1 Slice word

```c
typedef struct ba_slice { uint64_t meta; void* ptr; } ba_slice_t;
#define BA_SLOT_BITS 17u
#define BA_SLOT_MASK ((1ull << BA_SLOT_BITS) - 1ull)
#define BA_NULL_META UINT64_MAX
/* meta = (byte_size << BA_SLOT_BITS) | slot */
```

`ba_slice_t` and `buffetalligator::Slice` have identical size, alignment, and field order. The
wrapper asserts this with `static_assert` and reinterprets between them.

### 4.2 Plate table and plate state word

```c
#define BA_PLATE_COUNT (1u << BA_SLOT_BITS)          /* 131072 */
#define BA_MAX_PLACEMENTS 256u
typedef struct ba_plate {
    _Atomic uint64_t state;         /* [63] sealed, [62..32] issued_total, [31..0] balance + BA_BALANCE_BIAS */
    struct ba_slab*  slab;          /* NULL for novel plates */
    ba_handle_t*     handle;        /* the slab's handle, or the novel plate's own handle */
    uint8_t*         base;
    uint64_t         bytes;
    uint32_t         placement;
    uint32_t         kind;          /* BA_PLATE_FREE, BA_PLATE_THREAD, BA_PLATE_DIRECT, BA_PLATE_NOVEL */
    uint32_t         next_free;     /* slot free-list link */
    uint32_t         reserved;
} ba_plate_t;                       /* exactly 64 bytes, alignas(64) */
#define BA_BALANCE_BIAS (1ull << 31)
#define BA_SEALED_BIT   (1ull << 63)
```

The table is a static array `ba_plate_t g_plates[BA_PLATE_COUNT]` in BSS (8 MiB, pages are
committed only when touched). There is no pointer table. A slot converts to a plate by
`&g_plates[slot]`.

State word rules. These are the whole correctness argument for reference counting; implement
them exactly.

- The low 32 bits hold `balance = releases - retains + BA_BALANCE_BIAS`. The bias keeps a
  retain from borrowing into bit 32. Initial value of `state` for a thread plate is
  `BA_BALANCE_BIAS`.
- A release does `fetch_add(state, 1, acq_rel)`. A retain does `fetch_sub(state, 1, acq_rel)`.
  A retain never checks anything. A release checks the value it got back plus one.
- The owner seals a plate with `fetch_add(state, BA_SEALED_BIT | ((uint64_t)issued << 32),
  acq_rel)` and checks the value it got back plus that addend.
- A plate is dead exactly when `(v & BA_SEALED_BIT) != 0` and `(uint32_t)v == (uint32_t)((v >>
  32) & 0x7FFFFFFF) + BA_BALANCE_BIAS`. The single thread whose read-modify-write produced that
  value retires the plate. No other thread can observe it because a retain requires a live
  reference and a release requires the reference being released.
- Direct and novel plates are created already sealed with `issued_total = 1`: initial state is
  `BA_SEALED_BIT | (1ull << 32) | BA_BALANCE_BIAS`.

### 4.3 Thread-local plates

```c
typedef struct ba_tls_plate {
    uint8_t* cursor;
    uint8_t* limit;
    uint64_t slot;
    uint64_t issued;
} ba_tls_plate_t;                                       /* 32 bytes */
static _Thread_local ba_tls_plate_t g_tls[BA_MAX_PLACEMENTS];   /* zero-initialized */
```

Zero-initialized means `limit - cursor == 0`, so the first claim on any thread takes the slow
path naturally. There is no "initialized" flag and no null check.

### 4.4 Claim fast path (the only code on the claim path)

```c
ba_status_t ba_claim(uint32_t type, size_t bytes, uint32_t flags, ba_slice_t* out) {
    ba_tls_plate_t* plate = &g_tls[type];
    const size_t rounded = (bytes + 63u) & ~(size_t)63u;
    if (flags | ((size_t)(plate->limit - plate->cursor) < rounded)) {
        return ba_claim_slow(type, bytes, rounded, flags, out);
    }
    out->ptr = plate->cursor;
    out->meta = ((uint64_t)bytes << BA_SLOT_BITS) | plate->slot;
    plate->cursor += rounded;
    plate->issued += 1;
    return BA_OK;
}
```

One branch, zero atomics, one thread-local cache line. Claims larger than a plate never fit and
therefore route to the slow path by the same comparison; the slow path decides direct versus
novel. `flags` is nonzero only for `BA_CLAIM_NOVEL`.

### 4.5 Slow path

`ba_claim_slow(type, bytes, rounded, flags, out)`:

1. `p = &g_placements[type]`.
2. If `flags & BA_CLAIM_NOVEL` or `rounded >= p->slab_bytes`: return `ba_claim_novel`.
3. If `rounded > p->plate_bytes`: return `ba_claim_direct` (carve `rounded` bytes from the
   current slab as a direct plate, sealed with one issued claim; the thread plate is untouched).
4. Otherwise refill: if `plate->limit != NULL`, seal the thread plate (section 4.2), retire it
   if dead, update `claim_ewma` from `(cursor - base_of_plate) / issued`. Carve a new thread
   plate of `p->plate_bytes` from the current slab (section 4.6). Fill `g_tls[type]`. Serve the
   claim from it with the same four stores as the fast path.

Carving a plate, thread or direct, from the current slab:

```
loop:
    slab = load_acquire(p->current)
    if slab == BA_ADVANCING: yield; continue
    off  = fetch_add(slab->bump, plate_bytes, relaxed)
    if off + plate_bytes <= slab->bytes:
        fetch_add(slab->live_plates, 1, relaxed)
        slot = ba_slot_pop()            (lock-free stack, tagged head, section 4.7)
        init g_plates[slot] fields, store_release(state, initial)
        return slot, slab->base + off
    if cas(p->current, slab, BA_ADVANCING):     (winner)
        next = ba_runway_pop(p)               (SPSC ring, consumer side)
        if next == NULL: next = ba_slab_build(p) ; count a runway miss ; on error store slab back and return the error
        store_release(p->current, next)
        record consume_rate sample ; enqueue BA_ORDER_REPLENISH(type)
        if fetch_sub(slab->live_plates, 1, acq_rel) == 1: enqueue BA_ORDER_RETIRE_SLAB(slab)   (drop the open pin)
    continue
```

The open pin is a `live_plates` count of 1 that every slab holds while it is current. A slab
with zero live plates and no open pin is unreachable by any claimant and goes to the worker.

### 4.6 Slab

```c
typedef struct ba_slab {
    _Atomic uint64_t bump;
    _Atomic uint32_t live_plates;     /* plates carved and not retired, plus the open pin */
    uint32_t placement;
    uint64_t bytes;
    uint8_t* base;
    ba_handle_t* handle;
    void* context;
    struct ba_slab* next;             /* worker-private free-list link */
    uint64_t opened_ns;
} ba_slab_t;                          /* alignas(64), header allocated from the OS-page pool for slab headers */
```

Slab headers are allocated from a worker-private pool with a free list; never `malloc` per slab.

### 4.7 Slot free list

A Treiber stack of slot indices. Head is a `_Atomic uint64_t` holding `(tag << 32) | slot`,
with slot `0` reserved as the empty marker (slot 0 is never handed out; `g_plates[0]` stays
`BA_PLATE_FREE` forever). Push and pop are CAS loops; the tag increments on every push so pops
are ABA-safe. Initial population is done lazily: `next_fresh` is an atomic counter starting at
1; a pop from an empty stack takes `fetch_add(next_fresh, 1)` and fails with `BA_E_SLOTS` when
that reaches `BA_PLATE_COUNT`.

### 4.8 Release, retain, view

```c
void ba_release(ba_slice_t* s) {
    const uint64_t meta = s->meta;
    if (meta == BA_NULL_META) return;
    ba_plate_t* plate = &g_plates[meta & BA_SLOT_MASK];
    const uint64_t v = atomic_fetch_add_explicit(&plate->state, 1, memory_order_acq_rel) + 1;
    if ((v & BA_SEALED_BIT) && (uint32_t)v == (uint32_t)((v >> 32) & 0x7FFFFFFFu) + BA_BALANCE_BIAS) ba_plate_retire(plate);
    s->meta = BA_NULL_META;
    s->ptr = NULL;
}
void ba_retain(const ba_slice_t* s) {
    atomic_fetch_sub_explicit(&g_plates[s->meta & BA_SLOT_MASK].state, 1, memory_order_acq_rel);
}
ba_status_t ba_view(const ba_slice_t* s, size_t offset, size_t length, ba_slice_t* out);
```

`ba_view` keeps the existing contract checks because `slice_test.cpp` requires them: offset
beyond size is `BA_E_RANGE`, length beyond the remainder is `BA_E_RANGE`, `length == SIZE_MAX`
means the remainder, `length == 0` produces the null slice. On success it retains and writes
`out->meta = (length << BA_SLOT_BITS) | (s->meta & BA_SLOT_MASK)`, `out->ptr = s->ptr + offset`.

Retire:

- `BA_PLATE_THREAD` or `BA_PLATE_DIRECT`: `if fetch_sub(slab->live_plates, 1, acq_rel) == 1`
  enqueue `BA_ORDER_RETIRE_SLAB(slab)`. Then `kind = BA_PLATE_FREE` and push the slot.
- `BA_PLATE_NOVEL`: enqueue `BA_ORDER_RETIRE_NOVEL(slot)`. The worker frees the slot after it
  has cached or deallocated the memory.

### 4.9 Novel plates

`ba_claim_novel(type, bytes, rounded, out)`:

1. If the placement's `novel_cache_bytes` is nonzero, try `ba_novel_cache_pop(p, class)` where
   `class = pow2_ceil(rounded)`. A hit is already zeroed and already aligned; skip to step 3.
2. Call the placement's `alloc(class_or_rounded, context)` on the calling thread. The contract
   test requires novel allocation on the caller. Record `novel_bytes_allocated`.
3. Take a slot, fill the plate as `BA_PLATE_NOVEL` with initial state sealed and one issued,
   write `out->meta = (bytes << BA_SLOT_BITS) | slot`, `out->ptr = host pointer`.

The novel cache is one bounded MPMC ring per power-of-two class from `64 KiB` to `1 TiB`
(25 rings), capacity 16 entries each, plus one atomic byte counter per placement bounded by
`novel_cache_bytes`. Pop is done by claimants, push by the worker after re-zeroing.

### 4.10 Worker orders

```c
typedef enum { BA_ORDER_STOP, BA_ORDER_INIT_PLACEMENT, BA_ORDER_REPLENISH, BA_ORDER_RETIRE_SLAB, BA_ORDER_RETIRE_NOVEL } ba_order_kind_t;
typedef struct ba_order { uint32_t kind; uint32_t placement; uint64_t arg; } ba_order_t;
```

The order queue is a bounded MPMC ring (Vyukov sequence-number design) of 4096 entries, used
as multi-producer single-consumer. Producers never block; if the ring is full they spin with
yield, which only happens if the worker is dead. The worker spins on an empty ring for a bounded
number of iterations, then sets `sleeping = 1`, re-checks the ring, and waits on the OS event
with a timeout of `pressure_poll_ms`. A producer that observes `sleeping == 1` after its push
signals the event.

Worker handling:

- `INIT_PLACEMENT`: build `runway_floor + 1` slabs (one becomes current, the rest go to the
  runway), then signal the registering thread's completion event. Registration blocks on that
  event so the first claim after registration always finds a current slab and the contract test
  observes background allocation before the first claim returns.
- `REPLENISH`: pop slabs from the placement's free list into the runway until the runway holds
  `runway_target`; build new ones when the free list is empty and the budget allows.
- `RETIRE_SLAB`: re-zero (section 4.11), reset `bump = 0` and `live_plates = 0`, push on the
  free list. Then, if free list bytes plus runway bytes exceed the placement's reserve cap
  `(runway_target + 1) * slab_bytes`, pop from the free list and release to the placement.
- `RETIRE_NOVEL`: if caching is on and the class ring has room and the byte counter allows,
  re-zero and push; otherwise call the placement's `free`, record `novel_bytes_freed`. Then free
  the slot.
- `STOP`: drain the ring, release every free-list and runway slab to its placement, exit.

### 4.11 Zeroing

- The touched prefix is `touched = min(bump, bytes)`; `bump` can overshoot `bytes` because a
  losing carve adds to it before the slab is advanced.
- OS-page placements: `ba_os_rezero(base, round_up(touched, page))` remaps the touched prefix
  with fresh anonymous pages. Kernel zero-fill, RSS drops, no loop in our code.
- Custom placements: if the descriptor provides `zero(handle, offset, bytes, context)`, call
  it with `(0, touched)`. If it does not, `memset(host, 0, touched)` on the worker thread. This is
  the single permitted `memset` in the core besides the OS layer.
- Novel plates are re-zeroed the same way before entering the cache.
- Fresh slabs from OS pages are zero by construction. Fresh slabs from custom placements are
  zero because the placement's `alloc` contract requires zeroed memory (documented in the
  header today and unchanged).

### 4.12 Ordering summary

| Access | Order |
|---|---|
| `plate->state` retain, release, seal | acq_rel |
| `slab->bump` | relaxed |
| `slab->live_plates` carve | relaxed; drop is acq_rel |
| `p->current` store / load | release / acquire |
| runway ring producer tail / consumer head | release / acquire |
| slot stack CAS | acq_rel |
| stats counters | relaxed |

---

## 5. Phase 0: Baseline, leak fix, benchmark (C++ only)

### P0-T1: Benchmark executable

Files: `tests/bench_claim.cpp` (new), `tests/CMakeLists.txt` (add target, not a ctest test).

Steps:

1. Create `tests/bench_claim.cpp` measuring, in this order, each over one million operations
   of 1 KiB unless stated: single-thread claim and free; 4, 8, and 16 thread claim and free
   with per-thread means and a wall-clock aggregate; single-thread sub-slice view and drop;
   2000 novel claims of 1 MiB. Print mean ns, max us, and counts over 1 us, 10 us, 100 us,
   1 ms for each. After all runs, drop every slice, sleep 3 s, and print
   `Memory::total_allocations()`, `Memory::total_freed()`, and the process footprint.
2. Use `std::chrono::steady_clock` around each operation. Use `std::thread`. Footprint on
   macOS is `task_info(TASK_VM_INFO).phys_footprint`; on Linux read `/proc/self/statm`
   resident pages; guard each with the platform macro.
3. Add `add_executable(buffetalligator_bench bench_claim.cpp)` linked to `alligator::alligator`
   with `target_include_directories(... PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)`. Do not
   `add_test` it.

Acceptance: `./run_build.sh` green. `./build/current/tests/buffetalligator_bench` runs to
completion. Paste its output into the pull request description as the Phase 0 baseline.

Reference sample from 2026-09-07 on an M4 Max, 16 hardware threads, Apple clang 17, -O2:

| Run | Mean per op | Wall per op | Over 1 us | Max |
|---|---|---|---|---|
| 1 thread claim+free | 67 ns | 67 ns | 35 | 10.3 ms |
| 4 threads | 277 ns | 115 ns | 187 | 2.8 ms |
| 8 threads | 661 ns | 139 ns | 87,605 | 3.2 ms |
| 16 threads | 1,773 ns | 184 ns | 789,422 | 9.1 ms |
| view+drop | 16 ns | | | 23 us |
| 1 MiB novel | 4.9 us | | | 95 us |

Footprint after every slice was dropped: 4,426 MiB resident, 0 chain slabs freed.

### P0-T2: Fix the chain-slab reference leak in the existing C++ core

Files: `src/memory/buffet.cpp`.

Background: `Buffet::claim` takes a hold reference at the boundary-crossing branch
(`ref_count_.fetch_add(1)`) and never releases it. The hold is needed while the crossing thread
touches `pool_current_` and `pool_previous_`, because the next crossing thread on the successor
slab may demote this slab and delete its root pin concurrently.

Steps:

1. In `Buffet::claim(size_t)`, inside the `if (start_offset + size_they_ll_get >= size())`
   block, after the `if (current_pool.load(...) == this) { ... }` block, restructure the two
   exits so that the hold is released through `free()` after the result is computed:

```cpp
        Slice result;
        if (start_offset + size_they_ll_get == size()) {
            result = cold_->root.load(std::memory_order_acquire)->slice(start_offset, size_requested);
        } else {
            result = next()->claim(size_requested);
        }
        free();
        return result;
```

   `free()` decrements the count and runs teardown at zero, which is exactly the hold's
   release. Do not use a bare `fetch_sub`: if the hold is the last reference, teardown must run.
2. Remove nothing else. Do not touch `next()` or the runway loop; both are replaced in Phase 4.

Acceptance: add `tests/slab_lifecycle_test.cpp` and register it in `tests/CMakeLists.txt`:

1. Claim 1 KiB slices in a loop, dropping each immediately, until
   `Memory::placement_allocations(*Slice::default_placement())` has grown by at least 6 slab
   sizes. Read the slab size from the difference in `placement_allocations` across the first
   rollover.
2. Drop everything, then poll for up to 10 s until `Memory::placement_freed(...)` is at least
   `allocations - 7 * slab_bytes` (current, previous, and up to 5 runway slabs may stay pinned
   in the C++ core; Phase 4 tightens this bound).
3. Fail with a message naming the observed allocated and freed byte counts.

Rerun the benchmark. The "after 3 s" footprint must be under 1 GiB.

Pitfalls: `free()` sets `cold_` to null on the last reference, so compute `result` before
calling it. `Slice` copy-assignment adds a reference; use move assignment for `result`.

---

## 6. Phase 1: OS layer in C

### P1-T1: CMake C language support

Files: `CMakeLists.txt`.

Steps:

1. Change `LANGUAGES CXX` to `LANGUAGES C CXX`.
2. Add `set(CMAKE_C_STANDARD 11)`, `set(CMAKE_C_STANDARD_REQUIRED ON)`,
   `set(CMAKE_C_EXTENSIONS OFF)`.
3. Keep the existing warning flags; they apply to C sources through the same
   `target_compile_options`. On MSVC add `/std:c11 /experimental:c11atomics` for C sources
   using `$<$<COMPILE_LANGUAGE:C>:...>`.
4. Add `src/core/ba_os.c` to the `add_library` source list (file created in P1-T2).

Acceptance: `./run_build.sh` green with an empty `ba_os.c` containing only the file banner.

### P1-T2: `ba_os.h` / `ba_os.c`

Files: `src/core/ba_os.h`, `src/core/ba_os.c` (new). Private header, not installed.

Interface (implement every function; no stubs):

```c
typedef struct ba_sysinfo {
    uint64_t page;
    uint64_t large_page;         /* 0 when the OS reports none */
    uint64_t granule;
    uint32_t hw_threads;
    uint64_t physical;
    uint64_t available;
    uint64_t limit;              /* 0 when no limit is present */
    uint64_t rss;
} ba_sysinfo_t;
typedef enum { BA_PRESSURE_NONE, BA_PRESSURE_WARN, BA_PRESSURE_CRITICAL } ba_pressure_t;
void      ba_os_probe(ba_sysinfo_t* out);                 /* fills every field; refreshes available and rss on each call */
void*     ba_os_map(uint64_t bytes, uint64_t alignment, int want_large_pages);   /* zeroed, aligned; NULL on failure */
void      ba_os_unmap(void* base, uint64_t bytes);
int       ba_os_rezero(void* base, uint64_t bytes);       /* kernel zero-fill of the range, RSS released; 0 on success */
ba_pressure_t ba_os_pressure(void);
uint64_t  ba_os_now_ns(void);
typedef void (*ba_thread_fn)(void*);
int       ba_os_thread_start(ba_thread_fn fn, void* arg);  /* detached */
typedef struct ba_event ba_event_t;                       /* opaque, defined in ba_os.c */
void      ba_event_init(ba_event_t* e);
void      ba_event_wait(ba_event_t* e, uint64_t timeout_ns);
void      ba_event_signal(ba_event_t* e);
typedef void (*ba_tls_dtor)(void*);
int       ba_os_tls_key(ba_tls_dtor dtor);                /* registers a per-thread exit callback; 0 on success */
void      ba_os_tls_set(void* value);                     /* value handed to dtor at thread exit */
void      ba_os_yield(void);
```

Platform mapping:

| Function | macOS | Linux | Windows |
|---|---|---|---|
| page | `sysconf(_SC_PAGESIZE)` | same | `GetSystemInfo` |
| large_page | 0 | `/sys/kernel/mm/transparent_hugepage/hpage_pmd_size` if `enabled` is `always` or `madvise` | `GetLargePageMinimum` if one `MEM_LARGE_PAGES` probe map succeeds |
| hw_threads | `sysconf(_SC_NPROCESSORS_ONLN)` | count of `sched_getaffinity` bits | `GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)` |
| physical | `sysctl hw.memsize` | `sysconf(_SC_PHYS_PAGES) * page` | `GlobalMemoryStatusEx.ullTotalPhys` |
| available | `host_statistics64` free + inactive + speculative pages | `/proc/meminfo MemAvailable` | `ullAvailPhys` |
| limit | `getrlimit(RLIMIT_AS)`, `RLIMIT_DATA` | same plus cgroup v2 `memory.max` on the path from `/proc/self/cgroup`, cgroup v1 `memory.limit_in_bytes` | `QueryInformationJobObject` extended limit `ProcessMemoryLimit` plus rlimits absent |
| rss | `task_info(TASK_VM_INFO).phys_footprint` | `/proc/self/statm` resident | `GetProcessMemoryInfo.WorkingSetSize` |
| map | `mmap` anon private, over-map by `alignment` and unmap the head and tail to align | same, then `madvise(MADV_HUGEPAGE)` when `want_large_pages` and `large_page` | `VirtualAlloc` reserve+commit, `MEM_LARGE_PAGES` when asked and probed |
| rezero | `mmap` with `MAP_FIXED` over the range | same | `VirtualFree(MEM_DECOMMIT)` then `VirtualAlloc(MEM_COMMIT)` |
| pressure | `dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE)` on a private queue updating an atomic | parse `/proc/pressure/memory` on each call | `QueryMemoryResourceNotification` on a handle from `CreateMemoryResourceNotification` |
| now | `clock_gettime(CLOCK_MONOTONIC_RAW)` | `CLOCK_MONOTONIC` | `QueryPerformanceCounter` |
| thread | `pthread_create` + detach | same | `CreateThread` + close handle |
| event | `pthread_mutex` + `pthread_cond` | same | `SRWLOCK` + `CONDITION_VARIABLE` |
| tls key | `pthread_key_create` | same | `FlsAlloc` |

Acceptance: `tests/os_layer_test.cpp` registered in `tests/CMakeLists.txt`, C++ file that
includes `ba_os.h` inside `extern "C"`. Asserts: `page` is a power of two and at least 4096;
`granule` is a multiple of `page`; `hw_threads >= 1`; `physical > 0`; `available <= physical`;
`limit` is printed but not asserted, because an address-space rlimit can legitimately exceed
physical memory; `ba_os_map(4 * granule, granule, 0)` returns a pointer that is a multiple of
`granule` and whose first and last byte read zero; write a byte pattern across the block,
`ba_os_rezero` returns 0 and the block reads zero again; `ba_os_now_ns` strictly increases
across a 1 ms sleep; a thread started with `ba_os_thread_start` sets an atomic the main thread
observes within 1 s; a TLS destructor registered with `ba_os_tls_key` fires on that thread's
exit.

Pitfalls: `MAP_FIXED` over an existing private anonymous mapping is the documented POSIX way
to get fresh zero pages; do not use `madvise(MADV_FREE)`, whose contents are unspecified. The
macOS pressure source needs a dispatch queue created once in `ba_os_pressure` on first call
with a `pthread_once`. Never read `/proc/pressure/memory` on macOS; guard every file read
with `#if defined(__linux__)`.

---

## 7. Phase 2: C core without the worker

The old C++ core stays compiled and in use through this phase. The new C core is exercised by
its own tests only. Symbols do not collide because the C core uses the `ba_` prefix.

### P2-T1: `ba_core.h`

Files: `src/core/ba_core.h` (new). Add `src/core/ba_core.c` to CMake (created in P2-T2).

Contents, in this order: file banner, includes (`stdint.h`, `stddef.h`, `stdatomic.h`,
`ba_os.h`), the `#define`s and structs of sections 4.1, 4.2, 4.3, 4.6, 4.10, then:

```c
typedef enum ba_status {
    BA_OK = 0,
    BA_E_BUDGET,            /* the placement or the OS-page sum would exceed its budget */
    BA_E_SLOTS,             /* the 131072-entry plate table is exhausted */
    BA_E_PLACEMENT,         /* unknown type, or more than BA_MAX_PLACEMENTS registered */
    BA_E_ALIGNMENT,         /* descriptor base alignment below 64 */
    BA_E_RANGE,             /* view offset or length outside the parent */
    BA_E_ALLOC,             /* the placement's alloc returned NULL */
    BA_E_CLOSED             /* ba_shutdown has run */
} ba_status_t;
typedef struct ba_handle { void* substrate_handle; void* context; } ba_handle_t;
typedef ba_handle_t* (*ba_alloc_fn)(size_t bytes, void* context);
typedef void         (*ba_free_fn)(ba_handle_t* handle, void* context);
typedef void*        (*ba_host_ptr_fn)(ba_handle_t* handle);
typedef void*        (*ba_context_fn)(void);
typedef uint64_t     (*ba_query_fn)(void* context);
typedef void         (*ba_zero_fn)(ba_handle_t* handle, uint64_t offset, uint64_t bytes, void* context);
enum { BA_PLACEMENT_DEFAULT = 1u << 0, BA_PLACEMENT_OS_PAGES = 1u << 1, BA_PLACEMENT_LARGE_PAGES = 1u << 2 };
enum { BA_CLAIM_NOVEL = 1u << 0 };
typedef struct ba_placement_desc {
    uint32_t       struct_size;         /* sizeof(ba_placement_desc_t); rejects mismatched callers */
    uint32_t       flags;
    const char*    name;
    uint64_t       slab_bytes;          /* 0 = formula */
    uint32_t       base_alignment;      /* >= 64; OS-page placements report granule */
    uint32_t       reserved;
    uint64_t       budget_bytes;        /* 0 = formula */
    uint64_t       novel_cache_bytes;   /* 0 = no novel caching */
    ba_alloc_fn    alloc;               /* NULL for OS-page placements */
    ba_free_fn     free;
    ba_host_ptr_fn host_ptr;
    ba_context_fn  context;
    ba_query_fn    query_available;     /* optional */
    ba_zero_fn     zero;                /* optional */
} ba_placement_desc_t;
typedef struct ba_stats {
    uint64_t slab_bytes_allocated, slab_bytes_freed;
    uint64_t novel_bytes_allocated, novel_bytes_freed;
    uint64_t reserved_bytes;            /* free list plus runway */
    uint64_t plate_bytes_carved;        /* monotonic */
    uint64_t novel_cache_bytes;
    uint64_t budget_bytes;
    uint64_t slab_bytes, plate_bytes;
    uint32_t runway_target, runway_misses;
    uint32_t claim_ewma;                /* bytes */
    uint64_t build_ns_ewma;
} ba_stats_t;
typedef struct ba_tunables { uint32_t headroom_num, headroom_den, claims_per_plate, plates_per_thread, slab_budget_divisor, runway_floor, pressure_poll_ms, ewma_shift; } ba_tunables_t;
void        ba_init(void);                                  /* idempotent; also run from a load-time constructor */
ba_status_t ba_placement_register(const ba_placement_desc_t* desc, uint32_t* type_out);
uint32_t    ba_placement_count(void);
uint32_t    ba_placement_default(void);
const char* ba_placement_name(uint32_t type);
int         ba_placement_find(const char* name);            /* -1 when absent */
void        ba_placement_describe(uint32_t type, ba_placement_desc_t* copy_out);    /* copies the registered descriptor */
ba_status_t ba_claim(uint32_t type, size_t bytes, uint32_t flags, ba_slice_t* out);
void        ba_retain(const ba_slice_t* s);
void        ba_release(ba_slice_t* s);
ba_status_t ba_view(const ba_slice_t* s, size_t offset, size_t length, ba_slice_t* out);
uint32_t    ba_slice_placement(const ba_slice_t* s);
ba_handle_t* ba_slice_handle(const ba_slice_t* s);
void        ba_stats(uint32_t type, ba_stats_t* out);
void        ba_stats_total(ba_stats_t* out);
void        ba_sysinfo(ba_sysinfo_t* out);
ba_pressure_t ba_pressure(void);
void        ba_budget_set(uint32_t type, uint64_t bytes);
void        ba_trim(uint32_t type);                         /* synchronous: enqueue and wait */
void        ba_trim_all(void);
void        ba_tunables_get(ba_tunables_t* out);
void        ba_tunables_set(const ba_tunables_t* t);
void        ba_shutdown(void);
```

Acceptance: header compiles inside `extern "C"` from a C++ translation unit and as C. Build
green.

### P2-T2: Placement table, plate table, slot stack, slab build without the worker

Files: `src/core/ba_core.c` (new).

Steps:

1. Static state: `g_sys` (`ba_sysinfo_t`), `g_tunables`, `g_placements[BA_MAX_PLACEMENTS]`
   (each `alignas(64)`; hot read-only fields `slab_bytes`, `plate_bytes`, `type` first;
   `current` on its own line; stats counters `alignas(64)` each group), `g_placement_count`
   (`_Atomic uint32_t`), `g_default`, `g_plates[BA_PLATE_COUNT]`, `g_slot_head`,
   `g_slot_fresh`, `g_os_live_bytes`, `g_closed`.
2. `ba_init`: `ba_os_probe(&g_sys)`, compute `g_tunables` defaults, register the two built-in
   placements through `ba_placement_register` with `flags = BA_PLACEMENT_OS_PAGES` for `heap`
   and `BA_PLACEMENT_OS_PAGES | BA_PLACEMENT_LARGE_PAGES | BA_PLACEMENT_DEFAULT` for
   `aligned_heap`, `slab_bytes = 0`, `base_alignment = g_sys.granule`. Guard with a
   `pthread_once` equivalent implemented on `_Atomic int` state (0 fresh, 1 running, 2 done;
   spinners on 1 yield). Add a load-time constructor (`__attribute__((constructor))` on
   Clang and GCC, a `.CRT$XCU` entry on MSVC) that calls `ba_init`.
3. `ba_placement_register`: reject `struct_size` mismatch, alignment below 64, count at
   `BA_MAX_PLACEMENTS`, and `g_closed`. Assign `type = fetch_add(count, 1)`. Compute budget,
   plate and slab sizes from section 3 (descriptor values win when nonzero). Store the
   descriptor copy. For this phase, build one slab synchronously and store it as `current`
   (the worker takes over in Phase 3). Set `g_default` when the flag is set or when no default
   exists yet.
4. `ba_slab_build(p)`: check both budgets, then map (`ba_os_map(slab_bytes, granule,
   large_pages_flag)`) or call `alloc(slab_bytes, context())`. Take a slab header from the
   header pool. Record `slab_bytes_allocated`. Set `live_plates = 1`, `opened_ns = now`.
5. `ba_slab_release(slab)`: unmap or call `free`, record `slab_bytes_freed`, return the header
   to the pool. Used only by the worker in Phase 3 and by `ba_shutdown`.
6. Slot stack per section 4.7.
7. `ba_claim`, `ba_claim_slow`, `ba_claim_direct`, `ba_claim_novel` (cache always misses in
   this phase), `ba_retain`, `ba_release`, `ba_view`, `ba_slice_placement`, `ba_slice_handle`
   per section 4. Plate retire in this phase: slab plates drop `live_plates`; when it reaches
   zero, call `ba_slab_release` directly on the releasing thread (Phase 3 moves this to the
   worker). Novel plates call the placement `free` directly for the same reason.
8. Thread-exit hook: `ba_os_tls_key` registered in `ba_init`; the first slow-path call on a
   thread stores a non-NULL value with `ba_os_tls_set`; the destructor walks `g_tls[]` and
   seals every entry with `limit != NULL`.
9. `ba_stats`, `ba_stats_total`, `ba_sysinfo`, `ba_budget_set`, `ba_tunables_get/set`,
   `ba_placement_*` accessors. `ba_pressure`, `ba_trim`, `ba_trim_all`, `ba_shutdown` are
   implemented in Phase 3; in this phase they return `BA_PRESSURE_NONE` or do nothing, and the
   Phase 3 tasks replace them (this is the only place an incomplete function is allowed to
   land, because the phase boundary is a build boundary).

Acceptance: `tests/core_claim_test.cpp` registered in ctest, calling the C API directly:

1. `ba_placement_count() == 2` before any registration; names at 0 and 1 are `heap` and
   `aligned_heap`; default is 1.
2. Register a test placement with `aligned_alloc`-backed callbacks, `slab_bytes = 64 MiB`,
   alignment 64; type is 2.
3. For each of the three placements: 10,000 claims of sizes 1, 63, 64, 65, 1000, 4096, 65536,
   every returned pointer is a multiple of 64, every byte reads zero, `meta >> 17` equals the
   requested size, consecutive claims on one thread of 4096 then 1024 differ by exactly 4096.
4. A claim of `plate_bytes + 64` (read from `ba_stats`) returns a direct plate: its slot differs
   from the thread plate's slot and the thread plate's cursor is unchanged (probe by claiming
   64 bytes before and after and checking the distance is 64).
5. A claim of `slab_bytes` returns a novel plate whose handle differs from any slab handle
   and, on the test placement, was allocated on the calling thread.
6. Retain then release the same slice twice; the plate must not retire until the last release.
   Verify through `ba_stats` on the test placement: `slab_bytes_freed` stays 0 until the last
   release of every claim on the slab, then, in this phase only, rises by one slab after the
   last release.
7. Sixteen threads each claim and release 100,000 slices of 1 KiB concurrently; no crash;
   after join and after sealing (each thread exits), the test placement's live slab bytes,
   `slab_bytes_allocated - slab_bytes_freed`, equal exactly one slab (the current one).
8. Registering with alignment 16 returns `BA_E_ALIGNMENT`. A claim with a type equal to
   `ba_placement_count()` returns `BA_E_PLACEMENT`.

Pitfalls: the plate table lives in BSS; `g_plates[0]` is never used. The TLS array is indexed
by type without bounds checking; every type below `BA_MAX_PLACEMENTS` is in bounds, and a type
that is in bounds but not registered has an all-zero TLS entry, so it reaches the slow path,
which returns `BA_E_PLACEMENT` before touching any placement state. Callers never pass a type
at or above `BA_MAX_PLACEMENTS` because registration rejects it. Seal must publish `issued`
from TLS, not from the header.
`ba_view` with `offset == 0 && length == SIZE_MAX` is a plain retain.

---

## 8. Phase 3: Worker, recycling, runway, novel cache, budgets, pressure

### P3-T1: Order ring and worker thread

Files: `src/core/ba_worker.c` (new, add to CMake), `src/core/ba_core.c` (hook points),
`src/core/ba_core.h` (internal declarations under a `BA_INTERNAL` section).

Steps:

1. Implement the bounded MPMC ring of section 4.10 with 4096 `ba_order_t` cells, each cell
   carrying an `_Atomic uint64_t sequence`. Producers: claim a ticket with `fetch_add(enqueue_pos)`,
   spin until `cell.sequence == ticket`, write, publish `sequence = ticket + 1`. Consumer:
   mirror with `dequeue_pos` and `sequence == ticket + 1`.
2. `ba_worker_start` from `ba_init`: `ba_os_thread_start(ba_worker_main, NULL)`.
3. `ba_worker_main`: loop per section 4.10. Spin budget before sleeping: 4096 iterations of
   `ba_os_yield`. Timeout `pressure_poll_ms`.
4. `ba_enqueue(order)`: push, then `if load(sleeping) signal`.
5. Move slab release and novel free from the releasing thread (P2-T2 step 7) to
   `BA_ORDER_RETIRE_SLAB` and `BA_ORDER_RETIRE_NOVEL` handlers. In this task the handlers
   release immediately (no free list yet).
6. `INIT_PLACEMENT` handler and the registration-side completion event per section 4.10.
   `ba_placement_register` enqueues and waits.
7. `ba_shutdown`: set `g_closed`, enqueue `STOP`, wait on a completion event, then join
   semantics through the event (the worker signals after releasing everything).

Acceptance: `tests/core_worker_test.cpp` registered in ctest:

1. After registering a test placement, the placement's allocator was called on a thread other
   than the registering thread at least once before `ba_placement_register` returned.
2. Claim and drop across at least 6 slab rollovers, then poll `ba_stats` for up to 10 s:
   `slab_bytes_allocated - slab_bytes_freed <= (runway_target + 1) * slab_bytes`.
3. A novel claim of 8 MiB dropped on the main thread is freed by the placement callback on a
   different thread within 10 s.
4. `ba_shutdown` returns within 10 s; afterwards `ba_claim` returns `BA_E_CLOSED`.

### P3-T2: Runway and free list

Files: `src/core/ba_worker.c`, `src/core/ba_core.c`.

Steps:

1. Per placement: SPSC runway ring of 64 slab pointers (`_Atomic uint32_t head, tail`),
   worker-private free list (`ba_slab_t* free_head` plus `free_bytes`), `runway_count`.
2. Advance protocol in `ba_claim_slow` per section 4.5 using `ba_runway_pop`; on a miss build
   synchronously and `fetch_add(runway_misses, 1)`.
3. `REPLENISH` handler: compute `runway_target` per section 3; move from free list, then build,
   until the runway holds `runway_target` or the budget stops it.
4. `RETIRE_SLAB` handler: re-zero (section 4.11), reset, push on the free list; then apply the
   reserve cap.
5. Consume-rate and build-latency EWMAs; `runway_misses` reset each poll period.

Acceptance: extend `core_worker_test.cpp`:

1. After the rollover loop from P3-T1 check 2 and a 1 s settle, `reserved_bytes` equals
   `runway_count * slab_bytes + free_bytes` and is at most `(runway_target + 1) * slab_bytes`.
2. Every slab handed out after the first rollover on the test placement reads zero on its
   first 4 KiB and on the 4 KiB before its end (probe by claiming after each rollover).
3. With 16 threads claiming 1 KiB continuously for 2 s, `runway_misses` at the end is at most
   `hw_threads` (misses can occur only during the first advance per thread group).

### P3-T3: Novel cache, budgets, pressure, trim

Files: `src/core/ba_worker.c`, `src/core/ba_core.c`.

Steps:

1. Novel cache per section 4.9: 25 MPMC rings per placement, allocated lazily on the first
   novel retire of a placement with `novel_cache_bytes != 0`.
2. `RETIRE_NOVEL` handler: cache when allowed, else free.
3. Budget checks in `ba_slab_build` and in the novel allocation path; `ba_budget_set` updates
   the placement budget and enqueues `REPLENISH` so the reserve cap is re-evaluated.
4. Pressure poll each timeout; actions per section 3.
5. `ba_trim(type)` enqueues a `REPLENISH` with `arg = 1` meaning "trim to zero reserve" and
   waits on a completion event; `ba_trim_all` does it for every placement.

Acceptance: extend `core_worker_test.cpp`:

1. Register a placement with `novel_cache_bytes = 64 MiB`; two consecutive 8 MiB novel claims
   with a drop in between call the placement allocator once; the second claim reads zero.
2. `ba_budget_set(type, 2 * slab_bytes)` then claiming through a third slab returns
   `BA_E_BUDGET` from `ba_claim` (the test placement, so no OS budget interferes).
3. `ba_trim(type)` after the rollover loop leaves `reserved_bytes == 0` and
   `slab_bytes_freed` increased by the previously reserved bytes.
4. `ba_pressure()` returns a value in the enum; on Linux with `/proc/pressure/memory` present
   the call succeeds; elsewhere it returns `BA_PRESSURE_NONE` without touching the filesystem.

---

## 9. Phase 4: Wrapper swap

After this phase the old C++ core is gone. Do the tasks in order and build after each one.

### P4-T1: `Placemat` and `BuffetMenu` over the C core

Files: `include/alligator.hpp`, `src/memory/heapbuffet.cpp` (rewritten), `CMakeLists.txt`.

Steps:

1. In `Placemat`, append one private member after `default_slab_size_`: `uint32_t core_type_ =
   0;`. Do not reorder existing members. Existing accessors keep reading the existing members;
   the wrapper fills them from the registered descriptor.
2. `BuffetMenu::register_type_unchecked`: build a `ba_placement_desc_t` with `struct_size`,
   `name`, `slab_bytes = default_slab_size`, `base_alignment = bump_alignment`, `flags =
   set_as_default ? BA_PLACEMENT_DEFAULT : 0`, the four callbacks reinterpret-cast to the C
   types after `static_assert`s that `sizeof(Placemat::Handle) == sizeof(ba_handle_t)` and the
   two field offsets match, `novel_cache_bytes = 0`, `query_available = nullptr`, `zero =
   nullptr`. Call `ba_placement_register`; on non-OK status throw `AlligatorException` with the
   status name and the placement name. Create the `Placemat` wrapper, store `core_type_`, keep
   `placements_` and `placement_indices_` as today, notify listeners.
3. `BuffetMenu::ensure_builtins_slow`: call `ba_init()`, then create wrapper `Placemat` objects
   for core types 0 and 1 by reading their descriptors with `ba_placement_describe`, so the
   wrapper table mirrors the core table one to one. Remove the old built-in allocator functions
   from `heapbuffet.cpp`; the file now holds only this function.
4. `BuffetMenu::default_placement()` returns the wrapper for `ba_placement_default()`.
5. Add `static void BuffetMenu::shutdown()` calling `ba_shutdown()`.
6. Remove `include <thread>` uses that only served the old spin; keep the header's include
   list otherwise untouched (consumers may rely on transitive includes).

Acceptance: build green; `buffetalligator_builtin_default_test` passes.

### P4-T2: `Slice` over the C core

Files: `src/memory/slice.cpp` (rewritten).

Mapping, one function each, no other logic:

| Wrapper | Core |
|---|---|
| `Slice(size_t, const Placemat*)` | `ba_claim(type, size, 0, self)`; throw on status |
| `Slice(size_t, bool novel, const Placemat*)` | `ba_claim(type, size, novel ? BA_CLAIM_NOVEL : 0, self)` |
| `Slice(const void*, size_t, bool, const Placemat*)` | null or zero size: stay null; else claim as above then `memcpy` (this is one of the two permitted copies) |
| copy ctor / copy assign | `ba_retain` on the source, then copy the two words; assign releases the old value first |
| move ctor / move assign | two-word copy and null the source; assign releases the old value first |
| `free()` | `ba_release(self)` |
| `slice(offset, length)` | `ba_view`; `BA_E_RANGE` throws with the existing messages `Slice::slice: offset exceeds slice size` and `Slice::slice: length exceeds slice size` |
| `placement()` | null slice: `nullptr`; else `BuffetMenu::get(ba_slice_placement(self))` |
| `resize(...)` | same algorithm as today expressed over `slice`, the claim constructor, and one `memcpy` (the second permitted copy) |
| `default_placement()` | `BuffetMenu::default_placement()` |
| `Placemat::get_for(slice)` | `ba_slice_handle(self)` |

`self` is `reinterpret_cast<ba_slice_t*>(this)` after a file-scope `static_assert` on size,
alignment, and both member offsets. Throwing: build the message with the status name and the
placement name, `LOG_ERROR_STREAM` it, then `ALLIGATOR_THROW`.

Acceptance: `buffetalligator_slice_test` and `buffetalligator_builtin_default_test` pass.

### P4-T3: Public `Memory` class

Files: `include/alligator.hpp` (add the class after `BuffetMenu`), `src/memory/tracker.hpp`
(delete), `tests/memory_test.cpp` (change `#include <memory/tracker.hpp>` to nothing, since
`alligator.hpp` now provides it; remove `#include <memory/slicefriend.hpp>`),
`tests/slab_lifecycle_test.cpp` (same include edit), `tests/CMakeLists.txt` (drop the
`../src` include directory from `buffetalligator_memory_test`).

Interface (all static, all thin calls into `ba_stats`, `ba_stats_total`, `ba_sysinfo`,
`ba_budget_set`, `ba_trim`, `ba_pressure`):

```cpp
class Memory {
public:
    static size_t total_allocations();                         // slab + novel bytes ever obtained from placements
    static size_t total_freed();                               // slab + novel bytes ever returned
    static size_t placement_allocations(const Placemat&);
    static size_t placement_freed(const Placemat&);
    static size_t placement_usage(const Placemat&);            // allocations - freed
    static size_t placement_reserved(const Placemat&);         // free list + runway
    static size_t placement_live(const Placemat&);             // usage - reserved
    static size_t placement_budget(const Placemat&);
    static size_t placement_available(const Placemat&);        // budget - live, or SIZE_MAX when unlimited
    static void   set_placement_budget(const Placemat&, size_t bytes);
    static size_t placement_novel_cached(const Placemat&);
    static void   trim(const Placemat&);
    static void   trim_all();
    static size_t system_physical();
    static size_t system_available();
    static size_t system_limit();                              // 0 when none
    static size_t system_headroom();
    static size_t page_size();
    static size_t large_page_size();                           // 0 when none
    static unsigned hardware_threads();
    enum class Pressure { None, Warn, Critical };
    static Pressure pressure();
    Memory() = delete;
};
```

Acceptance: `buffetalligator_memory_test` passes unmodified except the include edits.
`slab_lifecycle_test` passes with its bound tightened to `(runway_target + 1) * slab` where
`runway_target` is read through a new `Memory::placement_runway_target(const Placemat&)`; add
that accessor too.

### P4-T4: Delete the old core

Files: delete `src/memory/alligator.hpp`, `src/memory/alligator.cpp`, `src/memory/buffet.hpp`,
`src/memory/buffet.cpp`, `src/memory/slicefriend.hpp`. Remove them from `CMakeLists.txt`.
The `SliceMap::find_internal` and `SliceMap::get_slice_internal` definitions and
`Placemat::get_for` live in `slice.cpp` today and must survive in the rewritten `slice.cpp`
because `SliceMap` is public. `SliceFriend::do_somthing_fun` is deleted together with
`slicefriend.hpp`. Keep `src/simd.hpp`; `SliceMap` still needs it.

Acceptance: build green, all tests pass, `grep -rn "Alligator::instance\|Buffet\b\|SliceFriend"
src include tests` returns only the `class Alligator; class Buffet;` forward declarations in the
public header (leave those; removing a forward declaration is an API change) and the `Buffet`
friend lines inside `Slice` and `Placemat` (leave those for the same reason).

### P4-T5: Remaining public-header fixes

Files: `include/alligator.hpp`.

1. `WeakSlice::slice`: the destination pointer must be `new_slice.data<uint8_t>()` without the
   `+ offset`.
2. `SliceMap` hazard pool: replace the fixed 256-row static array with a pool sized at first use
   to `4 * Memory::hardware_threads()` rows, allocated as a `Slice` from the default placement
   and leaked for the process lifetime. `hazard_claim_row` throws `AlligatorException` when the
   pool is exhausted instead of indexing past it.
3. `SliceT<T>::length()`: the `std::vector` branch must be guarded by `if constexpr (requires {
   typename T::value_type; })` before the `is_same_v` test so `SliceT<int>::length()` compiles.
4. `SliceT<T>::get_as()`: the `std::string_view` and `std::span` branches return by value from
   a function declared to return a reference; change the two specializations to a separate
   `template<typename U = T> U view() const` method and delete the branches from `get_as()`.
   `get_as()` keeps its signature and returns `*reinterpret_cast<U*>(raw())` unconditionally.

Acceptance: add `tests/header_fixes_test.cpp`: `WeakSlice` over a 256-byte buffer,
`slice(64, 128)` copies bytes 64..191 into a 128-byte slice with no write beyond it (verify by
allocating the destination through a `Slice(256)` sibling and checking the sibling's bytes are
untouched); `SliceT<int>` `length()` returns 4 for a 16-byte slice; `SliceT<std::string_view>`
`view()` returns a view of the right length; `SliceMap(8)` publish and find from
`2 * hardware_threads()` threads concurrently.

### P4-T6: Header NOTES block

Files: `include/alligator.hpp`.

Add a `NOTES:` section to the top `@file` comment with one dated entry per item in section 1.3,
each with `WHY:` and `CHANGE:` lines, signed with the implementing model's name and version.

Acceptance: the block follows the format in `CLAUDE.md` section 5. Build green.

---

## 10. Phase 5: Resource awareness through the public API

### P5-T1: Placement descriptor overload

Files: `include/alligator.hpp`, `src/memory/heapbuffet.cpp`.

Add a second `BuffetMenu::register_type` overload taking a public struct:

```cpp
struct PlacementDescription {
    const char* name = nullptr;
    size_t slab_bytes = 0;
    size_t base_alignment = 64;
    size_t budget_bytes = 0;
    size_t novel_cache_bytes = 0;
    bool set_as_default = false;
    Placemat::Handle* (*allocate)(size_t, void*) = nullptr;
    void (*deallocate)(Placemat::Handle*, void*) = nullptr;
    void* (*get_host_ptr)(Placemat::Handle*) = nullptr;
    void* (*get_context)() = nullptr;
    uint64_t (*query_available)(void*) = nullptr;
    void (*zero)(Placemat::Handle*, uint64_t, uint64_t, void*) = nullptr;
};
static uint16_t register_type(const PlacementDescription& description);
```

The old overload forwards to this one with the extra fields left at their defaults.

Acceptance: `tests/resource_test.cpp` registered in ctest: register through the new overload
with `novel_cache_bytes = 64 MiB` and a counting allocator; two consecutive 8 MiB novel
`Slice(8 MiB, true, placement)` claims with a drop between call the allocator once and the
second reads zero; `Memory::set_placement_budget(p, 2 * slab)` then claiming through a third
slab throws `AlligatorException`; `Memory::system_physical() > 0`; `Memory::page_size()` is a
power of two; `Memory::hardware_threads() >= 1`; `Memory::trim(p)` leaves
`placement_reserved(p) == 0`.

### P5-T2: README

Files: `README.md`.

Rewrite the Placemat section to describe plates, the worker, runway, the free list, novel
caching, budgets, pressure, and the two hard guarantees. Remove every sentence that describes
the old behavior (256 MiB startup commit, 64 MiB slabs as a constant). Add a "Memory API"
section listing the `Memory` statics and the descriptor overload. Keep the Build and Install
sections as they are.

Acceptance: every claim in the README is checked against the code by the implementing agent
and cited in the pull request description with the function that implements it.

---

## 11. Phase 6: Benchmark gate

### P6-T1: Compare against the Phase 0 baseline

Steps:

1. Run `buffetalligator_bench` three times; take the median of each row.
2. Required outcomes relative to the Phase 0 table on the same machine: 1-thread mean at most
   one third of baseline; 16-thread wall per op lower than the 1-thread baseline; no claim over
   1 ms in any run; footprint after the 3 s settle at most `(runway_target + 2) * slab_bytes`
   per OS-page placement plus the plate table.
3. If any outcome fails, profile with `xctrace` or `perf` before changing anything, and record
   the finding in the pull request. Do not lower the target.

### P6-T2: Cleanup

Delete `docs/IMPLEMENTATION_PLAN.md` references to phase-only scaffolding: none should exist
in code. Confirm `grep -rn "TODO\|FIXME\|Phase [0-9]" src include tests` is empty.

---

## 12. Task index

| ID | Title | Depends on | Files |
|---|---|---|---|
| P0-T1 | Benchmark executable | none | tests/bench_claim.cpp, tests/CMakeLists.txt |
| P0-T2 | Fix chain-slab leak in C++ core | P0-T1 | src/memory/buffet.cpp, tests/slab_lifecycle_test.cpp |
| P1-T1 | CMake C language | P0-T2 | CMakeLists.txt |
| P1-T2 | OS layer | P1-T1 | src/core/ba_os.h, src/core/ba_os.c, tests/os_layer_test.cpp |
| P2-T1 | Core header | P1-T2 | src/core/ba_core.h |
| P2-T2 | Core without worker | P2-T1 | src/core/ba_core.c, tests/core_claim_test.cpp |
| P3-T1 | Order ring and worker | P2-T2 | src/core/ba_worker.c, tests/core_worker_test.cpp |
| P3-T2 | Runway and free list | P3-T1 | src/core/ba_worker.c, src/core/ba_core.c |
| P3-T3 | Novel cache, budgets, pressure, trim | P3-T2 | same |
| P4-T1 | Placemat and BuffetMenu wrapper | P3-T3 | include/alligator.hpp, src/memory/heapbuffet.cpp |
| P4-T2 | Slice wrapper | P4-T1 | src/memory/slice.cpp |
| P4-T3 | Public Memory class | P4-T2 | include/alligator.hpp, tests |
| P4-T4 | Delete old core | P4-T3 | src/memory/*, CMakeLists.txt |
| P4-T5 | Header fixes | P4-T4 | include/alligator.hpp, tests/header_fixes_test.cpp |
| P4-T6 | NOTES block | P4-T5 | include/alligator.hpp |
| P5-T1 | Descriptor overload | P4-T6 | include/alligator.hpp, tests/resource_test.cpp |
| P5-T2 | README | P5-T1 | README.md |
| P6-T1 | Benchmark gate | P5-T2 | none |
| P6-T2 | Cleanup | P6-T1 | none |
