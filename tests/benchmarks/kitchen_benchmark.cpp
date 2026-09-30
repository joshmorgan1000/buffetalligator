/** --------------------------------------------------------------------------------------------------------- Kitchen Benchmark
 * @file kitchen_benchmark.cpp
 * @brief Measures Kitchen task throughput (single and bulk submits across producer counts) and
 * submit-to-completion latency on a warm and on a parked pool.
 */
#include <alligator/kitchen.hpp>
#include <logging.hpp>
#include <algorithm>
#include <barrier>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using buffetalligator::Kitchen;
using buffetalligator::Task;
using buffetalligator::TaskCountdown;
using Clock = std::chrono::steady_clock;
/** --------------------------------------------------------------------------------------------------------- Options
 * @struct Options
 * @brief Work sizes; everything else is probed from the machine.
 */
struct Options {
    size_t tasks = 1u << 20;         ///< Tasks per throughput repetition.
    size_t repetitions = 5;          ///< Timed repetitions per configuration.
    size_t latency_samples = 20000;  ///< Round trips per latency measurement.
    size_t batch = 256;              ///< Tasks per submit_bulk call.
};
/** --------------------------------------------------------------------------------------------------------- Parse
 * @brief Reads `--tasks`, `--repetitions`, `--latency-samples`, and `--batch`.
 */
Options parse(int count, char** arguments) {
    Options options;
    for (int index = 1; index + 1 < count; index += 2) {
        const std::string_view name(arguments[index]);
        const std::string_view text(arguments[index + 1]);
        size_t value = 0;
        if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{} || value == 0) {
            throw std::runtime_error("Benchmark options take positive integers, e.g. --tasks 1048576");
        }
        if (name == "--tasks") options.tasks = value;
        else if (name == "--repetitions") options.repetitions = value;
        else if (name == "--latency-samples") options.latency_samples = value;
        else if (name == "--batch") options.batch = value;
        else throw std::runtime_error("Unknown option; use --tasks, --repetitions, --latency-samples, --batch");
    }
    return options;
}
/** --------------------------------------------------------------------------------------------------------- Empty Task
 * @brief The measured unit of work: nothing, so the numbers are pure scheduling cost.
 */
void empty_task(void*) {}
/** --------------------------------------------------------------------------------------------------------- Median
 * @brief The median of a sample set, reordering it in place.
 */
double median(std::vector<double>& samples) {
    std::nth_element(samples.begin(), samples.begin() + samples.size() / 2, samples.end());
    return samples[samples.size() / 2];
}
/** --------------------------------------------------------------------------------------------------------- Produce
 * @brief One producer thread's share of a throughput repetition.
 * @param options Batch size for bulk submits.
 * @param finished The repetition's countdown every task arrives on.
 * @param start Releases every producer and the timer together.
 * @param first First task index this producer owns.
 * @param last One past the last task index this producer owns.
 * @param bulk Whether to use submit_bulk.
 */
void produce(
    const Options* options,
    TaskCountdown* finished,
    std::barrier<>* start,
    size_t first,
    size_t last,
    bool bulk
) {
    const Task task{&empty_task, nullptr, &TaskCountdown::arrive, finished};
    std::vector<Task> batch(options->batch, task);
    start->arrive_and_wait();
    if (bulk) {
        for (size_t index = first; index < last; index += options->batch) {
            Kitchen::inst().submit_bulk(batch.data(), std::min(options->batch, last - index));
        }
    } else {
        for (size_t index = first; index < last; ++index) Kitchen::inst().submit(task);
    }
}
/** --------------------------------------------------------------------------------------------------------- Throughput
 * @brief Splits `tasks` across `producers` threads and times first submit to last completion.
 * @param producers Submitting threads.
 * @param bulk Whether producers use submit_bulk in `batch` chunks.
 * @return Median nanoseconds per task over the repetitions.
 */
double throughput(const Options& options, size_t producers, bool bulk) {
    std::vector<double> per_task;
    for (size_t repetition = 0; repetition <= options.repetitions; ++repetition) {
        TaskCountdown finished(static_cast<uint32_t>(options.tasks));
        std::barrier start(static_cast<std::ptrdiff_t>(producers + 1));
        std::vector<std::thread> threads;
        threads.reserve(producers);
        for (size_t producer = 0; producer < producers; ++producer) {
            const size_t first = options.tasks * producer / producers;
            const size_t last = options.tasks * (producer + 1) / producers;
            threads.emplace_back(&produce, &options, &finished, &start, first, last, bulk);
        }
        start.arrive_and_wait();
        const Clock::time_point began = Clock::now();
        finished.wait();
        const double elapsed = std::chrono::duration<double, std::nano>(Clock::now() - began).count();
        for (std::thread& thread : threads) thread.join();
        if (repetition > 0) per_task.push_back(elapsed / static_cast<double>(options.tasks));
    }
    return median(per_task);
}
/** --------------------------------------------------------------------------------------------------------- Latency
 * @brief Times single-task round trips, optionally letting the pool park before each one.
 * @param samples Round trips to time.
 * @param idle_first Sleep before each submit so the round trip includes a worker wakeup.
 * @param p99 Receives the 99th percentile in nanoseconds.
 * @return The median round trip in nanoseconds.
 */
double latency(size_t samples, bool idle_first, double& p99) {
    std::vector<double> round_trips;
    round_trips.reserve(samples);
    TaskCountdown finished(1);
    for (size_t sample = 0; sample < samples; ++sample) {
        if (idle_first) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        finished.rearm(1);
        const Clock::time_point began = Clock::now();
        Kitchen::inst().submit(&empty_task, nullptr, &TaskCountdown::arrive, &finished);
        finished.wait();
        round_trips.push_back(std::chrono::duration<double, std::nano>(Clock::now() - began).count());
    }
    std::sort(round_trips.begin(), round_trips.end());
    p99 = round_trips[std::min(round_trips.size() - 1, round_trips.size() * 99 / 100)];
    return round_trips[round_trips.size() / 2];
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs throughput at 1, 2, and all-core producer counts, then warm and parked latency.
 */
int main(int count, char** arguments) {
    try {
        const Options options = parse(count, arguments);
        const size_t cores = std::max<size_t>(1u, std::thread::hardware_concurrency());
        std::vector<size_t> producer_counts{1, 2, cores};
        producer_counts.erase(std::unique(producer_counts.begin(), producer_counts.end()), producer_counts.end());
        LOG_INFO_STREAM << "Kitchen benchmark: " << options.tasks << " empty tasks x " << options.repetitions
            << " repetitions, " << Kitchen::inst().max_threads() << " max workers";
        for (const size_t producers : producer_counts) {
            for (const bool bulk : {false, true}) {
                LOG_INFO_STREAM << "Timing " << (bulk ? "bulk" : "single") << " submits from " << producers
                    << " producer thread" << (producers == 1 ? "" : "s") << "...";
                const double nanoseconds = throughput(options, producers, bulk);
                LOG_INFO_STREAM << "  " << nanoseconds << " ns/task, "
                    << 1000.0 / nanoseconds << " M tasks/s";
            }
        }
        double p99 = 0;
        LOG_INFO_STREAM << "Timing warm round trips (" << options.latency_samples << " samples)...";
        const double warm = latency(options.latency_samples, false, p99);
        LOG_INFO_STREAM << "  warm submit-to-completion: median " << warm << " ns, p99 " << p99 << " ns";
        const size_t parked_samples = std::max<size_t>(1, options.latency_samples / 100);
        LOG_INFO_STREAM << "Timing parked-pool round trips (" << parked_samples << " samples, ~"
            << parked_samples * 2 / 1000 + 1 << " s)...";
        const double parked = latency(parked_samples, true, p99);
        LOG_INFO_STREAM << "  parked submit-to-completion: median " << parked << " ns, p99 " << p99 << " ns";
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Kitchen benchmark failed: " << error.what();
        return 1;
    }
}
