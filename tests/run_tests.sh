#!/usr/bin/env bash
# Builds and runs isolated unit tests while preserving complete failure diagnostics.
set -uo pipefail
test_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
test_build="$test_root/build/current"
test_output=""
test_filter=""
test_build_enabled=true
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-build) test_build_enabled=false; shift ;;
        --build-dir|--output-dir|--test)
            if [[ $# -lt 2 || -z "$2" ]]; then
                printf 'Missing value for %s; run bash tests/run_tests.sh --help.\n' "$1" >&2
                exit 2
            fi
            case "$1" in
                --build-dir) test_build="$2" ;;
                --output-dir) test_output="$2" ;;
                --test) test_filter="$2" ;;
            esac
            shift 2 ;;
        --help)
            printf '%s\n' 'Usage: bash tests/run_tests.sh [--no-build] [--test REGEX]' \
                '       [--build-dir DIR] [--output-dir DIR]' \
                'Builds with run_build.sh, runs every selected test, and returns nonzero on failure.' \
                'A custom build directory requires --no-build; results default to build/test-results.'
            exit 0 ;;
        *) printf 'Unknown option: %s; run bash tests/run_tests.sh --help.\n' "$1" >&2; exit 2 ;;
    esac
done
if [[ "$test_build_enabled" == true ]]; then
    if [[ "$test_build" != "$test_root/build/current" ]]; then
        printf 'Use --no-build with an already configured --build-dir, or omit --build-dir.\n' >&2
        exit 2
    fi
    if ! (cd "$test_root" && ./run_build.sh --skip-tests -DBUFFETALLIGATOR_BUILD_TESTS=ON); then
        printf 'Unit tests could not build. Compiler diagnostics: %s/build/build_buffetalligator.log\n' \
            "$test_root" >&2
        exit 1
    fi
fi
if ! command -v ctest >/dev/null 2>&1; then
    printf 'CTest is missing. Install CMake, then rerun bash tests/run_tests.sh.\n' >&2
    exit 2
fi
if [[ ! -f "$test_build/CTestTestfile.cmake" ]]; then
    printf 'No configured tests in %s. Run bash tests/run_tests.sh to build them.\n' "$test_build" >&2
    exit 2
fi
test_build="$(cd -- "$test_build" && pwd)"
test_output="${test_output:-$test_root/build/test-results}"
mkdir -p "$test_output" || exit 2
test_output="$(cd -- "$test_output" && pwd)"
test_arguments=(--test-dir "$test_build" --output-on-failure --no-tests=error
    --parallel 1 --timeout 120 --output-junit "$test_output/results.xml")
if [[ -n "$test_filter" ]]; then test_arguments+=(-R "$test_filter"); fi
test_heartbeat=""
test_process=""
cleanup() {
    if [[ -n "$test_heartbeat" ]]; then kill "$test_heartbeat" 2>/dev/null || true; fi
}
interrupted() {
    printf '\nTest run interrupted; the suite has not passed.\n' >&2
    if [[ -n "$test_process" ]]; then kill -TERM "$test_process" 2>/dev/null || true; fi
    exit 130
}
trap cleanup EXIT
trap interrupted INT TERM
printf 'Running isolated unit tests; failures will not stop the remaining tests.\n'
ctest "${test_arguments[@]}" > "$test_output/tests.log" 2>&1 &
test_process=$!
(
    test_elapsed=0
    while sleep 1; do
        test_elapsed=$((test_elapsed + 1))
        printf '[running %ss] %s\n' "$test_elapsed" "$(tail -n 1 "$test_output/tests.log")"
    done
) &
test_heartbeat=$!
test_status=0
wait "$test_process" || test_status=$?
test_process=""
cleanup
test_heartbeat=""
cat "$test_output/tests.log"
printf '\nFull test log: %s/tests.log\nJUnit report: %s/results.xml\n' "$test_output" "$test_output"
if [[ "$test_status" != 0 ]]; then
    printf 'FAILED: one or more tests failed, crashed, timed out, or could not run (CTest status %s).\n' \
        "$test_status" >&2
    printf 'Each failure above includes its test name and captured output.\n' >&2
    printf 'Rerun failed tests with: ctest --test-dir %q --rerun-failed --output-on-failure\n' \
        "$test_build" >&2
    exit 1
fi
printf 'All executed unit tests passed; any unavailable tests are listed separately by CTest.\n'
