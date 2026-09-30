# GPU Backends, Slice Fixes, and the easygpu.hpp Contract

*Written 2026-09-30 against branch `slice-efficiency` at a0546af plus the uncommitted work in
`include/alligator.hpp`, `include/alligator/kitchen.hpp`, `src/kitchen/kitchen.cpp`,
`src/memory/chainbuffet.cpp`, and `src/memory/slice.cpp`.*

*Reviewed and updated 2026-09-30 against the current working files; implementation remains in
progress, so recheck symbols and ownership before each phase instead of relying on line numbers.*

## Context

The savepoint split `alligator.hpp` into `include/alligator/*.hpp` and replaced Plate/Placemat with
`BuffetDescriptor` + `ChainBuffet` + `SliceEntry` + 64 `Region`s. The GPU side has not followed:

- **The complete library and test suite do not build yet.**
  - `vulkan.cpp` defines `Shader::operator()` returning `shared_ptr<LightweightSemaphore>`, while
    `easygpu.hpp:71,85` declares `std::binary_semaphore`.
  - `vulkankernel.cpp` includes only `<alligator.hpp>`, so `VulkanKernel` is undeclared there. It
    also calls the removed `Alligator::gpu_table_address()`.
  - GPU tests still use removed API (`Placemat`, `SliceId`, `plate_for`, `gpu_table()`).
- **Only Vulkan exists as a compute backend.** `metal_allocator.mm` and `cuda_allocator.cpp` are in
  no CMake target, and neither implements `Shader` or `GPU`.
- **The GPU cannot resolve a Slice.**
  - GLSL indexes one flat table as `refs[s.id]`, but the host stores records per region at
    `regions[(id>>3)&63]->gpu_slots[id>>9]`.
  - `ChainBuffet::claim` writes `GPUBuf` size and offset in 64-byte granules, but every reader
    treats them as bytes.

Outcome: one `Shader` that speaks natively to Vulkan, Metal, and CUDA, uses GLSL as the portable
fallback source, completes through callbacks and `co_await` instead of semaphores, and addresses
Slices correctly on every backend.

### Decisions settled with Josh (2026-09-30)

1. **GPUBuf units are 64-byte granules.** Keep the granule representation and shift readers left
   by 6; this does not exempt ownership defects in `claim` from correction.
   The largest representable length is `(uint64_t{UINT32_MAX} << 6)` = 256 GiB minus 64 bytes, subject to
   the selected backend's smaller allocation limits; a represented byte length is a multiple of 64.
2. **Completion is callback-based, with `co_await` support.** Semaphores leave the GPU API.
3. **The Shader speaks natively to Metal and CUDA.**
   - `ShaderSource` carries `glsl`, `metal`, and `cuda` members.
   - When the active backend's member is unset, the GLSL body is compiled for that backend.
4. **GLSL fallback per backend:**
   - Vulkan: shaderc → SPIR-V (as today).
   - Metal: shaderc → SPIR-V → SPIRV-Cross MSL, run natively. SPIRV-Cross is already vendored in
     `deps/src/MoltenVK/External`.
   - CUDA: Slang's GLSL input mode → PTX → `cuModuleLoadData`; use the spelling supported by the
     pinned Slang release, since current documentation deprecates `-allow-glsl`.

### Subsequent implementation decisions

- Josh authorized the Slice and Shader handoff during execution on 2026-09-30.
- Josh explicitly rejected separate exact-byte metadata to preserve the smaller memory footprint.
  Size and offset remain `uint32_t` counts of 64-byte granules in the 16-byte GPUBuf record.
- Josh explicitly requested full live build output; do not suppress dependency or compiler output.
- Allocation lengths round up to granules, and sub-slice ranges round outward: the start rounds
  down and the end rounds up to a 64-byte boundary, within the represented parent range.
  Public byte-size accessors return the represented granule length; no residual or logical-length
  side record is stored.

### Contract gates before implementation

- **Granules versus byte views (settled during execution).** Update existing arbitrary-byte
  tests, networking, and mmap call sites to the whole-granule contract above; the represented
  Slice range includes its rounded bytes, and a sub-view must remain within its parent.
- **Initialization and address domain.** Freeze the active backend, device/context, and metadata
  placement before region 0 is allocated, including when the first operation is a CPU Slice claim.
  One GPUBuf address belongs to one backend/device; simultaneous Vulkan/native interoperability
  needs an explicit sharing design and is outside this plan's single-active-backend contract.
- **Asynchronous ownership and errors.** Settle resource retention, admission under saturation,
  cancellation, and terminal-error delivery before publishing the callback/awaiter API below.
  Callback-based completion and `co_await` remain the settled mechanisms.
- **Correctness precedes performance baselines.** A broken build or incorrect allocator is not a
  usable speed baseline; establish the first passing current-source integration baseline.

### Ownership rules for this work

- **Slice handoff completed during execution.** Josh authorized the listed Slice, ChainBuffet,
  header, and kitchen corrections after the initial independent fixes and build audit.
- **Code style:**
  - Bodies go in `.cpp`/`.mm`.
  - No new wrapper or helper methods.
  - No lambdas or `std::function`; completion uses static functions plus a `void*` context.
- **Public repo hygiene.** This repository is public. Commit messages, comments, and test names
  describe everything in alligator terms only.

---

## Phase 0: Core Slice defects (report to Josh; he decides)

Resolve these before GPU integration; update status against Josh's completed port. The earlier
review's missing-return and discarded-entry findings have changed in the current working files.

### Inside `ChainBuffet::claim` (`src/memory/chainbuffet.cpp`): propose, do not edit

| # | Current status / defect | Failure | Required action after handoff |
|---|---|---|---|
| C-1 | Return migration is now present: declaration and definition return `Slice`, and all successful branches return `Slice(slice_id)` | The old proposal to return a pair is stale | Keep the current return contract; compile its callers and test null, normal, exact-slab, oversized, and novel claims. |
| C-2 | Both dedicated branches now return, but neither releases the exchanged token wrapper | `SliceEntry::set` takes another reference, so the abandoned wrapper pins the allocation indefinitely | Release the exchanged wrapper after the entry owns its reference, in both novel and oversized paths; also unwind allocations and reserved entries if publication throws. |
| C-3 | The novel and oversize branches call `descriptor->device_address(buffer_)` | Records the *shared chain slab's* address for a dedicated buffer, so the GPU reads the wrong memory | `descriptor->device_address(n->buffer_)` |
| C-4 | Slab-retirement race | A successful reservation can race token removal before its SliceEntry owns a reference | Make the allocator's own in-progress claim retain ownership until SliceEntry publication completes; acquisition must occur while the token is still owned, because loading a raw token then incrementing it can race deletion. Use the existing ownership model where possible; no additional reader-tracking scheme is prescribed. |
| C-5 | Internally owned nodes are not reclaimed | `ChainBuffetToken::free` releases the buffet and tuple but not the node; unconditional `delete owner` also fails for caller-owned/stack nodes | Specify who owns each node and release internally allocated nodes when no internal chain ownership or in-progress claim still needs them; account for dedicated nodes and prepared successors without tracking caller-borrowed raw pointers. |

**Caller raw-pointer contract (clarified by Josh, 2026-09-30):** a borrowed raw pointer is valid
only while its owning Slice/buffer remains alive. Keeping that pointer after ownership ends is
the caller's error; the allocator does not discover, count, or protect those pointers. C-4 covers
only the library's own claim operation before it hands off a Slice. If those claims hold safely
acquired counted ownership, zero references can establish safe reclamation without another
reader-tracking mechanism.

### Adjacent code in files Josh is editing: propose after his port lands

- **`current_for` and `next()` sentinels** (`chainbuffet.cpp`, `-1` sentinel before `factory`).
  - If the factory throws (for example, a Vulkan OOM), the sentinel stays and every other claimer
    spins in `yield()` forever.
  - Proposed edit: return the sentinel to an explicit retryable/failed state, wake any waiters,
    and rethrow; apply the same exception cleanup to `Alligator::next_id` region publication.
    Release partially allocated resources and test a failure followed by a successful retry.
- **`current_for` silently remaps an unregistered index to heap** (`idx = 0`), while `next_id`
  still encodes the caller's type. If a slot is empty below `count()`, `factory` dereferences
  null.
- **`Slice(size_t, placement)` is implicit** (`alligator.hpp`).
  - Stale `Alligator::inst().gpubuf(id_)` calls previously selected a temporary size-constructed
    Slice instead of resolving the supplied id; implicit conversion must not hide unported calls.
  - The in-flight header now also has private `Slice(uint32_t)`, which competes with size
    construction and is not a distinct signature from `size_t` on every target.
  - Proposed edit: make public size construction explicit and keep raw-id adoption unambiguous
    (for example, a private tag constructor); port all stale id call sites to the chosen internal
    contract without relying on implicit conversion or restoring removed APIs.
- **`Slice()` claims a zero-byte entry**, because the default argument is `size = 0`.
  - Every default-constructed Slice, `SliceT<T>` member, and `return Slice();` burns a slot.
  - Proposed edit: default-construct to the null id `0xFFFFFFFF`.
  - `Slice::free()` must treat the null id as a no-op, since moved-from Slices are destroyed
    constantly. Today `Alligator::entry`/`gpubuf`/`destroy` index `slots[0x7FFFFF]`, far past the
    2^19 slots.
- **Constructors use an indeterminate `id_`.** The current size constructors move-assign the
  result of `claim`, which calls `free()` before `id_` has been initialized; the move constructor
  swaps with an uninitialized destination as well. Initialize the null state before assignment,
  preserve it on early return/exception, and leave moved-from objects null.
- **Copying must publish a complete new entry.** The current copy allocates a fresh id but uses
  `other_entry.region()` for the destination and does not copy the source GPUBuf metadata.
  Set the destination region from its own id, copy the address/size/offset record, and retain
  exactly one backing reference without leaking the temporary heap-allocated token wrapper.
  Exercise copying across a region boundary and destruction in either order.
- **Finish the placement accessor port.** `placement()` remains declared/called while its
  implementation has been renamed `descriptor()`; choose the intended surface and update callers.
- **`SharedBuffet` size is wrong** (`alligator.hpp:251,256`).
  - `std::max<size_t>(granules, 0xFFFFFFFFu)` reports the maximum for ordinary allocations and
    narrows larger values incorrectly.
  - Store the actual representable granule count; reject unsupported lengths at allocation setup
    instead of silently clamping them. Check rounding overflow and backend limits there.
- **`Slice::default_placement()` is declared** (`alligator.hpp:531`) **but never defined.** Every
  Slice constructor's default argument odr-uses it.
  - Proposed edit: define it as a forward to `BuffetDescriptors::default_placement()`.
- **Stale id-layout constants** (`alligator.hpp:349-361`, `:423`: `POOL_BITS=25`, `pool_type`,
  `ALLIGATOR_POOL_OVERHEAD`). They contradict the real encoding
  `(slot << 9) | (region << 3) | type`.
  - Also, `Slice::pool_index()` returns the raw id while its documentation says "slot index".
- **Kitchen** (pure code move in flight; the logic predates it):
  - `TaskCountdown::arrive` notifies after the count reaches 0, so a waiter can already have
    destroyed the stack countdown.
  - `hardware_concurrency() - 1` underflows when the call returns 0.

### Outside the in-flight files: safe to fix in Phase 1

- **Region is used without construction** (`alligator.cpp:149-153`, cast onto raw factory memory).
  - Establish `Region` lifetime and initialize its atomics/metadata at the existing SliceRegion
    allocation site, covering both region 0 and later regions; destroy it before freeing backing.
    Constructing only inside `next_id` misses the initial region. Check SliceEntry token-storage
    alignment and object lifetime as part of this change rather than relying on zero-filled bytes.
- **Dead sources in no target:**
  - `src/memory/pressure.{hpp,cpp}` (uses `Placemat`/`BuffetMenu`).
  - `src/memory/vulkan_allocator.cpp` (includes a missing `alligator_vulkan.hpp`).
  - These are listed for Josh to delete or port. They are not restored and not wired in.

### Findings that must not disappear from the work list

- **SliceMap resize/split ordering** (`slicemap.cpp`, `try_resize`): exclude a second resize until
  bucket splitting is complete, or correctly resolve marked source buckets before copying them.
  Add a controlled overlapping-growth regression with exact lookup/ownership checks and sanitizer
  teardown; the earlier stress hang alone did not prove this diagnosis.
- **Typed priority-queue move assignment** (`containers.hpp`, `PriorityT`): release destination
  payload handles before replacing its storage, preserve self-move safety, and verify each old
  payload is released exactly once while transferred payloads remain live.
- **Networking and arena teardown** (`alligator.cpp`, `ba_network.c`): stop and join the reactor
  before destroying the arena, including listen-first/allocate-later startup and active callbacks
  at exit. Do not rely on Vulkan destruction or accidental static/atexit registration order.
- **Whole-tree build completion:** audit public declarations and linked definitions, including
  HeapSlice methods, SharedBuffet/ChainBuffet template instantiations, and every test/benchmark
  still using removed symbols or relying on the old umbrella header. Do not restore deleted code.

Phase 0 supplies the core fixes for integration. Phase 1 lookup/build work and Phase 2's chosen
completion declarations/definitions must land consistently to resolve the existing Shader return
type mismatch; do not revive the old semaphore API to create a temporary baseline. Run the core
regressions as soon as that integrated build is available, and mark them verified only when they
pass. Capture the first valid baseline then, before subsequent native-backend/performance changes.

---

## Phase 1: Make the Vulkan backend build and resolve Slices correctly

Starts after the Slice port and core ownership fixes land; the former C-1 return mismatch is
already resolved in the current working files. Coordinate the completion ABI with Phase 2 so
this phase does not claim a standalone passing build while Shader declarations still disagree.

1. **Header hygiene** (`include/alligator/easygpu.hpp`)
   - Drop `#include <alligator/easyvulkan.hpp>`. `easygpu.hpp` then declares only the portable
     API, and `ShaderState` stays opaque.
   - Keep the full ShaderState/slot definitions in private source headers; internal tests include
     those explicitly, while public Vulkan interop tests include `easyvulkan.hpp`.
   - Add `<optional> <string> <string_view> <cstdint> <coroutine>`.
   - Fix the closing comment (`// namespace buffetalligator`).
   - Remove the `ShaderException` reference.
   - Correct the push-block size to 16 bytes.
2. **`src/vulkan/vulkankernel.cpp`**
   - Include `<alligator/easyvulkan.hpp>`.
   - Replace `Alligator::gpu_table_address()` with the region directory address from item 3.
   - Apply the granule shift to `device_address`: `address + (uint64_t(offset) << 6)`.
3. **Region directory: the GPU-side Slice lookup.**
   - Allocate one stable 64 × `uint64_t` directory during arena initialization, before publishing
     region 0; initialize absent entries to zero. Allocate its backing directly through the
     selected descriptor, not through Slice construction, which would recursively require itself.
   - When a region publishes, write its `gpu_slots` device address into
     `directory[region]` before any dispatch can consume its ids. Define the host publication and
     device visibility dependency explicitly; a C++ release store alone is not GPU synchronization.
     Keep directory and region backing stable until all referring dispatches retire.
   - Regions themselves are allocated on the active GPU placement whenever a GPU backend is
     selected, not implicitly on the host payload default. Use the same construction path for
     region 0 and subsequent regions. On CPU-only runs, use host metadata and report no GPU address.
   - The push block's `pool` becomes the directory address. The GLSL prelude
     (`easyvulkan.hpp:600-615`) resolves through the directory:
     ```glsl
     GPUBufRef slice_ref(Slice s) {
         uint64_t table = U64Array(vulkan_push.vulkan_pool_address).v[(s.id >> 3u) & 63u];
         return GPUBufRefArray(table).refs[s.id >> 9u];
     }
     uint64_t slice_address(Slice s) { GPUBufRef r = slice_ref(s); return r.address + (uint64_t(r.offset) << 6); }
     uint64_t slice_size(Slice s)    { return uint64_t(slice_ref(s).size) << 6; }
     ```
   - `slice_size` widens to `uint64_t`. Grep the prelude and the test bodies for `uint` consumers.
   - Remove `VulkanKernel::table_placement()` (declared, never defined, and superseded by the
     directory).
4. **Granule readers on the host**
   - Define the unit of each accessor once: GPUBuf fields contain granules; public size/offset
     accessors return bytes. Shift at the raw-record boundary, not again in callers.
   - Host pointers use `token()->raw(uint64_t(gpu_buf()->offset) << 6)`; Slice size reads similarly
     widen the raw size before shifting. Audit every writer, including copy, view, resize, network,
     and shader parameter setup, under the byte-view contract gate above.
   - Update the `GPUBuf` doc (`alligator.hpp:364`) and the GLSL `GPUBufRef` comment to say
     "64-byte granules".
5. **Stop mutating shared records** (`vulkan.cpp` `ShaderState::dispatch`/`dispatch(size_t)`).
   - Drop `gpubuf_for(parameters)->size = …`.
   - The prelude takes the list length from `gl_NumWorkGroups.y`, or `.z` for the jobs format,
     which is exact. `vulkan_count()` currently derives it from `slice_size / 4`, which granules
     would round.
6. **Entry point name.** The injected `main()` calls `alligator_main(…)`, per `easygpu.hpp`. Rename
   `vulkan_main` in `public_shader_source`, `reference_shader_source`, and every test body.
7. **Device probing** (`vulkan.cpp`)
   - **Unified-memory heuristic** (`unified_from_memory_properties`, :47-63): any
     DEVICE_LOCAL|HOST_VISIBLE type passes, so ReBAR discrete cards count as unified.
     - Define physical unified memory separately from host-visible device-local memory and CUDA
       managed-memory support. Vulkan has no generic "this is the host heap" flag; use documented
       device/platform evidence and keep allocation policy based on the actual compatible types.
   - **Discrete memory ladder** (:291-309): cached/coherent host-visible types are preferred.
     - Do not replace the current cached/coherent preference solely because a ReBAR heap is large.
       Compare CPU-read, CPU-write, and GPU throughput for the probed compatible memory types in
       the performance phase, then select policy at startup.
     - Check allocation requirements, memoryTypeBits, HOST_VISIBLE/HOST_COHERENT properties,
       allocation-size/count limits, and available budget for the actual requested size. Preserve
       zero-initialization and CPU-read-after-completion semantics; coherence does not remove
       execution dependencies. Report unsupported requested allocations explicitly.
       Refer to [Vulkan memory allocation rules](https://docs.vulkan.org/spec/latest/chapters/memory.html)
       when choosing compatible types and checking allocation limits.
   - **Missing features.** A device lacking `bufferDeviceAddress` or a compute queue currently
     throws out of the `VulkanContext` constructor. That runs inside `Alligator` construction,
     which breaks CPU-only use.
     - Probe every candidate for compute queues, bufferDeviceAddress, shaderInt64, and the required
       host-accessible allocation properties before selecting one. If none qualifies, retain
       CPU-only use and report no compatible GPU; an explicit request for an incompatible backend
       fails clearly rather than silently changing backend.
     - Fix the instance leak on those throws.
     - Chain `PhysicalDeviceInternallySynchronizedQueuesFeaturesKHR` only when the extension is
       listed.
   - **Queue lease exhaustion.** The sticky per-thread lease throws once threads outnumber queues.
     - Claim a queue once, select the already prepared command buffer for that queue's family,
       and submit on that same queue. Release the claim on success and exception; temporary
       contention needs a progress-safe waiting policy, not a lifetime lease or exhaustion error.
       Apply the same ownership rule to every externally synchronized operation on that queue.
       Pool recording/reset remains independently synchronized. See the
       [Vulkan command-pool family and synchronization rules](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html).
   - **Unused state:** `placement_type_indices_`/`placement_cpu_cached_` are never filled. Fill
     them from the probe.
8. **Heap and mmap device addresses.**
   - `AlignedHeapBuffer::device_address` and `MmapBuffer::device_address` return host pointers.
     That breaks the "0 when not GPU-visible" contract (`vulkan_test.cpp:128,235`), and on a
     discrete GPU a bound heap Slice would be dereferenced as a CPU virtual address.
   - Make both return 0. The hook stays non-null, so `claim`'s branch is unchanged.
9. **Memory accounting:** distinguish library-owned live slab bytes from device-wide heap
   budget/usage. Bind `VulkanKernel::gpu_usage()` to its intended documented metric; optional
   VK_EXT_memory_budget statistics cannot replace deterministic allocator-owned counters or
   report unavailable measurements as zero usage.

---

## Phase 2: A backend-neutral Shader with callbacks and co_await

### Public API (`include/alligator/easygpu.hpp`)

This is the callback/awaiter shape, not a finalized error-reporting ABI; settle the completion
result channel below before implementing these declarations. Keep opaque special members out of line.

```cpp
struct ShaderSource {
    std::string_view glsl;   ///< Body defining `void alligator_main(Slice slice)`; the portable fallback.
    std::string_view metal;  ///< MSL body with the same entry, used on Metal when set.
    std::string_view cuda;   ///< CUDA body with the same entry, used on CUDA when set.
};
class ShaderAwaiter;
class Shader {
    std::unique_ptr<ShaderState> state_;
    explicit Shader(std::unique_ptr<ShaderState> state);  // decode's constructor
    friend struct GPU;
public:
    explicit Shader(const ShaderSource& source, std::string_view name = "alligator_shader");
    explicit Shader(std::string_view glsl, std::string_view name = "alligator_shader");
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&&) noexcept;
    Shader& operator=(Shader&&) noexcept;
    ~Shader();
    void operator()(const Slice* slices, size_t count, void (*done)(void* context) = nullptr,
        void* context = nullptr, uint32_t workgroups = 1) const;
    void operator()(const Slice& slice, void (*done)(void* context) = nullptr,
        void* context = nullptr, uint32_t workgroups = 1) const;
    ShaderAwaiter dispatch(const Slice* slices, size_t count, uint32_t workgroups = 1) const;
};
```

- **Completion shape.** Execution is asynchronous after admission; admission may apply backpressure
  when slots are full. `done(context)` runs exactly once per accepted logical dispatch, after all
  its rounds retire and CPU-visible results are ready, including a defined zero-count operation.
  This is the same `void(*)(void*)` + context shape as `Kitchen::Task` and
  `VulkanContext::submit_command_buffer`.
  - It replaces the per-slice `void(*)(Slice)` callback and the semaphore return.
- **Failure contract.** Distinguish a synchronous rejection before ownership is accepted from a
  dispatch that later fails. Define caller-visible completion result storage with the callback
  pair before finalizing the API; logging alone is not failure delivery. The awaiter records that
  result and reports failures from `await_resume()`, which must not be unconditionally `noexcept`.
- **Resource retention.** Accepted work owns its prepared state, slot, parameter ids, and directly
  bound Slice resources through retirement. A Slice copy creates a different id: fill GPU parameter
  lists with ids that the operation actually retains. A retained slab alone does not retain a
  recycled table entry. The caller's array may disappear after admission only once metadata has
  been captured; payload bytes remain zero-copy. Explicitly register the lifetime dependencies of
  embedded Slice ids, which cannot be discovered by scanning arbitrary payloads.
- **co_await.** The awaiter owns a stable operation state, not just borrowed Shader/array pointers
  and a naked coroutine address. Publish the continuation safely even if completion races with
  `await_suspend`; do not access a destroyed awaiter after publication. Define frame destruction
  and cancellation so completion never resumes a destroyed coroutine and releases resources once.
- **Continuation execution.** Resume user code on the documented Kitchen worker executor. Current
  `WaitingThread::work` invokes `done` on the waiter thread, so add an explicit handoff if worker
  execution is promised. Contain user callback exceptions and record errors without killing a
  worker. Define context lifetime through callback return and test reentrant submission/destruction.
- **Shader destruction.** Accepted operations retain the implementation independently of the
  public Shader handle; releasing a Shader in its own callback must not wait for itself.
- **Scope of the semaphore migration.** Only the GPU completion API. These stay as they are:
  - `ba_queue.c`'s OS semaphore handoff (an internal C queue, not a completion API).
  - Test-harness `std::binary_semaphore`s (`memory_tracker_test`, `synchronization_functional_test`,
    `network_test`).
  - `static_assert(sizeof(Shader) == sizeof(void*))` still holds.

### GPU struct

- **`GPU::run(const Shader&, Slice*, n)`** and **`GPU::run(const Slice& program, Slice*, n)`**
  gain the same completion contract. Give the awaiter operation a distinct name (for example,
  `GPU::dispatch`) or explicit tag; do not select execution behavior via `Slice*` versus
  `const Slice*` overload resolution. Async retention and round-completion rules apply to both.
- **`GPU::encode`/`decode`** use a portable format:
  - Specify exact offsets, integer widths, byte order, and format version for magic `'BAGP'`,
    flags, content identity, source/name lengths, and aligned payloads; do not assert a 32-byte
    size until the chosen fields and widths demonstrably fit.
  - Then the sources, each padded to 8 bytes.
  - Retain owned source bytes during preparation; ShaderSource string_views cannot become cached
    dangling references. Decode validates lengths, arithmetic, format, and compiler input before
    allocation/compilation at this preparation boundary, outside dispatch.
  - `decode` recompiles through the private `Shader(std::unique_ptr<ShaderState>)`, which is the
    header gap that currently makes it throw. Reuse compatible module/pipeline caches and measure
    cache-hit versus recompilation cost; do not assume a pipeline cache eliminates source compilation.
- **`GPU::compile_glsl(body)`** returns the same encoded format with a "job-table body" flag.
  - `run(const Slice&)` keys engines by content plus format/entry flags, backend/device, compiler
    version/target, and options; compare retained content on a hash match rather than treating a
    64-bit hash as collision-free identity. Host addresses are reused after free.
  - It frees the engines at context teardown.
  - Publish preparing/ready/failed state and wake waiters on failure; clearing a key alone does
    not unblock callers spinning only on `state == nullptr`. Define retry and bounded cache
    ownership, and release engines only after their accepted operations finish.
- **`GPU::exists`/`unified_memory`/`device_name`** answer for the active backend.
  - Fix the doc labels ("available" → "exists", "Vulkan" → "GPU").

### ShaderState: in-flight dispatches

Today one fence, one mapped indirect record, one parameters Slice, and one command buffer are
shared by every call, so concurrent calls race (`vulkan.cpp` `Impl`).

- Prepare submission slots with separate mutable parameters/indirect records and completion state;
  Vulkan needs a fence and compatible command buffers/pools, CUDA a stream/event, and Metal a
  command buffer for each submission. Avoid per-dispatch pipeline compilation or region allocation.
- Queue count × 2 is a candidate depth to measure, not a portable hardware concurrency limit.
  Probe backend capabilities and choose preparation capacity from measured saturation, memory
  budget, and request sizes; CUDA stream/Metal queue counts are not Vulkan queue-family counts.
- Slots and completion contexts have stable addresses. GPU retirement and failure processing
  release reusable slots before scheduling reentrant user code, while retaining the operation
  result and resources that the callback still needs.
- Full-ring admission must make progress independently of callbacks queued behind blocked
  submitters. Define wakeups and shutdown behavior; never park all Kitchen workers waiting for
  continuations that require those same workers. Stress submitter counts beyond slot/queue counts.

### Completion per backend (no lambdas or blocks)

- **Vulkan:** `submit_command_buffer(cmd, fence, done, context)`, which already routes to
  `Kitchen::submit_waiting(&VulkanContext::wait_for_fence, fence, done, context)`.
  - Fix `static_cast<VkFence>(fence)` passed as `void*`. It only compiles where `VkFence` is a
    pointer; pass a stable typed slot context and update the waiter to dereference its fence.
    Keep it alive until the wait completes and report fence/device-loss failures.
- **Metal:** `Kitchen::submit_waiting(&metal_wait, commandBuffer, done, context)`, where
  `metal_wait` calls `[cb waitUntilCompleted]`.
- **CUDA:** `cuLaunchHostFunc(stream, &cuda_retired, slot)`. Since a host function may not call
  CUDA, `cuda_retired` hops `done(context)` onto `Kitchen::submit`.
  - Host functions may not execute after a CUDA context error; an independent completion/error
    path must retire failed operations and wake awaiters exactly once. Check Metal command-buffer
    status/error too. See [CUDA host-function semantics](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__EXEC.html).

### Ordered shutdown

Stop admission, stop/join networking, retire accepted GPU operations and drain their user
continuations while Kitchen and arena metadata remain live, release cached programs and retained
Slices, then destroy arenas/directories and backend contexts in dependency order. Kitchen currently
abandons queued tasks at shutdown, so GPU completion cannot depend on its ordinary destructor
draining them. Test explicit shutdown and process exit in multiple initialization orders.

### Backend selection

- A build carries Vulkan plus at most one native backend, because Metal and CUDA share type
  index 2 (`easycuda.hpp:7`).
- Freeze active backend selection during shared runtime initialization, before any arena metadata
  is allocated; prefer a compatible registered native backend and preserve explicit user choice.
  Provide a startup selection for tests to exercise Vulkan and native backends in separate processes.
- Automatic probing happens before the first Slice or GPU operation:
  - Metal: `MTLCreateSystemDefaultDevice()`.
  - CUDA: enumerate compatible devices, honor explicit selection, then retain the chosen primary
    context. SM count alone is not a cross-architecture performance comparison; record device
    identity and derive target architecture from its compute capability.
  - An explicit `register_type` before first use still wins.
- Resolve default payload placement once from the chosen memory policy and capabilities. CUDA
  managed memory is not proof of physical UMA or migration-free access; separately measure cold
  faults and alternating CPU/GPU ownership. Metadata must always be addressable by the frozen
  backend. Reject incompatible placement at preparation/admission without hidden staging copies.
  See [CUDA unified-memory behavior](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/unified-memory.html).

---

## Phase 3: Metal backend (`src/metal/metal.mm`, `include/alligator/easymetal.hpp`)

1. **CMake** (`cmake/AllocatorBackends.cmake`)
   - Enable the OBJCXX language on Apple builds before adding `.mm` sources. Add `if(APPLE)`:
     `BUFFETALLIGATOR_HAS_METAL=1` PUBLIC, sources `src/memory/metal_allocator.mm`
     and `src/metal/metal.mm`, compile option `-fobjc-arc`, and link `-framework Metal
     -framework Foundation`.
   - Verify and explicitly build/stage the required SPIRV-Cross core/MSL targets and headers from
     the pinned dependency version. A vendored source checkout does not guarantee standalone
     archives or exported targets; `run_build.sh` must stage and install what consumers link.
2. **Residency.** GPU code reaches MetalBuffers by raw `gpuAddress`, so every live buffer must be
   resident.
   - Probe for `MTLResidencySet` (macOS 15). Add a buffer to the set in `MetalBuffer`'s
     constructor, commit staged changes before use, and attach the set to the queue. Serialize
     set mutations and delay removal/release until all referencing submissions retire; residency
     alone does not track resource hazards. See [Metal residency sets](https://developer.apple.com/documentation/metal/mtlresidencyset).
   - Pre-15 devices: capability-selected `useResources:` over a retained, stable resource set per
     encoder; measure registry size and declaration cost rather than assuming this is free.
   - Regions and the region directory are MetalBuffers too.
3. **Prelude parity.** Add an MSL prelude string that mirrors the GLSL Slice API (`slice_address`,
   `slice_size`, `slice_load_*`/`slice_store_*`, `slice_is_null`). It uses the same directory
   lookup and granule shift over `device const GPUBufRef*`.
   - Entry: `kernel void alligator_entry(constant KernelPush& push [[buffer(0)]], uint3 group
     [[threadgroup_position_in_grid]], …)`, calling `alligator_main(slice_at(group.y))`.
   - Threadgroup shape (16,4,1).
   - Dispatch with `dispatchThreadgroups:MTLSizeMake(workgroups, n, 1)`, with `setBytes` for the
     push block.
4. **GLSL fallback.** When `ShaderSource::metal` is empty:
   - shaderc compiles `glsl` against the GLSL prelude.
   - Gate integration on an experiment executing the actual public-list and job-table preludes
     through the pinned SPIRV-Cross version, including physical pointers, nested Slice ids,
     granule conversions, vectors, and push-block layout. Source support for physical storage
     buffers is not enough to prove this complete ABI works.
   - Choose a supported MSL target from the device/platform requirements and map push constants
     explicitly to buffer(0); record compiler options and diagnostics.
   - `newLibraryWithSource:` compiles the result.
   - Cache the `MTLComputePipelineState` in `ShaderState`.
5. **Probed limits.**
   - Check device per-dimension threadgroup limits against (16,4,1), compiled pipeline
     `maxTotalThreadsPerThreadgroup` against 64, and the pipeline's threadgroup-memory use.
   - `maxBufferLength` (already probed).
   - Identify the documented API/GPU-family constraints for grid dimensions; do not invent a
     generic device property for maximum threadgroup counts. See
     [Metal threadgroup and grid sizing](https://developer.apple.com/documentation/metal/calculating-threadgroup-and-grid-sizes).
6. **Allocator fixes** (`metal_allocator.mm`)
   - Reset `address_`/`size_` in `release()`.
   - Drop the dead `register_descriptor(...) != type_idx()` check. It always returns 2.
   - Implement `memory_usage()` as it is now, and expose it through `GPU` via the active backend.

---

## Phase 4: CUDA backend (`src/cuda/cuda.cpp`, `include/alligator/easycuda.hpp`)

1. **Slang experiment (gate).** Before building the path, compile and execute the full GLSL
   prelude with the pinned release's GLSL input mode and PTX target. Current CLI documentation
   deprecates `-allow-glsl`; confirm whether `-lang glsl`/the input extension is the supported
   spelling for that release. Confirm coverage of:
   - `GL_EXT_buffer_reference` / `buffer_reference_align`
   - 64-bit address arithmetic and `uint64_t` ↔ reference casts
   - `gl_WorkGroupID`/`gl_NumWorkGroups`/`gl_LocalInvocationIndex`
   - Public-list and job-table entry points, push/launch parameter layout, embedded Slice ids,
     vector alignment, 64-bit sizes, multiple regions, and more than one dispatch round

   If the settled route cannot execute these contracts, report the precise unsupported feature
   before implementing a replacement route. See the
   [Slang CLI reference](https://docs.shader-slang.org/en/stable/external/slang/docs/command-line-slangc-reference.html).
2. **CMake**
   - `find_package(CUDAToolkit)` on non-Apple hosts; when found, set `BUFFETALLIGATOR_HAS_CUDA=1`
     PUBLIC.
   - Sources: `src/memory/cuda_allocator.cpp` and `src/cuda/cuda.cpp`. Link `CUDA::cuda_driver`
     and `CUDA::nvrtc`, plus the vendored `slang` library.
   - Detect compiler/toolkit availability separately from runtime device availability;
     `nvidia-smi` is not a required toolkit or driver probe. Support explicitly configured builds
     on machines without a local GPU. Offer missing required dependencies interactively or give
     the exact unattended next step. Pin Slang version/checksum/license and verify host OS/arch
     artifacts, with an explicit supported source-build path where an artifact is unavailable.
3. **Prelude parity.** Add a CUDA prelude with the same Slice API over `const GPUBufRef*` and the
   same directory lookup and granule shift.
   - Entry: `extern "C" __global__ void alligator_entry(KernelPush push)` calling
     `alligator_main(slice_at(blockIdx.y))`.
   - `blockDim` (16,4,1), grid `(workgroups, n, 1)`.
   - The native `cuda` source compiles through NVRTC to PTX and loads with `cuModuleLoadData`.
   - Set NVRTC/Slang target architecture from the selected device's compute capability; bind the
     retained CUDA context correctly on every allocation/submission/completion worker thread.
4. **GLSL fallback.** When `ShaderSource::cuda` is empty, Slang compiles `glsl` plus the GLSL
   prelude to PTX, loaded the same way.
5. **Probed limits.**
   - Probe X/Y/Z grid dimensions for public-list and job-table formats. The legal grid limit is
     not a command to allocate a parameters array that large: prepare capacity from limits,
     memory budget, and measured workload depth, then round larger logical dispatches explicitly.
   - Check `MAX_BLOCK_DIM_*`, `MAX_THREADS_PER_BLOCK`, compiled-function limits, and shared-memory
     requirements for the (16,4,1) shape.
   - The memory kind comes from `register_type`'s existing concurrent-managed-access probe.
6. **Allocator fixes** (`cuda_allocator.cpp`)
   - Clear `registered_context` if `register_type` throws after assigning it.
   - Drop the dead `!= type_idx()` check.
   - With auto-registration, `CudaBuffer(size)` can no longer run before `allocate_` is bound
     through the placement path.

---

## Phase 5: Packaging

- `CMakeLists.txt` installs only `include/alligator.hpp`, so `easygpu.hpp` and the other split
  headers are unreachable for installed consumers. Use
  `install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})`.
- Guard native includes in `buffet.hpp` and related public headers with exported backend feature
  definitions; the current unconditional `easycuda.hpp` include requires `cuda.h` even on hosts
  without CUDA. Compile each installed public header in isolation with its required platform.
- Export all static-link dependencies and runtime search requirements for Metal/SPIRV-Cross,
  CUDA/NVRTC/Slang, and Vulkan. Resolve imported targets from the installation prefix or declared
  system packages, never build-machine paths. Include dependency licenses/notices.
- Add an external consumer build using only a staged installation and `find_package(alligator)`;
  verify relocation to a second prefix, backend feature definitions, and library link/run success.
- Keep backend implementation, dispatch-slot state, compiler caches, and runtime ownership in
  private `src/` files; public headers expose only the intended caller contract.
- Stale docs to update:
  - `README.md` (Placemat, `VulkanContext::gpu_usage`, `VulkanContext::device_address`).
  - The `vulkankernel.cpp` file brief, which cites `gpu.hpp` and "DESIGN.md Law 3/4/5"; neither
    exists.
  - The `Slice` doc comment, which says copy is deleted.

---

## Tests

Port the three GPU tests to the current API; do not restore removed symbols:
`tests/shader_slice_test.cpp`, `tests/vulkan_interop_functional_test.cpp`, and
`tests/allocators/vulkan_test.cpp`.

Also port the CPU, allocator, networking, container, and benchmark contract targets affected by
the split headers, changed claim return, and granule/view policy. Compiling just the GPU targets
is insufficient. All regressions invoke production code with deterministic inputs, not copied
implementations or randomly generated embedding data.

- **Backend-neutral shader test.** Run each configured backend in a fresh process so address
  domains never change mid-test. Vulkan runs the GLSL path; Metal/CUDA run both native and
  translated source. Skip code 77 means the requested hardware is absent, not that compilation,
  translation, residency, or device execution failed.
  - `identity`, `embedded_slices`, `dispatch_rounds` (count > prepared per-round capacity,
    without requiring allocations as large as a device's maximum grid)
  - `callback_completion` and `co_await_completion` (a minimal coroutine task in the test)
  - `concurrent_dispatch`: N Kitchen tasks sharing one Shader
  - `encode_decode_roundtrip`
  - `region_spread`: slices whose ids land in several regions, which catches directory lookup
    errors
  - `oversize_and_novel_slices`: device address equals the dedicated buffer's base
  - `startup_order`, explicit backend selection, and incompatible-placement rejection
  - concurrent region publication, cross-region copies, and correct CPU reads after completion
  - zero-count completion, submit failure, async device failure, and full-ring progress
  - temporary input arrays, Shader destruction with accepted work, embedded-id dependencies,
    reentrant callbacks, coroutine cancellation/destruction, and completion racing with suspension
- **Granule assertions.** Consecutive records within one slab satisfy
  `offset == prior.offset + prior.size` in granules; host pointers equal
  `base + (uint64_t(offset) << 6)`. Check requested versus rounded/logical lengths explicitly.
- **Core regressions.** Cover null/default/moved-from Slice destruction, exact-size and oversized
  claims, dedicated reference release, forced rollover interleavings, region-construction failure
  and retry, map overlapping growth, typed queue move ownership, and listen-first process exit.
  Exercise alignment boundaries 0/1/63/64/65 and near slab limits under the settled view contract;
  test arithmetic limits without attempting unsupported physical allocations.
- **CMake registration.** Keep explicit cases, labels, timeouts, skip semantics, platform sources,
  and dependencies in sync; the existing source glob does not register new shader case names or
  configure backend-specific tests. GPU cases get `LABELS "gpu;functional"`; core regressions
  remain runnable without a GPU. Correctness checks stay enabled in Release builds.
- **`memory_leak_test.cpp`** still guards residency for Vulkan. Extend it to the active native
  backend using `memory_usage()`.
- **Diagnostics.** Use separate ASan/UBSan and supported TSan builds for CPU ownership/concurrency
  regressions, and backend validation/debug modes for GPU lifetimes and synchronization. These
  runs are correctness evidence, never performance samples.

## Phase 6: Performance testing and regression acceptance

Performance qualification is required for completion of this plan. Benchmarks are C++ harnesses
under `tests/benchmarks/` that invoke the production public interfaces; compiler feasibility
experiments stay under `experiments/`. Benchmarks report progress at least once per second from
outside the measured hot path. Ordinary CTest remains a correctness suite, without machine-speed
thresholds or automatic long performance sweeps.

### Baselines and measurement rules

1. Finish the core correctness/build gates first. Record the first passing current-source Release
   integration milestone as the baseline before subsequent native-backend and performance work.
   Changes required to reach that milestone cannot claim a valid before/after speedup against the
   broken tree. Earlier incorrect allocator results and stale prebuilt binaries are not baselines,
   and deleted source is not recovered from history. Published results are context only.
2. Archive the revision, working-tree patch, exact commands, compiler/linker and optimization/LTO
   settings, tracking flags, dependency/compiler versions, OS/architecture, CPU topology, RAM,
   backend/device/driver, allocation type, working-set size, queue family/count, and slot depth
   with every result. Benchmark the same corrected byte-view/ownership contract in both builds.
3. Use isolated Release builds on an otherwise idle host with recorded power/thermal conditions;
   never compare sanitizer/debug timings with Release. Use deterministic identities and fixed
   operation traces, plus representative application traces when available. Do not invent random
   embedding data or claim an allocator microbenchmark represents an embedding workload.
4. Separate cold initialization/compilation/first touch from warmed execution. Use at least two
   warmups and 15 measured repetitions for aggregate comparisons, rotating candidate order and
   repeating the series to establish host noise. Extend short workloads until timer overhead is
   negligible; validate output and ownership after every sample, outside the timing interval.
5. Report raw per-sample results and medians/ranges. For individually timestamped operations,
   report p50/p95/p99 latency and sample count; aggregate elapsed-time/operation is not request
   latency. Measure throughput separately if timestamp instrumentation perturbs the hot path.
   Record CPU time/utilization, peak and post-drain allocation bytes, and device/host memory use.
6. Use persistent worker teams for steady-state tests. Include allocation/refcount work when that
   is the operation being measured, GPU completion rather than just enqueue time for execution
   throughput, and separate callback/awaiter completion from device execution timestamps.
   Output verification, input generation, and progress printing stay outside measured work.

### Required workloads

| Area | Cases | Measurements / checks |
| --- | --- | --- |
| Slice allocation and ownership | Small claims at 1/63/64/65 bytes under the settled granule contract, 4 KiB and larger claims, slab-limit minus/equal/plus one granule, novel allocations, copy/move/sub-view/release; single thread through reported CPU concurrency and controlled oversubscription | Claims/s, separately sampled p50/p95/p99, bytes/op including metadata and rounding, contention, rollover tail latency, retained-slab high-water mark, release/drain latency |
| Allocation lifetime | Short-lived claims, one retained Slice per otherwise drained slab, repeated rollover, many regions, allocation failure/retry outside timing | Steady-state memory plateaus, exact accounting after owners release, no growth in abandoned nodes/token wrappers; caller-dangling raw pointers are outside the contract |
| Containers | Existing map fill/hit/miss/duplicates/stream/growth; queue single-thread, balanced and asymmetric producer/consumer teams; priority/heap push/pop/eviction and populated move assignment | Comparable-contract throughput versus existing standard-container baselines, resize contention, retained ownership cost, exact-once delivery and payload release |
| Kitchen and continuations | Warm submit, parked wakeup, single/bulk submission, more producers than workers, callbacks and coroutine resumes, saturation and drain | Tasks/s, actual round-trip p50/p95/p99, idle CPU, continuation delay, shutdown/drain time; distinguish scheduler cost from GPU work |
| GPU preparation | Context initialization, native-source compile, GLSL translation, module/pipeline creation, cold/warm program-cache lookup, encode/decode | Wall time by preparation stage, cache memory, diagnostics, compiler options; measure in fresh processes for true cold runs |
| GPU execution | Small identity/dispatch-overhead kernel, deterministic bandwidth and representative compute kernels, embedded ids, multiple regions, one and multiple rounds | Device execution time where supported, completed dispatches/s, useful bytes/s, end-to-end callback/awaiter p50/p95/p99; same algorithm, inputs, outputs, and workgroup shape for native versus translated source |
| GPU concurrency | One/many submitters sharing a Shader; submitters beyond queue count; slot-depth sweep through throughput saturation; sustained full-ring load; small and large batches | Queue/admission wait, CPU submission cost, completion latency, sustained throughput, bounded memory, forward progress, and fairness; determine depth from evidence rather than queue count × 2 folklore |
| Memory placement | Each supported host-visible/coherent/cached/device-local choice; UMA and discrete devices; CUDA managed cold first touch, warm reuse, alternating CPU/GPU access | CPU read/write throughput, GPU bandwidth, migration/page-fault cost where tooling supports it, residency/working-set high-water marks; compare correctness-qualified paths without hidden staging copies |

Add dedicated allocator and GPU performance executables (planned names:
`buffetalligator_allocator_benchmark` and `buffetalligator_gpu_dispatch_benchmark`); they do not
exist yet. Reuse existing benchmark support for options, persistent teams, logging, CSV, and
validation rather than duplicating container implementations. Audit the existing harnesses'
header dependencies and described algorithms against current code before using their results.
Extend the Kitchen and priority harness reporting where raw samples/percentiles are missing;
do not pretend their present summaries already contain them.

### Existing harness commands after the build is repaired

These are current executable names/options. Default map/queue topology sweeps use the reported
CPU count; use explicit supported worker counts for matched-host comparisons and respect the
containers' documented registration limits. Native/backend selection flags for new GPU harnesses
must be specified and documented when those harnesses are implemented.

```sh
./run_build.sh --build-dir build/perf --install-dir build/perf/install \
    -DCMAKE_BUILD_TYPE=Release -DBUFFETALLIGATOR_BUILD_TESTS=ON \
    -DBUFFETALLIGATOR_BUILD_BENCHMARKS=ON
mkdir -p build/perf/results
./build/perf/tests/benchmarks/buffetalligator_slicemapvsvecset \
    --case contracts
./build/perf/tests/benchmarks/buffetalligator_slicemapvsvecset \
    --rounds 15 --csv build/perf/results/map-comparison.csv
./build/perf/tests/benchmarks/buffetalligator_slice_map_benchmark \
    --warmup 2 --repetitions 15 --csv build/perf/results/map.csv
./build/perf/tests/benchmarks/buffetalligator_slice_queue_benchmark \
    --warmup 2 --repetitions 15 --csv build/perf/results/queue.csv
./build/perf/tests/benchmarks/buffetalligator_kitchen_benchmark \
    --tasks 1048576 --repetitions 15 --latency-samples 20000
./build/perf/tests/benchmarks/buffetalligator_priority_slice_benchmark
./build/perf/tests/benchmarks/buffetalligator_priority_slice_showdown
```

### Acceptance and recorded deliverables

- Capture the initial correctness-qualified CPU/Vulkan baseline at the first passing integrated
  milestone, then rerun affected workloads after each subsequent phase and the full matrix at completion.
  Correctness fixes stay required even if they expose that an earlier fast result was invalid.
- On controlled matched hosts, investigate a repeatable median-throughput loss above 5% or a
  p99-latency increase above 10%, once the measured change exceeds the baseline's repeat-run noise.
  Confirm in three independent comparison series; a noisy or undersampled result is inconclusive.
  These are initial review thresholds, not universal hardware promises or CTest pass/fail limits.
- No performance signoff with failed output checks, unbounded memory growth, abandoned accepted
  work, silent backend changes, or unexplained material regressions. Record the cause and explicit
  acceptance of any necessary correctness/performance tradeoff; do not lower hardware capability
  targets or add a fallback to make the numbers pass.
- Publish raw CSV samples, benchmark commands and environment manifest, baseline/candidate
  comparisons, preparation versus execution results, memory accounting, and the selected submission
  depth/memory policy rationale. State unavailable hardware measurements as unverified.
- Required qualification targets: macOS Apple Silicon with Metal and MoltenVK tested separately;
  Ubuntu x86-64 and ARM64 CPU paths; Vulkan on representative discrete and unified-memory hardware;
  native CUDA plus Vulkan on an NVIDIA host. Include supported Intel macOS build/CPU coverage.
  A build-only or GPU-skipped job is not evidence of native-backend correctness or performance.

## Verification

1. `./run_build.sh` on macOS (Apple Silicon) builds Vulkan (MoltenVK) + Metal. Run
   `ctest --test-dir build -L gpu --output-on-failure` for each explicitly selected backend in
   fresh processes; building both is not the same as exercising both. Include supported Intel
   macOS compilation/CPU testing and capability-qualified GPU execution where hardware exists.
2. `./run_build.sh` on the Linux build host (x86 and ARM).
   - Without a GPU: GPU cases skip with code 77 and all CPU tests pass. Separately exercise
     incompatible-device probing, which absence-only testing does not cover.
   - On an NVIDIA host: run Vulkan and CUDA separately, including native/translated public-list
     and job-table paths; qualify ARM64 CUDA where supported hardware is available.
3. Run the full `ctest` (not only `-L gpu`), because the region-directory and granule changes
   touch every Slice.
4. The build console preserves full live output as Josh requested during execution; missing Slang
   or CUDA is offered for install or explained with the exact next step, never a bare error.
5. Pass installed-consumer/relocation checks, CPU sanitizer regressions, GPU validation, and
   shutdown/lifetime tests. Record exact commands/results; missing hardware remains an open item.
6. Complete Phase 6 and attach its performance artifacts and regression assessment before marking
   the plan done. This plan update itself does not claim implementation, test, or benchmark success.
