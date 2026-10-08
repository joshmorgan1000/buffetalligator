/** --------------------------------------------------------------------------------------------------------- Kitchen Shutdown Test
 * @file kitchen_shutdown_test.cpp
 * @brief Checks singleton destruction completes every accepted Order and borrowed callback.
 */
#include <alligator/kitchen.hpp>
#include <memory/lifetime.hpp>
#include "functional_support.hpp"
#include <atomic>
#include <cstddef>
#include <latch>
#include <thread>

namespace {
constexpr size_t pending_count = 4096;
/** --------------------------------------------------------------------------------------------------------- Shutdown State
 * @brief Retains worker gates and completion evidence through the real runtime teardown.
 */
struct ShutdownState {
    std::atomic<size_t> completed_orders{0};
    std::atomic<size_t> completed_callbacks{0};
    std::latch release_workers{1};
    std::latch workers_entered{std::thread::hardware_concurrency()};
};
ShutdownState* pending = nullptr;
/** --------------------------------------------------------------------------------------------------------- Verify Shutdown
 * @brief Checks queued completion after the retained Kitchen has joined every worker.
 */
void verify_shutdown(void* context) {
    auto* state = static_cast<ShutdownState*>(context);
    functional::require(state->completed_orders.load() == pending_count,
        "Kitchen shutdown discarded an accepted Order");
    functional::require(state->completed_callbacks.load() == pending_count,
        "Kitchen shutdown discarded an accepted completion callback");
    LOG_INFO_STREAM << "Kitchen shutdown completed all accepted Orders and callbacks";
    delete state;
}
/** --------------------------------------------------------------------------------------------------------- Release Workers
 * @brief Opens the worker gate when main exits before retained runtime teardown begins.
 */
struct ReleaseWorkers {
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Releases occupied workers so Kitchen shutdown must process their queued backlog.
     */
    ~ReleaseWorkers() {
        pending->release_workers.count_down();
    }
};
ReleaseWorkers release_on_exit;
/** --------------------------------------------------------------------------------------------------------- Hold Worker
 * @brief Occupies one worker until the program begins static teardown.
 */
void hold_worker(ShutdownState* state) {
    state->workers_entered.count_down();
    state->release_workers.wait();
}
/** --------------------------------------------------------------------------------------------------------- Complete Order
 * @brief Records execution of one accepted queued Order.
 */
void complete_order(void* context) {
    static_cast<ShutdownState*>(context)->completed_orders.fetch_add(1, std::memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Complete Callback
 * @brief Records completion of one accepted queued Order.
 */
void complete_callback(void* context) {
    static_cast<ShutdownState*>(context)->completed_callbacks.fetch_add(1, std::memory_order_relaxed);
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Leaves a queued backlog behind occupied workers for process teardown to complete.
 */
int main() {
    LOG_INFO_STREAM << "Checking shutdown drains accepted Orders and completion callbacks";
    pending = new ShutdownState;
    static buffetalligator::RuntimeFinalizer verification(pending, &verify_shutdown);
    for (size_t worker = 0; worker < std::thread::hardware_concurrency(); ++worker) {
        buffetalligator::Kitchen::submit(&hold_worker, pending);
    }
    pending->workers_entered.wait();
    for (size_t index = 0; index < pending_count; ++index) {
        buffetalligator::Kitchen::submit(&complete_order, pending, &complete_callback, pending);
    }
    return 0;
}
