# CUDA GLSL compiler gate

The 2026-09-30 compiler experiment failed for the actual production Slice prelude.
The observed compiler was `/usr/local/bin/slangc`, version
`2026.13.1-1-g84792eb15`, on macOS ARM64; this records the installed executable and does
not establish a project dependency pin.

| Source | CUDA source target | PTX target |
| --- | --- | --- |
| Minimal GLSL compute shader | Passed, exit 0 | Failed, exit 255: NVRTC missing |
| Production public-list prelude and vector kernel | Failed, exit 255: `buffer_reference` | Same frontend failure |
| Production job-table prelude and vector kernel | Failed, exit 255: `buffer_reference` | Same frontend failure |

Both `-lang glsl` and `-lang glsl -allow-glsl` produced the same results. The first diagnostic
for each complete prelude is `E31217: unrecognized GLSL layout qualifier` at
`layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer SliceRef`.
All ten buffer-reference declarations receive the same diagnostic, before CUDA lowering.
The positive control proves that the compiler accepts the selected GLSL input mode.

The minimal PTX control separately reports `E00100: failed to load downstream compiler
'nvrtc'` and `E52002: pass-through compiler not found`; `slangc -nvrtc-version` reports
`not found`. No NVIDIA runtime or configured NVIDIA host was found in this project's context,
so GPU execution was not attempted. Passing frontend translation would still require PTX
compilation and repeated ABI execution on an actual NVIDIA device.

Current official documentation recommends GLSL file extensions or `-lang glsl`, and describes
`-allow-glsl` as a deprecated compatibility spelling. See the
[Slang compilation guide](https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/08-compiling.html)
and [Slang CLI reference](https://docs.shader-slang.org/en/stable/external/slang/docs/command-line-slangc-reference.html).
The [CUDA target guide](https://github.com/shader-slang/slang/blob/master/docs/cuda-target.md)
describes NVRTC as the downstream PTX compiler.

Reproduce from the repository root:

```sh
mkdir -p build/cuda-glsl-gate
set -o pipefail
bash tests/experiments/cuda_glsl_gate/run_gate.sh 2>&1 | tee build/cuda-glsl-gate/complete.log
```

Set `SLANGC` to select an explicitly chosen compiler executable. The script extracts the current
production GLSL core and both tails, reuses the Metal experiment's vector test body, and preserves
every command, source, diagnostic, and exit status under `build/cuda-glsl-gate`. It returns a
nonzero status when a compilation case fails. The complete observed output is in
`build/cuda-glsl-gate/complete.log`; individual diagnostics and status files identify each case.

Phase 4 remains blocked at its specified compiler gate; no replacement translation route or
production CUDA backend was introduced.

## Stock SPIR-V alternatives

The same production public-list and job-table SPIR-V artifacts used by the successful Metal
gate were checked on 2026-09-30 with installed SPIRV-Cross `vulkan-sdk-1.4.357.0` and Homebrew
LLVM `22.1.2`; these observed versions are not proposed dependency pins. The C++ probe uses the
installed CLI, separately from the pinned SPIRV-Cross library in the production Metal backend.

| Route | Observed result for both production variants |
| --- | --- |
| SPIRV-Cross C++ | Emission returns 0; emitted C++ fails syntax on incomplete physical-reference structs and invalid `struct _176(address)` expressions, as well as a push-constant name error. |
| MLIR SPIR-V import, default | Fails control-flow structurization because a block is used outside its enclosing construct. |
| MLIR SPIR-V import, structurization disabled | Passes and preserves `spirv.ConvertUToPtr` operations and physical-storage-buffer pointer types. |
| MLIR SPIR-V to LLVM dialect | Fails to legalize the `PushConstant` `spirv.GlobalVariable`; no LLVM IR or PTX is produced. |

LLVM's installed `llc` includes NVPTX targets, but that does not supply the missing input
lowering. The [MLIR conversion documentation](https://github.com/llvm/llvm-project/blob/main/mlir/docs/SPIRVToLLVMDialectConversion.md)
also describes incomplete conversion and currently ignores pointer storage classes. The
[Khronos SPIR-V LLVM translator](https://github.com/KhronosGroup/SPIRV-LLVM-Translator/blob/main/README.md)
supports modules with the OpenCL `Kernel` capability; these production modules instead use
`Shader`, `GLCompute`, and `PhysicalStorageBuffer64`. No `llvm-spirv` executable is installed here.

The successful MSL output retains Metal address spaces, entry attributes, builtins, and vector
library operations. It is therefore not directly CUDA C++; adapting it would require another
translator and ABI validation. SPIRV-Cross's [CUDA backend request](https://github.com/KhronosGroup/SPIRV-Cross/issues/1602)
remains open. These checks establish that the stock routes tested here do not complete the
gate; they do not establish that implementing a new compiler route is impossible.

Reproduce after running the Metal compiler gate:

```sh
mkdir -p build/cuda-glsl-alternatives
set -o pipefail
bash tests/experiments/cuda_glsl_gate/alternatives.sh 2>&1 | tee build/cuda-glsl-alternatives/complete.log
```

The script accepts artifact and SPIR-V input directories as its first two arguments, and
`SPIRV_CROSS`, `LLVM_BIN`, and `GLM_INCLUDE` select observed tools and C++ support headers.
It preserves generated sources, diagnostics, and exit statuses. The separate
`tests/experiments/slang_pointer_bridge` investigation tests a native-pointer prelude with unchanged
application GLSL; its results must be distinguished from accepting the original Vulkan prelude.

Independent review of that bridge's `typed_shader.entry-params.cu` and reflection JSON confirms
a by-value `AlligatorPush` kernel parameter, size 16 with two `uint64` members at offsets 0 and 8,
and a 16-by-4-by-1 threadgroup. The generated invocation context owns its push, and the module
has no `SLANG_globalParams` constant. This avoids the shared mutable binding found in the first
bridge attempt and follows Slang's documented
[entry-point uniforms for compute shaders](https://docs.shader-slang.org/en/stable/parameter-blocks.html).
The emitted Slice ID is 4 bytes; GPUBuf fields remain a 64-bit address plus two 32-bit granule
values, with unchanged directory decoding and 64-byte shifts. These are source and reflection
checks, not NVRTC, PTX, or concurrent NVIDIA execution results.

The initial unchanged vector, typed, boundary, and L2 bodies transitively call 27 of the 69
production core helpers. A subsequent `all_helpers.glsl` experiment makes the previously
unreached signed subword, scalar and packed fp16/bf16/fp8, and half-vector widening functions
reachable with `VULKAN_FLOAT16=1`; independent inspection finds all 69 core helper names in
its emitted CUDA. This closes helper emission coverage, while numerical execution remains
unproven on NVIDIA hardware.
The native atomic substitutions inspected here emit CUDA `atomicAnd` and `atomicOr`, preserving
the production bitwise operations, while the reduction's barrier emits `__syncthreads`.
