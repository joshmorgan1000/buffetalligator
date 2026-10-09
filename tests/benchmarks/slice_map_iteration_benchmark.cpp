/** --------------------------------------------------------------------------------------------------------- Slice Map Iteration Benchmark
 * @file slice_map_iteration_benchmark.cpp
 * @brief Compares bulk public iteration with the original repeated-size public accessor loops.
 */
#include "benchmark_support.hpp"
#include <alligator/dispatch.hpp>
#include <array>

namespace benchmarks {
using buffetalligator::SliceMap;
/** --------------------------------------------------------------------------------------------------------- Visits
 * @brief Retains one row's callback results without shared writes between dispatched workers.
 */
struct Visits {
    uint64_t identifiers = 0;
    uint64_t values = 0;
    size_t count = 0;
};
/** --------------------------------------------------------------------------------------------------------- Visit
 * @brief Uses the same out-of-line callback work for both public traversal implementations.
 */
__attribute__((noinline)) void visit(int64_t identifier, Slice* slice, void* context) {
    auto& result = (*static_cast<std::vector<Visits>*>(context))[static_cast<size_t>(identifier)];
    result.identifiers = static_cast<uint64_t>(identifier);
    result.values = slice->get_as<uint64_t>();
    ++result.count;
}
/** --------------------------------------------------------------------------------------------------------- Original IDs
 * @brief Reproduces the original identifier loop against an unchanged finite map.
 */
std::vector<int64_t> original_ids(const SliceMap& map) {
    std::vector<int64_t> result;
    result.reserve(map.size());
    for (size_t index = 0; index < map.size(); ++index) {
        result.push_back(map.id<int64_t>(index));
    }
    return result;
}
/** --------------------------------------------------------------------------------------------------------- Original For Each
 * @brief Reproduces the public accessor loop with a retained Slice matching the callback signature.
 */
void original_for_each(const SliceMap& map, std::vector<Visits>& result) {
    for (size_t index = 0; index < map.size(); ++index) {
        Slice retained = map.slice_at(index);
        visit(map.id<int64_t>(index), &retained, &result);
    }
}
/** --------------------------------------------------------------------------------------------------------- Measure IDs
 * @brief Times complete identifier snapshots and checks the last result after timing stops.
 */
template<bool Bulk>
double measure_ids(const SliceMap& map, size_t sweeps, size_t count) {
    std::vector<int64_t> result;
    const auto start = Clock::now();
    for (size_t sweep = 0; sweep < sweeps; ++sweep) {
        if constexpr (Bulk) result = map.ids();
        else result = original_ids(map);
    }
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    require(result.size() == count, "Identifier snapshot lost rows.");
    for (size_t index = 0; index < count; ++index) {
        require(result[index] == static_cast<int64_t>(index), "Identifier snapshot changed order.");
    }
    return seconds;
}
/** --------------------------------------------------------------------------------------------------------- Measure For Each
 * @brief Times callbacks through each implementation and verifies all IDs and payloads were consumed.
 */
template<bool Bulk>
double measure_for_each(const SliceMap& map, size_t sweeps, size_t count) {
    std::vector<Visits> result(count);
    const auto start = Clock::now();
    for (size_t sweep = 0; sweep < sweeps; ++sweep) {
        if constexpr (Bulk) map.for_each(&visit, &result);
        else original_for_each(map, result);
    }
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    for (size_t index = 0; index < count; ++index) {
        require(result[index].count == sweeps && result[index].identifiers == index
            && result[index].values == index,
            "Traversal lost identifiers or selected incorrect payloads.");
    }
    return seconds;
}
/** --------------------------------------------------------------------------------------------------------- Compare
 * @brief Alternates baseline and bulk samples on the same populated production SliceMap.
 */
void compare(const Options& options) {
    const size_t sweeps = std::max(size_t(1), options.lookups / options.items);
    require(options.items <= SIZE_MAX / sweeps, "Iteration operation count overflow.");
    const size_t operations = options.items * sweeps;
    Reporter reporter(options);
    Progress progress("SliceMap iteration", options.timeout);
    SliceMap map(1);
    LOG_INFO_STREAM << "Preparing " << options.items << " deterministic SliceMap rows";
    for (size_t index = 0; index < options.items; ++index) {
        Slice payload(sizeof(uint64_t));
        payload.get_as<uint64_t>() = index;
        map.add_slice(static_cast<int64_t>(index), std::move(payload));
    }
    std::array<std::vector<double>, 4> samples;
    for (size_t sample = 0; sample < options.warmup + options.repetitions; ++sample) {
        LOG_INFO_STREAM << "Iteration " << (sample < options.warmup ? "warmup " : "sample ")
            << (sample < options.warmup ? sample + 1 : sample - options.warmup + 1)
            << ": " << sweeps << " full traversals per method";
        std::array<double, 4> seconds{};
        for (size_t order = 0; order < 2; ++order) {
            if ((sample + order) % 2 == 0) {
                seconds[0] = measure_ids<false>(map, sweeps, options.items);
                seconds[2] = measure_for_each<false>(map, sweeps, options.items);
            } else {
                seconds[1] = measure_ids<true>(map, sweeps, options.items);
                seconds[3] = measure_for_each<true>(map, sweeps, options.items);
            }
        }
        if (sample < options.warmup) continue;
        for (size_t method = 0; method < seconds.size(); ++method) {
            samples[method].push_back(seconds[method]);
        }
    }
    const Shape shape{1, 1, true};
#if defined(__APPLE__)
    const size_t workers = std::thread::hardware_concurrency();
#else
    const size_t workers = static_cast<size_t>(omp_get_max_threads());
#endif
    const Shape parallel_shape{1, std::min(options.items, workers)};
    const double original_identifiers = reporter.report("original-loop", "ids", shape,
        options, operations, samples[0]);
    const double bulk_identifiers = reporter.report("SliceMap", "ids", shape,
        options, operations, samples[1]);
    const double original_callbacks = reporter.report("original-loop", "for_each", shape,
        options, operations, samples[2]);
    const double bulk_callbacks = reporter.report("SliceMap", "for_each", parallel_shape,
        options, operations, samples[3]);
    LOG_INFO_STREAM << "Bulk iteration speedup: ids=" << original_identifiers / bulk_identifiers
        << "x, for_each=" << original_callbacks / bulk_callbacks << "x";
}
} // namespace benchmarks
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs configurable iteration comparisons without mutating the timed map.
 */
int main(int count, char** arguments) {
    using namespace benchmarks;
    try {
        const Options options = parse(count, arguments, false);
        if (options.help) {
            LOG_INFO_STREAM << "Options: --items N (4096) --lookups N (65536) "
                << "--repetitions N (5) --warmup N (1) --timeout SECONDS (120) --csv PATH";
            LOG_INFO_STREAM << "Lookups sets the approximate rows visited per sample; "
                << "SliceMap for_each dispatches callbacks across the available workers.";
            return 0;
        }
        require(options.producers == 0, "Iteration comparison probes worker count; omit topology options.");
        compare(options);
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Iteration benchmark failed: " << error.what();
        return 1;
    }
}
