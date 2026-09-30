# Native Metal GLSL translation gate

This isolated experiment runs the current production public-list and job-table GLSL preludes
through shaderc, the SPIRV-Cross revision pinned by MoltenVK, and the native Metal compiler.
It is a correctness gate for backend integration, not a performance benchmark or backend.
It does not install or export any targets and does not change the library's selected backend.

From the repository root, build through the normal entry point:

```sh
./run_build.sh --build-dir build/gpu-plan \
  -DCMAKE_PROJECT_INCLUDE="$PWD/experiments/metal_glsl_gate/register.cmake"
```

Run with Metal API and shader validation, preserving the complete output:

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  ./build/gpu-plan/buffetalligator_metal_glsl_gate \
  ./build/gpu-plan/metal-glsl-artifacts 2>&1 | tee ./build/gpu-plan/metal-glsl-gate.log
```

The artifact directory contains the complete GLSL, SPIR-V, generated MSL, shaderc diagnostics,
Metal compiler diagnostics, and pipeline diagnostics for both formats. The executable reports
every checked output row and fails with a nonzero exit status if compilation, execution, or a
Release-enabled assertion fails. Check the executable's status when using a shell pipeline.

The inputs cover two directory entries (regions 3 and 37), nested Slice identifiers, nonzero
64-byte offsets, 64-byte lengths, floating and integer vectors, 64-bit addresses, null Slice
recognition, live dispatch counts, and the reflected 16-byte push block mapped to buffer 0.
Each pipeline runs eight times with changed input and parameter contents; every output and
untouched guard is checked after completion. All indirectly accessed native buffers are retained
and declared with `useResources:count:usage:` for each encoder.

The experiment deliberately uses native shared MTLBuffers rather than the unfinished production
Metal allocator. A passing result establishes translation and ABI feasibility on the logged
device, operating system, pinned compiler revision, and MSL target. It does not qualify native
allocator ownership, asynchronous completion, residency-set management, other GPU families,
or production backend selection.
