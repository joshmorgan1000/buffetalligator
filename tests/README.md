Run the unit tests with:

```sh
bash tests/run_tests.sh
```

The runner builds through `run_build.sh`, executes tests in separate processes, shows progress every second, and continues after failures. It returns **1** if a test fails, crashes, times out, or the selection contains no tests. Configuration errors return **2**. Unavailable capabilities are reported as skipped rather than silently counted as coverage. CTest 3.21 or newer is required for the JUnit report.

Results are saved in `build/test-results/tests.log` and `build/test-results/results.xml`. Failed assertions include the CTest name, source file and line, function, reason, expression, and expected/actual values. Crashes and timeouts retain the test's captured output and last explicitly logged phase; that phase is not a claim to identify the faulting instruction. Uncaught exceptions include their original message and last assertion or phase checkpoint.

After building, select tests without rebuilding:

```sh
bash tests/run_tests.sh --no-build --test '^memory_'
bash tests/run_tests.sh --no-build --test '^audit_'
ctest --test-dir build/current --rerun-failed --output-on-failure
```

`memory_contracts.cpp` uses only `alligator.hpp` to check 64-byte alignment, zeroing and non-overlap of live odd-sized claims, prepared backing, `SliceT<T>(count)`, and shared views surviving owner-thread exit and slab rollover before release on other threads. Existing tests cover copying, moving, slicing, resizing, queue delivery, registry growth and exhaustion, budgets, recycling, network protocols, and Vulkan placement preservation. The audit adds allocation interception, concurrent live ranges, queue identity, wire allocation checks, and verification of GPU-written buffer contents.

`TEST_EQUAL(actual, expected, reason)` captures both operand values on failure. `TEST_REQUIRE(condition, reason)` reports a predicate and its true/false result. Both remain active in Release builds. Assertions stop their individual process with a failure code, including when triggered in a worker thread; CTest continues to the next test. No failed production contract is marked as an expected failure.

`buffetalligator_test_runner_test` verifies the runner with isolated intentional assertions, an uncaught exception, a worker failure, a crash, a timeout, and a skip, followed by a passing case. It also checks successful selections and empty-selection rejection. Its fixture failures are solely tests of the harness and are stored under `build/current/tests/runner-fixtures`.

Allocation interception counts calls from the instrumented library C objects, including mapping and registry commitment; it excludes dependency internals and C++ runtime allocations. The public prepared-capacity test separately counts calls to its real placement callback on the claiming thread. Allocation on slab rollover is permitted and is not classified as a violation of the prepared-capacity test.
