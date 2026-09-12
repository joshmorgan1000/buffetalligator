/** --------------------------------------------------------------------------------------------------------- Diagnostic Fixtures
 * @file diagnostics_test.cpp
 * @brief Supplies isolated failures for verifying the test runner's own reporting and exit status.
 */
#include "test_support.hpp"
#include <chrono>
#include <csignal>
#include <stdexcept>
#include <string_view>
#include <thread>

/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Executes one deliberately selected diagnostic fixture without invoking the library.
 */
int main(int count, char** arguments) {
    test_support::start(__FILE__);
    TEST_EQUAL(count, 2, "Select one diagnostic fixture");
    const std::string_view selected(arguments[1]);
    if (selected == "comparison") {
        TEST_EQUAL(7, 9, "Intentional comparison failure for runner verification");
    } else if (selected == "predicate") {
        TEST_REQUIRE(false, "Intentional predicate failure for runner verification");
    } else if (selected == "worker") {
        std::thread worker([] {
            TEST_EQUAL(11, 13, "Intentional worker-thread failure for runner verification");
        });
        worker.join();
    } else if (selected == "exception") {
        test_support::step("Triggering an intentional uncaught exception");
        throw std::runtime_error("intentional uncaught exception detail");
    } else if (selected == "crash") {
        test_support::step("Triggering an intentional SIGABRT");
        std::raise(SIGABRT);
    } else if (selected == "timeout") {
        test_support::step("Waiting in an intentional timeout fixture");
        std::this_thread::sleep_for(std::chrono::seconds(10));
    } else if (selected == "skip") {
        test_support::step("Intentional unavailable capability fixture");
        return 77;
    } else {
        TEST_EQUAL(selected, std::string_view("pass"), "Unknown diagnostic fixture");
        int evaluations = 0;
        TEST_EQUAL(++evaluations, 1, "Comparison expressions must be evaluated exactly once");
        TEST_EQUAL(evaluations, 1, "The diagnostic helper repeated a side effect");
        test_support::step("The passing fixture ran after all intentional failures");
    }
    return 0;
}
