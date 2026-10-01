#!/usr/bin/env bash
set -euo pipefail
gate_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
gate_artifacts="${1:-${gate_root}/build/slang-pointer-bridge}"
ruby "${gate_root}/tests/experiments/slang_pointer_bridge/generate.rb" "${gate_artifacts}"
ruby "${gate_root}/tests/experiments/slang_pointer_bridge/compile_matrix.rb" "${gate_artifacts}"
