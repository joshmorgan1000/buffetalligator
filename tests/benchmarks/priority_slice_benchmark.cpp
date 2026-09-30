/** --------------------------------------------------------------------------------------------------------- Priority Slice Benchmark
 * @file priority_slice_benchmark.cpp
 * @brief Measures fixed-capacity raw and typed priority operations with deterministic inputs.
 */
#include <alligator.hpp>
#include <alligator/containers.hpp>
#include <logging.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace {
using buffetalligator::PrioritySlice;
using buffetalligator::PrioritySliceT;
constexpr uint32_t ITEMS = 100000;
constexpr size_t REPETITIONS = 7;
volatile uint64_t checksum = 0;
/// @brief The queue operation measured by one benchmark case.
enum class Workload { accepted_push, rejected_push, pop_and_refill, pop_and_rotate };
/** --------------------------------------------------------------------------------------------------------- Push
 * @brief Pushes the same key and value into the raw or typed queue.
 */
template<bool Typed, typename Queue>
bool push(Queue& queue, uint32_t key) {
    if constexpr (Typed) return queue.push(key, key);
    else {
        const uint64_t word = PrioritySlice::pack(key, key);
        return queue.push(word) != word;
    }
}
/** --------------------------------------------------------------------------------------------------------- Pop
 * @brief Pops one entry from the raw or typed queue.
 */
template<bool Typed, typename Queue>
uint64_t pop(Queue& queue) {
    if constexpr (Typed) {
        uint32_t key;
        uint32_t value;
        if (!queue.pop(key, value)) throw std::runtime_error("typed queue emptied early");
        return PrioritySlice::pack(key, value);
    } else {
        const uint64_t word = queue.pop();
        if (word == PrioritySlice::EMPTY) throw std::runtime_error("raw queue emptied early");
        return word;
    }
}
/** --------------------------------------------------------------------------------------------------------- Measure
 * @brief Times an accepted, rejected, or pop-and-refill workload.
 */
template<bool Typed, Workload Mode>
double measure(size_t capacity) {
    using Queue = std::conditional_t<Typed, PrioritySliceT<uint32_t, uint32_t>, PrioritySlice>;
    Queue queue(capacity);
    for (uint32_t index = 0; index < capacity; ++index) {
        const uint32_t key = Mode == Workload::accepted_push
            ? ITEMS + static_cast<uint32_t>(capacity) + index : index;
        if (!push<Typed>(queue, key)) throw std::runtime_error("failed to fill the queue");
    }
    uint64_t observed = 0;
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t index = 0; index < ITEMS; ++index) {
        if constexpr (Mode == Workload::accepted_push) {
            observed += push<Typed>(queue, ITEMS - index);
        } else if constexpr (Mode == Workload::rejected_push) {
            observed += push<Typed>(queue, ITEMS + static_cast<uint32_t>(capacity) + index);
        } else if constexpr (Mode == Workload::pop_and_refill) {
            const uint64_t word = pop<Typed>(queue);
            observed += PrioritySlice::key_of(word);
            observed += push<Typed>(queue, PrioritySlice::key_of(word));
        } else {
            const uint64_t word = pop<Typed>(queue);
            observed += PrioritySlice::key_of(word) == index;
            observed += push<Typed>(queue, static_cast<uint32_t>(capacity) + index);
        }
    }
    const auto end = std::chrono::steady_clock::now();
    if ((Mode == Workload::accepted_push && observed != ITEMS)
        || (Mode == Workload::rejected_push && observed != 0)
        || (Mode == Workload::pop_and_refill && observed != ITEMS)
        || (Mode == Workload::pop_and_rotate && observed != 2 * ITEMS)) {
        throw std::runtime_error("priority workload returned an unexpected result");
    }
    checksum = checksum + observed;
    return std::chrono::duration<double, std::nano>(end - start).count() / ITEMS;
}
/** --------------------------------------------------------------------------------------------------------- Report
 * @brief Reports the median and range across identical repetitions.
 */
template<bool Typed, Workload Mode>
void report(size_t capacity, const char* operation) {
    std::array<double, REPETITIONS> samples;
    (void)measure<Typed, Mode>(capacity);
    for (double& sample : samples) sample = measure<Typed, Mode>(capacity);
    std::sort(samples.begin(), samples.end());
    LOG_INFO_STREAM << (Typed ? "typed" : "raw") << " capacity=" << capacity << " " << operation
        << ((Mode == Workload::accepted_push || Mode == Workload::rejected_push)
            ? " ns/op median=" : " ns/pair median=")
        << samples[REPETITIONS / 2] << " min=" << samples.front()
        << " max=" << samples.back();
}
} // namespace
int main() {
    for (size_t capacity : {size_t(32), size_t(256)}) {
        report<false, Workload::accepted_push>(capacity, "accepted-push");
        report<true, Workload::accepted_push>(capacity, "accepted-push");
        report<false, Workload::rejected_push>(capacity, "rejected-push");
        report<true, Workload::rejected_push>(capacity, "rejected-push");
        report<false, Workload::pop_and_refill>(capacity, "pop-and-refill");
        report<true, Workload::pop_and_refill>(capacity, "pop-and-refill");
        report<false, Workload::pop_and_rotate>(capacity, "pop-and-rotate");
        report<true, Workload::pop_and_rotate>(capacity, "pop-and-rotate");
    }
    LOG_INFO_STREAM << "PrioritySlice benchmark checksum=" << checksum;
}
