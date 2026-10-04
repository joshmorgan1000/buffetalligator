/** --------------------------------------------------------------------------------------------------------- Kitchen Order Latency
 * @file kitchen_order_latency.cpp
 * @brief Measures completed typed Orders, registered fanout rounds, and three-stage chains.
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
#include <latch>
#include <limits>
#include <semaphore>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
using buffetalligator::Kitchen;
using buffetalligator::Order;
using buffetalligator::OrderCompletion;
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
 * @brief Validates experiment settings before allocating samples or registering teams.
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
/** --------------------------------------------------------------------------------------------------------- State
 * @struct State
 * @brief Holds preallocated participant slots and plain writes published between chain stages.
 */
struct State {
    size_t* slots;
    size_t prepared = 0;
    size_t terminal = 0;
};
/** --------------------------------------------------------------------------------------------------------- Prepare
 * @brief Publishes the typed Order's argument value for subsequent observation or fanout.
 * @param state The preallocated invocation state.
 * @param value The invocation's unique expected value.
 */
void prepare(State* state, size_t value) {
    state->prepared = value;
}
/** --------------------------------------------------------------------------------------------------------- Parallel
 * @brief Writes one distinct preallocated result slot per registered participant.
 * @param rank The current participant's rank.
 * @param count The registered participant count.
 * @param state The invocation state shared by the registered team.
 */
void parallel(size_t rank, size_t count, State* state) {
    state->slots[rank] = state->prepared + rank + count;
}
/** --------------------------------------------------------------------------------------------------------- Finalize
 * @brief Publishes terminal execution after all fanout participants have completed.
 * @param state The preallocated invocation state.
 */
void finalize(State* state) {
    state->terminal = state->prepared;
}
using Registration = decltype(Kitchen::inst().register_fanout(size_t{1}, &parallel));
/** --------------------------------------------------------------------------------------------------------- Workload
 * @enum Workload
 * @brief Selects a typed Order, a registered round, or an ordinary-to-fanout-to-ordinary chain.
 */
enum class Workload { Single, Fanout, Chain };
/** --------------------------------------------------------------------------------------------------------- Completion
 * @struct Completion
 * @brief Keeps each invocation's one-shot observers alive until the final untimed drain.
 */
struct Completion {
    std::latch terminal{1};
    OrderCompletion parallel;
};
/** --------------------------------------------------------------------------------------------------------- Measure
 * @brief Times arena construction through terminal observation before verifying results.
 * @tparam workload The compile-time dispatch shape.
 * @param options The validated measurement settings.
 * @param label The workload name written to logs and CSV.
 * @param threads The fanout size or one for the ordinary Order.
 * @param registration The registered team used by fanout workloads.
 * @param csv The open raw-sample output stream.
 */
template<Workload workload>
void measure(
    const Options& options,
    const char* label,
    size_t threads,
    Registration* registration,
    std::ofstream& csv
) {
    const size_t total = options.warmup + options.samples;
    std::vector<double> samples(options.samples);
    std::vector<Completion> completions(total);
    std::vector<size_t> slots(threads);
    State state{slots.data()};
    Kitchen::inst().prepare_producer();
    LOG_INFO_STREAM << "Measuring " << label << ", team=" << threads
        << ", warmup=" << options.warmup << ", samples=" << options.samples;
    Progress progress(label, threads, total);
    for (size_t round = 0; round < total; ++round) {
        const size_t expected = round + 1;
        state.prepared = workload == Workload::Fanout ? expected : 0;
        state.terminal = 0;
        Completion& completion = completions[round];
        const auto started = Clock::now();
        if constexpr (workload == Workload::Single) {
            Order order(&prepare, &state, expected);
            order.complete_with(completion.terminal);
            Kitchen::inst().submit(std::move(order));
        } else if constexpr (workload == Workload::Fanout) {
            registration->invoke(completion.terminal, &state);
        } else {
            Order terminal(&finalize, &state);
            terminal.complete_with(completion.terminal);
            Order fanout = registration->order(completion.parallel, &state);
            fanout.then(std::move(terminal));
            Order initial(&prepare, &state, expected);
            initial.then(std::move(fanout));
            Kitchen::inst().submit(std::move(initial));
        }
        completion.terminal.wait();
        const double elapsed = std::chrono::duration<double, std::nano>(
            Clock::now() - started).count();
        if constexpr (workload == Workload::Chain) {
            completion.parallel.wait();
            if (state.terminal != expected) {
                throw MeasurementException(
                    "The terminal Order did not publish the expected value."
                );
            }
        }
        if (state.prepared != expected) {
            throw MeasurementException("The typed Order did not publish the expected value.");
        }
        if constexpr (workload != Workload::Single) {
            for (size_t rank = 0; rank < threads; ++rank) {
                if (slots[rank] != expected + rank + threads) {
                    throw MeasurementException("A fanout participant did not publish its result.");
                }
            }
        }
        if (round >= options.warmup) {
            samples[round - options.warmup] = elapsed;
        }
        progress.completed(round + 1);
    }
    if constexpr (workload != Workload::Single) {
        registration->drain();
    }
    Kitchen::inst().drain();
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
 * @brief Measures typed dispatch and registered teams without claiming application speedups.
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
            << "timing includes typed arena construction and dispatch through terminal latch wait;"
            << " setup, verification, CSV output, and final drain are excluded";
        LOG_INFO_STREAM << "One fresh retained latch per invocation; one invocation in flight; "
            << "participant slots are contiguous; these results are not application speedups";
        measure<Workload::Single>(options, "typed-order", 1, nullptr, csv);
        for (const size_t threads : {size_t{4}, hardware}) {
            auto registration = Kitchen::inst().register_fanout(threads, &parallel, 1);
            measure<Workload::Fanout>(options, "fanout", threads, &registration, csv);
            measure<Workload::Chain>(options, "order-fanout-order", threads, &registration, csv);
            if (hardware == 4) {
                break;
            }
        }
        LOG_INFO_STREAM << "Raw completed-invocation latency samples: " << options.csv;
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Kitchen Order latency experiment failed: " << error.what();
        return 1;
    }
}
