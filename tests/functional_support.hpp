#pragma once
/** --------------------------------------------------------------------------------------------------------- Functional Support
 * @file functional_support.hpp
 * @brief Runs isolated public API cases with checks that remain active in release builds.
 */
#include <logging.hpp>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <string_view>

namespace functional {
/// @brief The lanes of chains each placement claims from.
inline constexpr size_t LANES = 4;
/// @brief The slab size a test placement chains; a lane slab is never smaller than this.
inline constexpr size_t SLAB_BYTES = 256ull << 20;
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Stops immediately when a functional contract fails, including inside worker threads.
 */
template<typename... Args>
inline void require(bool condition, Args... message) {
    if (!condition) {
        LOG_ERROR_STREAM << "FAILED: ";
        ((LOG_ERROR_STREAM << message), ...);
        std::abort();
    }
}
/** --------------------------------------------------------------------------------------------------------- Require Throws
 * @brief Checks that invalid input is rejected by an exception.
 */
template<typename Function, typename... Args>
void require_throws(Function&& function, Args... message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    require(false, "Exception not thrown: ", message...);
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
