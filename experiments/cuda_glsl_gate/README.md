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
bash experiments/cuda_glsl_gate/run_gate.sh 2>&1 | tee build/cuda-glsl-gate/complete.log
```

Set `SLANGC` to select an explicitly chosen compiler executable. The script extracts the current
production GLSL core and both tails, reuses the Metal experiment's vector test body, and preserves
every command, source, diagnostic, and exit status under `build/cuda-glsl-gate`. It returns a
nonzero status when a compilation case fails. The complete observed output is in
`build/cuda-glsl-gate/complete.log`; individual diagnostics and status files identify each case.

Phase 4 remains blocked at its specified compiler gate; no replacement translation route or
production CUDA backend was introduced.
