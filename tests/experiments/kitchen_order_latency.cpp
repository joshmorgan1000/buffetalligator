/** --------------------------------------------------------------------------------------------------------- Kitchen Order Latency
 * @file kitchen_order_latency.cpp
 * @brief Measures typed Order construction and submit-to-future completion latency.
 */
#include <alligator/kitchen.hpp>
#include <logging.hpp>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <future>
#include <memory>
#include <limits>
#include <semaphore>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
using buffetalligator::Kitchen;
using Clock = std::chrono::steady_clock;
/** --------------------------------------------------------------------------------------------------------- Measurement Exception
 * @class MeasurementException
 * @brief Reports invalid measurement settings or failed result verification.
 */
class MeasurementException : public threadsafe_logger::Exception {
public:
    using threadsafe_logger::Exception::Exception;
    using threadsafe_logger::Exception::what;
};
/** --------------------------------------------------------------------------------------------------------- Options
 * @struct Options
 * @brief Selects discarded warmup invocations, measured invocations, and raw sample output.
 */
struct Options {
    size_t warmup = 1000;
    size_t samples = 10000;
    std::string csv = "kitchen_order_latency.csv";
    bool help = false;
};
/** --------------------------------------------------------------------------------------------------------- Parse
 * @brief Validates experiment settings before allocating samples.
 * @param count The number of command-line arguments.
 * @param arguments The command-line arguments.
 * @return The validated measurement settings.
 */
Options parse(int count, char** arguments) {
    Options options;
    for (int index = 1; index < count; ++index) {
        const std::string_view name(arguments[index]);
        if (name == "--help") {
            options.help = true;
            continue;
        }
        if (++index == count) {
            throw MeasurementException("Missing option value; use --help.");
        }
        const std::string_view value(arguments[index]);
        if (name == "--csv") {
            options.csv = value;
            continue;
        }
        size_t number = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
            throw MeasurementException("Sample counts require unsigned decimal integers.");
        }
        if (name == "--samples") {
            options.samples = number;
        } else if (name == "--warmup") {
            options.warmup = number;
        } else {
            throw MeasurementException("Unknown option; use --help.");
        }
    }
    if (!options.samples || options.csv.empty()
        || options.warmup > std::numeric_limits<size_t>::max() - options.samples) {
        throw MeasurementException(
            "Use positive --samples, a CSV path, and a representable total."
        );
    }
    return options;
}
/** --------------------------------------------------------------------------------------------------------- Progress
 * @class Progress
 * @brief Reports progress from a parked monitor without placing logging inside measured intervals.
 */
class Progress {
private:
    const char* label_;
    size_t threads_;
    size_t total_;
    std::atomic<size_t> completed_{0};
    std::binary_semaphore stopped_{0};
    std::thread monitor_;
    /** ------------------------------------------------------------------------------------------- Monitor
     * @brief Logs completed invocation counts once per second until measurement ends.
     * @param progress The active workload monitor.
     */
    static void monitor(Progress* progress) {
        while (!progress->stopped_.try_acquire_for(std::chrono::seconds(1))) {
            LOG_INFO_STREAM << progress->label_ << ", team=" << progress->threads_
                << ": completed " << progress->completed_.load(std::memory_order_relaxed)
                << '/' << progress->total_ << " warmup and measured invocations";
        }
    }
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Starts progress reporting before any timed invocation.
     * @param label The workload name.
     * @param threads The participating handler count.
     * @param total The combined warmup and measured invocation count.
     */
    Progress(const char* label, size_t threads, size_t total)
        : label_(label), threads_(threads), total_(total), monitor_(&Progress::monitor, this) {}
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Stops and joins the reporting thread outside the measured intervals.
     */
    ~Progress() {
        stopped_.release();
        monitor_.join();
    }
    /** ------------------------------------------------------------------------------------------- Completed
     * @brief Publishes one verified invocation's progress outside its measured interval.
     * @param count The number of completed invocations.
     */
    void completed(size_t count) {
        completed_.store(count, std::memory_order_relaxed);
    }
};
/** --------------------------------------------------------------------------------------------------------- Identity
 * @brief Returns the invocation's expected value through its typed future.
 */
size_t identity(size_t value) { return value; }
/** --------------------------------------------------------------------------------------------------------- Measure
 * @brief Times typed construction through future completion before verifying each result.
 * @param options The validated measurement settings.
 * @param csv The output stream receiving raw samples.
 */
void measure(const Options& options, std::ofstream& csv) {
    const char* label = "typed-order";
    const size_t threads = 1;
    const size_t total = options.warmup + options.samples;
    std::vector<double> samples(options.samples);
    LOG_INFO_STREAM << "Measuring " << label << ", warmup=" << options.warmup
        << ", samples=" << options.samples;
    Progress progress(label, threads, total);
    for (size_t round = 0; round < total; ++round) {
        const size_t expected = round + 1;
        std::unique_ptr<std::future<size_t>> completion;
        const auto started = Clock::now();
        Kitchen::submit(&identity, completion, expected);
        const size_t result = completion->get();
        const double elapsed = std::chrono::duration<double, std::nano>(
            Clock::now() - started).count();
        if (result != expected) {
            throw MeasurementException("The typed Order did not return the expected value.");
        }
        if (round >= options.warmup) samples[round - options.warmup] = elapsed;
        progress.completed(round + 1);
    }
    for (size_t index = 0; index < samples.size(); ++index) {
        csv << label << ',' << threads << ',' << index + 1 << ','
            << std::setprecision(12) << samples[index] << '\n';
    }
    csv.flush();
    if (!csv.good()) {
        throw MeasurementException("Cannot write raw latency samples.");
    }
    std::sort(samples.begin(), samples.end());
    LOG_INFO_STREAM << label << ", team=" << threads << ": p50="
        << samples[(samples.size() - 1) / 2] << " ns, p95="
        << samples[(samples.size() - 1) * 95 / 100] << " ns, p99="
        << samples[(samples.size() - 1) * 99 / 100] << " ns per completed invocation";
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Measures typed dispatch through standard future completion.
 * @param count The number of command-line arguments.
 * @param arguments The command-line arguments.
 * @return Zero on successful verification or one on failure.
 */
int main(int count, char** arguments) {
    try {
        const Options options = parse(count, arguments);
        if (options.help) {
            LOG_INFO_STREAM << "Options: --warmup 1000 --samples 10000 "
                << "--csv kitchen_order_latency.csv";
            return 0;
        }
        const size_t hardware = std::thread::hardware_concurrency();
        if (!hardware) {
            throw MeasurementException("The operating system did not report a CPU count.");
        }
        std::ofstream csv(options.csv);
        if (!csv.is_open()) {
            throw MeasurementException("Cannot open the requested latency CSV output.");
        }
        csv << "workload,team_size,sample,latency_ns\n";
        LOG_INFO_STREAM << "Dispatch-overhead experiment on " << hardware << " reported CPUs; "
            << "timing includes typed Order construction and dispatch through future completion; "
            << "verification and CSV output are excluded";
        measure(options, csv);
        LOG_INFO_STREAM << "Raw completed-invocation latency samples: " << options.csv;
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Kitchen Order latency experiment failed: " << error.what();
        return 1;
    }
}
