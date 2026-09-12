<div align="center">
  <img src="buffetalligator_logo.png" alt="Buffet Alligator Logo" width="150"/>
</div>

# BuffetAlligator

BuffetAlligator is a C++20 memory arena backed by a private C11 core. Its 16-byte `Slice` handles reference zeroed regions of preallocated slabs. Ordinary claims bump a thread-local plate cursor without atomic operations; reference counting happens at plate granularity.

The project is pre-release. API and ABI compatibility are not guaranteed until 1.0.

## Install

```sh
cmake --install build/current --prefix /your/install/prefix
```

Installed CMake consumers can use the exported target:

```cmake
find_package(alligator CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE alligator::alligator)
```

Concurrent claims and independent `Slice` handles are supported. Concurrent mutation of the same `Slice` object requires external synchronization.

## Placemat

A registered `Placemat` is a process-lifetime memory source with stable host-pointer access. `heap` has identifier 0, `aligned_heap` has identifier 1 and is the default, and custom identifiers start at 2. Both built-ins use anonymous OS pages. Every claim has a host pointer aligned to at least 64 bytes and reads as zero when returned. Sub-slice views retain the original allocation and can begin at an arbitrary byte offset.

Slabs are divided into plates. Each thread bumps within its own plate; sealing the plate publishes its issued claim count, and thread exit seals its remaining plates. Larger claims receive direct plates or dedicated novel allocations. Holding a small slice keeps its plate and parent slab alive.

One worker prepares a runway of slabs, recycles retired slabs into a bounded free list, zeroes reusable memory, and releases excess capacity. Runtime page size, available memory, process limits, hardware threads, and measured consumption determine slab geometry and runway depth. A nonzero requested slab size is rounded to the OS granule and honored without a fixed minimum; zero requests runtime sizing. A runway miss builds a slab on the caller, subject to its budget.

Registration waits for the worker to prepare the first current slab and runway. Register custom placements before starting concurrent use. Callbacks can run concurrently on callers and the worker. The allocator must return zeroed memory with the declared alignment, wrapped in a newly allocated `Placemat::Handle`; deallocation releases the substrate, and the framework deletes the handle. Names and callback context must remain valid for the placement's lifetime. Device addresses, transfers, and synchronization belong to the consuming implementation.

OS-page placements share a global capacity ceiling derived from three quarters of startup headroom. Each placement also has its own budget. Custom placements can supply a budget or a capacity query; without either they are unlimited. Allocations that exceed a budget throw `AlligatorException`.

Small OS-backed novel allocations round to the base page size; eligible requests at least one large page retain large-page backing. Slab geometry is unchanged. The shared Slice layout defaults to 24 slot bits, allowing 16,777,215 reusable backing slots per process and slice sizes below 1 TiB. Slots cover simultaneously live regions rather than total claims or stored rows; copies and sub-slices share a slot, and worker retirement returns it for reuse.

The backing registry reserves stable virtual addresses and starts with 1 MiB writable, extending in 1 MiB chunks as fresh slot IDs need storage. Growth never moves live reference counters. Only fresh-slot acquisition checks capacity; ordinary thread-plate claims and recycled-slot acquisition do not. Failed commitment reports `BA_E_ALLOC` without consuming a slot, while reaching the encoded ceiling reports `BA_E_SLOTS`. The registry remains available for process lifetime so live slices can be released after shutdown.

The default slot width changed from 21 to 24; rebuild the library and all consumers together when changing this setting. Registry growth does not change the encoding at runtime.

Novel allocation happens on the caller. Optional novel caches hold worker-zeroed allocations in bounded power-of-two size classes from 64 KiB through 1 TiB. A later claim can reuse a buffer after the worker publishes it to the cache. OS recycling remaps touched pages to obtain kernel zero-fill; custom recycling calls the supplied zero callback or zeroes the touched prefix on the worker.

The worker polls memory pressure. Warning pressure reduces free-list and novel-cache retention; critical pressure clears those caches and reduces the runway target to its policy floor, within the budget. `Memory::trim` synchronously releases idle runway, free-list, and novel-cache capacity without invalidating live slices. Idle polling does not immediately rebuild explicitly trimmed reserves; subsequent slab advancement can replenish them.

```cpp
#include <alligator.hpp>

buffetalligator::Slice bytes(4096);
auto* values = bytes.data<uint32_t>();
buffetalligator::Slice long_lived(1024 * 1024, true);
```

Stop claim-producing threads and release application slices before calling `BuffetMenu::shutdown()`. Shutdown is explicit, does not run from a static destructor, and replaces the former `AtomicRegistry` `stop_signal` convention. The allocator cannot be restarted after shutdown.

## Memory API

`Memory` is available directly from `<alligator.hpp>`; no private headers are needed. Allocation and freed totals count bytes, including both slabs and novel buffers.

| Methods | Meaning |
|---|---|
| `total_allocations()`, `total_freed()` | Process-wide cumulative allocation and deallocation bytes |
| `placement_allocations(p)`, `placement_freed(p)` | Per-placement cumulative totals |
| `placement_usage(p)` | Allocated bytes minus freed bytes |
| `placement_reserved(p)` | Free-list plus runway capacity |
| `placement_live(p)` | Usage excluding free-list and runway reserves |
| `placement_budget(p)`, `placement_available(p)` | Capacity ceiling and budget minus live bytes; unlimited availability is `SIZE_MAX` |
| `set_placement_budget(p, bytes)` | Set a capacity ceiling for subsequent allocation |
| `placement_novel_cached(p)` | Published zeroed novel-cache bytes |
| `placement_runway_target(p)`, `placement_slab_size(p)` | Current prepared-slab target and resolved slab capacity |
| `trim(p)`, `trim_all()` | Synchronously release idle capacity |
| `system_physical()`, `system_available()` | Fresh system capacity and availability |
| `system_limit()`, `system_headroom()` | Effective process ceiling and remaining system/process headroom |
| `page_size()`, `large_page_size()` | Native page size and usable reported large-page size, or zero for none |
| `hardware_threads()` | Hardware threads available to the process |
| `pressure()` | `Memory::Pressure::None`, `Warn`, or `Critical` |

The original `BuffetMenu::register_type(name, slab_bytes, alignment, allocate, deallocate, get_host_ptr, get_context, set_as_default)` overload remains available. The descriptor overload adds resource policy:

```cpp
buffetalligator::PlacementDescription description;
description.name = "custom";
description.slab_bytes = 16 * 1024 * 1024;
description.base_alignment = 64;
description.budget_bytes = 256 * 1024 * 1024;
description.novel_cache_bytes = 64 * 1024 * 1024;
description.allocate = allocate_zeroed;
description.deallocate = release_substrate;
description.get_host_ptr = host_pointer;
const auto type = buffetalligator::BuffetMenu::register_type(description);
```

`get_context`, `query_available`, and `zero` are optional descriptor callbacks. `set_as_default` selects the new placement. A base alignment below 64 is rejected. Counters are live observations; query them after workload quiescence for comparisons across multiple calls.

## Build

BuffetAlligator requires a C++20 compiler, CMake 3.20 or newer, Git, Autoconf, Automake, Libtool, pkg-config, OpenSSL, and a platform threading library. `run_build.sh` is the supported one-shot build on macOS and Linux; it uses Ninja when available.

```sh
./run_build.sh
```

Override the slot width with the `ALLIGATOR_SLOT_BITS` CMake cache setting (default `24`, integer range `1` through `31`):

```sh
./run_build.sh -DALLIGATOR_SLOT_BITS=20
```

Direct CMake configuration accepts the same `-DALLIGATOR_SLOT_BITS=20` option. The `alligator::alligator` target propagates the selected definition to consumers, including installed packages. Each additional slot bit doubles the registry's reserved address range and halves the maximum representable slice size; writable storage still grows on demand. Existing CMake caches retain their selected width; use `-DALLIGATOR_SLOT_BITS=24` to adopt the new default in an existing build.

The script fetches dependencies under `deps/src` and installs libuv, libsodium, libfabric, and Vulkan headers into `deps/`. It builds MoltenVK on macOS and Vulkan-Loader on Linux. An existing threadsafe-logger checkout is preserved. OpenSSL is discovered with `find_package` and is not vendored. Use `--deps-dir DIR` to select a different dependency directory, `--deps-only` to prepare dependencies, or `--rebuild-vendored` to rebuild them. The library, its dependency archives, Vulkan headers, and Vulkan runtime are installed together for CMake consumers. See `THIRD_PARTY_NOTICES.md` for licenses.

The standalone registry benchmark compares fixed capacity, atomic page growth, and shared-lock access on macOS and Linux:

```sh
build/current/tests/buffetalligator_registry_bench --verify
build/current/tests/buffetalligator_registry_bench > registry.csv
```

See [registry growth measurements](benchmarks/registry_growth.md) for results, methodology, and limitations.

The [integrated growth report](benchmarks/integrated_growth.md) covers the allocator implementation, failure tests, and before/after claim measurements.

The optional [queue comparison](benchmarks/queue_tls_comparison.md) benchmarks the C TLS queue against native and equally buffered moodycamel queues, including throughput, delivery latency, and correctness checks.
The [SliceMap follow-up](benchmarks/queue_slicemap_comparison.md) adds atomic message sequences and compares ID lookup with direct-slot consumption through the existing map API.

## SliceMap

`SliceMap` keeps fixed-capacity rows in append order and uses a preallocated hash index for ID lookups. Duplicate IDs resolve to the earliest published slot. Rows use the TLS Slice allocator; producer counters and each thread's hazard records occupy separate cache lines. `get_slice()` retains its payload inside the hazard-protected window, including when another thread resets the map.

Reset must not overlap producers, and merge/move/destruction require quiescence as before. The index stays at or below 50% occupancy and adds 16–32 bytes per capacity slot on a 64-bit target, depending on power-of-two rounding. It trades additional publication work and storage for expected constant-time lookup. The `SliceMap` layout changed, so rebuild applications against the updated header and library; `Slice` remains 16 bytes.

See the [SliceMap performance report](benchmarks/slicemap_performance.md) for before/after measurements and synchronization details.

## SliceQueue

`SliceQueue` is available from `<alligator.hpp>` and built into `alligator::alligator` without a moodycamel dependency. Its C backend transfers thread-local blocks of 256 Slices through sharded mailboxes and semaphore wakeups. Moving a Slice through the queue adds no reference-count operation; replacing an occupied consumer destination releases that destination's previous Slice normally.

```cpp
#include <alligator.hpp>
#include <thread>

buffetalligator::SliceQueue queue(1, 1);
std::thread consumer([&] {
    auto reader = queue.consumer(0);
    buffetalligator::Slice message;
    while (reader.pop(message)) {
        // Process the owned message.
    }
});
{
    auto writer = queue.producer(0);
    writer.push(buffetalligator::Slice(128, true));
    writer.flush();
}
queue.close();
consumer.join();
```

The constructor takes producer count, consumer count, and optional **capacity per producer** (default 4,096, a positive multiple of 256). Each worker binds a unique index in its role. Handles stay on their creating thread and cannot be copied or moved; each thread may hold one producer binding and one consumer binding at a time, across all queues. All configured consumers must participate until the queue is drained.

`push(Slice&&)` transfers ownership and nulls its source. `push(std::span<Slice>)` does the same for every input. Full blocks publish automatically; call `flush()` at a burst boundary to expose partial blocks promptly. Destroying a producer handle flushes its last partial block. Push blocks when its producer pool has no free block.

`pop(Slice&)` waits for work and replaces its destination only on success. It returns false after closure when that consumer has no available work, preserving the destination; a successfully received null Slice still returns true. `pop(std::span<Slice>)` fills up to one block and returns the number received, leaving unused destinations unchanged. An empty span returns zero immediately. Consume until closure and drain all locally held blocks before destroying a consumer handle; abandoning a partly consumed local block is a contract violation.

Call `close()` only after every producer has finished and flushed, and do not push afterward. Join consumers before destroying the queue. Destruction requires all handles to be gone and releases any remaining queued ownership. `reset()` reopens a fully drained queue only while every worker is quiescent. The queue preserves order within each block; blocks may reorder even within a producer, and there is no global FIFO guarantee.

## Novel backing

`bool Slice::is_novel() const noexcept` reports whether the Slice's backing is a dedicated novel allocation. It reads the existing backing registry without adding a field or changing the 16-byte Slice layout, packed size range, alignment, null representation, or ownership rules.

The method returns false for null/released/moved-from Slices and slab-backed claims, including direct plates carved from a slab. Explicit novel allocations and oversized allocations automatically routed to novel backing return true. Copies and subviews report their shared backing's kind; this does not imply exclusive ownership. A preserving shrink keeps that identity, while a resize that allocates new backing reports the new backing's kind.

## SliceChannel

`SliceChannel` sends Slices between processes and machines through three static operations:

```cpp
using buffetalligator::Slice;
using buffetalligator::SliceChannel;
using Protocol = SliceChannel::Protocol;
void receive(Slice slice) {
    SliceChannel::send(std::move(slice), "", 9000, Protocol::TCP);
}
void response(Slice slice) {
    if (slice) consume(std::move(slice));
}
SliceChannel::listen(9000, Protocol::TCP, receive);
SliceChannel::send(Slice("hello", 5), "127.0.0.1", 9000, Protocol::TCP, response);
// Close after the application has finished its exchanges.
SliceChannel::close(9000, Protocol::TCP);
```

`listen` binds before returning. `send` captures the source bytes before returning and delivers the peer's response asynchronously. The Slice passed to either callback retains its memory normally and may be moved into an application queue. Callbacks execute serially on the channel thread and should return promptly. An empty address is valid only inside the receive callback: it replies to that callback's sender, using the same port and protocol. Each request accepts one reply. Omitting `resp` makes the send one-way; a receiver may use its usual reply path, and that reply is discarded. A response callback receives a null Slice if its exchange fails or is canceled. Setup errors throw `AlligatorException`. A null Slice cannot be sent.

`close(port, protocol)` stops that listener, closes its accepted connections, and cancels outgoing exchanges targeting the same port and protocol. Calls from callbacks are supported. `BuffetMenu::shutdown()` stops network activity before the allocator worker; invoke it after application threads quiesce.

Protocols are `TCP`, `UDP`, `RDMA`, `EncryptedTCP`, `EncryptedUDP`, and `EncryptedRDMA`. TCP and UDP accept IPv4, IPv6, and hostnames. RDMA uses an available libfabric message provider; provider selection follows libfabric's normal configuration, including `FI_PROVIDER`. The test suite exercises this path with libfabric's TCP provider, so real RDMA hardware is not required to run the tests. A hardware RDMA deployment needs its provider, drivers, and networking configured on both machines.

TCP and RDMA accept Slices up to 64 MiB. UDP sends one datagram per Slice, subject to the operating system's datagram limit including placement information; oversized messages fail. UDP retains its normal delivery semantics: messages can be lost, duplicated, or reordered. There are no application timeouts or automatic retries; `close` cancels an exchange whose peer does not answer.

Encrypted protocols use libsodium XChaCha20-Poly1305 with a 32-byte shared key. Set `ALLIGATOR_NETWORK_KEY` to the same 64 hexadecimal digits on both machines before starting channels. For example, generate a key with `openssl rand -hex 32` and distribute it through your application's secret configuration. Encryption authenticates the Slice and its placement information; it does not establish individual identities among holders of the same key or provide application-level duplicate suppression. Plain and encrypted listeners cannot share the same port for the same transport.

Placement names are matched across machines, independently of local registration order. Register matching custom placements on each receiver. Unsupported or missing placements fail the exchange; they are never replaced with ordinary host memory.

## Vulkan-backed Slices

```cpp
const auto* placement = buffetalligator::BuffetMenu::vulkan(
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
buffetalligator::Slice slice(4096, placement);
std::memset(slice.raw(), 42, slice.size_bytes());
slice.vulkan_sync(true);
auto buffer = slice.vulkan_buffer();
```

`BuffetMenu::vulkan(properties)` selects a memory type satisfying the requested Vulkan property bits. Supported combinations use `DEVICE_LOCAL`, `HOST_VISIBLE`, `HOST_COHERENT`, and `HOST_CACHED`. Unsupported combinations throw. `Slice::vulkan_buffer()` returns the buffer, byte offset, and range for the exact Slice view; `BuffetMenu::vulkan_device()` returns its allocation device. These resources remain owned by the library and must not be destroyed by callers.

Host-visible allocations remain mapped. Device-local allocations without host visibility have a staging view so `raw()` remains usable. Call `vulkan_sync(true)` after writing through that view; call `vulkan_sync(false)` to read completed GPU output. External-copy construction uploads its initial bytes automatically. Complete GPU access to the Slice before synchronization or sending it, and do not modify it concurrently with those operations.

Sending a Vulkan Slice reads its device contents. Receiving it creates the same requested memory class locally and uploads the bytes before invoking the callback. Receivers create Vulkan placements on demand; a receiver without compatible Vulkan memory rejects the exchange. Applications receiving GPU data can retain the Slice and its buffer range normally.

## License

Apache License 2.0. See `LICENSE`.