# GPU backend and Slice plan execution

Started 2026-09-30 from the existing working tree on `slice-efficiency`.
The implementation and qualification plan is **not complete**.

## Decisions applied during core implementation

Josh authorized taking over the Slice and Shader fixes after the initial audit.
He then explicitly directed that no separate exact-byte metadata be retained.
GPUBuf remains 16 bytes, with `uint32_t` size and offset counts in 64-byte units.
Allocation sizes round up; sub-slice starts round down and ends round up to cover
the requested range within the parent. Public sizes describe these whole granules.
The temporary byte-bound type, per-region array, and reader/writer hooks have been
removed rather than relocated into another structure.

The core ownership, region publication, Shader completion, and affected tests now
pass the first integrated Release build under this contract.

## First passing integration baseline

On 2026-09-30, `run_build.sh` completed the Release build, all **66 CTest cases**,
and installation on Apple M4 Max (16 CPU threads, 128 GiB RAM, macOS 26.6.2,
AppleClang 17, ARM64). All **13 GPU cases executed and passed** through MoltenVK;
the relocated installed consumer and networking process-exit scenarios passed too.
CTest elapsed time was 17.49 seconds; this is correctness timing, not a benchmark.

The command remains the one recorded below. Host access was needed for GPU
discovery and loopback binds; sandbox skips were not counted as GPU qualification.
The exact tracked patch, new files, build/test transcript, compiler commands, and
CMake cache are archived under `build/gpu-plan/baseline/` with a manifest.

Sanitizers, the native translation gates, performance qualification, and the
remaining hardware matrix are subsequent work; this milestone does not complete
the entire plan.

## Changes prepared

- SliceMap keeps resize ownership until every bucket split completes and no longer
  allocates the replacement table twice; a controlled overlapping-growth regression
  checks production lookup, row identity, and final payload destruction.
- PriorityT releases destination payload ownership before move assignment and
  preserves self-move; a regression checks each displaced payload's release and
  the transferred payloads through source destruction, pop, and final destruction.
- Arena destruction stops and joins networking before releasing regions; concurrent
  shutdown callers share a completed join, and reactor callbacks can initiate shutdown
  without joining themselves; a fresh-process regression covers both startup orders
  and active callbacks.
- Public headers are installed as a complete directory, the umbrella respects
  backend feature definitions, and split-header dependencies are explicit in
  affected tests and benchmarks.
- The portable GPU and core headers no longer include the Vulkan SDK; SDK types stay
  behind the explicit Vulkan interface header.
- An installed-consumer CTest fixture installs to a temporary prefix, relocates it,
  compiles each installed header, verifies backend definitions and original-path
  exclusion, and links/runs a production ownership check with `NDEBUG` enabled.
  The new map, network, and packaging tests have explicit labels and timeouts.

The build script remains unchanged: full live output and its transcript log are
preserved as requested by Josh during execution.

## Verification

Command used for the first current-source integration attempt:

```sh
./run_build.sh --no-color --build-dir build/gpu-plan \
    --install-dir build/gpu-plan/install -DCMAKE_BUILD_TYPE=Release
```

Configuration succeeded with AppleClang 17 on macOS ARM64. Compilation failed on
the unfinished Slice port, Shader declaration/definition mismatch, and stale
call sites. The build did not reach CTest, installation, or performance runs.
This attempt is not a correctness or performance baseline.

The same command was rerun after the independent changes: configuration again
succeeded, and the modified networking, arena, map, and priority implementation
objects compiled. Integration remains blocked by removed `Plate`/`HostPtr`/`SliceId`
references, missing `memory/plate.hpp` includes in old tests, ambiguous Slice
constructors, the removed `gpu_table_address` call, and the Shader return mismatch.
Full output remains in `build/gpu-plan/build_buffetalligator.log`; the initial
attempt is preserved in `build/gpu-plan/initial-build-attempt.log`.

Syntax checks passed for all ten public headers with the configured Vulkan
feature definition, and for the new controlled SliceMap and network-exit regressions
against production interfaces. The installed-consumer fixture passed CMake
configuration/generation in an isolated imported-target syntax harness, without
claiming an actual installed library link or run. Native Metal/CUDA-enabled headers, runtime
behavior, sanitizers, installed-consumer execution, and performance remain
unverified. `bash -n run_build.sh` and `git diff --check` passed.

## Core implementation verified by the first integration pass

- Slice default and moved-from states are null, copies publish complete metadata in
  the destination region, and views round outward without byte-bound side records.
- Dedicated allocations use their own device address; counted controls protect
  in-progress claims, caller-owned chains, retired slabs, and prepared successors.
- Publication failures restore retryable states, and region objects and entry tokens
  now have established C++ lifetimes and alignment.
- The stable GPU directory and region records share the selected metadata placement;
  heap and mmap allocations report no GPU address.
- SharedBuffet and placement accessors are ported, HeapSlice definitions are added,
  and priority/heap capacity survives storage adoption despite granule padding.
- Kitchen tracks accepted tasks through callback return, drains before teardown,
  and fixes countdown lifetime and zero hardware-concurrency underflow.
- Deterministic regressions cover granule boundaries, dedicated releases, factory
  retries, concurrent rollover, region failure/retry, and copying across regions.

Shader callback and coroutine completion, retained identities and embedded
dependencies, independent submission slots, portable program storage, and Vulkan
allocation checks passed the current GPU regression cases. Native backends and
the performance qualification matrix remain open; no native or performance
success is claimed.
