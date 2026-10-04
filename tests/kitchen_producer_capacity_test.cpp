/** --------------------------------------------------------------------------------------------------------- Kitchen Producer Capacity Test
 * @file kitchen_producer_capacity_test.cpp
 * @brief Checks new producers progress after another live producer exhausts queue storage.
 */
#include <alligator/kitchen.hpp>
#include "functional_support.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <latch>
#include <semaphore>
#include <thread>
#include <utility>

namespace {
using buffetalligator::Kitchen;
using buffetalligator::Order;
using functional::require;
constexpr size_t batch_size = 256;
/** --------------------------------------------------------------------------------------------------------- Watchdog
 * @class Watchdog
 * @brief Bounds queue draining and thread teardown throughout the regression.
 */
class Watchdog {
private:
    std::binary_semaphore finished_{0};
    std::thread thread_;
    /** ------------------------------------------------------------------------------------------- Monitor
     * @brief Aborts when the regression exceeds its global deadline.
     * @param watchdog The monitor whose release marks test completion.
     */
    static void monitor(Watchdog* watchdog) {
        require(watchdog->finished_.try_acquire_for(std::chrono::seconds(30)),
            "producer capacity regression exceeded its deadline");
    }
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Starts the independent deadline monitor.
     */
    Watchdog() : thread_(&Watchdog::monitor, this) {}
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Releases and joins the deadline monitor after the test finishes.
     */
    ~Watchdog() {
        finished_.release();
        thread_.join();
    }
};
/** --------------------------------------------------------------------------------------------------------- Capacity State
 * @struct CapacityState
 * @brief Keeps both producer registrations alive through queue exhaustion and draining.
 */
struct CapacityState {
    std::binary_semaphore first_prepared{0};
    std::binary_semaphore first_filled{0};
    std::binary_semaphore second_prepared{0};
    std::latch begin_fill{1};
    std::latch retire_first{1};
    std::latch begin_second{1};
    std::latch blocker_entered{1};
    std::latch release_blocker{1};
    std::atomic<size_t> first_completed{0};
    std::atomic<size_t> single_completed{0};
    std::atomic<size_t> batch_completed{0};
    size_t accepted = 0;
};
/** --------------------------------------------------------------------------------------------------------- Await Semaphore
 * @brief Bounds a producer setup handshake before continuing the controller.
 * @param signal The producer's setup signal.
 * @param message The failure message for an expired deadline.
 */
void await_semaphore(std::binary_semaphore& signal, const char* message) {
    require(signal.try_acquire_for(std::chrono::seconds(5)), message);
}
/** --------------------------------------------------------------------------------------------------------- Await Latch
 * @brief Bounds callback completion without changing the latch's one-shot contract.
 * @param completion The callback latch.
 * @param message The failure message for an expired deadline.
 */
void await_latch(std::latch& completion, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!completion.try_wait()) {
        require(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::yield();
    }
    completion.wait();
}
/** --------------------------------------------------------------------------------------------------------- Block Worker
 * @brief Holds the only compute worker while the first producer exhausts queue storage.
 * @param context The shared capacity-test state.
 */
void block_worker(void* context) {
    auto& state = *static_cast<CapacityState*>(context);
    state.blocker_entered.count_down();
    state.release_blocker.wait();
}
/** --------------------------------------------------------------------------------------------------------- Count Execution
 * @brief Records one real Order execution without owning or allocating argument storage.
 * @param context The execution counter.
 */
void count_execution(void* context) {
    static_cast<std::atomic<size_t>*>(context)->fetch_add(1, std::memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Complete Callback
 * @brief Publishes one completed callback through the caller's standard latch.
 * @param context The latch armed for these callbacks.
 */
void complete_callback(void* context) {
    static_cast<std::latch*>(context)->count_down();
}
/** --------------------------------------------------------------------------------------------------------- Retain Exhausted Producer
 * @brief Exhausts compute slots and keeps their producer registration alive after draining.
 * @param state The setup gates, execution counters, and retained-producer release.
 */
void retain_exhausted_producer(CapacityState* state) {
    try {
        Kitchen& kitchen = Kitchen::inst();
        kitchen.prepare_producer();
        state->first_prepared.release();
        state->begin_fill.wait();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            require(std::chrono::steady_clock::now() < deadline,
                "blocked compute queue never reached its preallocated capacity");
            Order order(&count_execution, static_cast<void*>(&state->first_completed));
            if (!kitchen.try_submit(std::move(order))) {
                break;
            }
            ++state->accepted;
        }
        state->first_filled.release();
        state->retire_first.wait();
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "First producer failed: " << error.what();
        std::abort();
    }
}
/** --------------------------------------------------------------------------------------------------------- Submit From New Producer
 * @brief Requires single and bulk progress while the exhausted producer still retains its lane.
 * @param state The controller gates and execution counters.
 */
void submit_from_new_producer(CapacityState* state) {
    try {
        Kitchen& kitchen = Kitchen::inst();
        kitchen.prepare_producer();
        state->second_prepared.release();
        state->begin_second.wait();
        std::latch single_done(1);
        Order single(&count_execution, &state->single_completed,
            &complete_callback, &single_done);
        const auto single_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!kitchen.try_submit(std::move(single))) {
            require(std::chrono::steady_clock::now() < single_deadline,
                "new producer cannot submit after the exhausted lane has drained");
            std::this_thread::yield();
        }
        await_latch(single_done, "new producer single callback did not complete");
        std::latch batch_done(batch_size);
        std::array<Order, batch_size> orders;
        for (Order& order : orders) {
            order = Order(&count_execution, &state->batch_completed,
                &complete_callback, &batch_done);
        }
        const auto batch_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!kitchen.try_submit_bulk(orders.data(), orders.size())) {
            require(std::chrono::steady_clock::now() < batch_deadline,
                "new producer cannot submit a batch after its setup work has drained");
            std::this_thread::yield();
        }
        await_latch(batch_done, "new producer bulk callbacks did not all complete");
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "New producer failed: " << error.what();
        std::abort();
    }
}
/** --------------------------------------------------------------------------------------------------------- Waiting Isolation State
 * @struct WaitingIsolationState
 * @brief Holds every waiting worker while a fresh producer submits independent compute work.
 */
struct WaitingIsolationState {
    std::latch waiting_entered;
    std::latch release_waiting{1};
    std::latch compute_done{1};
    std::atomic<size_t> compute_completed{0};
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Arms entry observation for the hardware-sized waiting pool.
     * @param workers The number of waiting workers that must enter the gate.
     */
    explicit WaitingIsolationState(size_t workers)
    : waiting_entered(static_cast<std::ptrdiff_t>(workers)) {}
};
/** --------------------------------------------------------------------------------------------------------- Block Waiting Worker
 * @brief Records waiting-pool entry before parking behind the controller's release latch.
 * @param context The waiting-pool isolation gates.
 */
void block_waiting_worker(void* context) {
    auto& state = *static_cast<WaitingIsolationState*>(context);
    state.waiting_entered.count_down();
    state.release_waiting.wait();
}
/** --------------------------------------------------------------------------------------------------------- Submit Cold Compute
 * @brief Submits compute work from a fresh thread without preparing a waiting producer lane.
 * @param state The compute execution counter and completion latch.
 */
void submit_cold_compute(WaitingIsolationState* state) {
    try {
        Kitchen::inst().submit(&count_execution, &state->compute_completed,
            &complete_callback, &state->compute_done);
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Cold compute producer failed: " << error.what();
        std::abort();
    }
}
/** --------------------------------------------------------------------------------------------------------- Waiting Pool Isolation
 * @brief Requires cold compute completion before releasing any blocked waiting worker.
 */
void waiting_pool_isolation() {
    LOG_INFO_STREAM << "Checking cold compute progress while every waiting worker is blocked";
    Kitchen& kitchen = Kitchen::inst();
    const size_t waiting_workers = std::thread::hardware_concurrency();
    require(waiting_workers != 0, "hardware did not report the waiting pool's worker count");
    kitchen.drain();
    WaitingIsolationState state(waiting_workers);
    for (size_t index = 0; index < waiting_workers; ++index) {
        kitchen.submit_waiting(&block_waiting_worker, &state);
    }
    await_latch(state.waiting_entered, "waiting workers did not all enter their blocking Orders");
    std::thread producer(&submit_cold_compute, &state);
    await_latch(state.compute_done,
        "cold compute registration waited for the blocked waiting pool");
    require(state.compute_completed.load(std::memory_order_relaxed) == 1,
        "cold compute Order did not complete before releasing the waiting pool");
    state.release_waiting.count_down();
    producer.join();
    kitchen.drain();
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Verifies retained producer capacity and independent cold compute submissions.
 * @return Zero when every accepted Order and callback completes.
 */
int main() {
    Watchdog watchdog;
    Kitchen& kitchen = Kitchen::inst();
    const size_t original_threads = kitchen.max_threads();
    LOG_INFO_STREAM << "Checking new-producer capacity after a retained producer fills the queue";
    kitchen.set_max_threads(1);
    kitchen.prepare_producer();
    kitchen.drain();
    CapacityState state;
    std::thread first(&retain_exhausted_producer, &state);
    await_semaphore(state.first_prepared, "first producer setup did not complete");
    kitchen.drain();
    kitchen.submit(&block_worker, &state);
    await_latch(state.blocker_entered, "the sole compute worker did not enter its blocking Order");
    state.begin_fill.count_down();
    await_semaphore(state.first_filled, "first producer did not exhaust available queue storage");
    require(state.accepted != 0, "prepared first producer had no usable queue storage");
    LOG_INFO_STREAM << "Draining " << state.accepted
        << " Orders while their producer keeps its queue registration alive";
    state.release_blocker.count_down();
    kitchen.drain();
    require(state.first_completed.load(std::memory_order_relaxed) == state.accepted,
        "draining the exhausted lane lost an accepted Order");
    LOG_INFO_STREAM << "Preparing a second producer for single and 256-Order bulk submissions";
    std::thread second(&submit_from_new_producer, &state);
    await_semaphore(state.second_prepared, "new producer setup did not complete");
    kitchen.drain();
    state.begin_second.count_down();
    second.join();
    require(state.single_completed.load(std::memory_order_relaxed) == 1,
        "new producer single Order did not execute exactly once");
    require(state.batch_completed.load(std::memory_order_relaxed) == batch_size,
        "new producer bulk Orders did not all execute");
    state.retire_first.count_down();
    first.join();
    kitchen.drain();
    kitchen.set_max_threads(original_threads);
    waiting_pool_isolation();
    LOG_INFO_STREAM << "Retained and newly prepared producer capacity checks passed";
    return 0;
}
