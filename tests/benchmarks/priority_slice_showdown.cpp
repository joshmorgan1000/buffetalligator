/** --------------------------------------------------------------------------------------------------------- Priority Slice Showdown
 * @file priority_slice_showdown.cpp
 * @brief Times PrioritySliceT, HeapSliceT, and std::priority_queue on the same deterministic
 * workloads at three capacities, single threaded, giving the heap whichever order suits each
 * workload.
 */
#include <alligator.hpp>
#include <alligator/containers.hpp>
#include <logging.hpp>
#include "benchmark_support.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
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
volatile uint64_t checksum = 0;
/** --------------------------------------------------------------------------------------------------------- Options
 * @brief Configures repeated observations of the fixed deterministic operation traces.
 */
struct Options {
    size_t warmup = 2;
    size_t repetitions = 15;
    size_t timeout = 120;
    std::string csv = "priority_showdown_samples.csv";
    bool help = false;
};
/** --------------------------------------------------------------------------------------------------------- Parse
 * @brief Parses warmups, measured repetitions, progress timeout, and raw sample output.
 */
Options parse(int count, char** arguments) {
    Options options;
    for (int index = 1; index < count; ++index) {
        const std::string_view argument(arguments[index]);
        if (argument == "--help") { options.help = true; continue; }
        benchmarks::require(index + 1 < count, "An option is missing its value; use --help.");
        const std::string_view value(arguments[++index]);
        if (argument == "--csv") { options.csv = value; continue; }
        size_t number = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        benchmarks::require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size(),
            "Expected an unsigned integer option value; use --help.");
        benchmarks::require(number != 0 || argument == "--warmup", "Only --warmup accepts zero.");
        if (argument == "--warmup") options.warmup = number;
        else if (argument == "--repetitions") options.repetitions = number;
        else if (argument == "--timeout") options.timeout = number;
        else throw std::runtime_error("Unknown option: " + std::string(argument));
    }
    return options;
}
/// @brief Which container a measurement drives.
enum class Subject { slice, heap_slice, std_heap };
/// @brief The queue operation measured by one benchmark case.
enum class Workload { accepted_push, rejected_push, pop_and_refill, pop_and_rotate };
/** --------------------------------------------------------------------------------------------------------- Std Heap
 * @brief A std::priority_queue in the order that suits the workload, with bounded push.
 */
template<Workload Mode>
struct StdHeap {
    using Heap = std::conditional_t<Mode == Workload::accepted_push
        || Mode == Workload::rejected_push, MaxHeap, MinHeap>;
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
 * @brief Drives one workload on a subject and returns measured elapsed seconds.
 */
template<Workload Mode, typename Target>
double run(Target& target, size_t capacity) {
    for (uint32_t index = 0; index < capacity; ++index) {
        const uint32_t key = Mode == Workload::accepted_push
            ? ITEMS + static_cast<uint32_t>(capacity) + index : index;
        if (!target.push(key, index)) throw std::runtime_error("failed to fill");
    }
    uint64_t observed = 0;
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t index = 0; index < ITEMS; ++index) {
        if constexpr (Mode == Workload::accepted_push) {
            observed += target.push(ITEMS - index, index);
        } else if constexpr (Mode == Workload::rejected_push) {
            observed += target.push(ITEMS + static_cast<uint32_t>(capacity) + index, index);
        } else if constexpr (Mode == Workload::pop_and_refill) {
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
    if ((Mode == Workload::accepted_push && observed != ITEMS)
        || (Mode == Workload::rejected_push && observed != 0)
        || (Mode == Workload::pop_and_refill && observed != ITEMS)
        || (Mode == Workload::pop_and_rotate && observed != 2 * ITEMS)) {
        throw std::runtime_error("workload returned an unexpected result");
    }
    checksum = checksum + observed;
    return std::chrono::duration<double>(end - start).count();
}
/** --------------------------------------------------------------------------------------------------------- Measure
 * @brief Builds a fresh subject and times one workload on it.
 */
template<Subject Which, Workload Mode>
double measure(size_t capacity) {
    if constexpr (Which == Subject::slice) {
        SliceSubject<PrioritySliceT<uint32_t, uint32_t>> target(capacity);
        return run<Mode>(target, capacity);
    } else if constexpr (Which == Subject::heap_slice) {
        SliceSubject<HeapSliceT<uint32_t, uint32_t>> target(capacity);
        return run<Mode>(target, capacity);
    } else {
        StdHeap<Mode> target{{}, capacity};
        return run<Mode>(target, capacity);
    }
}
/** --------------------------------------------------------------------------------------------------------- Report Subject
 * @brief Saves every measured sample and reports its aggregate per-iteration median and range.
 */
template<Subject Which, Workload Mode>
void report_subject(const Options& options, std::ofstream& csv, size_t capacity, const char* operation) {
    const char* kind = Which == Subject::slice ? "PrioritySliceT"
        : Which == Subject::heap_slice ? "HeapSliceT" : "std::priority_queue";
    const char* unit = Mode == Workload::accepted_push || Mode == Workload::rejected_push
        ? "push" : "pop-push-pair";
    benchmarks::Progress progress(std::string("Priority showdown ") + kind + ' ' + operation
        + " capacity=" + std::to_string(capacity), options.timeout);
    for (size_t index = 0; index < options.warmup; ++index) (void)measure<Which, Mode>(capacity);
    std::vector<double> samples(options.repetitions);
    for (size_t index = 0; index < samples.size(); ++index) {
        samples[index] = measure<Which, Mode>(capacity);
        csv << kind << ',' << capacity << ',' << operation << ',' << unit << ',' << index + 1
            << ',' << ITEMS << ',' << std::setprecision(12) << samples[index] << ','
            << samples[index] * 1e9 / ITEMS << '\n';
    }
    csv.flush();
    benchmarks::require(csv.good(), "Failed to write priority showdown samples.");
    std::sort(samples.begin(), samples.end());
    const size_t middle = samples.size() / 2;
    const double median = samples.size() % 2 ? samples[middle]
        : (samples[middle - 1] + samples[middle]) / 2;
    LOG_INFO_STREAM << kind << " capacity=" << capacity << ' ' << operation << " ns/" << unit
        << " (aggregate) median=" << median * 1e9 / ITEMS
        << " min=" << samples.front() * 1e9 / ITEMS << " max=" << samples.back() * 1e9 / ITEMS
        << "; measured repetitions=" << samples.size();
}
/** --------------------------------------------------------------------------------------------------------- Report
 * @brief Measures all three subjects on the same workload and capacity.
 */
template<Workload Mode>
void report(const Options& options, std::ofstream& csv, size_t capacity, const char* workload) {
    report_subject<Subject::slice, Mode>(options, csv, capacity, workload);
    report_subject<Subject::heap_slice, Mode>(options, csv, capacity, workload);
    report_subject<Subject::std_heap, Mode>(options, csv, capacity, workload);
}
} // namespace
int main(int count, char** arguments) {
    try {
        const Options options = parse(count, arguments);
        if (options.help) {
            LOG_INFO_STREAM << "Options: --warmup N (2) --repetitions N (15) --timeout SECONDS (120) "
                << "--csv PATH (priority_showdown_samples.csv); each sample keeps the fixed "
                << "100000-operation trace at capacities 32, 256, and 1024.";
            return 0;
        }
        std::ofstream csv(options.csv);
        benchmarks::require(csv.is_open(), "Cannot open priority showdown CSV output.");
        csv << "queue,capacity,workload,iteration_unit,repetition,iterations,seconds,aggregate_nanoseconds_per_iteration\n";
        LOG_INFO_STREAM << "PrioritySlice showdown: warmups=" << options.warmup
            << "; measured repetitions=" << options.repetitions << "; iterations/sample=" << ITEMS;
#ifndef NDEBUG
        LOG_WARN_STREAM << "Assertions enabled; these timings are not Release performance results.";
#endif
        for (size_t capacity : {size_t(32), size_t(256), size_t(1024)}) {
            report<Workload::accepted_push>(options, csv, capacity, "accepted-push");
            report<Workload::rejected_push>(options, csv, capacity, "rejected-push");
            report<Workload::pop_and_refill>(options, csv, capacity, "pop-and-refill");
            report<Workload::pop_and_rotate>(options, csv, capacity, "pop-and-rotate");
        }
        LOG_INFO_STREAM << "PrioritySlice showdown checksum=" << checksum
            << "; raw samples=" << options.csv;
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "PrioritySlice showdown failed: " << error.what();
        return 1;
    }
}
