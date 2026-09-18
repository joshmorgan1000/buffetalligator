<div align="center">
  <img src="buffetalligator_logo.png" alt="Buffet Alligator Logo" width="150"/>
</div>

# BuffetAlligator

BuffetAlligator is a C++20 memory arena that serves 16-byte `Slice` handles from preallocated, zeroed slabs. Claims are 64-byte-granular atomic bump-pointer operations; exhausted slabs advance through successors prepared by one dedicated allocator thread.

The project is pre-release. API and ABI compatibility are not guaranteed until 1.0.

## Placemat

Each registered `Placemat` (placement) is a process-lifetime factory and owns one independent Buffer chain. A placement supplies only allocation, deallocation, context, and stable host-pointer access. Device addresses, transfers, and synchronization remain private to the consuming implementation.

BuffetAlligator includes basic heap and 64-byte-aligned heap placements. Aligned heap is the default. Applications may register additional placements before creating the first `Slice` and may install a default placement strategy method.

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

GPU buffer allocators are disabled by default while their integration is being migrated.
The normal build includes host and mmap allocations without GPU SDK or runtime dependencies.
To enable GPU allocators explicitly:

```sh
./run_build.sh -DBUFFETALLIGATOR_ENABLE_GPU_ALLOCATORS=ON
```

Register placements before creating the first Slice; each backend registers one process-lifetime placement and leaves the default placement unchanged.

```cpp
#include <alligator_allocators.hpp>

using namespace buffetalligator;
const Placemat* mapped = MmapAllocator::register_type("/path/to/scratch");
Slice bytes(4096, mapped);
bytes.data<uint64_t>()[0] = 42;
MmapAllocator::flush(bytes);
```

The mmap allocator reserves storage for each slab in the supplied directory and maps it with `MAP_SHARED`. Its temporary files are unlinked immediately and remain open until the existing arena reclamation callback releases the slab. Choose a directory on disk for disk-backed scratch storage; a RAM filesystem provides RAM-backed storage. Fresh slabs are zero-filled by the filesystem, without eagerly touching every mapped page. `file_descriptor(slice)` returns a borrowed slab descriptor, and `file_offset(slice)` includes sub-slice offsets. `flush(slice)` synchronously writes the covered pages back; these scratch files do not provide reopenable persistent storage.

| Placement | Registration | Native access | Allocation |
| --- | --- | --- | --- |
| CUDA | `CudaAllocator::register_type(CUcontext, CudaMemoryKind)` | `device_address(slice)` | Managed memory by default; explicitly selectable mapped pinned host memory |
| Metal | `MetalAllocator::register_type(void* device)` | `buffer(slice)`, `buffer_offset(slice)` | Zero-initialized shared `MTLBuffer` |
| Vulkan | `VulkanAllocator::register_type(VkPhysicalDevice, VkDevice)` | `buffer(slice)`, `buffer_offset(slice)` | Zero-initialized, persistently mapped coherent storage/transfer buffer |

CUDA declarations are in `<alligator_cuda.hpp>`, Vulkan declarations in `<alligator_vulkan.hpp>`, and Metal declarations in `<alligator_allocators.hpp>`. Pass a bridged `id<MTLDevice>` to Metal; its returned buffer is a borrowed, bridged `id<MTLBuffer>`. CUDA and Vulkan borrow the supplied context/device; those resources must outlive the Alligator thread and every arena, including retained chain slabs. Metal retains the supplied device. Keep Slices alive until submitted GPU work finishes, and establish CPU/GPU synchronization in the consuming application. Native accessors require a live Slice from their corresponding placement.

CUDA managed allocation requires concurrent managed access so background allocation and CPU access do not require stopping unrelated GPU work. The mapped-host mode requires host-mapping support. Vulkan requires API 1.1 or newer and probes compatible host-visible, coherent memory once, preferring device-local and then host-cached memory. Its buffers support storage and transfer usage; byte offsets from Slices are not a promise of descriptor-offset alignment, so callers must observe their device's binding requirements.

These placements use the existing allocation and reclamation callbacks. They do not add pressure policy, relocation, GPU submissions, or changes to the arena chain and SliceMap.

### Memory usage

Detailed allocation-location tracking is controlled at compile time and disabled by default:

```sh
./run_build.sh -DBUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING=ON
```

The internal `Memory` tracker always counts completed backing slab allocations and deallocations
globally and per placement, including dedicated buffers; Slice copies and views do not add
allocations. The optional switch adds live records containing the allocation site's file, line,
function, timestamp, byte count, and placement, queried with `Memory::allocation_info(handle)`.
Disabling it removes the record map and its locking; the same API remains callable, detail hooks
become no-ops, and detail queries return no value.

```cpp
#include <alligator.hpp>

const buffetalligator::HostMemoryUsage usage = buffetalligator::BuffetMenu::memory_usage();
// usage.physical_bytes, usage.available_bytes, usage.resident_bytes
```

This explicit query reports bytes and does not run on Slice access or allocation. On Linux, physical and available memory come from `/proc/meminfo` (`MemTotal`, `MemAvailable`); process residency is the approximate `/proc/self/statm` resident-page count. On macOS, capacity comes from `hw.memsize`, available memory is an estimate using free plus inactive pages, and process residency comes from `MACH_TASK_BASIC_INFO`. These are host measurements, not container limits or live arena allocation totals; macOS available memory is not the system's memory-pressure classification.

Each GPU allocator also exposes `memory_usage()` after registration, returning `DeviceMemoryUsage` with optional byte fields:

| Backend | Capacity | Available | Process usage | Budget |
| --- | --- | --- | --- | --- |
| CUDA | `cuMemGetInfo` total | `cuMemGetInfo` free | Unavailable | Unavailable |
| Metal | Unavailable | Unavailable | `currentAllocatedSize` | `recommendedMaxWorkingSetSize` |
| Vulkan | Selected memory heap size | Unavailable | `VK_EXT_memory_budget` heap usage, when supported | Extension heap budget, when supported |

Unavailable measurements are `std::nullopt`; driver budgets are estimates and are not interchangeable with physically free bytes. Host and device counts can overlap on unified-memory systems.

Allocator tests cover mmap writeback, retained views, chain advancement, worker cleanup, host statistics, and native GPU writes through Slice storage. Run them with `ctest --test-dir build -L allocators --output-on-failure`. GPU tests are built only when `BUFFETALLIGATOR_ENABLE_GPU_ALLOCATORS=ON`; CUDA additionally requires the CUDA Toolkit. GPU tests report a skip when the required device or memory capability is absent. Within the GPU opt-in, backend switches are `BUFFETALLIGATOR_ENABLE_CUDA`, `BUFFETALLIGATOR_ENABLE_METAL`, and `BUFFETALLIGATOR_ENABLE_VULKAN`; available backends export corresponding `BUFFETALLIGATOR_HAS_*` definitions.

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

## Build

BuffetAlligator requires a C++20 compiler, CMake 3.20 or newer, Git, and a platform threading library. `run_build.sh` is the supported one-shot build on macOS and Linux; it uses Ninja when available.

```sh
./run_build.sh
```

The script prepares threadsafe-logger, libuv, libsodium, and libfabric under `deps`, then builds the library and runs its contract tests. Vulkan dependencies are prepared only when GPU allocators and the Vulkan backend are enabled. Each script invocation defaults GPU allocators off, including when reusing a build directory. See `THIRD_PARTY_NOTICES.md` for dependency notices.

### Functional validation

The suite covers placement registration and allocation, slice ownership and resizing, typed and weak slices, atomic values and registries, shared mutexes, reusable barriers, slice queues, and slice maps. It also checks lookup across vector lanes and a pipeline that copies borrowed input into a queue, transforms it, and gathers typed results in a map.

Functional cases run independently with release-build assertions and time limits. Concurrent cases check exact update counts, caller-synchronized map access, retained Slice ownership after map reset, and repeated barrier phases; exception cases check that failed operations leave containers and registries usable. After building, run this group separately with:

```sh
ctest --test-dir build -L functional --output-on-failure
```

### Container benchmarks

Run the map comparison with `build/tests/benchmarks/buffetalligator_slicemapvsvecset`.
It checks correctness before timing VecSet, SliceMap, HazardMap, an immutable-version
candidate, and mutex-protected `std::unordered_map`, then prints a final summary table.

The standalone build also produces SliceMap versus `std::unordered_map` and
SliceQueue versus `std::deque` benchmarks in `build/tests/benchmarks/`.
They include single-thread baselines and multiple producer/consumer teams with
mutex-protected standard containers, warmups, repeated timings, correctness
checks, and optional CSV output. See [the benchmark guide](tests/benchmarks/README.md)
for commands and the current containers' concurrency limits.

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

Concurrent claims and independent `Slice` handles are supported. Concurrent mutation of the same `Slice` object requires external synchronization.

## License

Apache License 2.0. See `LICENSE`.
