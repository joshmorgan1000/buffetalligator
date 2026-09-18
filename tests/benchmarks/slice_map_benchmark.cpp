/** --------------------------------------------------------------------------------------------------------- Slice Map Benchmark
 * @file slice_map_benchmark.cpp
 * @brief Compares synchronized publication and lookup phases against std::unordered_map.
 */
#include "benchmark_support.hpp"
#include <loggingutils.hpp>
#include <array>
#include <sstream>
#include <unordered_map>

namespace benchmarks {
/** --------------------------------------------------------------------------------------------------------- Slice Rows
 * @brief Adapts the public SliceMap operations without adding benchmark-side synchronization.
 */
class SliceRows {
private:
    buffetalligator::SliceMap rows_;
public:
    explicit SliceRows(size_t count) : rows_(count) {}
    void reset() { rows_.reset(); }
    void insert(int64_t identifier, Slice&& payload) {
        rows_.add_slice(identifier, std::move(payload));
    }
    Slice lookup(int64_t identifier) { return rows_.get_slice(identifier); }
    /** ------------------------------------------------------------------------------------------- Validate
     * @brief Checks every published row at quiescence without performing a second lookup workload.
     */
    void validate(size_t count) const {
        require(rows_.size() == count, "SliceMap lost an insertion.");
        std::vector<unsigned char> seen(count, 0);
        for (size_t slot = 0; slot < count; ++slot) {
            const auto identifier = rows_.id<uint64_t>(slot);
            require(identifier < count && seen[identifier] == 0, "SliceMap row identity differs.");
            seen[identifier] = 1;
            const Slice payload = rows_.slice_at(slot);
            require(payload && payload.get_as<uint64_t>() == identifier,
                "SliceMap row payload differs.");
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Hash Rows
 * @brief Reserves std::unordered_map buckets and protects concurrent calls with one mutex.
 */
template<bool Synchronized>
class HashRows {
private:
    std::unordered_map<int64_t, Slice> rows_;
    std::mutex mutex_;
public:
    explicit HashRows(size_t count) { rows_.reserve(count); }
    void reset() { rows_.clear(); }
    /** ------------------------------------------------------------------------------------------- Insert
     * @brief Measures node allocation and insertion under the baseline's container mutex.
     */
    void insert(int64_t identifier, Slice&& payload) {
        std::unique_lock lock(mutex_, std::defer_lock);
        if constexpr (Synchronized) lock.lock();
        rows_.emplace(identifier, std::move(payload));
    }
    /** ------------------------------------------------------------------------------------------- Lookup
     * @brief Shares the found Slice claim while its entry is protected by the container mutex.
     */
    Slice lookup(int64_t identifier) {
        std::unique_lock lock(mutex_, std::defer_lock);
        if constexpr (Synchronized) lock.lock();
        const auto found = rows_.find(identifier);
        return found == rows_.end() ? Slice() : found->second.slice();
    }
    /** ------------------------------------------------------------------------------------------- Validate
     * @brief Checks every hash entry after all publishers have completed.
     */
    void validate(size_t count) const {
        require(rows_.size() == count, "unordered_map lost an insertion.");
        for (const auto& [identifier, payload] : rows_) {
            require(identifier >= 0 && static_cast<uint64_t>(identifier) < count,
                "unordered_map row identity differs.");
            require(payload && payload.template get_as<uint64_t>() == static_cast<uint64_t>(identifier),
                "unordered_map row payload differs.");
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Map Phase
 * @brief Separates concurrent publishers from readers of the current non-atomic SIMD ID scan.
 */
enum class MapPhase { insert, hit, miss };
/** --------------------------------------------------------------------------------------------------------- Map Summary
 * @brief Collects paired medians for one comparison table after every worker team has finished.
 */
class MapSummary {
private:
    std::vector<std::vector<std::string>> columns_{
        {"Workers"}, {"Operation"}, {"Baseline\nlock"},
        {"SliceMap\nns/op"}, {"unordered_map\nns/op"}, {"Speedup"}
    };
    /** ------------------------------------------------------------------------------------------- Format
     * @brief Formats a summary measurement with two decimal places.
     */
    static std::string format(double value) {
        std::ostringstream output;
        output << std::fixed << std::setprecision(2) << value;
        return output.str();
    }
public:
    /** ------------------------------------------------------------------------------------------- Add
     * @brief Records per-operation medians and their relative throughput outside the timed region.
     */
    void add(
        const Shape& shape, std::string_view operation, size_t operations,
        double slice_seconds, double hash_seconds
    ) {
        columns_[0].push_back(shape.label());
        columns_[1].emplace_back(operation);
        columns_[2].push_back(shape.serial ? "none" : "mutex");
        columns_[3].push_back(format(slice_seconds * 1e9 / operations));
        columns_[4].push_back(format(hash_seconds * 1e9 / operations));
        columns_[5].push_back(format(hash_seconds / slice_seconds) + "x");
    }
    /** ------------------------------------------------------------------------------------------- Print
     * @brief Prints the completed run with units and the baseline synchronization policy.
     */
    void print(const Options& options) const {
        LOG_INFO_STREAM << "SliceMap benchmark summary: " << options.items << " rows, "
            << options.lookups << " lookups per phase, " << options.repetitions << " measured repetitions.";
        LOG_INFO_STREAM << "Median aggregate ns/op; lower is faster. Baseline lock applies only to "
            << "std::unordered_map; the benchmark adds no mutex around SliceMap. "
            << "Speedup = unordered_map time / SliceMap time; above 1x favors SliceMap.";
        threadsafe_logger::logging::print_table(
            columns_, {}, threadsafe_logger::logging::TTYCYAN);
    }
};
/** --------------------------------------------------------------------------------------------------------- Map Workload
 * @brief Runs identical insert and lookup partitions with per-operation ownership semantics.
 */
template<typename Rows>
class MapWorkload {
private:
    const Options& options_;
    Shape shape_;
    Rows rows_;
    MapPhase phase_ = MapPhase::insert;
    std::vector<Slice> input_;
    std::vector<Slice> output_;
    std::vector<int64_t> hit_queries_;
    std::vector<int64_t> miss_queries_;
public:
    MapWorkload(const Options& options, Shape shape)
    : options_(options), shape_(shape), rows_(options.items), output_(options.lookups) {
        hit_queries_.reserve(options.lookups);
        miss_queries_.reserve(options.lookups);
        for (size_t index = 0; index < options.lookups; ++index) {
            const auto identifier = static_cast<int64_t>(index % options.items);
            hit_queries_.push_back(identifier);
            miss_queries_.push_back(-2 - identifier);
        }
    }
    /** ------------------------------------------------------------------------------------------- Prepare
     * @brief Allocates and releases payload claims outside each measured phase.
     */
    void prepare(MapPhase phase, const std::vector<Slice>& payloads) {
        phase_ = phase;
        output_.clear();
        output_.resize(options_.lookups);
        if (phase == MapPhase::insert) {
            rows_.reset();
            input_ = payloads;
        }
    }
    /** ------------------------------------------------------------------------------------------- Run
     * @brief Runs a producer or reader partition without overlapping the two access phases.
     */
    void run(size_t worker) {
        if (phase_ == MapPhase::insert) {
            if (worker >= shape_.producers) return;
            const size_t first = partition(options_.items, worker, shape_.producers);
            const size_t last = partition(options_.items, worker + 1, shape_.producers);
            for (size_t index = first; index < last; ++index) {
                rows_.insert(static_cast<int64_t>(index), std::move(input_[index]));
            }
        } else {
            if (!shape_.serial && worker < shape_.producers) return;
            const size_t reader = shape_.serial ? 0 : worker - shape_.producers;
            const size_t first = partition(options_.lookups, reader, shape_.consumers);
            const size_t last = partition(options_.lookups, reader + 1, shape_.consumers);
            const auto& queries = phase_ == MapPhase::hit ? hit_queries_ : miss_queries_;
            for (size_t index = first; index < last; ++index) {
                output_[index] = rows_.lookup(queries[index]);
            }
        }
    }
    /** ------------------------------------------------------------------------------------------- Validate
     * @brief Checks all rows, returned values, or misses after the phase timer has stopped.
     */
    void validate() const {
        if (phase_ == MapPhase::insert) {
            rows_.validate(options_.items);
            for (const Slice& source : input_) require(source.is_null(), "Insert retained its source.");
        } else if (phase_ == MapPhase::hit) {
            for (size_t index = 0; index < output_.size(); ++index) {
                require(output_[index] && output_[index].template get_as<uint64_t>()
                    == static_cast<uint64_t>(hit_queries_[index]), "Map lookup returned the wrong row.");
            }
        } else {
            for (const Slice& result : output_) require(result.is_null(), "Missing ID unexpectedly hit.");
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Measure Map
 * @brief Times insertion, successful lookup, and missing lookup as independent synchronized phases.
 */
template<typename Rows>
std::array<double, 3> measure_map(
    MapWorkload<Rows>& workload, Team<MapWorkload<Rows>>& team, const std::vector<Slice>& payloads
) {
    std::array<double, 3> seconds;
    size_t index = 0;
    for (const auto phase : {MapPhase::insert, MapPhase::hit, MapPhase::miss}) {
        workload.prepare(phase, payloads);
        seconds[index++] = team.measure();
        workload.validate();
    }
    return seconds;
}
/** --------------------------------------------------------------------------------------------------------- Compare Maps
 * @brief Alternates paired samples at identical row counts and lookup counts.
 */
template<bool Synchronized>
void compare_maps(const Options& options, Shape shape, Reporter& reporter, MapSummary& summary) {
    const size_t workers = shape.serial ? 1 : shape.producers + shape.consumers;
    require(workers < buffetalligator::SliceMap::kHazardMaxThreads,
        "SliceMap supports 255 benchmark workers plus the coordinator; reduce the requested team.");
    LOG_INFO_STREAM << "Map " << shape.label() << ": " << options.items
        << " rows, " << options.lookups << " operations per lookup phase";
    Progress progress("Map " + shape.label(), options.timeout);
    const auto input = payloads(options.items);
    MapWorkload<SliceRows> slices(options, shape);
    MapWorkload<HashRows<Synchronized>> hashes(options, shape);
    Team slice_team(slices, workers);
    Team hash_team(hashes, workers);
    std::array<std::vector<double>, 3> slice_samples, hash_samples;
    for (size_t sample = 0; sample < options.warmup + options.repetitions; ++sample) {
        LOG_INFO_STREAM << "Map " << shape.label() << ' '
            << (sample < options.warmup ? "warmup " : "sample ")
            << (sample < options.warmup ? sample + 1 : sample - options.warmup + 1);
        std::array<double, 3> slice_seconds, hash_seconds;
        for (size_t order = 0; order < 2; ++order) {
            if ((sample + order) % 2 == 0) slice_seconds = measure_map(slices, slice_team, input);
            else hash_seconds = measure_map(hashes, hash_team, input);
        }
        if (sample >= options.warmup) {
            for (size_t phase = 0; phase < slice_seconds.size(); ++phase) {
                slice_samples[phase].push_back(slice_seconds[phase]);
                hash_samples[phase].push_back(hash_seconds[phase]);
            }
        }
    }
    constexpr std::array<std::string_view, 3> names{"insert", "lookup-hit", "lookup-miss"};
    for (size_t phase = 0; phase < names.size(); ++phase) {
        const size_t operations = phase == 0 ? options.items : options.lookups;
        const double slice_median = reporter.report("SliceMap", names[phase], shape,
            options, operations, slice_samples[phase]);
        const double hash_median = reporter.report(
            Synchronized ? "unordered_map+mutex" : "unordered_map", names[phase], shape,
            options, operations, hash_samples[phase]);
        LOG_INFO_STREAM << "SliceMap " << names[phase] << " relative throughput: "
            << hash_median / slice_median << "x unordered_map (greater than 1 means SliceMap is faster).";
        summary.add(shape, names[phase], operations, slice_median, hash_median);
    }
}
} // namespace benchmarks
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs one-thread and synchronized producer/reader map comparisons.
 */
int main(int count, char** arguments) {
    using namespace benchmarks;
    try {
        const Options options = parse(count, arguments, false);
        if (options.help) { help(false); return 0; }
        Reporter reporter(options);
        MapSummary summary;
        LOG_INFO_STREAM << "SliceMap uses growing SIMD hash buckets and hazard-protected replacement; "
            << "this benchmark measures separate insertion and lookup phases with retained payloads.";
        for (const Shape shape : shapes(options)) {
            if (shape.serial) compare_maps<false>(options, shape, reporter, summary);
            else compare_maps<true>(options, shape, reporter, summary);
        }
        summary.print(options);
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Map benchmark failed: " << error.what();
        return 1;
    }
}
