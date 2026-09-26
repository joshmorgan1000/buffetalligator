<div align="center">
  <img src="buffetalligator_logo.png" alt="Buffet Alligator Logo" width="150"/>
</div>

# BuffetAlligator

*It was supposed to be "Buffer Allocator" but voice-to-text thought differently, and the name stuck.*

BuffetAlligator is a C++20 memory arena that serves 32-bit `Slice` handles from preallocated, zeroed slabs. Claims are atomic bump-pointer operations aligned to the placement's bump alignment (64 bytes for the default placement); exhausted slabs advance through successors in a linked list pre-allocated ahead of necessity.

The project is pre-release. API and ABI compatibility are not guaranteed until 1.0.

## Placemat

Each registered `Placemat` (placement) is a process-lifetime factory and owns one independent Buffer chain. A placement supplies only allocation, deallocation, context, and stable host-pointer access. Device addresses, transfers, and synchronization remain private to the consuming implementation.

BuffetAlligator includes basic heap and 64-byte-aligned heap placements, plus the Vulkan buffer placement described below. Aligned heap is the default on hosts without unified memory; on a unified-memory machine the one-time device probe makes the Vulkan buffer placement the default, so ordinary Slices are host-coherent GPU buffers. Applications may register additional placements before creating the first `Slice` and may install a default placement strategy method.

Arena initialization allocates a 64 MiB current slab and a 64 MiB successor for every registered placement. The two built-in placements therefore commit 256 MiB before custom placements; each custom placement adds 128 MiB. The allocator thread then keeps a runway of additional prepared successors beyond each active slab so chain rollovers never wait on allocation; teardown of drained slabs is also deferred to that thread. Placemat callbacks may run concurrently on the calling thread and the dedicated allocator thread, and placement instances must remain alive for the process lifetime.

```cpp
#include <alligator.hpp>

buffetalligator::Slice bytes(4096);
auto* values = bytes.data<uint32_t>();
```

Dedicated novel buffers remain available for long-lived claims:

```cpp
buffetalligator::Slice long_lived(1024 * 1024, true);
```

### File and GPU allocators

Everything below is declared in `<alligator.hpp>`; there are no separate allocator headers. Register placements before creating the first Slice; each backend registers one process-lifetime placement.

```cpp
#include <alligator.hpp>

using namespace buffetalligator;
const Placemat* mapped = MmapAllocator::register_type("/path/to/scratch");
Slice bytes(4096, mapped);
bytes.data<uint64_t>()[0] = 42;
MmapAllocator::flush(bytes);
```

The mmap allocator reserves storage for each slab in the supplied directory and maps it with `MAP_SHARED`. Its temporary files are unlinked immediately and remain open until the existing arena reclamation callback releases the slab. Choose a directory on disk for disk-backed scratch storage; a RAM filesystem provides RAM-backed storage. Fresh slabs are zero-filled by the filesystem, without eagerly touching every mapped page. `file_descriptor(slice)` returns a borrowed slab descriptor, and `file_offset(slice)` includes sub-slice offsets. `flush(slice)` synchronously writes the covered pages back; these scratch files do not provide reopenable persistent storage. `enable_spillover(reserve_bytes, resume_bytes)`, called before the first Slice, makes new slabs fall back to the mmap placement whenever available host RAM would drop below `reserve_bytes`, and returns to heap slabs once it climbs above `resume_bytes`.

Vulkan is not optional. The library links the Vulkan runtime unconditionally (MoltenVK on macOS, Vulkan-Loader on Linux), probes for a device once, and on a machine without one simply reports no device (`GPU::exists()`, `VulkanKernel::available()`). With a device, `VulkanContext::buffer_placement()` is the placement whose slabs are zero-initialized, persistently mapped, coherent storage and transfer buffers; `VulkanContext::device_address(slice)` returns the device address of a Slice's first byte, and `VulkanContext::gpu_usage()` reports the bytes held in Vulkan slabs. Byte offsets from Slices are not a promise of descriptor-offset alignment, so callers must observe their device's binding requirements. Keep Slices alive until submitted GPU work finishes, and establish CPU/GPU synchronization in the consuming application.

> **REVIEW-STALE (2026-09-26):** `CudaAllocator` and `MetalAllocator` are declared in `alligator.hpp` with `register_type`, `device_address` or `buffer` and `buffer_offset`, and `memory_usage()`, and their sources exist as `src/memory/cuda_allocator.cpp` and `src/memory/metal_allocator.mm`, but neither file is in the CMake source list, so neither backend is compiled or tested. Decide whether they return to the build or leave the tree; the table below describes the declared surface only.

| Placement | Registration | Native access | Allocation |
| --- | --- | --- | --- |
| CUDA | `CudaAllocator::register_type(CUcontext, CudaMemoryKind)` | `device_address(slice)` | Managed memory by default; explicitly selectable mapped pinned host memory |
| Metal | `MetalAllocator::register_type(void* device)` | `buffer(slice)`, `buffer_offset(slice)` | Zero-initialized shared `MTLBuffer` |

These placements use the existing allocation and reclamation callbacks. They do not add pressure policy, relocation, GPU submissions, or changes to the arena chain and containers.

### Memory usage

Detailed allocation-location tracking is controlled at compile time and disabled by default:

```sh
./run_build.sh -DBUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING=ON
```

The internal `Memory` tracker always counts completed backing slab allocations and deallocations
globally and per placement, including dedicated buffers; Slice copies and views do not add
allocations. The optional switch adds live records containing the allocation site's file, line,
function, timestamp, byte count, and placement, queried with `Memory::allocation_info(plate)`.
Disabling it removes the record map and its locking; the same API remains callable, detail hooks
become no-ops, and detail queries return no value.

```cpp
#include <alligator.hpp>

const buffetalligator::HostMemoryUsage usage = buffetalligator::BuffetMenu::memory_usage();
// usage.physical_bytes, usage.available_bytes, usage.resident_bytes
```

This explicit query reports bytes and does not run on Slice access or allocation. On Linux, physical and available memory come from `/proc/meminfo` (`MemTotal`, `MemAvailable`); process residency is the approximate `/proc/self/statm` resident-page count. On macOS, capacity comes from `hw.memsize`, available memory is an estimate using free plus inactive pages, and process residency comes from `MACH_TASK_BASIC_INFO`. These are host measurements, not container limits or live arena allocation totals; macOS available memory is not the system's memory-pressure classification.

`VulkanContext::gpu_usage()` reports the bytes currently held in Vulkan slabs. The declared but unbuilt CUDA and Metal backends also declare `memory_usage()` returning `DeviceMemoryUsage`, whose fields are optional bytes; see the review note above.

Allocator tests cover mmap writeback, retained views, chain advancement, worker cleanup, host statistics, and native Vulkan writes through Slice storage. Run them with `ctest --test-dir build -L allocators --output-on-failure`. They build with `BUFFETALLIGATOR_BUILD_ALLOCATOR_TESTS`, which follows the tests option, and the Vulkan case reports a skip when no device is present. The build exports `BUFFETALLIGATOR_HAS_VULKAN=1` to consumers.

## Containers

Every container hands out and stores `Slice` handles; payloads live in arena memory and the containers hold references.

- `SliceMap` and `SliceMapT<T>`: lock-free hash map from 64-bit identifiers to Slices with stable positions, hazard-protected replacement, publish hooks, merge, and reset.
- `SliceQueue`: producer and consumer handles that move Slices through thread-local blocks with semaphore wakeups.
- `PrioritySlice` and `PrioritySliceT<K, V>`: lock-free bounded priority queue in one Slice. Entries are 64-bit words, key bits above a 32-bit value; a push either takes a free slot or evicts the worst entry it beats, a header rejects hopeless pushes in constant time, and per-block caches confine each scan to one 32-slot block. Ordering is exact under push-only load and relaxed under mixed load.
- `HeapSlice` and `HeapSliceT<K, V>`: the same words as a single-owner min-max heap, popping the best and evicting the worst in logarithmic time.
- `SliceCache<Key>`: byte-budgeted least-recently-used cache of Slices built on Folly's implicitly weighted evicting map, every entry weighing its Slice's size.
- `SIMDMisc`: the identifier, maximum, and minimum scan kernels the containers use, with backends for Apple simd, AVX-512, AVX2, SSE4.2, VSX, and NEON.

Folly itself is linked publicly, so its containers, `RelaxedConcurrentPriorityQueue` among them, are available to consumers through `alligator::alligator`.

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

The script updates threadsafe-logger to its main branch, then prepares libuv, libsodium, libfabric, the moodycamel queues, Abseil, Folly, simdjson, curl, the Vulkan headers and runtime, and shaderc under `deps`, then builds the library, runs its contract tests, and installs into `build/install`. Folly's system libraries (Boost components, glog, gflags, fmt, double-conversion, libevent, zstd, lz4, snappy) are checked first and offered for installation through Homebrew or apt. Useful options are `--skip-tests`, `--clean`, `--rebuild-vendored`, `--deps-only`, and `-DNAME=VALUE` passthrough to CMake; `--help` lists the rest. See `THIRD_PARTY_NOTICES.md` for dependency notices.

### Functional validation

The suite covers placement registration and allocation, slice ownership and resizing, typed and weak slices, atomic values and registries, shared mutexes, reusable barriers, slice queues, and slice maps. It also checks lookup across vector lanes and a pipeline that copies borrowed input into a queue, transforms it, and gathers typed results in a map.

Functional cases run independently with release-build assertions and time limits. Concurrent cases check exact update counts, caller-synchronized map access, retained Slice ownership after map reset, and repeated barrier phases; exception cases check that failed operations leave containers and registries usable. After building, run this group separately with:

```sh
ctest --test-dir build -L functional --output-on-failure
```

The priority queue, heap, cache, SIMD kernel, and Folly container tests run in the ordinary suite; the priority queue test includes sixteen-pusher exact trimming and exact-once accounting under concurrent pushers and poppers.

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

The exported config resolves Folly from the `deps/folly` prefix recorded at configure time and Folly's system libraries from the machine, so a consumer needs the same Homebrew or apt packages the build script checks for.

Concurrent claims and independent `Slice` handles are supported. Concurrent mutation of the same `Slice` object requires external synchronization.

## License

Apache License 2.0. See `LICENSE`.
