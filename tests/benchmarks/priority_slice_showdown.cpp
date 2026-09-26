/** --------------------------------------------------------------------------------------------------------- Priority Slice Showdown
 * @file priority_slice_showdown.cpp
 * @brief Times PrioritySliceT, HeapSliceT, and std::priority_queue on the same deterministic
 * workloads at three capacities, single threaded, giving the heap whichever order suits each
 * workload.
 */
#include <alligator.hpp>
#include <logging.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <queue>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using buffetalligator::HeapSliceT;
using buffetalligator::PrioritySliceT;
using Entry = std::pair<uint32_t, uint32_t>;
/// @brief Max-heap keeping the smallest keys under a bound, for the push workloads.
using MaxHeap = std::priority_queue<Entry>;
/// @brief Min-heap popping the best key first, for the pop workloads.
using MinHeap = std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>>;
constexpr uint32_t ITEMS = 100000;
constexpr size_t REPETITIONS = 7;
volatile uint64_t checksum = 0;
/// @brief Which container a measurement drives.
enum class Subject { slice, heap_slice, std_heap };
/** --------------------------------------------------------------------------------------------------------- Std Heap
 * @brief A std::priority_queue in the order that suits the workload, with bounded push.
 */
template<int Workload>
struct StdHeap {
    using Heap = std::conditional_t<Workload < 2, MaxHeap, MinHeap>;
    Heap heap;
    size_t capacity;
    bool push(uint32_t key, uint32_t value) {
        if (heap.size() < capacity) {
            heap.emplace(key, value);
            return true;
        }
        if constexpr (std::is_same_v<Heap, MaxHeap>) {
            if (key >= heap.top().first) return false;
            heap.pop();
            heap.emplace(key, value);
            return true;
        } else {
            throw std::runtime_error("min-heap workloads never push into a full heap");
        }
    }
    uint32_t pop() {
        if constexpr (std::is_same_v<Heap, MinHeap>) {
            if (heap.empty()) throw std::runtime_error("heap emptied early");
            const uint32_t key = heap.top().first;
            heap.pop();
            return key;
        } else {
            throw std::runtime_error("max-heap workloads never pop");
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Slice Subject
 * @brief A slice-backed container with the typed push and pop shape the workloads expect.
 */
template<typename Container>
struct SliceSubject {
    Container container;
    explicit SliceSubject(size_t capacity) : container(capacity) {}
    bool push(uint32_t key, uint32_t value) { return container.push(key, value); }
    uint32_t pop() {
        uint32_t key;
        uint32_t value;
        if (!container.pop(key, value)) throw std::runtime_error("container emptied early");
        return key;
    }
};
/** --------------------------------------------------------------------------------------------------------- Run
 * @brief Drives one workload on a subject and returns nanoseconds per operation.
 */
template<int Workload, typename Target>
double run(Target& target, size_t capacity) {
    for (uint32_t index = 0; index < capacity; ++index) {
        const uint32_t key =
            Workload == 0 ? ITEMS + static_cast<uint32_t>(capacity) + index : index;
        if (!target.push(key, index)) throw std::runtime_error("failed to fill");
    }
    uint64_t observed = 0;
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t index = 0; index < ITEMS; ++index) {
        if constexpr (Workload == 0) {
            observed += target.push(ITEMS - index, index);
        } else if constexpr (Workload == 1) {
            observed += target.push(ITEMS + static_cast<uint32_t>(capacity) + index, index);
        } else if constexpr (Workload == 2) {
            const uint32_t key = target.pop();
            observed += key;
            observed += target.push(key, index);
        } else {
            const uint32_t key = target.pop();
            observed += key == index;
            observed += target.push(static_cast<uint32_t>(capacity) + index, index);
        }
    }
    const auto end = std::chrono::steady_clock::now();
    if ((Workload == 0 && observed != ITEMS) || (Workload == 1 && observed != 0)
        || (Workload == 2 && observed != ITEMS) || (Workload == 3 && observed != 2 * ITEMS)) {
        throw std::runtime_error("workload returned an unexpected result");
    }
    checksum = checksum + observed;
    return std::chrono::duration<double, std::nano>(end - start).count() / ITEMS;
}
/** --------------------------------------------------------------------------------------------------------- Measure
 * @brief Builds a fresh subject and times one workload on it.
 */
template<Subject Which, int Workload>
double measure(size_t capacity) {
    if constexpr (Which == Subject::slice) {
        SliceSubject<PrioritySliceT<uint32_t, uint32_t>> target(capacity);
        return run<Workload>(target, capacity);
    } else if constexpr (Which == Subject::heap_slice) {
        SliceSubject<HeapSliceT<uint32_t, uint32_t>> target(capacity);
        return run<Workload>(target, capacity);
    } else {
        StdHeap<Workload> target{{}, capacity};
        return run<Workload>(target, capacity);
    }
}
/** --------------------------------------------------------------------------------------------------------- Median
 * @brief Runs a workload with one warmup and returns the median of the timed repetitions.
 */
template<Subject Which, int Workload>
double median(size_t capacity) {
    std::array<double, REPETITIONS> samples;
    (void)measure<Which, Workload>(capacity);
    for (double& sample : samples) sample = measure<Which, Workload>(capacity);
    std::sort(samples.begin(), samples.end());
    return samples[REPETITIONS / 2];
}
/** --------------------------------------------------------------------------------------------------------- Report
 * @brief Logs one workload row: the three subjects at one capacity.
 */
template<int Workload>
void report(size_t capacity, const char* workload) {
    const double slice = median<Subject::slice, Workload>(capacity);
    const double heap_slice = median<Subject::heap_slice, Workload>(capacity);
    const double std_heap = median<Subject::std_heap, Workload>(capacity);
    LOG_INFO_STREAM << "capacity=" << capacity << " " << workload
        << (Workload < 2 ? " ns/op" : " ns/pair") << " PrioritySliceT=" << slice
        << " HeapSliceT=" << heap_slice << " std::priority_queue=" << std_heap;
}
} // namespace
int main() {
    for (size_t capacity : {size_t(32), size_t(256), size_t(1024)}) {
        report<0>(capacity, "accepted-push");
        report<1>(capacity, "rejected-push");
        report<2>(capacity, "pop-and-refill");
        report<3>(capacity, "pop-and-rotate");
    }
    LOG_INFO_STREAM << "PrioritySlice showdown checksum=" << checksum;
}
