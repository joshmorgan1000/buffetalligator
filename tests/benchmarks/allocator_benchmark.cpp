/** --------------------------------------------------------------------------------------------------------- Allocator Benchmark
 * @file allocator_benchmark.cpp
 * @brief Measures public Slice allocation and ownership with persistent workers and checked deterministic traces.
 */
#include "benchmark_support.hpp"
#include <array>
#include <cstdint>
#include <limits>
#include <utility>

namespace {
using namespace benchmarks;
using buffetalligator::BuffetDescriptor;
/** --------------------------------------------------------------------------------------------------------- Configuration
 * @brief Keeps allocator-specific worker and latency choices alongside shared benchmark options.
 */
struct Configuration {
    Options common;
    size_t workers = 0;
    size_t latency_samples = 128;
};
/** --------------------------------------------------------------------------------------------------------- Configuration Parser
 * @brief Parses allocator controls and delegates shared measurement options to benchmark support.
 */
Configuration configuration(int count, char** arguments) {
    Configuration result;
    std::vector<char*> shared{arguments[0]};
    bool specified_warmup = false;
    bool specified_repetitions = false;
    for (int index = 1; index < count; ++index) {
        const std::string_view argument(arguments[index]);
        if (argument == "--workers" || argument == "--latency-samples") {
            require(index + 1 < count, "An allocator option is missing its value; use --help.");
            const std::string_view value(arguments[++index]);
            size_t number = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()
                && number != 0, "Allocator options require a positive integer.");
            if (argument == "--workers") result.workers = number;
            else result.latency_samples = number;
            continue;
        }
        require(argument != "--producers" && argument != "--consumers" && argument != "--lookups",
            "Allocator benchmarks use --workers and --latency-samples.");
        specified_warmup = specified_warmup || argument == "--warmup";
        specified_repetitions = specified_repetitions || argument == "--repetitions";
        shared.push_back(arguments[index]);
        if (argument != "--help" && argument != "--single-thread") {
            require(index + 1 < count, "An option is missing its value; use --help.");
            shared.push_back(arguments[++index]);
        }
    }
    result.common = parse(static_cast<int>(shared.size()), shared.data(), false);
    if (!specified_warmup) result.common.warmup = 2;
    if (!specified_repetitions) result.common.repetitions = 15;
    if (result.common.csv.empty()) result.common.csv = "allocator_samples.csv";
    require(result.common.repetitions <= SIZE_MAX - result.common.warmup, "Sample count overflow.");
    require(!result.common.single_thread || result.workers <= 1,
        "Use --single-thread or --workers N, not both.");
    require(result.common.items <= SIZE_MAX / 4096, "--items overflows the retained byte count.");
    return result;
}
/** --------------------------------------------------------------------------------------------------------- Operation
 * @brief Selects one public ownership operation per measured item.
 */
enum class Operation { claim, novel, copy, move, view, release };
constexpr std::array operations{Operation::claim, Operation::novel, Operation::copy,
    Operation::move, Operation::view, Operation::release};
constexpr std::array<const char*, 6> operation_names{"claim", "novel", "copy", "move", "view", "release"};
/** --------------------------------------------------------------------------------------------------------- Workload
 * @brief Retains every result until validation and separates instrumentation from throughput passes.
 */
struct Workload {
    const BuffetDescriptor* placement;
    size_t bytes;
    size_t count;
    size_t workers;
    size_t stride;
    Operation operation;
    bool instrumented = false;
    std::vector<Slice> sources;
    std::vector<Slice> outputs;
    std::vector<void*> addresses;
    std::vector<double> nanoseconds;
    Workload(const BuffetDescriptor* selected, size_t size, size_t items, size_t threads,
        size_t samples, Operation selected_operation)
    : placement(selected), bytes(size), count(items), workers(threads),
        stride(items / std::min(items, samples) + (items % std::min(items, samples) != 0)),
        operation(selected_operation), sources(items), outputs(items), addresses(items),
        nanoseconds(items, -1) {}
    /** ------------------------------------------------------------------------------------------- Prepare
     * @brief Creates source ownership and deterministic payloads outside the measured interval.
     */
    void prepare() {
        for (size_t index = 0; index < count; ++index) {
            outputs[index].free();
            sources[index].free();
            nanoseconds[index] = -1;
            if (operation == Operation::claim || operation == Operation::novel) continue;
            Slice& source = operation == Operation::release ? outputs[index] : sources[index];
            source = Slice(bytes, operation == Operation::release, placement);
            addresses[index] = source.raw();
            source.data<uint64_t>()[0] = index + 1;
            source.data<uint64_t>()[source.size<uint64_t>() - 1] = ~(uint64_t(index) + 1);
        }
    }
    /** ------------------------------------------------------------------------------------------- Run
     * @brief Runs a disjoint ownership trace on one persistent worker.
     */
    void run(size_t worker) {
        const size_t begin = partition(count, worker, workers);
        const size_t end = partition(count, worker + 1, workers);
        for (size_t index = begin; index < end; ++index) {
            const bool sampled = instrumented && index % stride == 0;
            Clock::time_point started;
            if (sampled) started = Clock::now();
            switch (operation) {
            case Operation::claim: outputs[index] = Slice(bytes, false, placement); break;
            case Operation::novel: outputs[index] = Slice(bytes, true, placement); break;
            case Operation::copy: outputs[index] = sources[index]; break;
            case Operation::move: outputs[index] = std::move(sources[index]); break;
            case Operation::view:
                outputs[index] = sources[index].slice(1, sources[index].size_bytes() - 2);
                break;
            case Operation::release: outputs[index].free(); break;
            }
            if (sampled) {
                nanoseconds[index] = std::chrono::duration<double, std::nano>(
                    Clock::now() - started).count();
            }
        }
    }
    /** ------------------------------------------------------------------------------------------- Validate
     * @brief Checks represented size, zero initialization, aliasing, payloads, and released ownership.
     */
    void validate() const {
        const size_t represented = (bytes + 63) & ~size_t(63);
        for (size_t index = 0; index < count; ++index) {
            const Slice& output = outputs[index];
            if (operation == Operation::release) {
                require(output.is_null(), "Release retained an owning Slice.");
                continue;
            }
            require(output.valid() && output.size_bytes() == represented,
                "Allocator operation returned the wrong represented range.");
            require(reinterpret_cast<uintptr_t>(output.raw()) % 64 == 0,
                "Allocator operation returned an unaligned range.");
            require(output.placement() == placement, "Allocator operation changed placement.");
            const uint64_t first = output.data<uint64_t>()[0];
            const uint64_t last = output.data<uint64_t>()[output.size<uint64_t>() - 1];
            if (operation == Operation::claim || operation == Operation::novel) {
                require(first == 0 && last == 0, "Fresh allocation payload was not initialized.");
                require(output.is_novel() == (operation == Operation::novel
                    || represented >= placement->default_size), "Allocation backing kind differs.");
            } else {
                require(output.raw() == addresses[index], "Ownership operation copied payload storage.");
                require(first == index + 1 && last == ~(uint64_t(index) + 1),
                    "Ownership operation lost deterministic payload markers.");
                if (operation == Operation::move) {
                    require(sources[index].is_null(), "Move retained source ownership.");
                } else {
                    require(sources[index].raw() == addresses[index], "Source ownership was changed.");
                }
            }
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Measure
 * @brief Records raw throughput and independent per-operation latency samples after validating each run.
 */
void measure(const Configuration& config, Reporter& reporter, std::ofstream& latency_csv,
    const BuffetDescriptor* placement, size_t bytes, size_t workers, bool boundary
) {
    Options options = config.common;
    if (boundary) options.items = 1;
    options.capacity = 0;
    const Shape shape{workers, 0, workers == 1};
    const size_t represented = (bytes + 63) & ~size_t(63);
    LOG_INFO_STREAM << "Slice requested=" << bytes << " B; represented=" << represented
        << " B; workers=" << workers << "; retained payload budget="
        << options.items * represented << " B; boundary=" << boundary;
    for (const Operation operation : operations) {
        if (boundary && (operation == Operation::copy || operation == Operation::move
            || operation == Operation::view)) continue;
        const std::string label = std::string(operation_names[static_cast<size_t>(operation)])
            + '-' + std::to_string(bytes) + "B";
        Progress progress("Slice " + label + ' ' + shape.label(), options.timeout);
        Workload workload(placement, bytes, options.items, workers,
            config.latency_samples, operation);
        Team team(workload, workers);
        std::vector<double> elapsed;
        elapsed.reserve(options.repetitions);
        for (size_t sample = 0; sample < options.warmup + options.repetitions; ++sample) {
            workload.prepare();
            const double seconds = team.measure();
            workload.validate();
            if (sample >= options.warmup) elapsed.push_back(seconds);
        }
        reporter.report("Slice", label, shape, options, options.items, std::move(elapsed));
        workload.instrumented = true;
        std::vector<double> latencies;
        for (size_t sample = 0; sample < options.repetitions; ++sample) {
            workload.prepare();
            static_cast<void>(team.measure());
            workload.validate();
            for (size_t index = 0; index < workload.count; index += workload.stride) {
                const double latency = workload.nanoseconds[index];
                require(latency >= 0, "An instrumented operation has no timestamp sample.");
                latencies.push_back(latency);
                latency_csv << label << ',' << workers << ',' << bytes << ',' << represented
                    << ',' << sample + 1 << ',' << index << ',' << std::setprecision(12)
                    << latency << '\n';
            }
        }
        latency_csv.flush();
        require(latency_csv.good(), "Failed to write allocator latency samples.");
        std::sort(latencies.begin(), latencies.end());
        const size_t zero_samples = std::upper_bound(latencies.begin(), latencies.end(), 0.0)
            - latencies.begin();
        LOG_INFO_STREAM << "Slice " << label << ' ' << shape.label()
            << " instrumented operation latency: p50=" << latencies[(latencies.size() - 1) / 2]
            << " ns, p95=" << latencies[(latencies.size() - 1) * 95 / 100]
            << " ns, p99=" << latencies[(latencies.size() - 1) * 99 / 100]
            << " ns; samples=" << latencies.size() << "; below clock resolution=" << zero_samples;
    }
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Benchmarks granule edges, warmed ownership, slab rollover, and bounded oversubscription.
 */
int main(int count, char** arguments) {
    try {
        const Configuration config = configuration(count, arguments);
        if (config.common.help) {
            LOG_INFO_STREAM << "Options: --items N (4096) --repetitions N (15) --warmup N (2) "
                << "--workers N --single-thread --latency-samples N (128 per repetition) "
                << "--timeout SECONDS (120) --csv PATH (allocator_samples.csv)";
            LOG_INFO_STREAM << "Default workers: powers of two through reported CPU concurrency "
                << "and twice that concurrency; all producer workers perform the same Slice trace.";
            LOG_INFO_STREAM << "Slab boundaries use one retained allocation and one worker; "
                << "latencies use a separate instrumented pass and PATH.latencies.csv.";
            return 0;
        }
#ifndef NDEBUG
        throw std::runtime_error("Allocator performance runs require a Release build from ./run_build.sh.");
#endif
        Reporter reporter(config.common);
        std::ofstream latency_csv(config.common.csv + ".latencies.csv");
        require(latency_csv.is_open(), "Cannot open allocator latency CSV output.");
        latency_csv << "workload,workers,requested_bytes,represented_bytes,repetition,operation_index,nanoseconds\n";
        Slice initial;
        double initialization = 0;
        {
            Progress progress("Slice first public claim", config.common.timeout);
            const auto initialized = Clock::now();
            initial = Slice(64);
            initialization = std::chrono::duration<double>(Clock::now() - initialized).count();
        }
        require(initial.valid() && initial.size_bytes() == 64 && initial.get_as<uint64_t>() == 0,
            "First public Slice claim failed validation.");
        const BuffetDescriptor* placement = initial.placement();
        initial.free();
        LOG_INFO_STREAM << "Placement=" << placement->type_name << "; first public claim="
            << initialization * 1000 << " ms; Slice handle=" << sizeof(Slice)
            << " B; slab=" << placement->default_size << " B; startup time excluded.";
        LOG_INFO_STREAM << "Throughput and instrumented latency are separate; represented payload "
            << "budgets exclude internal metadata and prepared backing slabs.";
        std::vector<size_t> worker_counts;
        if (config.common.single_thread) worker_counts.push_back(1);
        else if (config.workers != 0) worker_counts.push_back(config.workers);
        else {
            const size_t hardware = std::thread::hardware_concurrency();
            require(hardware != 0, "CPU count unavailable; supply --workers N.");
            for (size_t workers = 1; workers < hardware; workers *= 2) {
                worker_counts.push_back(workers);
            }
            worker_counts.push_back(hardware);
            worker_counts.push_back(hardware * 2);
        }
        constexpr std::array<size_t, 5> sizes{1, 63, 64, 65, 4096};
        for (const size_t workers : worker_counts) {
            for (const size_t bytes : sizes) {
                measure(config, reporter, latency_csv, placement, bytes, workers, false);
            }
        }
        require(placement->default_size > 64
            && placement->default_size <= std::numeric_limits<size_t>::max() - 64,
            "Placement slab size cannot represent boundary workloads.");
        for (const size_t bytes : {placement->default_size - 64,
            placement->default_size, placement->default_size + 64}) {
            measure(config, reporter, latency_csv, placement, bytes, 1, true);
        }
        LOG_INFO_STREAM << "Allocator raw samples: " << config.common.csv << " and "
            << config.common.csv << ".latencies.csv";
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Allocator benchmark failed: " << error.what();
        return 1;
    }
}
