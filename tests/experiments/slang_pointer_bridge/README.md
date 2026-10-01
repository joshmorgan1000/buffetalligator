# Slang native-pointer bridge experiment

This experiment establishes a CUDA source-emission route for the current Slice shader ABI while preserving application GLSL bodies. It does not change production code or qualify CUDA execution.

Run from the repository root:

```sh
bash tests/experiments/slang_pointer_bridge/run_gate.sh
```

`SLANGC` selects the compiler. An optional first argument selects the artifact directory. The script extracts the current production helper strings, public/reference tails, application test bodies, and production L2 reduction example. It retains generated GLSL, CUDA, reflection JSON, source-body hashes, full diagnostics, exact commands, and exit statuses under `build/slang-pointer-bridge` by default.

Observed with `slangc 2026.13.1-1-g84792eb15` on 2026-09-30:

The installed `/usr/local/bin/slangc` executable has SHA-256
`8366c03cd29bd0e5d102c4e6413561c67ed520d7fc310251f78a9c5133600c8f`;
this records the experiment tool and does not establish a packaged dependency pin.

| Case | CUDA emission | Reflected launch ABI |
| --- | --- | --- |
| Unmodified production buffer-reference prelude | Fails: unsupported `buffer_reference` | Not produced |
| Actual public-list vector body | Pass | Pass |
| Actual job-table vector body | Pass | Pass |
| Production typed scalar/vector/packed test body | Pass | Pass |
| Production nested Slice/boundary test body | Pass | Pass |
| Production L2 reduction example | Pass | Pass |
| Production reference-ring test body | Pass | Pass |
| Every typed helper, float32 arithmetic | Pass | Pass |
| Every typed helper, including float16 widening | Pass | Pass |
| GLSL entry importing a native Slang pointer module | Pass | Pass |

The float16 case emits all 70 runtime helper functions: the current 69 production functions plus one native unpack adapter. The script fails if any helper is absent, any positive compile fails, reflection contains global parameters, or the emitted kernel loses its by-value launch parameter. Each positive case reports a 16-byte uniform entry parameter containing two uint64 fields at offsets 0 and 8 and workgroup dimensions 16×4×1.

The final PTX probe exits 255 because this host cannot load `nvrtc`. The script reports that status separately: its successful exit qualifies CUDA source emission only. NVIDIA compilation, execution, numerical results, concurrent dispatch, allocation residency, and error handling remain unqualified.

## Runtime-owned changes tested

The generator changes only the injected prelude and wrapper. Application bodies are appended directly from their current source files; the manifest records their SHA-256 values.

- Physical buffer-reference blocks become native pointer fields with Slang constructors that cast their uint64 address; the Slice reference constructor loads the same four-byte ID.
- The 64-entry directory lookup, ID decomposition, 16-byte `GPUBufRef`, 64-bit byte-address arithmetic, granule shifts, 4-byte stage IDs, 32-byte job records, reference-ring fields, and typed helper algorithms remain in the extracted production text.
- Runtime packed stores use `InterlockedAnd` and `InterlockedOr`, which emit CUDA `atomicAnd` and `atomicOr`; the original GLSL intrinsic spellings are rejected for CUDA by this compiler.
- The runtime reduction uses `GroupMemoryBarrierWithGroupSync`, which emits `__syncthreads`; its scratch array emits shared storage.
- The runtime half-unpack adapter explicitly converts `unpackHalf2x16` into `f16vec2`; Slang does not recognize the production `unpackFloat2x16` spelling.
- The runtime push block becomes a private per-invocation struct initialized from `main(uniform AlligatorPush parameters)`; the CUDA entry receives that struct by value.
- For the production job body that defines `main`, the wrapper temporarily maps that entry name to `alligator_application_main` with the preprocessor, then calls it after initializing the per-invocation push value.

Using a GLSL push-constant block with scalar layout also emits CUDA, but produces a module-global `SLANG_globalParams` symbol. Updating that symbol between overlapping dispatches would share the launch table. The tested by-value entry form removes the symbol entirely; launch-parameter reflection and emitted source are checked automatically.

The standalone `module_import.glsl` imports `bridge_native.slang` and calls a native pointer operation successfully. A module split is feasible, but the complete helper gate currently injects the native prelude directly, so it does not depend on a separate module-loading integration.

## Remaining production gate

On an NVIDIA qualification host, compile the emitted source with NVRTC for the probed device architecture and verify the complete current ABI with actual mapped allocations and repeated overlapping prepared slots. Reflection must drive launch binding rather than assuming a compiler-specific mangled entry or parameter convention. Run the unchanged public, job, reference, nested-region, packed-type, and reduction cases, then compare packed and half numerical outputs against the existing Vulkan/Metal results. Preserve compiler errors for user GLSL features that the CUDA target cannot represent; this experiment proves the exercised current helper surface, not arbitrary GLSL compatibility.

Slang documents native pointers for CUDA and provides a CUDA backend with NVRTC integration. Its module system supports importing exported declarations, and its parameter guidance recommends entry-point parameters for compute. These capabilities support the tested approach; the installed compiler's emitted source and reflection are the evidence for the exact ABI here. Sources: [native pointer types](https://docs.shader-slang.org/en/stable/external/slang/docs/language-reference/types-pointer.html), [CUDA target and NVRTC](https://docs.shader-slang.org/en/stable/external/slang/docs/cuda-target.html), [compute entry-point parameters](https://docs.shader-slang.org/en/stable/parameter-blocks.html), [module system](https://docs.shader-slang.org/en/stable/external/slang/docs/user-guide/04-modules-and-access-control.html).
