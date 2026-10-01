#!/usr/bin/env bash
set -euo pipefail
gate_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
gate_artifacts="${1:-${gate_root}/build/cuda-glsl-alternatives}"
gate_inputs="${2:-${gate_root}/build/gpu-plan/metal-glsl-artifacts}"
gate_cross="${SPIRV_CROSS:-spirv-cross}"
gate_llvm="${LLVM_BIN:-/opt/homebrew/opt/llvm/bin}"
gate_glm="${GLM_INCLUDE:-/opt/homebrew/opt/glm/include}"
mkdir -p "${gate_artifacts}"
"${gate_cross}" --revision
"${gate_llvm}/mlir-translate" --version
"${gate_llvm}/llc" --version
gate_failed=0
for gate_source in public-list job-table; do
    for gate_stage in cpp cpp-syntax mlir-structured mlir-unstructured llvm-dialect; do
        case "${gate_stage}" in
            cpp)
                gate_command=("${gate_cross}" "${gate_inputs}/${gate_source}.spv"
                    --cpp --output "${gate_artifacts}/${gate_source}.cpp") ;;
            cpp-syntax)
                gate_command=("${gate_llvm}/clang++" -std=c++17 -fsyntax-only -ferror-limit=6
                    -I"${gate_root}/deps/src/MoltenVK/External/SPIRV-Cross/include"
                    -I"${gate_glm}" "${gate_artifacts}/${gate_source}.cpp") ;;
            mlir-structured)
                gate_command=("${gate_llvm}/mlir-translate" --deserialize-spirv
                    "${gate_inputs}/${gate_source}.spv"
                    -o "${gate_artifacts}/${gate_source}.structured.mlir") ;;
            mlir-unstructured)
                gate_command=("${gate_llvm}/mlir-translate" --deserialize-spirv
                    --spirv-structurize-control-flow=false "${gate_inputs}/${gate_source}.spv"
                    -o "${gate_artifacts}/${gate_source}.unstructured.mlir") ;;
            llvm-dialect)
                gate_command=("${gate_llvm}/mlir-opt" --convert-spirv-to-llvm
                    "${gate_artifacts}/${gate_source}.unstructured.mlir"
                    -o "${gate_artifacts}/${gate_source}.llvm.mlir") ;;
        esac
        printf '\nCommand:'
        printf ' %q' "${gate_command[@]}"
        printf '\n'
        gate_status=0
        "${gate_command[@]}" 2>&1 | tee \
            "${gate_artifacts}/${gate_source}.${gate_stage}.diagnostics.txt" || gate_status=$?
        printf 'Compiler exit status: %s\n' "${gate_status}"
        printf '%s\n' "${gate_status}" > "${gate_artifacts}/${gate_source}.${gate_stage}.status.txt"
        if [[ "${gate_status}" != 0 ]]; then gate_failed=1; fi
    done
done
printf '\nAlternative compiler probes finished; these are not production dependency pins.\n'
exit "${gate_failed}"
