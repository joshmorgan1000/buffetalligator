/** --------------------------------------------------------------------------------------------------------- Kitchen Benchmark
 * @file kitchen_benchmark.cpp
 * @brief Measures completed task throughput and repeated warm or parked submit-to-completion latency.
 */
#include "benchmark_support.hpp"
#include <alligator/kitchen.hpp>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>

namespace {
using namespace benchmarks;
using buffetalligator::Kitchen;
using buffetalligator::Task;
using buffetalligator::TaskCountdown;
/** --------------------------------------------------------------------------------------------------------- Configuration
 * @brief Keeps task and latency work sizes alongside shared sample-reporting options.
 */
struct Configuration {
    Options common;
    size_t latency_samples = 20000;
    Configuration() {
        common.items = 1u << 20;
        common.repetitions = 15;
        common.warmup = 2;
        common.batch = 256;
        common.capacity = 0;
        common.csv = "kitchen_samples.csv";
    }
};
/** --------------------------------------------------------------------------------------------------------- Configuration Parser
 * @brief Preserves task controls while adding explicit warmup, progress, and CSV output options.
 */
Configuration configuration(int count, char** arguments) {
    Configuration result;
    for (int index = 1; index < count; ++index) {
        const std::string_view option(arguments[index]);
        if (option == "--help") { result.common.help = true; continue; }
        require(index + 1 < count, "A Kitchen benchmark option is missing its value; use --help.");
        const std::string_view text(arguments[++index]);
        if (option == "--csv") {
            require(!text.empty(), "--csv requires a nonempty output path.");
            result.common.csv = text;
            continue;
        }
        size_t value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(),
            "Kitchen benchmark options require unsigned integers; use --help.");
        require(value != 0 || option == "--warmup", "Only --warmup accepts zero.");
        if (option == "--tasks") result.common.items = value;
        else if (option == "--repetitions") result.common.repetitions = value;
        else if (option == "--warmup") result.common.warmup = value;
        else if (option == "--latency-samples") result.latency_samples = value;
        else if (option == "--batch") result.common.batch = value;
        else if (option == "--timeout") result.common.timeout = value;
        else throw std::runtime_error("Unknown Kitchen benchmark option: " + std::string(option));
    }
    require(result.common.items <= UINT32_MAX, "--tasks exceeds the completion countdown range.");
    require(result.common.repetitions <= SIZE_MAX - result.common.warmup, "Sample count overflow.");
    require(result.latency_samples <= SIZE_MAX / result.common.repetitions,
        "Latency sample count overflow.");
    return result;
}
/** --------------------------------------------------------------------------------------------------------- Empty Task
 * @brief Leaves the measured work entirely in task submission and completion scheduling.
 */
void empty_task(void*) {}
/** --------------------------------------------------------------------------------------------------------- Throughput Workload
 * @brief Reuses batch storage while every producer waits for the final completed task.
 */
struct ThroughputWorkload {
    size_t tasks;
    size_t producers;
    bool bulk;
    TaskCountdown finished;
    Task task;
    std::vector<Task> batch;
    ThroughputWorkload(const Configuration& config, size_t workers, bool batched)
        : tasks(config.common.items), producers(workers), bulk(batched),
          finished(static_cast<uint32_t>(tasks)),
          task{&empty_task, nullptr, &TaskCountdown::arrive, &finished},
          batch(config.common.batch, task) {}
    /** ------------------------------------------------------------------------------------------- Prepare
     * @brief Rearms completion after the persistent producer team has finished its previous sample.
     */
    void prepare() { finished.rearm(static_cast<uint32_t>(tasks)); }
    /** ------------------------------------------------------------------------------------------- Run
     * @brief Submits an exact partition and includes completion observation in the measured interval.
     */
    void run(size_t producer) {
        const size_t first = partition(tasks, producer, producers);
        const size_t last = partition(tasks, producer + 1, producers);
        if (bulk) {
            for (size_t index = first; index < last;) {
                const size_t count = std::min(batch.size(), last - index);
                Kitchen::inst().submit_bulk(batch.data(), count);
                index += count;
            }
        } else {
            for (size_t index = first; index < last; ++index) Kitchen::inst().submit(task);
        }
        finished.wait();
    }
};
/** --------------------------------------------------------------------------------------------------------- Throughput
 * @brief Measures repeated completed-task throughput without rebuilding producer threads per sample.
 */
void throughput(const Configuration& config, size_t producers, bool bulk, Reporter& reporter) {
    Options options = config.common;
    options.batch = bulk ? options.batch : 1;
    const Shape shape{producers, Kitchen::inst().max_threads(), false};
    const std::string workload = bulk ? "bulk-completed" : "single-completed";
    LOG_INFO_STREAM << "Timing Kitchen " << workload << ' ' << shape.label()
        << "; tasks=" << options.items << "; batch=" << options.batch;
    Progress progress("Kitchen " + workload + ' ' + shape.label(), options.timeout);
    ThroughputWorkload pending(config, producers, bulk);
    Team team(pending, producers);
    std::vector<double> seconds;
    seconds.reserve(options.repetitions);
    for (size_t sample = 0; sample < options.warmup + options.repetitions; ++sample) {
        pending.prepare();
        const double elapsed = team.measure();
        if (sample >= options.warmup) seconds.push_back(elapsed);
    }
    reporter.report("Kitchen", workload, shape, options, options.items, std::move(seconds));
}
/** --------------------------------------------------------------------------------------------------------- Latency
 * @brief Writes measured round trips after each sample and reports pooled completion percentiles.
 */
void latency(const Configuration& config, bool parked, std::ofstream& csv, size_t cores) {
    const size_t count = parked ? std::max<size_t>(1, config.latency_samples / 100)
        : config.latency_samples;
    const std::string label = parked ? "parked" : "warm";
    LOG_INFO_STREAM << "Timing Kitchen " << label << " latency; " << count
        << " round trips per repetition; parking=" << (parked ? 2 : 0) << " ms";
    Progress progress("Kitchen " + label + " submit-to-completion", config.common.timeout);
    TaskCountdown finished(1);
    std::vector<double> sample(count);
    std::vector<double> measured;
    measured.reserve(count * config.common.repetitions);
    for (size_t repetition = 0;
        repetition < config.common.warmup + config.common.repetitions; ++repetition) {
        for (size_t index = 0; index < count; ++index) {
            if (parked) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            finished.rearm(1);
            const Clock::time_point started = Clock::now();
            Kitchen::inst().submit(&empty_task, nullptr, &TaskCountdown::arrive, &finished);
            finished.wait();
            sample[index] = std::chrono::duration<double, std::nano>(Clock::now() - started).count();
        }
        if (repetition < config.common.warmup) continue;
        for (size_t index = 0; index < count; ++index) {
            measured.push_back(sample[index]);
            csv << label << ',' << repetition - config.common.warmup + 1 << ',' << index + 1
                << ',' << std::setprecision(12) << sample[index] << ',' << cores << ','
                << Kitchen::inst().max_threads() << ',' << (parked ? 2 : 0) << '\n';
        }
        csv.flush();
        require(csv.good(), "Failed to write Kitchen latency samples.");
    }
    std::sort(measured.begin(), measured.end());
    LOG_INFO_STREAM << "Kitchen " << label << " submit-to-completion: p50="
        << measured[(measured.size() - 1) / 2] << " ns, p95="
        << measured[(measured.size() - 1) * 95 / 100] << " ns, p99="
        << measured[(measured.size() - 1) * 99 / 100] << " ns; samples=" << measured.size();
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs persistent producer teams through reported-core and oversubscribed topologies.
 */
int main(int count, char** arguments) {
    try {
        const Configuration config = configuration(count, arguments);
        if (config.common.help) {
            LOG_INFO_STREAM << "Kitchen benchmark options: --tasks 1048576 --repetitions 15 "
                << "--warmup 2 --latency-samples 20000 --batch 256 --timeout 120 "
                << "--csv kitchen_samples.csv; latency rows use the .latency.csv suffix";
            return 0;
        }
        const size_t cores = std::thread::hardware_concurrency();
        require(cores != 0, "The operating system did not report a CPU count for topology selection.");
        require(cores <= SIZE_MAX / 2, "The reported CPU count overflows oversubscription sizing.");
        std::vector<size_t> producers{1, 2, cores, cores * 2};
        std::sort(producers.begin(), producers.end());
        producers.erase(std::unique(producers.begin(), producers.end()), producers.end());
        Reporter reporter(config.common);
        std::ofstream latency_csv(config.common.csv + ".latency.csv");
        require(latency_csv.is_open(), "Cannot open Kitchen latency CSV output.");
        latency_csv << "workload,repetition,sample,latency_ns,logical_cpus,max_workers,parking_ms\n";
        LOG_INFO_STREAM << "Kitchen max workers=" << Kitchen::inst().max_threads()
            << "; reported logical CPUs=" << cores << "; oversubscribed producers=" << cores * 2
            << "; timings end after all task callbacks complete";
        for (const size_t workers : producers) {
            throughput(config, workers, false, reporter);
            throughput(config, workers, true, reporter);
        }
        latency(config, false, latency_csv, cores);
        latency(config, true, latency_csv, cores);
        LOG_INFO_STREAM << "Kitchen raw samples: " << config.common.csv << " and "
            << config.common.csv << ".latency.csv";
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Kitchen benchmark failed: " << error.what();
        return 1;
    }
}
