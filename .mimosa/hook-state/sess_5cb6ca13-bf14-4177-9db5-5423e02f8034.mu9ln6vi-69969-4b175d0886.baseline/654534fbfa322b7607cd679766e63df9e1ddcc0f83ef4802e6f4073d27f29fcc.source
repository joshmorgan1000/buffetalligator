#pragma once
/** --------------------------------------------------------------------------------------------------------- Benchmark Support
 * @file benchmark_support.hpp
 * @brief Shares deterministic inputs, persistent workers, progress reporting, and timing summaries.
 */
#include <alligator.hpp>
#include <logging.hpp>
#include <algorithm>
#include <barrier>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace benchmarks {
using buffetalligator::Slice;
using Clock = std::chrono::steady_clock;
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Rejects invalid configuration or results outside the measured workload.
 */
inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
/** --------------------------------------------------------------------------------------------------------- Options
 * @brief Configures the amount of work and the producer/consumer topology.
 */
struct Options {
    size_t items = 4096;
    size_t repetitions = 5;
    size_t warmup = 1;
    size_t producers = 0;
    size_t consumers = 0;
    size_t capacity = 4096;
    size_t lookups = 65536;
    size_t batch = 1;
    size_t timeout = 120;
    bool single_thread = false;
    bool help = false;
    std::string csv;
};
/** --------------------------------------------------------------------------------------------------------- Parse
 * @brief Parses explicit benchmark sizes without changing machine or library configuration.
 */
inline Options parse(int count, char** arguments, bool queue) {
    Options options;
    if (queue) options.items = 1048576;
    for (int index = 1; index < count; ++index) {
        const std::string_view argument(arguments[index]);
        if (argument == "--help") { options.help = true; continue; }
        if (argument == "--single-thread") { options.single_thread = true; continue; }
        require(index + 1 < count, "An option is missing its value; use --help.");
        const std::string_view value(arguments[++index]);
        if (argument == "--csv") { options.csv = value; continue; }
        size_t number = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size(),
            "Expected an unsigned integer option value; use --help.");
        if (argument == "--items") options.items = number;
        else if (argument == "--repetitions") options.repetitions = number;
        else if (argument == "--warmup") options.warmup = number;
        else if (argument == "--producers") options.producers = number;
        else if (argument == "--consumers") options.consumers = number;
        else if (argument == "--timeout") options.timeout = number;
        else if (queue && argument == "--capacity") options.capacity = number;
        else if (queue && argument == "--batch") options.batch = number;
        else if (!queue && argument == "--lookups") options.lookups = number;
        else throw std::runtime_error("Unknown option: " + std::string(argument));
        require(number != 0 || argument == "--warmup", "Only --warmup accepts zero.");
    }
    require((options.producers == 0) == (options.consumers == 0),
        "Specify both --producers and --consumers.");
    require(!options.single_thread || options.producers == 0,
        "Use --single-thread or a producer/consumer topology, not both.");
    require(options.items <= static_cast<size_t>(INT64_MAX) / 64,
        "--items exceeds the supported payload size.");
    require(options.repetitions <= SIZE_MAX - options.warmup, "Sample count overflow.");
    require(options.producers <= SIZE_MAX - options.consumers, "Worker count overflow.");
    if (queue) {
        require(options.capacity >= buffetalligator::SliceQueue::block_size
            && options.capacity % buffetalligator::SliceQueue::block_size == 0,
            "--capacity must be a positive multiple of SliceQueue::block_size (256).");
        require(options.batch <= buffetalligator::SliceQueue::block_size,
            "--batch must be between 1 and 256.");
    }
    return options;
}
/** --------------------------------------------------------------------------------------------------------- Help
 * @brief Describes defaults and the units reported by the selected benchmark.
 */
inline void help(bool queue) {
    LOG_INFO_STREAM << "Options: --items N --repetitions N (5) --warmup N (1) "
        << "--producers N --consumers N --single-thread --timeout SECONDS (120) --csv PATH";
    if (queue) {
        LOG_INFO_STREAM << "Queue defaults: --items 1048576 --capacity 4096 --batch 1; "
            << "capacity is per producer, batch is 1..256; throughput counts delivered messages.";
    } else {
        LOG_INFO_STREAM << "Map defaults: --items 4096 --lookups 65536; "
            << "insert, successful lookup, and missing lookup are timed separately.";
    }
    LOG_INFO_STREAM << "Without topology options, run one-thread and hardware-sized "
        << "balanced/asymmetric comparisons; producer/consumer counts describe distinct workers.";
}
/** --------------------------------------------------------------------------------------------------------- Shape
 * @brief Distinguishes one-thread round trips from concurrently scheduled worker roles.
 */
struct Shape {
    size_t producers;
    size_t consumers;
    bool serial = false;
    std::string label() const {
        return serial ? "one-thread" : std::to_string(producers) + "P/"
            + std::to_string(consumers) + "C";
    }
};
/** --------------------------------------------------------------------------------------------------------- Shapes
 * @brief Includes single-worker, balanced, and asymmetric topologies up to the reported CPU count.
 */
inline std::vector<Shape> shapes(const Options& options) {
    if (options.single_thread) return {{1, 1, true}};
    if (options.producers) return {{options.producers, options.consumers}};
    const size_t hardware = std::thread::hardware_concurrency();
    require(hardware != 0, "CPU count is unavailable; supply --producers and --consumers.");
    std::vector<Shape> result{{1, 1, true}, {1, 1}};
    for (size_t workers = 4; workers < hardware; workers *= 2) {
        result.push_back({workers / 2, workers / 2});
    }
    if (hardware > 2) {
        result.push_back({hardware / 2, hardware - hardware / 2});
        if (hardware > 3) result.push_back({1, hardware - 1});
        result.push_back({hardware - 1, 1});
    }
    return result;
}
/** --------------------------------------------------------------------------------------------------------- Partition
 * @brief Splits every item exactly once even when the item count is not divisible by workers.
 */
inline size_t partition(size_t items, size_t worker, size_t workers) {
    return (items / workers) * worker + std::min(items % workers, worker);
}
/** --------------------------------------------------------------------------------------------------------- Payloads
 * @brief Preallocates independent arena claims carrying deterministic message identifiers.
 */
inline std::vector<Slice> payloads(size_t count) {
    std::vector<Slice> values;
    values.reserve(count);
    for (size_t identifier = 0; identifier < count; ++identifier) {
        values.emplace_back(64);
        values.back().get_as<uint64_t>() = identifier;
    }
    return values;
}
/** --------------------------------------------------------------------------------------------------------- Progress
 * @brief Reports long-running work once per second and terminates a stalled sample with context.
 */
class Progress {
private:
    std::string label_;
    size_t timeout_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool stopped_ = false;
    std::thread thread_;
    /** ------------------------------------------------------------------------------------------- Monitor
     * @brief Watches wall time without touching the measured workers' hot counters.
     */
    static void monitor(Progress* progress) {
        const auto start = Clock::now();
        std::unique_lock lock(progress->mutex_);
        while (!progress->stopped_) {
            progress->changed_.wait_for(lock, std::chrono::seconds(1));
            if (progress->stopped_) break;
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                Clock::now() - start).count();
            if (elapsed >= static_cast<int64_t>(progress->timeout_)) {
                LOG_ERROR_STREAM << progress->label_ << " exceeded " << progress->timeout_
                    << "s; check for stalled workers or increase --timeout for this workload.";
                std::abort();
            }
            LOG_INFO_STREAM << progress->label_ << ": working (" << elapsed << "s)...";
        }
    }
public:
    Progress(std::string label, size_t timeout)
    : label_(std::move(label)), timeout_(timeout), thread_(&Progress::monitor, this) {}
    ~Progress() {
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
        }
        changed_.notify_one();
        thread_.join();
    }
};
/** --------------------------------------------------------------------------------------------------------- Worker Timing
 * @brief Separates worker timestamps to avoid false sharing on 64-byte and 128-byte cache lines.
 */
struct alignas(128) WorkerTiming {
    Clock::time_point start;
    Clock::time_point finish;
};
/** --------------------------------------------------------------------------------------------------------- Team
 * @brief Reuses a synchronized worker team across warmups and measured samples.
 */
template<typename Workload>
class Team {
private:
    Workload& workload_;
    std::barrier<> gate_;
    std::vector<WorkerTiming> timings_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
    /** ------------------------------------------------------------------------------------------- Worker
     * @brief Publishes per-worker timestamps and workload results through the completion barrier.
     */
    static void worker(Team* team, size_t index) {
        for (;;) {
            team->gate_.arrive_and_wait();
            if (team->stopping_) return;
            team->timings_[index].start = Clock::now();
            try {
                team->workload_.run(index);
            } catch (const std::exception& error) {
                LOG_ERROR_STREAM << "Benchmark worker failed: " << error.what();
                std::abort();
            }
            team->timings_[index].finish = Clock::now();
            team->gate_.arrive_and_wait();
        }
    }
public:
    Team(Workload& workload, size_t workers)
    : workload_(workload), gate_(static_cast<std::ptrdiff_t>(workers + 1)), timings_(workers) {
        workers_.reserve(workers);
        try {
            for (size_t index = 0; index < workers; ++index) {
                workers_.emplace_back(&Team::worker, this, index);
            }
        } catch (const std::exception& error) {
            LOG_ERROR_STREAM << "Cannot start the benchmark team: " << error.what();
            std::abort();
        }
    }
    ~Team() {
        stopping_ = true;
        gate_.arrive_and_wait();
        for (auto& worker : workers_) worker.join();
    }
    /** ------------------------------------------------------------------------------------------- Measure
     * @brief Measures earliest worker entry through latest completion, excluding thread creation.
     */
    double measure() {
        gate_.arrive_and_wait();
        gate_.arrive_and_wait();
        auto start = timings_.front().start;
        auto finish = timings_.front().finish;
        for (const auto& timing : timings_) {
            start = std::min(start, timing.start);
            finish = std::max(finish, timing.finish);
        }
        return std::chrono::duration<double>(finish - start).count();
    }
};
/** --------------------------------------------------------------------------------------------------------- Reporter
 * @brief Prints throughput distributions and optionally writes one CSV row per measured sample.
 */
class Reporter {
private:
    std::ofstream csv_;
public:
    explicit Reporter(const Options& options) {
        if (!options.csv.empty()) {
            csv_.open(options.csv);
            require(csv_.is_open(), "Cannot open --csv output path.");
            csv_ << "container,workload,shape,items,operations,capacity_per_producer,batch,"
                << "sample,seconds,operations_per_second\n";
        }
        LOG_INFO_STREAM << "Logical CPUs: " << std::thread::hardware_concurrency()
            << "; warmups: " << options.warmup << "; measured repetitions: " << options.repetitions;
#ifdef NDEBUG
        LOG_INFO_STREAM << "Assertions disabled; use the Release build from ./run_build.sh.";
#else
        LOG_WARN_STREAM << "Assertions enabled; these timings are not Release performance results.";
#endif
    }
    /** ------------------------------------------------------------------------------------------- Report
     * @brief Reports median and range without enforcing machine-dependent performance thresholds.
     */
    double report(
        std::string_view container, std::string_view workload, const Shape& shape,
        const Options& options, size_t operations, std::vector<double> samples
    ) {
        if (csv_.is_open()) {
            for (size_t index = 0; index < samples.size(); ++index) {
                csv_ << container << ',' << workload << ',' << shape.label() << ','
                    << options.items << ',' << operations << ',' << options.capacity << ','
                    << options.batch << ',' << index + 1 << ',' << std::setprecision(12)
                    << samples[index] << ',' << operations / samples[index] << '\n';
            }
            csv_.flush();
            require(csv_.good(), "Failed to write benchmark CSV output.");
        }
        std::sort(samples.begin(), samples.end());
        const size_t middle = samples.size() / 2;
        const double median = samples.size() % 2 ? samples[middle]
            : (samples[middle - 1] + samples[middle]) / 2;
        LOG_INFO_STREAM << container << ' ' << workload << ' ' << shape.label()
            << ": median=" << median * 1000 << " ms, range=[" << samples.front() * 1000
            << ", " << samples.back() * 1000 << "] ms, " << operations / median / 1e6
            << " Mops/s, " << median * 1e9 / operations << " ns/op (aggregate)";
        return median;
    }
};
} // namespace benchmarks
