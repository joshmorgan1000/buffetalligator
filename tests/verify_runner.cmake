file(MAKE_DIRECTORY "${FIXTURE_DIR}")
set(fixture_tests "")
foreach(fixture comparison predicate worker exception crash timeout skip pass)
    set(fixture_name "fixture_${fixture}")
    if(fixture STREQUAL "pass")
        set(fixture_name "fixture_pass_after_failures")
    endif()
    string(APPEND fixture_tests
        "add_test(${fixture_name} \"${FIXTURE_PROGRAM}\" ${fixture})\n"
        "set_tests_properties(${fixture_name} PROPERTIES TIMEOUT 5 ENVIRONMENT BUFFETALLIGATOR_TEST_NAME=${fixture_name})\n")
endforeach()
string(APPEND fixture_tests "set_tests_properties(fixture_skip PROPERTIES SKIP_RETURN_CODE 77)\n")
string(APPEND fixture_tests "set_tests_properties(fixture_timeout PROPERTIES TIMEOUT 1)\n")
file(WRITE "${FIXTURE_DIR}/CTestTestfile.cmake" "${fixture_tests}")
execute_process(COMMAND bash "${TEST_RUNNER}" --no-build --build-dir "${FIXTURE_DIR}"
    --output-dir "${FIXTURE_DIR}/failures"
    RESULT_VARIABLE failure_status OUTPUT_VARIABLE failure_output ERROR_VARIABLE failure_errors TIMEOUT 20)
if(NOT failure_status STREQUAL "1")
    message(FATAL_ERROR "Runner must return 1 for failed tests; actual=${failure_status}\n${failure_output}\n${failure_errors}")
endif()
foreach(expected "[FAIL] fixture_comparison" "Check: 7 == 9" "Expected: 9" "Actual: 7"
    "[FAIL] fixture_predicate" "Expected: true" "Actual: false"
    "[FAIL] fixture_worker" "Expected: 13" "Actual: 11"
    "[FAIL] fixture_exception" "intentional uncaught exception detail"
    "Triggering an intentional SIGABRT" "Subprocess aborted"
    "Waiting in an intentional timeout fixture" "***Timeout"
    "fixture_pass_after_failures" "Passed" "fixture_skip" "Skipped")
    string(FIND "${failure_output}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Runner omitted diagnostic '${expected}'\n${failure_output}\n${failure_errors}")
    endif()
endforeach()
if(NOT failure_output MATCHES "diagnostics_test.cpp:[0-9]+")
    message(FATAL_ERROR "Runner omitted the failed assertion's source file and line\n${failure_output}")
endif()
file(READ "${FIXTURE_DIR}/failures/results.xml" junit)
if(NOT junit MATCHES "failures=\"6\"" OR NOT junit MATCHES "skipped=\"1\"")
    message(FATAL_ERROR "JUnit must retain all six failures and one skip\n${junit}")
endif()
execute_process(COMMAND bash "${TEST_RUNNER}" --no-build --build-dir "${FIXTURE_DIR}"
    --output-dir "${FIXTURE_DIR}/passing" --test "^fixture_pass_after_failures$"
    RESULT_VARIABLE passing_status OUTPUT_VARIABLE passing_output ERROR_VARIABLE passing_errors TIMEOUT 10)
if(NOT passing_status STREQUAL "0" OR NOT passing_output MATCHES "100% tests passed")
    message(FATAL_ERROR "Passing selection did not succeed\n${passing_output}\n${passing_errors}")
endif()
execute_process(COMMAND bash "${TEST_RUNNER}" --no-build --build-dir "${FIXTURE_DIR}"
    --output-dir "${FIXTURE_DIR}/empty" --test "^test_that_does_not_exist$"
    RESULT_VARIABLE empty_status OUTPUT_VARIABLE empty_output ERROR_VARIABLE empty_errors TIMEOUT 10)
if(NOT empty_status STREQUAL "1")
    message(FATAL_ERROR "An empty selection must fail; actual=${empty_status}\n${empty_output}\n${empty_errors}")
endif()
message(STATUS "Runner verified assertion operands, source lines, worker failures, exceptions, crashes, timeouts, continuation, skips, success, and empty-selection failure")
