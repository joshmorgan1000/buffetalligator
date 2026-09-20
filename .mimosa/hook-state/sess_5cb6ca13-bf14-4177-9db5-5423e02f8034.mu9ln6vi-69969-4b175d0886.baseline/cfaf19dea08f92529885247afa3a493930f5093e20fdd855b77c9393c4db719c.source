/** --------------------------------------------------------------------------------------------------------- Logo Animation Demo
 * @file demo_logo.cpp
 * @brief Standalone harness for tweaking the LogoAnimation. Stands up
 * the Nebula compositor with nothing but a `LogoAnimation` window at
 * the top of the terminal, blocks on stdin until the user presses
 * Enter, fires the finale, waits for the explode-then-rest sequence
 * to play through, then tears down cleanly.
 *
 * Filename intentionally does not match `test_*.cpp` so the test glob
 * in `tests/CMakeLists.txt` does not pick it up — this is a demo, not
 * a test, and it blocks on stdin so it would hang any CTest run.
 *
 * Usage:
 *   cmake -S . -B build -DLOGGING_BUILD_TESTS=ON && cmake --build build --target demo_logo
 *   ./build/tests/demo_logo
 *   <press Enter when you have seen enough — finale plays, program exits>
 */
#include <loggingutils.hpp>
#include <chrono>
#include <iostream>
#include <thread>

namespace {
/// @brief Rows reserved above the logo so it has somewhere to land
/// without trampling whatever the user already had on screen.
constexpr int kBlankLeadRows = 8;
/// @brief Compositor row (0-indexed) the logo window's top edge sits on.
constexpr int kLogoTopRow = 0;
/// @brief Hard cap on how long we'll wait for `is_complete()` after
/// `trigger_finale()`. Matches the 5s window the LogoAnimation uses
/// internally (4500 ms explode + 500 ms rest tail) plus a small safety
/// margin so a slow scheduler can't strand us with the terminal in
/// alt-screen / hidden-cursor mode.
constexpr int64_t kFinaleTimeoutMs = 8000;
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}
} // namespace
int main() {
    if (!threadsafe_logger::logging::stdout_is_tty()) {
        std::cerr << "demo_logo: stdout is not a TTY — animation needs "
                  << "ANSI escape support; aborting.\n";
        return 1;
    }
    /// 8 blank lines so wherever the user's cursor was, the logo isn't
    /// painted on top of their last command's output.
    for (int i = 0; i < kBlankLeadRows; ++i) {
        std::cout << '\n';
    }
    std::cout.flush();
    threadsafe_logger::logging::Terminal& term = threadsafe_logger::logging::Terminal::instance();
    threadsafe_logger::logging::LogoAnimation* logo = term.add_logo_window(kLogoTopRow);
    term.poke();
    std::cout << "press Enter to trigger finale and exit..." << std::endl;
    /// Block on stdin. `std::cin.get()` returns when the user hits
    /// Enter (or pipes a newline). EOF (Ctrl-D on an empty line) also
    /// drops through and triggers the finale — desirable for piped
    /// runs.
    std::cin.get();
    logo->trigger_finale();
    const int64_t deadline = now_ms() + kFinaleTimeoutMs;
    while (!logo->is_complete() && now_ms() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    term.shutdown();
    return 0;
}
