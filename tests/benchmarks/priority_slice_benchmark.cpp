/** --------------------------------------------------------------------------------------------------------- Priority Slice Benchmark
 * @file priority_slice_benchmark.cpp
 * @brief Measures fixed-capacity raw and typed priority operations with deterministic inputs.
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
#include <iomanip>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {
using buffetalligator::PrioritySlice;
using buffetalligator::PrioritySliceT;
constexpr uint32_t ITEMS = 100000;
volatile uint64_t checksum = 0;
/** --------------------------------------------------------------------------------------------------------- Options
 * @brief Configures repeated observations without changing the fixed deterministic operation traces.
 */
struct Options {
    size_t warmup = 2;
    size_t repetitions = 15;
    size_t timeout = 120;
    std::string csv = "priority_samples.csv";
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
    return std::chrono::duration<double>(end - start).count();
}
/** --------------------------------------------------------------------------------------------------------- Report
 * @brief Reports the median and range across identical repetitions.
 */
template<bool Typed, Workload Mode>
void report(const Options& options, std::ofstream& csv, size_t capacity, const char* operation) {
    const char* kind = Typed ? "typed" : "raw";
    const char* unit = Mode == Workload::accepted_push || Mode == Workload::rejected_push
        ? "push" : "pop-push-pair";
    benchmarks::Progress progress(std::string("PrioritySlice ") + kind + ' ' + operation
        + " capacity=" + std::to_string(capacity), options.timeout);
    for (size_t index = 0; index < options.warmup; ++index) (void)measure<Typed, Mode>(capacity);
    std::vector<double> samples(options.repetitions);
    for (size_t index = 0; index < samples.size(); ++index) {
        samples[index] = measure<Typed, Mode>(capacity);
        csv << kind << ',' << capacity << ',' << operation << ',' << unit << ',' << index + 1
            << ',' << ITEMS << ',' << std::setprecision(12) << samples[index] << ','
            << samples[index] * 1e9 / ITEMS << '\n';
    }
    csv.flush();
    benchmarks::require(csv.good(), "Failed to write priority benchmark samples.");
    std::sort(samples.begin(), samples.end());
    const size_t middle = samples.size() / 2;
    const double median = samples.size() % 2 ? samples[middle]
        : (samples[middle - 1] + samples[middle]) / 2;
    LOG_INFO_STREAM << kind << " capacity=" << capacity << ' ' << operation << " ns/" << unit
        << " (aggregate) median=" << median * 1e9 / ITEMS
        << " min=" << samples.front() * 1e9 / ITEMS << " max=" << samples.back() * 1e9 / ITEMS
        << "; measured repetitions=" << samples.size();
}
} // namespace
int main(int count, char** arguments) {
    try {
        const Options options = parse(count, arguments);
        if (options.help) {
            LOG_INFO_STREAM << "Options: --warmup N (2) --repetitions N (15) --timeout SECONDS (120) "
                << "--csv PATH (priority_samples.csv); each sample keeps the fixed 100000-operation trace.";
            return 0;
        }
        std::ofstream csv(options.csv);
        benchmarks::require(csv.is_open(), "Cannot open priority benchmark CSV output.");
        csv << "queue,capacity,workload,iteration_unit,repetition,iterations,seconds,aggregate_nanoseconds_per_iteration\n";
        LOG_INFO_STREAM << "PrioritySlice benchmark: warmups=" << options.warmup
            << "; measured repetitions=" << options.repetitions << "; iterations/sample=" << ITEMS;
#ifndef NDEBUG
        LOG_WARN_STREAM << "Assertions enabled; these timings are not Release performance results.";
#endif
        for (size_t capacity : {size_t(32), size_t(256)}) {
            report<false, Workload::accepted_push>(options, csv, capacity, "accepted-push");
            report<true, Workload::accepted_push>(options, csv, capacity, "accepted-push");
            report<false, Workload::rejected_push>(options, csv, capacity, "rejected-push");
            report<true, Workload::rejected_push>(options, csv, capacity, "rejected-push");
            report<false, Workload::pop_and_refill>(options, csv, capacity, "pop-and-refill");
            report<true, Workload::pop_and_refill>(options, csv, capacity, "pop-and-refill");
            report<false, Workload::pop_and_rotate>(options, csv, capacity, "pop-and-rotate");
            report<true, Workload::pop_and_rotate>(options, csv, capacity, "pop-and-rotate");
        }
        LOG_INFO_STREAM << "PrioritySlice benchmark checksum=" << checksum << "; raw samples=" << options.csv;
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "PrioritySlice benchmark failed: " << error.what();
        return 1;
    }
}
