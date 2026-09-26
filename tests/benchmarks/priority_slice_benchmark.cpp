/** --------------------------------------------------------------------------------------------------------- Priority Slice Benchmark
 * @file priority_slice_benchmark.cpp
 * @brief Measures fixed-capacity raw and typed priority operations with deterministic inputs.
 */
#include <alligator.hpp>
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
template<bool Typed, int Workload>
double measure(size_t capacity) {
    using Queue = std::conditional_t<Typed, PrioritySliceT<uint32_t, uint32_t>, PrioritySlice>;
    Queue queue(capacity);
    for (uint32_t index = 0; index < capacity; ++index) {
        const uint32_t key = Workload == 0
            ? ITEMS + static_cast<uint32_t>(capacity) + index : index;
        if (!push<Typed>(queue, key)) throw std::runtime_error("failed to fill the queue");
    }
    uint64_t observed = 0;
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t index = 0; index < ITEMS; ++index) {
        if constexpr (Workload == 0) {
            observed += push<Typed>(queue, ITEMS - index);
        } else if constexpr (Workload == 1) {
            observed += push<Typed>(queue, ITEMS + static_cast<uint32_t>(capacity) + index);
        } else if constexpr (Workload == 2) {
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
    if ((Workload == 0 && observed != ITEMS) || (Workload == 1 && observed != 0)
        || (Workload == 2 && observed != ITEMS)
        || (Workload == 3 && observed != 2 * ITEMS)) {
        throw std::runtime_error("priority workload returned an unexpected result");
    }
    checksum = checksum + observed;
    return std::chrono::duration<double, std::nano>(end - start).count() / ITEMS;
}
/** --------------------------------------------------------------------------------------------------------- Report
 * @brief Reports the median and range across identical repetitions.
 */
template<bool Typed, int Workload>
void report(size_t capacity, const char* operation) {
    std::array<double, REPETITIONS> samples;
    (void)measure<Typed, Workload>(capacity);
    for (double& sample : samples) sample = measure<Typed, Workload>(capacity);
    std::sort(samples.begin(), samples.end());
    LOG_INFO_STREAM << (Typed ? "typed" : "raw") << " capacity=" << capacity << " " << operation
        << (Workload < 2 ? " ns/op median=" : " ns/pair median=")
        << samples[REPETITIONS / 2] << " min=" << samples.front()
        << " max=" << samples.back();
}
} // namespace
int main() {
    for (size_t capacity : {size_t(32), size_t(256)}) {
        report<false, 0>(capacity, "accepted-push");
        report<true, 0>(capacity, "accepted-push");
        report<false, 1>(capacity, "rejected-push");
        report<true, 1>(capacity, "rejected-push");
        report<false, 2>(capacity, "pop-and-refill");
        report<true, 2>(capacity, "pop-and-refill");
        report<false, 3>(capacity, "pop-and-rotate");
        report<true, 3>(capacity, "pop-and-rotate");
    }
    LOG_INFO_STREAM << "PrioritySlice benchmark checksum=" << checksum;
}
