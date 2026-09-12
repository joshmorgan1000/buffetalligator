#!/usr/bin/env bash
# Runs the unchanged library's tests and benchmarks with separate evidence files.
set -uo pipefail
audit_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
audit_output="${1:-$audit_root/build/arena-audit}"
mkdir -p "$audit_output"
audit_output="$(cd -- "$audit_output" && pwd)"
cd "$audit_root" || exit 1
if ! ./run_build.sh --skip-tests -DBUFFETALLIGATOR_BUILD_TESTS=ON; then
    printf 'The audit could not build; inspect build/build_buffetalligator.log.\n' >&2
    exit 1
fi
{
    uname -a
    if [[ "$(uname -s)" == Darwin ]]; then
        sysctl machdep.cpu.brand_string hw.memsize hw.physicalcpu hw.logicalcpu
        sw_vers
    else
        lscpu
    fi
    c++ --version
    sed -n '/CMAKE_BUILD_TYPE:/p; /CMAKE_C_COMPILER:/p; /CMAKE_CXX_COMPILER:/p; /CMAKE_C_FLAGS:/p; /CMAKE_CXX_FLAGS:/p' build/current/CMakeCache.txt
    git rev-parse HEAD
} > "$audit_output/environment.txt" 2>&1
audit_failures=0
run_stage() {
    local label="$1" output="$2" errors="$3"
    shift 3
    printf '%s...\n' "$label" >&2
    "$@" > "$audit_output/$output" 2> "$audit_output/$errors" &
    local process=$! elapsed=0 status=0
    while kill -0 "$process" 2>/dev/null; do
        printf '\r%s (%ss)...' "$label" "$elapsed" >&2
        sleep 1
        elapsed=$((elapsed + 1))
    done
    wait "$process" || status=$?
    if [[ "$status" == 0 ]]; then
        printf '\r%s: complete (%ss).\n' "$label" "$elapsed" >&2
    elif [[ "$status" == 77 ]]; then
        printf '\r%s: unavailable on this machine.\n' "$label" >&2
    else
        printf '\r%s: reported failure; see %s/%s and %s.\n' "$label" "$audit_output" "$output" "$errors" >&2
        audit_failures=$((audit_failures + 1))
    fi
}
run_stage 'Testing library and arena contracts' contracts.txt contracts_errors.txt \
    ctest --test-dir build/current --verbose
for audit_case in layout references prepared_chain alignment registry_commitment; do
    run_stage "Observing $audit_case" "observation_$audit_case.txt" "observation_${audit_case}_errors.txt" \
        build/current/tests/audit/buffetalligator_audit_contracts "$audit_case"
done
run_stage 'Measuring arena and queue operations' arena.csv arena_progress.txt \
    build/current/tests/audit/buffetalligator_audit_performance 3
run_stage 'Measuring separate-process network exchanges' network.csv network_progress.txt \
    build/current/tests/audit/buffetalligator_audit_network_performance 3
run_stage 'Measuring Vulkan Slice transfers' vulkan.csv vulkan_progress.txt \
    build/current/tests/audit/buffetalligator_audit_vulkan_performance
printf 'Evidence saved to %s; %s stages reported failures.\n' "$audit_output" "$audit_failures" >&2
[[ "$audit_failures" == 0 ]]
