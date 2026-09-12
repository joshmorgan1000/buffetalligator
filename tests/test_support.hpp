#pragma once
/** --------------------------------------------------------------------------------------------------------- Test Diagnostics
 * @file test_support.hpp
 * @brief Reports test failures independently of the allocator and production logger under test.
 */
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <source_location>
#include <sstream>
#include <string>
#include <type_traits>

namespace test_support {
inline const char* source_file = "unknown test source";
inline thread_local const char* last_operation = "test entry";
inline thread_local std::source_location last_location = std::source_location::current();
/** --------------------------------------------------------------------------------------------------------- Checkpoint
 * @brief Records the last assertion or named operation on the current test thread.
 */
inline void checkpoint(
    const char* operation, std::source_location location = std::source_location::current()
) {
    last_operation = operation;
    last_location = location;
}
/** --------------------------------------------------------------------------------------------------------- Fail
 * @brief Prints a complete diagnostic before terminating only this isolated test process.
 */
[[noreturn]] inline void fail(
    const char* message, const char* expression, const char* expected, const char* actual,
    std::source_location location = std::source_location::current()
) {
    const char* name = std::getenv("BUFFETALLIGATOR_TEST_NAME");
    std::fprintf(stderr,
        "\n[FAIL] %s\n  Source: %s:%u\n  Function: %s\n  Reason: %s\n"
        "  Check: %s\n  Expected: %s\n  Actual: %s\n",
        name ? name : source_file, location.file_name(), location.line(), location.function_name(),
        message, expression, expected, actual);
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}
/** --------------------------------------------------------------------------------------------------------- Unexpected Exception
 * @brief Preserves an uncaught exception and identifies the last checkpoint without claiming its throw site.
 */
[[noreturn]] inline void unexpected_exception() {
    try {
        if (const auto exception = std::current_exception()) std::rethrow_exception(exception);
    } catch (const std::exception& error) {
        fail("Uncaught exception; source identifies the last test checkpoint, not the throw site",
            last_operation, "operation completes without an unexpected exception", error.what(), last_location);
    } catch (...) {
        fail("Uncaught non-standard exception at or after this checkpoint", last_operation,
            "operation completes without an unexpected exception", "non-standard exception", last_location);
    }
    fail("std::terminate called without an active exception", last_operation,
        "normal test completion", "termination at or after this checkpoint", last_location);
}
/** --------------------------------------------------------------------------------------------------------- Start
 * @brief Installs exception diagnostics and emits the test's entry checkpoint.
 */
inline void start(
    const char* file, std::source_location location = std::source_location::current()
) {
    source_file = file;
    checkpoint("test entry", location);
    std::set_terminate(unexpected_exception);
    const char* name = std::getenv("BUFFETALLIGATOR_TEST_NAME");
    std::fprintf(stderr, "[RUN] %s\n  Source: %s:%u\n",
        name ? name : file, location.file_name(), location.line());
    std::fflush(stderr);
}
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Checks a predicate in every build configuration and preserves its source expression.
 */
inline void require(
    bool condition, const char* expression, const char* message,
    std::source_location location = std::source_location::current()
) {
    if (!condition) fail(message, expression, "true", "false", location);
}
/** --------------------------------------------------------------------------------------------------------- Value
 * @brief Formats comparison operands only after a check fails.
 */
template<class Value> std::string value(const Value& operand) {
    std::ostringstream stream;
    if constexpr (std::is_enum_v<Value>) stream << static_cast<std::underlying_type_t<Value>>(operand);
    else if constexpr (std::is_pointer_v<Value> && std::is_object_v<std::remove_pointer_t<Value>>)
        stream << static_cast<const void*>(operand);
    else if constexpr (std::is_same_v<Value, unsigned char> || std::is_same_v<Value, signed char>)
        stream << static_cast<int>(operand);
    else stream << std::boolalpha << operand;
    return stream.str();
}
/** --------------------------------------------------------------------------------------------------------- Equal
 * @brief Reports both comparison operands without evaluating either expression twice.
 */
template<class Actual, class Expected> void equal(
    const Actual& actual, const Expected& expected, const char* expression, const char* message,
    std::source_location location = std::source_location::current()
) {
    bool matches;
    if constexpr (std::is_integral_v<Actual> && std::is_integral_v<Expected>
        && std::is_signed_v<Actual> != std::is_signed_v<Expected>) {
        if constexpr (std::is_signed_v<Actual>)
            matches = actual >= 0 && static_cast<std::make_unsigned_t<Actual>>(actual) == expected;
        else matches = expected >= 0 && actual == static_cast<std::make_unsigned_t<Expected>>(expected);
    } else matches = actual == expected;
    if (!matches) {
        const auto actual_text = value(actual);
        const auto expected_text = value(expected);
        fail(message, expression, expected_text.c_str(), actual_text.c_str(), location);
    }
}
/** --------------------------------------------------------------------------------------------------------- Step
 * @brief Marks an operation so crash and timeout logs retain the last started phase.
 */
inline void step(const char* message, std::source_location location = std::source_location::current()) {
    checkpoint(message, location);
    std::fprintf(stderr, "[STEP] %s\n  Source: %s:%u\n", message, location.file_name(), location.line());
    std::fflush(stderr);
}
}
#define TEST_REQUIRE(condition, message) do { \
    test_support::checkpoint(#condition); \
    test_support::require(static_cast<bool>(condition), #condition, message); \
} while (false)
#define TEST_EQUAL(actual, expected, message) do { \
    test_support::checkpoint(#actual " == " #expected); \
    test_support::equal((actual), (expected), #actual " == " #expected, message); \
} while (false)
