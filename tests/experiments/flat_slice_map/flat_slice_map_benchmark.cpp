/** --------------------------------------------------------------------------------------------------------- Flat Slice Map Experiment
 * @file flat_slice_map_benchmark.cpp
 * @brief Compares exclusive mutation and retained read phases through three map implementations.
 */
#include "flat_slice_map.hpp"
#include "../../benchmarks/benchmark_support.hpp"
#include <array>
#include <optional>
#include <unordered_map>

namespace experiments {
using namespace benchmarks;
/** --------------------------------------------------------------------------------------------------------- Slice Rows
 * @brief Adapts the production concurrent map to the experiment's key operations.
 */
class SliceRows {
private:
    buffetalligator::SliceMap rows_;
public:
    explicit SliceRows(size_t capacity) : rows_(capacity) {}
    void insert(int64_t identifier, Slice payload) {
        rows_.add_slice(identifier, std::move(payload));
    }
    Slice lookup(int64_t identifier) { return rows_.get_slice(identifier); }
    size_t size() const { return rows_.size(); }
    size_t capacity() const { return rows_.capacity(); }
};
/** --------------------------------------------------------------------------------------------------------- Standard Rows
 * @brief Adapts an unlocked std map with replacement and ordinary Slice-copy semantics.
 */
class StandardRows {
private:
    std::unordered_map<int64_t, Slice> rows_;
public:
    explicit StandardRows(size_t capacity) { rows_.reserve(capacity); }
    void insert(int64_t identifier, Slice payload) {
        rows_.insert_or_assign(identifier, std::move(payload));
    }
    Slice lookup(int64_t identifier) const {
        const auto found = rows_.find(identifier);
        return found == rows_.end() ? Slice() : found->second;
    }
    size_t size() const { return rows_.size(); }
    size_t capacity() const { return rows_.bucket_count(); }
};
/** --------------------------------------------------------------------------------------------------------- Flat Rows
 * @brief Adapts the opaque Abseil candidate through its proposed narrow API.
 */
class FlatRows {
private:
    FlatSliceMap rows_;
public:
    explicit FlatRows(size_t capacity) : rows_(capacity) {}
    void insert(int64_t identifier, Slice payload) {
        rows_.add_slice(identifier, std::move(payload));
    }
    Slice lookup(int64_t identifier) const { return rows_.get_slice(identifier); }
    size_t size() const { return rows_.size(); }
    size_t capacity() const { return rows_.capacity(); }
};
/** --------------------------------------------------------------------------------------------------------- Inputs
 * @brief Prepares dense or aligned keys and exact expected payloads outside timed work.
 */
struct Inputs {
    std::vector<int64_t> keys;
    std::vector<int64_t> hits;
    std::vector<int64_t> misses;
    std::vector<uint64_t> expected;
    std::vector<Slice> initial;
    std::vector<Slice> replacement;
    Inputs(const Options& options, bool strided)
    : initial(payloads(options.items)), replacement(payloads(options.items)) {
        keys.reserve(options.items);
        hits.reserve(options.lookups);
        misses.reserve(options.lookups);
        expected.reserve(options.lookups);
        for (size_t index = 0; index < options.items; ++index) {
            keys.push_back(static_cast<int64_t>((index + 1) * (strided ? 256 : 1)));
            replacement[index].get_as<uint64_t>() = index + options.items;
        }
        for (size_t query = 0; query < options.lookups; ++query) {
            const size_t ordinal = strided ? (query * 7919) & (options.items - 1)
                : query % options.items;
            hits.push_back(keys[ordinal]);
            misses.push_back(-keys[ordinal]);
            expected.push_back(ordinal);
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Phase
 * @brief Separates mutation from shared reads of immutable map contents.
 */
enum class Phase : size_t { insert, hit, miss, replace };
/** --------------------------------------------------------------------------------------------------------- Workload
 * @brief Measures identical payload transfers and retained lookups for each map.
 */
template<typename Rows>
class Workload {
private:
    const Options& options_;
    const Inputs& inputs_;
    Shape shape_;
    size_t reservation_;
    Phase phase_ = Phase::insert;
    std::optional<Rows> rows_;
    std::vector<Slice> incoming_;
    std::vector<Slice> output_;
public:
    Workload(const Options& options, const Inputs& inputs, Shape shape, size_t reservation)
    : options_(options), inputs_(inputs), shape_(shape), reservation_(reservation) {}
    size_t capacity() const { return rows_->capacity(); }
    /** ------------------------------------------------------------------------------------------- Prepare
     * @brief Constructs an equally reserved empty map or prepares the next phase outside timing.
     */
    void prepare(Phase phase) {
        phase_ = phase;
        output_.clear();
        output_.resize(options_.lookups);
        if (phase == Phase::insert) {
            rows_.reset();
            buffetalligator::SliceMap::gc();
            rows_.emplace(reservation_);
            incoming_ = inputs_.initial;
        } else if (phase == Phase::replace) {
            incoming_ = inputs_.replacement;
        }
    }
    /** ------------------------------------------------------------------------------------------- Run
     * @brief Uses one mutator or partitioned readers with no overlapping mutation.
     */
    void run(size_t worker) {
        if (phase_ == Phase::insert || phase_ == Phase::replace) {
            if (worker != 0) return;
            for (size_t index = 0; index < options_.items; ++index) {
                rows_->insert(inputs_.keys[index], std::move(incoming_[index]));
            }
        } else {
            if (!shape_.serial && worker == 0) return;
            const size_t reader = shape_.serial ? 0 : worker - 1;
            const size_t first = partition(options_.lookups, reader, shape_.consumers);
            const size_t last = partition(options_.lookups, reader + 1, shape_.consumers);
            const auto& queries = phase_ == Phase::hit ? inputs_.hits : inputs_.misses;
            for (size_t index = first; index < last; ++index) {
                output_[index] = rows_->lookup(queries[index]);
            }
        }
    }
    /** ------------------------------------------------------------------------------------------- Validate
     * @brief Checks all keys and ownership transfers outside every measured phase.
     */
    void validate() {
        require(rows_->size() == options_.items, "Map distinct key count differs.");
        if (phase_ == Phase::insert || phase_ == Phase::replace) {
            const size_t increment = phase_ == Phase::replace ? options_.items : 0;
            for (size_t index = 0; index < options_.items; ++index) {
                const Slice result = rows_->lookup(inputs_.keys[index]);
                require(result && result.get_as<uint64_t>() == index + increment,
                    "Inserted or replaced payload differs.");
                require(incoming_[index].is_null(), "Insertion retained the moved source.");
            }
        } else if (phase_ == Phase::hit) {
            for (size_t index = 0; index < output_.size(); ++index) {
                require(output_[index]
                    && output_[index].template get_as<uint64_t>() == inputs_.expected[index],
                    "Retained lookup returned the wrong payload.");
            }
        } else {
            for (const Slice& result : output_) require(result.is_null(), "Missing key matched.");
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Measure
 * @brief Runs four isolated phases with correctness checks outside their timers.
 */
template<typename Rows>
std::array<double, 4> measure(Workload<Rows>& workload, Team<Workload<Rows>>& team) {
    std::array<double, 4> seconds;
    size_t index = 0;
    for (const auto phase : {Phase::insert, Phase::hit, Phase::miss, Phase::replace}) {
        workload.prepare(phase);
        seconds[index++] = team.measure();
        workload.validate();
    }
    return seconds;
}
/** --------------------------------------------------------------------------------------------------------- Compare
 * @brief Rotates all three containers within each repeated sample on the same input.
 */
void compare(const Options& options, Shape shape, bool strided, bool growing) {
    LOG_INFO_STREAM << "FlatSliceMap experiment: " << shape.label() << ", " << options.items
        << " rows; keys=" << (strided ? "strided-permuted" : "sequential")
        << "; initial reservation=" << (growing ? 16 : options.items);
    Progress progress("FlatSliceMap comparison", options.timeout);
    Reporter reporter(options);
    const Inputs inputs(options, strided);
    const size_t reservation = growing ? 16 : options.items;
    const size_t workers = shape.serial ? 1 : 1 + shape.consumers;
    Workload<SliceRows> slices(options, inputs, shape, reservation);
    Workload<StandardRows> standard(options, inputs, shape, reservation);
    Workload<FlatRows> flat(options, inputs, shape, reservation);
    Team slice_team(slices, workers);
    Team standard_team(standard, workers);
    Team flat_team(flat, workers);
    std::array<std::array<std::vector<double>, 4>, 3> samples;
    for (size_t sample = 0; sample < options.warmup + options.repetitions; ++sample) {
        LOG_INFO_STREAM << "FlatSliceMap " << (sample < options.warmup ? "warmup " : "sample ")
            << (sample < options.warmup ? sample + 1 : sample - options.warmup + 1);
        for (size_t order = 0; order < 3; ++order) {
            const size_t container = (sample + order) % 3;
            std::array<double, 4> seconds;
            switch (container) {
                case 0: seconds = measure(slices, slice_team); break;
                case 1: seconds = measure(standard, standard_team); break;
                case 2: seconds = measure(flat, flat_team); break;
            }
            if (sample >= options.warmup) {
                for (size_t phase = 0; phase < seconds.size(); ++phase) {
                    samples[container][phase].push_back(seconds[phase]);
                }
            }
        }
    }
    constexpr std::array<std::string_view, 3> containers{"SliceMap", "unordered_map", "FlatSliceMap"};
    LOG_INFO_STREAM << "Final layout: SliceMap row capacity=" << slices.capacity()
        << ", std buckets=" << standard.capacity() << ", Abseil slots=" << flat.capacity();
    constexpr std::array<std::string_view, 4> phases{"insert", "lookup-hit", "lookup-miss", "replace"};
    for (size_t container = 0; container < containers.size(); ++container) {
        for (size_t phase = 0; phase < phases.size(); ++phase) {
            const size_t operations = phase == 0 || phase == 3 ? options.items : options.lookups;
            reporter.report(containers[container], phases[phase], shape, options,
                operations, samples[container][phase]);
        }
    }
}
} // namespace experiments
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Accepts one exclusive writer with single or shared immutable read phases.
 */
int main(int count, char** arguments) {
    using namespace experiments;
    try {
        Options options = parse(count, arguments, false);
        if (options.help) { help(false); return 0; }
        require(options.single_thread || options.producers == 1,
            "Use --single-thread or --producers 1 --consumers N for exclusive writes.");
        require(options.consumers < buffetalligator::SliceMap::kHazardMaxThreads - 2,
            "Requested reader count exceeds SliceMap hazard slots.");
        const char* layout_environment = std::getenv("FLAT_MAP_LAYOUT");
        const char* reservation_environment = std::getenv("FLAT_MAP_RESERVATION");
        require(layout_environment && reservation_environment,
            "Set FLAT_MAP_LAYOUT=sequential|strided and FLAT_MAP_RESERVATION=reserved|growing.");
        const std::string_view layout(layout_environment);
        const std::string_view reservation(reservation_environment);
        require(layout == "sequential" || layout == "strided", "Unknown key layout.");
        require(reservation == "reserved" || reservation == "growing", "Unknown reservation mode.");
        require((options.items & (options.items - 1)) == 0, "Use a power-of-two row count.");
        require(options.items <= static_cast<size_t>(INT64_MAX) / 256,
            "Key layout exceeds the signed identifier domain.");
        options.capacity = reservation == "growing" ? 16 : options.items;
        const Shape shape = options.single_thread ? Shape{1, 1, true} : Shape{1, options.consumers};
        compare(options, shape, layout == "strided", reservation == "growing");
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "FlatSliceMap experiment failed: " << error.what();
        return 1;
    }
}
