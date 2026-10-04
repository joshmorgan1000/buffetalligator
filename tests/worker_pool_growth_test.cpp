/** --------------------------------------------------------------------------------------------------------- Worker Pool Capacity Test
 * @file worker_pool_growth_test.cpp
 * @brief Verifies configured worker capacity is available before submission and survives resizing.
 */
#include <alligator/kitchen.hpp>
#include "functional_support.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace {
using buffetalligator::Kitchen;
using buffetalligator::OrderCountdown;
using functional::require;
/** --------------------------------------------------------------------------------------------------------- Burst State
 * @struct BurstState
 * @brief Counts callbacks held concurrently until the submitting thread releases them.
 */
struct BurstState {
    std::atomic<size_t> entered{0};
    std::atomic<bool> released{false};
};
/** --------------------------------------------------------------------------------------------------------- Hold Worker
 * @brief Records entry before parking until the submitting thread releases the burst.
 * @param context The burst state.
 */
void hold_worker(void* context) {
    BurstState& state = *static_cast<BurstState*>(context);
    state.entered.fetch_add(1, std::memory_order_release);
    state.released.wait(false, std::memory_order_acquire);
}
/** --------------------------------------------------------------------------------------------------------- Burst
 * @brief Requires configured workers to enter while queued excess work remains blocked.
 * @param workers The expected simultaneous callback count.
 */
void burst(size_t workers) {
    BurstState state;
    const size_t count = workers * 2;
    OrderCountdown finished(static_cast<uint32_t>(count));
    for (size_t index = 0; index < count; ++index) {
        Kitchen::inst().submit(&hold_worker, &state, &OrderCountdown::arrive, &finished);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (state.entered.load(std::memory_order_acquire) < workers
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const size_t concurrent = state.entered.load(std::memory_order_acquire);
    state.released.store(true, std::memory_order_release);
    state.released.notify_all();
    finished.wait();
    require(concurrent == workers, "worker capacity differs from set_max_threads");
    require(state.entered.load(std::memory_order_acquire) == count,
        "worker resizing lost queued work");
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Shrinks and expands the idle pool twice while checking full configured concurrency.
 * @return Zero when capacity and resizing checks pass.
 */
int main() {
    Kitchen& kitchen = Kitchen::inst();
    const size_t hardware = std::thread::hardware_concurrency();
    require(hardware != 0, "the operating system did not report its worker capacity");
    require(kitchen.max_threads() == hardware, "Kitchen did not start at reported CPU capacity");
    for (size_t round = 0; round < 2; ++round) {
        for (const size_t workers : {std::min<size_t>(2, hardware), hardware}) {
            LOG_INFO_STREAM << "Checking configured Kitchen capacity: " << workers << " workers";
            kitchen.set_max_threads(workers);
            require(kitchen.max_threads() == workers,
                "Kitchen did not retain its configured limit");
            burst(workers);
        }
    }
    LOG_INFO_STREAM << "Configured worker capacity and repeated resizing passed";
    return 0;
}
