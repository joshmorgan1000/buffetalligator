/** --------------------------------------------------------------------------------------------------------- Dispatch Test
 * @file dispatch_test.cpp
 * @brief Verifies parallel ranges, packaged completions, and completed task-team work.
 */
#include <alligator/dispatch.hpp>
#include "functional_support.hpp"
#include <array>
#include <atomic>
#include <span>
#include <tuple>

namespace {
using buffetalligator::PackagedFunction;
using buffetalligator::TaskForce;
using buffetalligator::dispatch_for;
using buffetalligator::dispatch_foreach;
using buffetalligator::dispatch_parallel;
using functional::require;
std::array<std::atomic<unsigned>, 67> range_visits{};
/** --------------------------------------------------------------------------------------------------------- Visit Index
 * @brief Records one parallel range invocation.
 */
void visit_index(size_t index) { range_visits[index].fetch_add(1, std::memory_order_relaxed); }
/** --------------------------------------------------------------------------------------------------------- Double Element
 * @brief Modifies one element through its original container reference.
 */
void double_element(int& value) { value *= 2; }
/** --------------------------------------------------------------------------------------------------------- Parallel Loops
 * @brief Verifies offset bounds, empty ranges, and mutation through an indexed view.
 */
void parallel_loops() {
    dispatch_for(3, 64, &visit_index);
    dispatch_for(9, 9, &visit_index);
    for (size_t index = 0; index < range_visits.size(); ++index) {
        require(range_visits[index].load() == (index >= 3 && index < 64 ? 1u : 0u),
            "dispatch_for did not visit exactly its half-open range");
    }
    std::array<int, 4> values{1, 2, 3, 4};
    std::span<int> elements(values);
    dispatch_foreach(elements, &double_element);
    require(values == std::array<int, 4>{2, 4, 6, 8},
        "dispatch_foreach did not update the original elements");
}
/** --------------------------------------------------------------------------------------------------------- Package State
 * @struct PackageState
 * @brief Records one package's result and completion callback.
 */
struct PackageState {
    unsigned calls = 0;
    unsigned completions = 0;
    int result = 0;
};
/** --------------------------------------------------------------------------------------------------------- Package Value
 * @brief Produces a result and records one invocation.
 */
int package_value(PackageState* state, int value) {
    ++state->calls;
    return value * 3;
}
/** --------------------------------------------------------------------------------------------------------- Package Value Complete
 * @brief Records a value result and checks the supplied arguments.
 */
void package_value_complete(int result, std::tuple<PackageState*, int> arguments) {
    PackageState* state = std::get<0>(arguments);
    require(result == std::get<1>(arguments) * 3, "packaged result differs");
    state->result = result;
    ++state->completions;
}
/** --------------------------------------------------------------------------------------------------------- Package Void
 * @brief Records a void task invocation.
 */
void package_void(PackageState* state) { ++state->calls; }
/** --------------------------------------------------------------------------------------------------------- Package Void Complete
 * @brief Records the void task's completion callback.
 */
void package_void_complete(std::tuple<PackageState*> arguments) {
    ++std::get<0>(arguments)->completions;
}
/** --------------------------------------------------------------------------------------------------------- Parallel Invocable
 * @brief Checks that dispatch accepts only a variadic pack of packaged function specializations.
 */
template<typename... Functions>
concept ParallelInvocable = requires(Functions&&... functions) {
    dispatch_parallel(std::forward<Functions>(functions)...);
};
using ValuePackage = PackagedFunction<decltype(&package_value), PackageState*, int>;
using VoidPackage = PackagedFunction<decltype(&package_void), PackageState*>;
static_assert(ParallelInvocable<ValuePackage&, VoidPackage&>);
static_assert(ParallelInvocable<ValuePackage, VoidPackage, ValuePackage>);
static_assert(!ParallelInvocable<void (*)()>);
static_assert(!ParallelInvocable<ValuePackage&, void (*)()>);
/** --------------------------------------------------------------------------------------------------------- Parallel Packages
 * @brief Requires value and void packages to complete before parallel dispatch returns.
 */
void parallel_packages() {
    PackageState value_state;
    PackageState void_state;
    PackagedFunction value(&package_value, std::tuple{&value_state, 7}, &package_value_complete);
    PackagedFunction nothing(&package_void, std::tuple{&void_state}, &package_void_complete);
    dispatch_parallel(value, nothing);
    require(value_state.calls == 1 && value_state.completions == 1 && value_state.result == 21,
        "value package did not complete before dispatch returned");
    require(void_state.calls == 1 && void_state.completions == 1,
        "void package did not complete before dispatch returned");
}
/** --------------------------------------------------------------------------------------------------------- Team Item
 * @struct TeamItem
 * @brief Records phase visibility and exact worker participation for one team item.
 */
struct TeamItem {
    unsigned warmed = 0;
    std::atomic<unsigned> tasks{0};
};
/** --------------------------------------------------------------------------------------------------------- Team Warmup
 * @brief Publishes a non-atomic warmup value through the team's phase barrier.
 */
void team_warmup(void* item, void*) { ++static_cast<TeamItem*>(item)->warmed; }
/** --------------------------------------------------------------------------------------------------------- Team Task
 * @brief Verifies warmup visibility and records one worker's invocation.
 */
void team_task(void* item, void*) {
    TeamItem& record = *static_cast<TeamItem*>(item);
    require(record.warmed == 1, "team task preceded its item warmup");
    record.tasks.fetch_add(1, std::memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Queued Team
 * @brief Verifies every team member completes each queued item before callers stop the team.
 */
void queued_team() {
    constexpr size_t worker_count = 4;
    std::array<TeamItem, 16> items{};
    std::array<std::future<void*>, 16> futures;
    TaskForce force(worker_count, &team_warmup, &team_task, nullptr);
    for (size_t index = 0; index < items.size(); ++index) {
        futures[index] = force.enqueue(&items[index]);
    }
    for (size_t index = 0; index < items.size(); ++index) {
        require(futures[index].get() == &items[index], "team future returned another item");
        require(items[index].warmed == 1 && items[index].tasks.load() == worker_count,
            "team future completed before every worker ran its task");
    }
    force.stop();
}
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs the public dispatch and completed task-force contracts.
 */
int main() {
    LOG_INFO_STREAM << "Checking dispatch ranges, package callbacks, and task-team phases";
    parallel_loops();
    parallel_packages();
    queued_team();
    LOG_INFO_STREAM << "Dispatch contracts passed";
    return 0;
}
