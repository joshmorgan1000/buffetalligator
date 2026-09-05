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
#include <buffetalligator.hpp>

buffetalligator::Slice bytes(4096);
auto* values = bytes.data<uint32_t>();
```

Dedicated novel buffers remain available for long-lived claims:

```cpp
buffetalligator::Slice long_lived(1024 * 1024, true);
```

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
find_package(buffetalligator CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE buffetalligator::buffetalligator)
```

Concurrent claims and independent `Slice` handles are supported. Concurrent mutation of the same `Slice` object requires external synchronization.

## License

Apache License 2.0. See `LICENSE`.
