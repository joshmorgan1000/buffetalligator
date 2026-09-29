/** --------------------------------------------------------------------------------------------------------- Worker Pool Idle Test
 * @file worker_pool_idle_test.cpp
 * @brief Verifies idle workers park, resume queued work, and stop during process teardown.
 */
#include <alligator.hpp>
#include "functional_support.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <ctime>
#include <future>
#include <thread>
#include <vector>

namespace {
using buffetalligator::Alligator;
using buffetalligator::make_task;
using functional::require;
constexpr size_t producer_count = 4;
constexpr size_t tasks_per_producer = 64;
using CompletionCounts = std::array<std::atomic<unsigned>, producer_count * tasks_per_producer>;
/** --------------------------------------------------------------------------------------------------------- Await
 * @brief Requires a submitted task to complete without another submission waking the pool.
 */
template<typename Result>
Result await(std::future<Result>& future) {
    require(future.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
        "queued task did not wake a worker");
    return future.get();
}
/** --------------------------------------------------------------------------------------------------------- Advance
 * @brief Checks task-chain ordering and records each completed stage.
 */
size_t advance(std::atomic<size_t>* completed, size_t expected) {
    const size_t previous = completed->fetch_add(1, std::memory_order_relaxed);
    require(previous == expected, "task chain ran out of order or more than once");
    return previous + 1;
}
/** --------------------------------------------------------------------------------------------------------- Continuations
 * @brief Starts a cold worker pool from a waiting-task continuation before any regular submission.
 */
void continuations() {
    std::atomic<size_t> completed{0};
    auto [waiting_task, waiting_future] = make_task<size_t>(advance, &completed, size_t{0});
    auto [worker_task, worker_future] = make_task<size_t>(advance, &completed, size_t{1});
    auto final_future = worker_task.after_this<size_t>(advance, &completed, size_t{2});
    waiting_task.after_this(std::move(worker_task));
    Alligator::inst().submit_waiting(std::move(waiting_task));
    require(await(waiting_future) == 1, "waiting task returned the wrong result");
    require(await(worker_future) == 2, "waiting continuation returned the wrong result");
    require(await(final_future) == 3, "worker continuation returned the wrong result");
}
/** --------------------------------------------------------------------------------------------------------- Idle Wakeups
 * @brief Alternates callable and preconstructed submissions after workers have time to park.
 */
void idle_wakeups() {
    std::atomic<size_t> completed{0};
    for (size_t round = 0; round < 16; ++round) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto callable_future = Alligator::inst().submit<size_t>(advance, &completed, round * 2);
        require(await(callable_future) == round * 2 + 1, "idle callable result differs");
        auto [task, task_future] = make_task<size_t>(advance, &completed, round * 2 + 1);
        Alligator::inst().submit(std::move(task));
        require(await(task_future) == round * 2 + 2, "preconstructed task result differs");
    }
}
/** --------------------------------------------------------------------------------------------------------- Complete Once
 * @brief Records execution of a unique task from a concurrent submission burst.
 */
void complete_once(CompletionCounts* completed, size_t task_index) {
    require((*completed)[task_index].fetch_add(1, std::memory_order_relaxed) == 0,
        "concurrent submission executed more than once");
}
/** --------------------------------------------------------------------------------------------------------- Produce
 * @brief Submits and awaits one producer's independent batch of tasks.
 */
void produce(CompletionCounts* completed, std::barrier<>* start, size_t producer_index) {
    std::vector<std::future<void>> futures;
    futures.reserve(tasks_per_producer);
    start->arrive_and_wait();
    for (size_t task_index = 0; task_index < tasks_per_producer; ++task_index) {
        futures.push_back(Alligator::inst().submit(complete_once, completed,
            producer_index * tasks_per_producer + task_index));
    }
    for (std::future<void>& future : futures) await(future);
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
    idle_wakeups();
    concurrent_producers();
    LOG_INFO_STREAM << "Measuring idle worker CPU before process teardown";
    idle_cpu();
    LOG_INFO_STREAM << "Worker queue checks passed; stopping idle workers during process teardown";
    return 0;
}
