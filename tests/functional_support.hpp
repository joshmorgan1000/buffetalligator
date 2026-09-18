#pragma once
/** --------------------------------------------------------------------------------------------------------- Functional Support
 * @file functional_support.hpp
 * @brief Runs isolated public API cases with checks that remain active in release builds.
 */
#include <logging.hpp>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <string_view>

namespace functional {
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Stops immediately when a functional contract fails, including inside worker threads.
 */
inline void require(bool condition, const char* message) {
    if (!condition) {
        LOG_ERROR_STREAM << "FAILED: " << message;
        std::abort();
    }
}
/** --------------------------------------------------------------------------------------------------------- Require Throws
 * @brief Checks that invalid input is rejected by an exception.
 */
template<typename Function>
void require_throws(Function&& function, const char* message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    require(false, message);
}
/** --------------------------------------------------------------------------------------------------------- Case
 * @brief Associates a CTest case name with its public API exercise.
 */
struct Case {
    std::string_view name;
    void (*run)();
};
/** --------------------------------------------------------------------------------------------------------- Run
 * @brief Executes one named case so failures cannot mask unrelated components.
 */
inline int run(int count, char** arguments, std::initializer_list<Case> cases) {
    if (count != 2) return 2;
    for (const Case& test : cases) {
        if (test.name != arguments[1]) continue;
        LOG_INFO_STREAM << "Running " << arguments[1];
        try {
            test.run();
        } catch (const std::exception& error) {
            LOG_ERROR_STREAM << "FAILED: " << error.what();
            return 1;
        }
        LOG_INFO_STREAM << "Passed " << arguments[1];
        return 0;
    }
    return 2;
}
}
