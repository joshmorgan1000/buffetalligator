<div align="center">
  <img src="buffetalligator_logo.png" alt="Buffet Alligator Logo" width="150"/>
</div>

# BuffetAlligator

BuffetAlligator is a C++20 memory arena backed by a private C11 core. Its 16-byte `Slice` handles reference zeroed regions of preallocated slabs. Ordinary claims bump a thread-local plate cursor without atomic operations; reference counting happens at plate granularity.

The project is pre-release. API and ABI compatibility are not guaranteed until 1.0.

## Placemat

A registered `Placemat` is a process-lifetime memory source with stable host-pointer access. `heap` has identifier 0, `aligned_heap` has identifier 1 and is the default, and custom identifiers start at 2. Both built-ins use anonymous OS pages. Every claim has a host pointer aligned to at least 64 bytes and reads as zero when returned. Sub-slice views retain the original allocation and can begin at an arbitrary byte offset.

Slabs are divided into plates. Each thread bumps within its own plate; sealing the plate publishes its issued claim count, and thread exit seals its remaining plates. Larger claims receive direct plates or dedicated novel allocations. Holding a small slice keeps its plate and parent slab alive.

One worker prepares a runway of slabs, recycles retired slabs into a bounded free list, zeroes reusable memory, and releases excess capacity. Runtime page size, available memory, process limits, hardware threads, and measured consumption determine slab geometry and runway depth. A nonzero requested slab size is rounded to the OS granule and honored without a fixed minimum; zero requests runtime sizing. A runway miss builds a slab on the caller, subject to its budget.

Registration waits for the worker to prepare the first current slab and runway. Register custom placements before starting concurrent use. Callbacks can run concurrently on callers and the worker. The allocator must return zeroed memory with the declared alignment, wrapped in a newly allocated `Placemat::Handle`; deallocation releases the substrate, and the framework deletes the handle. Names and callback context must remain valid for the placement's lifetime. Device addresses, transfers, and synchronization belong to the consuming implementation.

OS-page placements share a global capacity ceiling derived from three quarters of startup headroom. Each placement also has its own budget. Custom placements can supply a budget or a capacity query; without either they are unlimited. Allocations that exceed a budget throw `AlligatorException`.

Small OS-backed novel allocations round to the base page size; eligible requests at least one large page retain large-page backing. Slab geometry is unchanged. The shared Slice layout provides 2,097,151 reusable backing slots per process, covering simultaneously live regions rather than total claims or stored rows. Copies and sub-slices share a slot, and worker retirement returns it for reuse. The layout changed from 17 to 21 slot bits; rebuild the library and all consumers together.

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

BuffetAlligator requires a C++20 compiler, CMake 3.20 or newer, Git, and a platform threading library. `run_build.sh` is the supported one-shot build on macOS and Linux; it uses Ninja when available.

```sh
./run_build.sh
```

The script checks out [threadsafe-logger](https://github.com/joshmorgan1000/threadsafe-logger) at commit `52588cec8fda78ffa5af31b8479b4e97a9417de8` under `deps/src`, builds both libraries statically, and runs the contract tests. The dependency is MIT-licensed; see `THIRD_PARTY_NOTICES.md`.

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

## License

Apache License 2.0. See `LICENSE`.
