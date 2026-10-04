/** --------------------------------------------------------------------------------------------------------- Worker Pool Idle Test
 * @file worker_pool_idle_test.cpp
 * @brief Verifies idle workers park, resume queued work, run completion callbacks, and stop during
 * process teardown.
 */
#include <alligator/kitchen.hpp>
#include "functional_support.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <ctime>
#include <thread>

namespace {
using buffetalligator::Kitchen;
using buffetalligator::Order;
using buffetalligator::OrderCountdown;
using functional::require;
constexpr size_t producer_count = 4;
constexpr size_t tasks_per_producer = 64;
using CompletionCounts = std::array<std::atomic<unsigned>, producer_count * tasks_per_producer>;
/** --------------------------------------------------------------------------------------------------------- Await
 * @brief Requires queued work to finish within two seconds without another submission waking the pool.
 * @param countdown The countdown armed for that work.
 */
void await(OrderCountdown& countdown) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!countdown.ready()) {
        require(std::chrono::steady_clock::now() < deadline, "queued task did not wake a worker");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    countdown.wait();
}
/** --------------------------------------------------------------------------------------------------------- Stage
 * @struct Stage
 * @brief One step of an ordered task chain: the shared stage counter and the value it must see.
 */
struct Stage {
    std::atomic<size_t>* completed;
    size_t expected;
    size_t result = 0;
};
/** --------------------------------------------------------------------------------------------------------- Advance
 * @brief Checks chain ordering and records each completed stage.
 * @param context The Stage being run.
 */
void advance(void* context) {
    Stage* stage = static_cast<Stage*>(context);
    const size_t previous = stage->completed->fetch_add(1, std::memory_order_relaxed);
    require(previous == stage->expected, "task chain ran out of order or more than once");
    stage->result = previous + 1;
}
/** --------------------------------------------------------------------------------------------------------- Chain
 * @struct Chain
 * @brief A waiter-pool stage whose completion queues a worker stage, whose completion queues a final one.
 */
struct Chain {
    std::atomic<size_t> completed{0};
    std::array<Stage, 3> stages{Stage{&completed, 0}, Stage{&completed, 1}, Stage{&completed, 2}};
    OrderCountdown finished{1};
};
/** --------------------------------------------------------------------------------------------------------- Worker Stage Done
 * @brief Continuation of the worker stage: queues the final stage on the worker pool.
 * @param context The Chain.
 */
void worker_stage_done(void* context) {
    Chain* chain = static_cast<Chain*>(context);
    Kitchen::inst().submit(&advance, &chain->stages[2], &OrderCountdown::arrive, &chain->finished);
}
/** --------------------------------------------------------------------------------------------------------- Waiting Stage Done
 * @brief Continuation of the waiter-pool stage: queues the worker stage.
 * @param context The Chain.
 */
void waiting_stage_done(void* context) {
    Chain* chain = static_cast<Chain*>(context);
    Kitchen::inst().submit(&advance, &chain->stages[1], &worker_stage_done, chain);
}
/** --------------------------------------------------------------------------------------------------------- Continuations
 * @brief Starts a cold worker pool from a waiter-pool completion callback before any regular submission.
 */
void continuations() {
    Chain chain;
    Kitchen::inst().submit_waiting(&advance, &chain.stages[0], &waiting_stage_done, &chain);
    await(chain.finished);
    require(chain.stages[0].result == 1 && chain.stages[1].result == 2 && chain.stages[2].result == 3,
        "chained completion callbacks returned the wrong results");
}
/** --------------------------------------------------------------------------------------------------------- Blocked Batch
 * @brief Retains a batch gate and completion count while workers wait for their peers.
 */
struct BlockedBatch {
    std::atomic<size_t> entered{0};
    std::atomic<bool> release{false};
};
/** --------------------------------------------------------------------------------------------------------- Hold Batch
 * @brief Keeps a worker occupied until every requested peer has entered.
 */
void hold_batch(void* context) {
    BlockedBatch& batch = *static_cast<BlockedBatch*>(context);
    batch.entered.fetch_add(1, std::memory_order_release);
    batch.release.wait(false, std::memory_order_acquire);
}
/** --------------------------------------------------------------------------------------------------------- Blocked Backlog
 * @brief Requires accepted backlog to wake parked workers while existing workers are blocked.
 */
void blocked_backlog() {
    LOG_INFO_STREAM << "Checking parked workers wake while accepted orders are blocked";
    const size_t count = std::min(size_t(8), Kitchen::inst().max_threads());
    BlockedBatch batch;
    OrderCountdown finished(static_cast<uint32_t>(count));
    for (size_t index = 0; index < count; ++index) {
        Kitchen::inst().submit(hold_batch, &batch, &OrderCountdown::arrive, &finished);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (batch.entered.load(std::memory_order_acquire) != count) {
        require(std::chrono::steady_clock::now() < deadline,
            "blocked workers prevented accepted backlog from starting");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    batch.release.store(true, std::memory_order_release);
    batch.release.notify_all();
    finished.wait();
    Kitchen::inst().drain();
}
/** --------------------------------------------------------------------------------------------------------- Idle Wakeups
 * @brief Alternates single and bulk submissions after workers have time to park.
 */
void idle_wakeups() {
    std::atomic<size_t> completed{0};
    for (size_t round = 0; round < 16; ++round) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Stage single{&completed, round * 2};
        OrderCountdown single_done(1);
        Kitchen::inst().submit(&advance, &single, &OrderCountdown::arrive, &single_done);
        await(single_done);
        require(single.result == round * 2 + 1, "idle single submission result differs");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Stage bulk{&completed, round * 2 + 1};
        OrderCountdown bulk_done(1);
        Order batch[1] = {Order{&advance, &bulk, &OrderCountdown::arrive, &bulk_done}};
        Kitchen::inst().submit_bulk(batch, 1);
        await(bulk_done);
        require(bulk.result == round * 2 + 2, "idle bulk submission result differs");
    }
}
/** --------------------------------------------------------------------------------------------------------- Complete Once
 * @brief Records execution of a unique task from a concurrent submission burst.
 * @param context The task's own completion counter.
 */
void complete_once(void* context) {
    std::atomic<unsigned>* count = static_cast<std::atomic<unsigned>*>(context);
    require(count->fetch_add(1, std::memory_order_relaxed) == 0, "concurrent submission executed more than once");
}
/** --------------------------------------------------------------------------------------------------------- Produce
 * @brief Submits and awaits one producer's independent batch of tasks.
 */
void produce(CompletionCounts* completed, std::barrier<>* start, size_t producer_index) {
    OrderCountdown finished(static_cast<uint32_t>(tasks_per_producer));
    start->arrive_and_wait();
    for (size_t task_index = 0; task_index < tasks_per_producer; ++task_index) {
        Kitchen::inst().submit(&complete_once, &(*completed)[producer_index * tasks_per_producer + task_index],
            &OrderCountdown::arrive, &finished);
    }
    await(finished);
}
/** --------------------------------------------------------------------------------------------------------- Concurrent Producers
 * @brief Verifies concurrent queue wakeups preserve exactly-once task completion.
 */
void concurrent_producers() {
    CompletionCounts completed{};
    std::barrier start(static_cast<std::ptrdiff_t>(producer_count));
    std::array<std::thread, producer_count> producers;
    for (size_t producer_index = 0; producer_index < producer_count; ++producer_index) {
        producers[producer_index] = std::thread(produce, &completed, &start, producer_index);
    }
    for (std::thread& producer : producers) producer.join();
    for (const std::atomic<unsigned>& count : completed) {
        require(count.load(std::memory_order_relaxed) == 1, "concurrent submission was lost");
    }
}
/** --------------------------------------------------------------------------------------------------------- Idle CPU
 * @brief Bounds aggregate process CPU consumption while all warmed workers have no work.
 */
void idle_cpu() {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const std::clock_t cpu_start = std::clock();
    const auto wall_start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const std::clock_t cpu_end = std::clock();
    const double wall_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - wall_start).count();
    require(cpu_start != static_cast<std::clock_t>(-1)
        && cpu_end != static_cast<std::clock_t>(-1),
        "process CPU clock is unavailable");
    const double cpu_seconds = static_cast<double>(cpu_end - cpu_start) / CLOCKS_PER_SEC;
    LOG_INFO_STREAM << "Idle worker CPU: " << cpu_seconds << " seconds over "
        << wall_seconds << " wall seconds";
    require(cpu_seconds < wall_seconds * 0.4, "idle worker pool consumed CPU instead of parking");
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs public worker-queue checks and returns with every worker idle for bounded teardown.
 */
int main() {
    LOG_INFO_STREAM << "Checking cold continuations, idle wakeups, and concurrent submissions";
    continuations();
    blocked_backlog();
    idle_wakeups();
    concurrent_producers();
    LOG_INFO_STREAM << "Measuring idle worker CPU before process teardown";
    idle_cpu();
    LOG_INFO_STREAM << "Worker queue checks passed; stopping idle workers during process teardown";
    return 0;
}
