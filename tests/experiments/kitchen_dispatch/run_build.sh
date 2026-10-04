#!/usr/bin/env bash
set -euo pipefail
EXPERIMENT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd "${EXPERIMENT_ROOT}/../../.." && pwd)"
EXPERIMENT_BUILD="${1:-${REPOSITORY_ROOT}/build/kitchen-dispatch}"
EXPERIMENT_RESULTS="${2:-${EXPERIMENT_BUILD}/results}"
"${REPOSITORY_ROOT}/run_build.sh" --skip-tests --build-dir "${EXPERIMENT_BUILD}" \
    --install-dir "${EXPERIMENT_BUILD}/install" \
    -DBUFFETALLIGATOR_BUILD_TESTS=OFF -DBUFFETALLIGATOR_BUILD_ALLOCATOR_TESTS=OFF \
    -DBUFFETALLIGATOR_BUILD_BENCHMARKS=OFF \
    -DCMAKE_PROJECT_alligator_INCLUDE="${EXPERIMENT_ROOT}/register.cmake"
ctest --test-dir "${EXPERIMENT_BUILD}" --output-on-failure
ruby "${EXPERIMENT_ROOT}/run.rb" "${EXPERIMENT_BUILD}" "${EXPERIMENT_RESULTS}"
