/** --------------------------------------------------------------------------------------------------------- Kitchen Catering Setup Test
 * @file kitchen_catering_setup_test.cpp
 * @brief Checks setup-only registration and reusable catering arguments through the public ABI.
 */
#include <alligator/kitchen.hpp>
#include "functional_support.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <latch>
#include <memory>
#include <type_traits>
#include <utility>

namespace {
using buffetalligator::Kitchen;
using buffetalligator::Slice;
using functional::require;
constexpr size_t participants = 3;
constexpr size_t rounds = 3;
std::atomic<size_t> preparation_calls{0};
std::atomic<size_t> catering_calls{0};
std::atomic<size_t> configuration_destructions{0};
/** --------------------------------------------------------------------------------------------------------- Configuration
 * @brief Tracks an aligned move-only configuration retained across multiple catering rounds.
 */
struct alignas(128) Configuration {
    uint64_t value;
    size_t prepared_rounds = 0;
    explicit Configuration(uint64_t input) : value(input) {}
    Configuration(const Configuration&) = delete;
    Configuration& operator=(const Configuration&) = delete;
    Configuration(Configuration&& other) noexcept
        : value(std::exchange(other.value, 0)), prepared_rounds(other.prepared_rounds) {}
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Counts destruction only for the configuration that still owns its value.
     */
    ~Configuration() {
        if (value != 0) configuration_destructions.fetch_add(1, std::memory_order_relaxed);
    }
};
/** --------------------------------------------------------------------------------------------------------- Work
 * @brief Owns one round's completion latch and independently written participant results.
 */
struct Work {
    size_t expected_round;
    size_t preparations = 0;
    uintptr_t configuration_address = 0;
    std::array<std::atomic<size_t>, participants> executions{};
    std::array<uint64_t, participants> results{};
    std::latch completed{static_cast<std::ptrdiff_t>(participants)};
    explicit Work(size_t round) : expected_round(round) {}
};
static_assert(std::is_trivially_destructible_v<Work>);
/** --------------------------------------------------------------------------------------------------------- Prepare
 * @brief Prepares each work Slice exactly once using the same retained configuration.
 */
void prepare(Slice slice, Configuration& configuration) {
    Work& work = slice.get_as<Work>();
    const uintptr_t address = reinterpret_cast<uintptr_t>(&configuration);
    require(address % alignof(Configuration) == 0, "catering configuration is misaligned");
    require(configuration.value == 17, "catering configuration was moved out or lost");
    require(work.preparations == 0, "work was prepared more than once");
    require(++configuration.prepared_rounds == work.expected_round,
        "catering configuration did not retain its state across rounds");
    work.configuration_address = address;
    work.preparations = 1;
    preparation_calls.fetch_add(1, std::memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Cater
 * @brief Observes preparation before recording exactly one execution for each participant.
 */
void cater(Slice slice, size_t rank, Configuration& configuration) {
    Work& work = slice.get_as<Work>();
    require(rank < participants, "catering started an unexpected participant");
    require(work.preparations == 1, "catering started before preparation completed");
    require(work.configuration_address == reinterpret_cast<uintptr_t>(&configuration),
        "participant received a different configuration from preparation");
    require(configuration.value == 17 && configuration.prepared_rounds == work.expected_round,
        "participant did not observe the prepared configuration");
    require(work.executions[rank].fetch_add(1, std::memory_order_relaxed) == 0,
        "a catering participant executed the same work more than once");
    work.results[rank] = configuration.value + work.expected_round + rank;
    catering_calls.fetch_add(1, std::memory_order_relaxed);
    work.completed.count_down();
}
/** --------------------------------------------------------------------------------------------------------- Failing Work
 * @brief Selects a failing phase while retaining independently counted rank executions.
 */
struct FailingWork {
    bool fail_preparation = false;
    size_t failing_rank = participants;
    std::array<std::atomic<size_t>, participants> executions{};
    std::latch completed{static_cast<std::ptrdiff_t>(participants)};
};
/** --------------------------------------------------------------------------------------------------------- Prepare Failure
 * @brief Throws before rank execution when the request selects a preparation failure.
 */
void prepare_failure(Slice data) {
    if (data.get_as<FailingWork>().fail_preparation) {
        ALLIGATOR_KITCHEN_THROW("expected catering preparation failure");
    }
}
/** --------------------------------------------------------------------------------------------------------- Cater Failure
 * @brief Counts every rank and throws from the selected failing participant.
 */
void cater_failure(Slice data, size_t rank) {
    FailingWork& work = data.get_as<FailingWork>();
    work.executions[rank].fetch_add(1, std::memory_order_relaxed);
    if (rank == work.failing_rank) {
        ALLIGATOR_KITCHEN_THROW("expected catering rank failure");
    }
    work.completed.count_down();
}
/** --------------------------------------------------------------------------------------------------------- Failure Recovery
 * @brief Uses a later successful round to prove failed phases leave the team reusable.
 */
void failure_recovery() {
    Kitchen::cater("catering-failure-test", &prepare_failure, &cater_failure, uint8_t{2}).get();
    bool duplicate_failed = false;
    try {
        Kitchen::cater(
            "catering-failure-test",
            &prepare_failure,
            &cater_failure,
            uint8_t{2}
        ).get();
    } catch (const buffetalligator::KitchenException&) {
        duplicate_failed = true;
    }
    require(duplicate_failed, "duplicate catering setup did not fail through its future");
    const auto* placement = buffetalligator::BuffetDescriptors::descriptor_for(
        static_cast<buffetalligator::AlignedHeapBuffer*>(nullptr));
    std::array<Slice, 3> requests;
    for (Slice& request : requests) {
        request = Slice(sizeof(FailingWork), placement);
        std::construct_at(request.data<FailingWork>());
    }
    requests[0].get_as<FailingWork>().fail_preparation = true;
    requests[1].get_as<FailingWork>().failing_rank = 1;
    auto* queue = Kitchen::inst().cater_event("catering-failure-test");
    for (Slice& request : requests) {
        require(queue->enqueue(request), "catering failure test could not enqueue a request");
    }
    requests[2].get_as<FailingWork>().completed.wait();
    for (size_t rank = 0; rank < participants; ++rank) {
        require(requests[0].get_as<FailingWork>().executions[rank].load() == 0,
            "preparation failure still executed a catering rank");
        require(requests[1].get_as<FailingWork>().executions[rank].load() == 1,
            "one failing rank prevented another rank from executing");
        require(requests[2].get_as<FailingWork>().executions[rank].load() == 1,
            "phase failure prevented the following successful round");
    }
}
/** --------------------------------------------------------------------------------------------------------- Make Work
 * @brief Constructs a completion latch and result slots inside a host-accessible work Slice.
 */
Slice make_work(size_t round) {
    Slice slice(sizeof(Work), buffetalligator::BuffetDescriptors::descriptor_for(
        static_cast<buffetalligator::AlignedHeapBuffer*>(nullptr)));
    std::construct_at(slice.data<Work>(), round);
    return slice;
}
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Registers one team and supplies three work Slices through its public queue.
 */
int main() {
    LOG_INFO_STREAM << "Checking catering registration and retained variadic configuration";
    Kitchen::cater("catering-setup-test", &prepare, &cater, uint8_t{2}, Configuration{17}).get();
    require(preparation_calls.load() == 0 && catering_calls.load() == 0,
        "registration ran catering work before any work Slice was queued");
    require(configuration_destructions.load() == 0,
        "registration destroyed its persistent configuration");
    auto* queue = Kitchen::inst().cater_event("catering-setup-test");
    require(queue != nullptr, "completed registration did not publish its queue");
    std::array<Slice, rounds> work_slices;
    for (size_t index = 0; index < rounds; ++index) {
        work_slices[index] = make_work(index + 1);
        require(queue->enqueue(work_slices[index]), "catering queue rejected a work Slice");
    }
    LOG_INFO_STREAM << "Checking preparation ordering and participant results across three rounds";
    uintptr_t configuration_address = 0;
    for (size_t index = 0; index < rounds; ++index) {
        Work& work = work_slices[index].get_as<Work>();
        work.completed.wait();
        if (index == 0) configuration_address = work.configuration_address;
        require(work.configuration_address == configuration_address,
            "catering replaced its stored configuration between rounds");
        require(work.executions[0].load() == 1 && work.executions[1].load() == 1
            && work.executions[2].load() == 1, "a catering participant was lost");
        const uint64_t expected = 17 + work.expected_round;
        require(work.results[0] == expected && work.results[1] == expected + 1
            && work.results[2] == expected + 2, "catering produced incorrect rank results");
    }
    require(preparation_calls.load() == rounds && catering_calls.load() == rounds * participants,
        "catering did not execute the expected number of phases");
    require(configuration_destructions.load() == 0,
        "catering released its configuration while its registration remained alive");
    LOG_INFO_STREAM << "Checking duplicate setup and phase failure recovery";
    failure_recovery();
    LOG_INFO_STREAM << "Catering setup and reusable argument checks passed";
    return 0;
}
