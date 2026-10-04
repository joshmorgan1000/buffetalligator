<div align="center">
  <img src="buffetalligator_logo.png" alt="Buffet Alligator Logo" width="150"/>
</div>

# BuffetAlligator

*It was supposed to be "Buffer Allocator" but voice-to-text thought differently, and the name stuck.*

BuffetAlligator is a C++20 memory arena that serves 32-bit `Slice` handles from preallocated, zeroed slabs. Claims are atomic bump-pointer operations aligned to the placement's bump alignment (64 bytes for the default placement); exhausted slabs advance through successors in a linked list pre-allocated ahead of necessity.

The project is pre-release. API and ABI compatibility are not guaranteed until 1.0.

## Memory placement and granules

A registered `BuffetDescriptor` supplies allocation, release, host access, and device-address hooks for one `ChainBuffet`. Chains allocate on demand, prepare successors during rollover, and reclaim drained backing buffers after their last counted owner releases them. Custom descriptors must be registered before concurrent use and remain valid for the process lifetime.

`Slice` is a four-byte owning handle. A default Slice is null; copying creates a distinct identifier sharing the backing allocation, and moving leaves the source null. Allocation sizes round up to 64 bytes. Sub-slice starts round down and ends round up to cover the requested range within the represented parent. `size_bytes()` reports that represented range. The GPU record remains 16 bytes, with `uint32_t` size and offset fields in 64-byte units; no exact-byte side record is stored.

```cpp
#include <alligator.hpp>

buffetalligator::Slice bytes(65);
auto* values = bytes.data<uint32_t>(); // 128 represented bytes.
buffetalligator::Slice long_lived(1024 * 1024, true);
```

Dedicated allocations are useful for long-lived claims that should not retain an otherwise drained slab. Borrowed pointers remain valid only while an owning Slice or backing buffer remains alive.

### File-backed storage

```cpp
#include <alligator.hpp>
#include <alligator/easymmap.hpp>

using namespace buffetalligator;
const BuffetDescriptor* mapped = MmapBuffer::register_type("/path/to/scratch");
Slice bytes(4096, mapped);
bytes.data<uint64_t>()[0] = 42;
MmapBuffer::flush(bytes);
```

`MmapBuffer` creates shared mappings backed by temporary files in the supplied directory. Files are unlinked immediately and their descriptors remain open through backing-buffer ownership. `file_descriptor(slice)` borrows the descriptor, `file_offset(slice)` includes the view offset, and `flush(slice)` writes the covered pages. These scratch files do not provide reopenable persistent storage.

### GPU execution

Include `<alligator/easygpu.hpp>` for `GPU`, `Shader`, `ShaderSource`, and completion results; Vulkan interop is declared separately in `<alligator/easyvulkan.hpp>`. Backend SDK types are absent from the portable GPU header.

Set `ALLIGATOR_GPU_BACKEND` before the first Slice or GPU operation: `auto`, `cpu`, `vulkan`, or `metal`. Automatic selection prefers compatible native Metal on Apple builds, then Vulkan, then CPU when no compatible device exists. Explicit unavailable backend requests fail. Selection freezes the device address domain and metadata placement. Default payloads use the active placement on physically unified devices and aligned host memory otherwise; GPU inputs require a compatible device-visible placement.

Vulkan remains linked on every build through MoltenVK on macOS or Vulkan-Loader on Linux. `VulkanContext::buffer_placement()` selects its coherent mapped buffers, `VulkanKernel::device_address(slice)` resolves a live Slice address, and `VulkanKernel::gpu_usage()` reports tracked Vulkan backing bytes. Native Metal uses shared buffers and the pinned SPIRV-Cross compiler for GLSL translation. The [CUDA compiler investigation](tests/experiments/slang_pointer_bridge/README.md) produces CUDA source through a Slang pointer prelude; production integration still requires PTX compilation and NVIDIA execution qualification.

`ShaderSource` accepts GLSL, Metal, and CUDA source members; preparation owns the source bytes. Vulkan compiles GLSL. Metal uses its native source when supplied and otherwise translates GLSL. Public list shaders define `alligator_main` and resolve Slice identities through the stable region directory with 64-byte granule conversion.

Shader dispatch accepts a `ShaderResult` and an optional static callback plus `void*` context. Accepted work retains directly bound identities, prepared resources, and explicitly supplied embedded dependencies through retirement. Completion runs on Kitchen workers after device writes are CPU-visible. Call `result.rethrow()` to inspect terminal failure; `Shader::dispatch` provides `co_await`. An external coroutine-frame owner must retain the result and call `cancel()` before concurrent frame destruction. Admission can apply backpressure when prepared slots are busy.

`GPU::encode` and `GPU::decode` use versioned portable source records and a bounded program cache. `GPU::exists()`, `unified_memory()`, `device_name()`, and `memory_usage()` report the selected backend; unavailable native memory counters remain empty optionals.

### Memory accounting

Internal backing-allocation counters track allocations and releases by placement, including dedicated buffers. Slice copies and views do not allocate another payload. Optional allocation-site records are disabled by default and can be enabled with:

```sh
./run_build.sh -DBUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING=ON
```

These internal diagnostic records are separate from Slice metadata. Installed consumers receive backend feature definitions through `alligator::alligator`. Allocator regressions run with `ctest --test-dir build -L allocators --output-on-failure`; hardware absence is reported as a skip, while translation and execution errors fail the test.

## Containers

Every container hands out and stores `Slice` handles; payloads live in arena memory and the containers hold references.

- `SliceMap` and `SliceMapT<T>`: lock-free hash map from 64-bit identifiers to Slices with stable positions, hazard-protected replacement, publish hooks, merge, and reset.
- `SliceQueue`: producer and consumer handles that move Slices through thread-local blocks with semaphore wakeups.
- `PrioritySlice` and `PrioritySliceT<K, V>`: lock-free bounded priority queue in one Slice. Entries are 64-bit words, key bits above a 32-bit value; a push either takes a free slot or evicts the worst entry it beats, a header rejects hopeless pushes in constant time, and per-block caches confine each scan to one 32-slot block. Ordering is exact under push-only load and relaxed under mixed load.
- `HeapSlice` and `HeapSliceT<K, V>`: the same words as a single-owner min-max heap, popping the best and evicting the worst in logarithmic time.
- `SliceCache<Key, Evict>`: byte-budgeted cache of Slices with default least-recently-used eviction or a custom compile-time eviction function; every entry weighs its Slice's size, and keys support `std::hash<Key>` and equality.
- `SIMDMisc`: the identifier, maximum, and minimum scan kernels the containers use, with backends for Apple simd, AVX-512, AVX2, SSE4.2, VSX, and NEON.

Pass a free or static function as the cache's second template argument to choose which entry is evicted:

```cpp
using buffetalligator::Slice;
using buffetalligator::SliceCache;
using buffetalligator::SliceCacheIterator;

/** --------------------------------------------------------------------------------------------------------- Evict Most Recent
 * @brief Selects the most recently used eligible entry for eviction.
 */
template<typename Key>
SliceCacheIterator<Key> evict_most_recent(
    SliceCacheIterator<Key> first, SliceCacheIterator<Key>
) noexcept {
    return first;
}
SliceCache<int64_t, evict_most_recent<int64_t>> cache(128);
cache.set(1, Slice(64));
cache.set(2, Slice(64));
cache.set(3, Slice(64)); // Evicts key 2 and protects the newly stored key 3.
cache.resize(64); // Evicts key 3 and retains key 1.
```

The function receives a nonempty `[first, last)` range in most-to-least-recently-used order, exposing immutable key and Slice pairs. It must be `noexcept`, return an iterator inside that range, and never reenter or mutate the cache. Each call selects one victim; the cache releases it and repeats until its byte budget holds. During `set`, the new or replaced entry is excluded from the range and an oversized entry stays alone; during `resize`, every entry is eligible. The function is invoked directly through its compile-time template argument. Omit it to retain default LRU eviction.

## Kitchen and Orders

`Order` binds a concrete function to variadic arguments held in the Slice arena. Arguments are
decayed and owned; use `std::ref` for an explicit borrow. Orders are move-only, and callbacks can
own their own variadic arguments through `add_callback`. `OrderCompletion::wait()` observes
handler writes, argument destruction, and callbacks, then rethrows the first exception.

```cpp
OrderCompletion complete;
Order order(&process_request, request_id, std::move(request_slice));
order.complete_with(complete);
Kitchen::inst().submit(std::move(order));
complete.wait();
```

Use `submit_waiting` for blocking storage work. `submit_slice` schedules a handler returning a
`Slice` and moves that result into a caller-owned destination without copying its payload:

```cpp
Slice destination;
OrderCompletion loaded;
Kitchen::inst().submit_slice(&read_block, destination, loaded, descriptor, offset, bytes);
loaded.wait();
```

The destination, completion, and explicitly borrowed arguments must outlive completion.
Replacing the destination does not retarget previously copied Slice handles. Completion objects
are one-shot: construct a fresh completion or latch for each invocation.

Register a reusable fanout handler with signature
`void(size_t rank, size_t count, const Arguments&...)`:

```cpp
auto fanout = Kitchen::inst().register_fanout(Kitchen::inst().max_threads(), &process_partition);
std::latch processed{1};
fanout.invoke(processed, input_slice);
processed.wait();
```

Chain a single-threaded preparation Order into a prepared fanout invocation:

```cpp
std::latch finished{1};
Order preparation(&prepare_input, &shared_state);
preparation.then(fanout.order(finished, &shared_state));
Kitchen::inst().submit(std::move(preparation));
finished.wait();
```

`then` (also available as `add_callback(Order&&)`) owns the next Order and submits it after its
predecessor succeeds. A failure skips the remaining handlers and reaches their terminal completion.
The registered fanout must outlive prepared invocations and chains that reference it. A caller's
`std::latch` receives one arrival per completed invocation after all ranks and callbacks finish;
use `OrderCompletion` when the caller also needs exception propagation. Completion objects and
latches attached to the same Order must both remain alive until both signals have been observed.

Each registration owns exactly the requested number of persistent threads and a separate bounded
queue. Every rank runs once per invocation; queued invocations execute one team-wide round at a
time, with `std::latch` joining its ranks and atomic wait/notify waking the team between rounds.
Arguments are shared as const values, while pointers and `std::ref` still require the
handler to synchronize any shared mutations. Registration destruction drains and joins its team.

Thread creation, producer registration, and queue provisioning happen during setup. Call
`Kitchen::prepare_producer()` on each submitting thread to register its tokens and reserve reusable
storage for each producer lane before its submission hot path. Queue submissions never grow queue
storage: `try_submit`,
`try_submit_waiting`, and `try_submit_bulk` leave rejected Orders untouched, while `submit`
variants throw `KitchenException` on exhaustion. Bulk submission accepts ordinary compute Orders
only. Fanout capacity is configurable at registration and counts queued invocations separately
from the active invocation; `try_invoke` returns false
when full and destroys its newly constructed arguments without signaling completion.

Order arguments use arena claims rather than separate heap objects; arena provisioning and
allocations performed by application argument types remain their owners' responsibility.
`Kitchen::drain()`, fanout drain/destruction, and `set_max_threads()` require quiescent external
producers and must run outside the work they are waiting for. Resizing drains and restarts the
compute team; the default compute and waiting teams each use the reported hardware thread count.

## Networking

`SliceChannel` provides TCP, UDP, and libfabric message transports, with encrypted variants of each. Both peers must register matching placement names. Sends capture the Slice's bytes before returning; responses arrive asynchronously on the shared network thread.

```cpp
using buffetalligator::Slice;
using buffetalligator::SliceChannel;

void receive(Slice request) {
    SliceChannel::send(std::move(request), "", 9000, SliceChannel::Protocol::TCP);
}
void response(Slice reply) {
    if (!reply) return;
    // Consume the reply.
}
SliceChannel::listen(9000, SliceChannel::Protocol::TCP, receive);
SliceChannel::send(Slice(4096), "127.0.0.1", 9000, SliceChannel::Protocol::TCP, response);
```

An empty address replies to the sender during its receive callback. Callbacks should finish promptly: blocking a callback also blocks other channels and their timers. `close(port, protocol)` cancels that listener and its matching pending exchanges.

Exchanges have a **30-second deadline** covering connection setup, authentication, transfer, and response waiting. The listener remains open while stalled incoming exchanges expire.

Setup failures throw an exception. A pending response receives one null Slice on timeout, cancellation, or transport failure. One-way asynchronous failures are logged. UDP remains an unreliable datagram transport; it does not retry lost packets.

Encrypted channels require `ALLIGATOR_NETWORK_KEY` to contain the same 64 hexadecimal digits on both peers, configured before channel use. Each encrypted exchange authenticates a fresh receiver challenge before sending application data, adding one round trip. UDP challenges expire and are consumed once; TCP and libfabric challenges belong to one connection. This rejects captured requests after use, expiry, connection replacement, or listener restart without relying on synchronized clocks. Applications that intentionally retry an operation must still handle their own duplicate operation identifiers.

TCP and libfabric retain the **64 MiB payload limit** and allocate complete incoming frames. UDP's complete frame, including metadata and encryption overhead, must fit within 65,507 bytes. The libfabric transport uses registered message buffers. Provider selection follows libfabric configuration, including `FI_PROVIDER`; it does not guarantee a hardware RDMA provider simply because the RDMA enum was selected.

The corrected challenge protocol uses **wire version 2**. Upgrade communicating peers together; version 1 frames are rejected.

> **REVIEW-STALE (2026-09-26):** no wire-version constant exists by that name in `src/network`; confirm the version-2 claim against `ba_wire.c` or drop the paragraph.

## Build

BuffetAlligator requires a C++20 compiler, CMake 3.20 or newer, Git, and a platform threading library. `run_build.sh` is the supported one-shot build on macOS and Linux; it uses Ninja when available.

```sh
./run_build.sh
```

The script updates threadsafe-logger to its main branch, then prepares libuv, libsodium, libfabric, the moodycamel queues, Abseil, simdjson, curl, the Vulkan headers and runtime, and shaderc under `deps`, then builds the library, runs its contract tests, and installs into `build/install`. Useful options are `--skip-tests`, `--clean`, `--rebuild-vendored`, `--deps-only`, and `-DNAME=VALUE` passthrough to CMake; `--help` lists the rest. See `THIRD_PARTY_NOTICES.md` for dependency notices.

### Functional validation

The suite covers placement registration and allocation, slice ownership and resizing, typed and weak slices, atomic values and registries, shared mutexes, reusable barriers, slice queues, and slice maps. It also checks lookup across vector lanes and a pipeline that copies borrowed input into a queue, transforms it, and gathers typed results in a map.

Functional cases run independently with release-build assertions and time limits. Concurrent cases check exact update counts, caller-synchronized map access, retained Slice ownership after map reset, and repeated barrier phases; exception cases check that failed operations leave containers and registries usable. After building, run this group separately with:

```sh
ctest --test-dir build -L functional --output-on-failure
```

The priority queue, heap, cache, and SIMD kernel tests run in the ordinary suite; the priority queue test includes sixteen-pusher exact trimming and exact-once accounting under concurrent pushers and poppers.

### Container benchmarks

Run the map comparison with `build/tests/benchmarks/buffetalligator_slicemapvsvecset`.
It checks correctness before timing VecSet, SliceMap, HazardMap, an immutable-version
candidate, and mutex-protected `std::unordered_map`, then prints a final summary table.

The standalone build also produces SliceMap versus `std::unordered_map`, SliceQueue versus
`std::deque`, and PrioritySlice and HeapSlice versus `std::priority_queue` benchmarks in
`build/tests/benchmarks/`. They include single-thread baselines and, for the map and queue,
multiple producer/consumer teams with mutex-protected standard containers, warmups, repeated
timings, correctness checks, and optional CSV output. See [the benchmark guide](tests/benchmarks/README.md)
for commands and the current containers' concurrency limits, and
[the priority queue results](tests/benchmarks/PRIORITY_SLICE_SHOWDOWN.md) for the measured numbers.

### Network validation

The network suites cover cross-process request/reply, concurrent and large one-way messages, encrypted replay rejection, listener restart, silent peers, and incomplete streams. They inherit libfabric provider selection by default. To select its TCP provider explicitly:

```sh
./run_build.sh -DBUFFETALLIGATOR_TEST_FABRIC_PROVIDER=tcp
```

Hardware RDMA requires a separate run on machines with a working provider and reachable RDMA interface addresses. The same cross-process contract executable supports separate hosts; for example, with a configured verbs provider, run on the server:

```sh
FI_PROVIDER=verbs build/tests/buffetalligator_network_test --listen 2 9000
```

Then run on the client, replacing the address with the server's RDMA interface address:

```sh
FI_PROVIDER=verbs build/tests/buffetalligator_network_test --connect 2 9000 192.0.2.10
```

Repeat with protocol `130` for encrypted RDMA. The test executable sets a fixed test key in both processes; this key is only for the contract tests. A passing TCP-provider run does not establish hardware RDMA compatibility.

## Install

```sh
cmake --install build --prefix /your/install/prefix
```

Installed CMake consumers can use the exported target:

```cmake
find_package(alligator CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE alligator::alligator)
```

The exported config resolves OpenSSL and platform threads from the system and the installed library dependencies from the installation prefix.

Concurrent claims and independent `Slice` handles are supported. Concurrent mutation of the same `Slice` object requires external synchronization.

## License

Apache License 2.0. See `LICENSE`.
