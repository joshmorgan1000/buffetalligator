/** --------------------------------------------------------------------------------------------------------- I/O Isolation Experiment
 * @file io_isolation.cpp
 * @brief Measures compute admission during controlled blocking pipe reads with optional coroutine handoff.
 */
#include "executors.hpp"
#include <logging.hpp>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <fstream>
#include <future>
#include <iomanip>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <sys/resource.h>
#include <unistd.h>

namespace kitchen_dispatch {
using Clock = std::chrono::steady_clock;
using buffetalligator::Order;
/** --------------------------------------------------------------------------------------------------------- Options
 * @brief Selects one executor and one blocking-read placement for a complete experiment process.
 */
struct Options {
    std::string executor = "blocking";
    std::string path = "separate";
    std::string csv;
    size_t rounds = 101;
};
/** --------------------------------------------------------------------------------------------------------- Parse Options
 * @brief Reads the experiment arguments without changing the executor's hardware-derived worker count.
 */
Options parse_options(int count, char** arguments) {
    Options options;
    for (int index = 1; index < count; ++index) {
        const std::string_view argument(arguments[index]);
        if (index + 1 == count) throw std::invalid_argument("Missing experiment option value");
        const std::string value(arguments[++index]);
        if (argument == "--executor") options.executor = value;
        else if (argument == "--path") options.path = value;
        else if (argument == "--rounds") options.rounds = std::stoull(value);
        else if (argument == "--csv") options.csv = value;
        else throw std::invalid_argument("Unknown experiment option: " + std::string(argument));
    }
    if (options.rounds == 0) throw std::invalid_argument("Round count must be positive");
    if (options.path != "shared" && options.path != "separate" && options.path != "coroutine")
        throw std::invalid_argument("Path must be shared, separate, or coroutine");
    return options;
}
/** --------------------------------------------------------------------------------------------------------- CPU Seconds
 * @brief Returns process CPU time accumulated by all worker, controller, and release threads.
 */
double cpu_seconds() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        throw std::system_error(errno, std::generic_category(), "Reading process CPU time");
    return double(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec)
        + double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
}
/** --------------------------------------------------------------------------------------------------------- Read Coroutine
 * @brief Retains a suspended frame until its final compute callback has published completion.
 */
struct ReadCoroutine {
    /** ------------------------------------------------------------------------------------------- Promise
     * @brief Suspends both endpoints so the round controller owns frame destruction.
     */
    struct promise_type {
        ReadCoroutine get_return_object() {
            return ReadCoroutine(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };
    std::coroutine_handle<promise_type> handle;
    explicit ReadCoroutine(std::coroutine_handle<promise_type> frame) : handle(frame) {}
    ReadCoroutine(const ReadCoroutine&) = delete;
    ReadCoroutine& operator=(const ReadCoroutine&) = delete;
    ReadCoroutine(ReadCoroutine&& other) noexcept : handle(std::exchange(other.handle, {})) {}
    ~ReadCoroutine() { if (handle) handle.destroy(); }
};
/** --------------------------------------------------------------------------------------------------------- Read Request
 * @brief Owns one unfilled pipe and the completion state retained throughout its executor callbacks.
 */
template<typename Executor>
struct ReadRequest {
    Executor* executor;
    int descriptors[2];
    std::promise<void> entered;
    std::promise<void> finished;
    std::coroutine_handle<> continuation;
    ssize_t bytes = -1;
    int error = 0;
    unsigned char value = 0;
    explicit ReadRequest(Executor* selected) : executor(selected) {
        if (pipe(descriptors) != 0)
            throw std::system_error(errno, std::generic_category(), "Creating controlled read pipe");
    }
    ReadRequest(const ReadRequest&) = delete;
    ReadRequest& operator=(const ReadRequest&) = delete;
    ~ReadRequest() {
        close(descriptors[0]);
        close(descriptors[1]);
    }
    /** ------------------------------------------------------------------------------------------- Read
     * @brief Publishes read entry before blocking until the independent release thread writes one byte.
     */
    static void read(void* context) {
        auto* request = static_cast<ReadRequest*>(context);
        std::promise<void> entered(std::move(request->entered));
        entered.set_value();
        do {
            request->bytes = ::read(request->descriptors[0], &request->value, 1);
        } while (request->bytes < 0 && errno == EINTR);
        if (request->bytes < 0) request->error = errno;
    }
    /** ------------------------------------------------------------------------------------------- Finish
     * @brief Transfers notification ownership before publishing that the callback no longer needs its request.
     */
    static void finish(void* context) {
        auto* request = static_cast<ReadRequest*>(context);
        std::promise<void> finished(std::move(request->finished));
        finished.set_value();
    }
    /** ------------------------------------------------------------------------------------------- Resume
     * @brief Runs the final coroutine segment to its suspended endpoint on a compute worker.
     */
    static void resume(void* context) {
        auto* request = static_cast<ReadRequest*>(context);
        request->continuation.resume();
    }
    /** ------------------------------------------------------------------------------------------- Read And Resume
     * @brief Completes the blocking read before transferring further request access to the compute queue.
     */
    static void read_and_resume(void* context) {
        auto* request = static_cast<ReadRequest*>(context);
        read(request);
        Executor* executor = request->executor;
        executor->compute(Order{&resume, request, &finish, request});
    }
};
/** --------------------------------------------------------------------------------------------------------- Read Awaiter
 * @brief Moves one suspended coroutine's blocking read onto the executor's storage queue.
 */
template<typename Executor>
struct ReadAwaiter {
    ReadRequest<Executor>* request;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> continuation) const {
        request->continuation = continuation;
        Executor* executor = request->executor;
        executor->storage(Order{&ReadRequest<Executor>::read_and_resume, request});
    }
    void await_resume() const noexcept {}
};
/** --------------------------------------------------------------------------------------------------------- Storage Coroutine
 * @brief Offloads exactly one blocking read and returns after resumption on the compute executor.
 */
template<typename Executor>
ReadCoroutine storage_coroutine(ReadRequest<Executor>* request) {
    co_await ReadAwaiter<Executor>{request};
}
/** --------------------------------------------------------------------------------------------------------- Probe
 * @brief Records a normal compute task's entry and callback completion relative to its submission.
 */
struct Probe {
    Clock::time_point submitted;
    Clock::time_point entered;
    Clock::time_point completed;
    std::promise<void> finished;
    static void run(void* context) {
        auto* probe = static_cast<Probe*>(context);
        probe->entered = Clock::now();
    }
    static void finish(void* context) {
        auto* probe = static_cast<Probe*>(context);
        probe->completed = Clock::now();
        std::promise<void> finished(std::move(probe->finished));
        finished.set_value();
    }
};
/** --------------------------------------------------------------------------------------------------------- Pipe Releaser
 * @brief Releases blocked reads at an independently published deadline established before probe submission.
 */
template<typename Executor>
struct PipeReleaser {
    std::vector<std::unique_ptr<ReadRequest<Executor>>>* requests;
    Clock::time_point deadline;
    std::latch ready{1};
    std::latch start{1};
    int error = 0;
    static void run(PipeReleaser* release) {
        release->ready.count_down();
        release->start.wait();
        std::this_thread::sleep_until(release->deadline);
        const unsigned char marker = 42;
        for (const auto& request : *release->requests) {
            ssize_t written;
            do {
                written = write(request->descriptors[1], &marker, 1);
            } while (written < 0 && errno == EINTR);
            if (written != 1) release->error = errno;
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Sample
 * @brief Retains latency samples and whole-run resource consumption after all request storage is released.
 */
struct Sample {
    std::vector<double> entry;
    std::vector<double> completion;
    std::vector<double> release_lead;
    size_t reads = 0;
    double wall_seconds = 0;
    double cpu_seconds = 0;
};
/** --------------------------------------------------------------------------------------------------------- Measure
 * @brief Measures one compute probe per round after every controlled blocking read has entered its worker.
 */
template<typename Executor>
Sample measure(Executor& executor, const Options& options, size_t workers) {
    typename Executor::Producer producer(executor);
    Sample sample;
    sample.entry.reserve(options.rounds);
    sample.completion.reserve(options.rounds);
    sample.release_lead.reserve(options.rounds);
    const double initial_cpu = cpu_seconds();
    const auto initial_time = Clock::now();
    auto last_progress = initial_time;
    for (size_t round = 0; round < options.rounds; ++round) {
        std::vector<std::unique_ptr<ReadRequest<Executor>>> requests;
        std::vector<std::future<void>> entered;
        std::vector<std::future<void>> finished;
        std::vector<ReadCoroutine> coroutines;
        requests.reserve(workers);
        entered.reserve(workers);
        finished.reserve(workers);
        coroutines.reserve(workers);
        for (size_t index = 0; index < workers; ++index) {
            requests.push_back(std::make_unique<ReadRequest<Executor>>(&executor));
            entered.push_back(requests.back()->entered.get_future());
            finished.push_back(requests.back()->finished.get_future());
        }
        for (const auto& request : requests) {
            Order task{&ReadRequest<Executor>::read, request.get(),
                &ReadRequest<Executor>::finish, request.get()};
            if (options.path == "shared") producer.compute(std::move(task));
            else if (options.path == "separate") producer.storage(std::move(task));
            else {
                coroutines.push_back(storage_coroutine(request.get()));
                coroutines.back().handle.resume();
            }
        }
        for (auto& future : entered) future.get();
        Probe probe;
        auto completed = probe.finished.get_future();
        PipeReleaser<Executor> release{&requests, {}};
        std::thread release_thread(&PipeReleaser<Executor>::run, &release);
        release.ready.wait();
        release.deadline = Clock::now() + std::chrono::milliseconds(20);
        release.start.count_down();
        probe.submitted = Clock::now();
        producer.compute(Order{&Probe::run, &probe, &Probe::finish, &probe});
        completed.get();
        for (auto& future : finished) future.get();
        release_thread.join();
        if (release.error != 0)
            throw std::system_error(release.error, std::generic_category(), "Releasing controlled pipe reads");
        for (const auto& request : requests) {
            if (request->bytes != 1 || request->value != 42 || request->error != 0)
                throw std::runtime_error("Controlled pipe read returned an incorrect payload");
            ++sample.reads;
        }
        sample.entry.push_back(std::chrono::duration<double, std::micro>(
            probe.entered - probe.submitted).count());
        sample.completion.push_back(std::chrono::duration<double, std::micro>(
            probe.completed - probe.submitted).count());
        sample.release_lead.push_back(std::chrono::duration<double, std::micro>(
            release.deadline - probe.submitted).count());
        const auto now = Clock::now();
        if (now - last_progress >= std::chrono::seconds(1)) {
            LOG_INFO_STREAM << "Pipe isolation " << options.executor << '/' << options.path
                << ": completed " << round + 1 << '/' << options.rounds << " rounds";
            last_progress = now;
        }
    }
    sample.wall_seconds = std::chrono::duration<double>(Clock::now() - initial_time).count();
    sample.cpu_seconds = cpu_seconds() - initial_cpu;
    std::sort(sample.entry.begin(), sample.entry.end());
    std::sort(sample.completion.begin(), sample.completion.end());
    std::sort(sample.release_lead.begin(), sample.release_lead.end());
    return sample;
}
/** --------------------------------------------------------------------------------------------------------- Quantile
 * @brief Uses nearest-rank order statistics without interpolating independently observed probe samples.
 */
double quantile(const std::vector<double>& values, size_t percentile) {
    return values[(values.size() * percentile + 99) / 100 - 1];
}
/** --------------------------------------------------------------------------------------------------------- Report
 * @brief Writes measured latency and CPU totals for controlled pipes without claiming physical storage latency.
 */
void report(const Options& options, const Sample& sample, size_t workers) {
    LOG_INFO_STREAM << options.executor << '/' << options.path << ": compute entry p50 "
        << quantile(sample.entry, 50) << " us, p99 " << quantile(sample.entry, 99)
        << " us; completion p99 " << quantile(sample.completion, 99) << " us; "
        << sample.reads << " verified pipe reads; CPU " << sample.cpu_seconds << " s over "
        << sample.wall_seconds << " s; planned release lead p50 "
        << quantile(sample.release_lead, 50) << " us from probe submission";
    if (options.csv.empty()) return;
    std::ofstream output(options.csv);
    if (!output) throw std::runtime_error("Cannot open CSV output: " + options.csv);
    output << "executor,path,workers,rounds,release_delay_ms,pipe_reads,entry_p50_us,entry_p95_us,"
        "entry_p99_us,entry_max_us,completion_p50_us,completion_p99_us,wall_seconds,cpu_seconds,"
        "cpu_percent,planned_release_lead_p50_us\n";
    output << std::setprecision(12) << options.executor << ',' << options.path << ',' << workers
        << ',' << options.rounds << ",20," << sample.reads << ',' << quantile(sample.entry, 50)
        << ',' << quantile(sample.entry, 95) << ',' << quantile(sample.entry, 99) << ','
        << sample.entry.back() << ',' << quantile(sample.completion, 50) << ','
        << quantile(sample.completion, 99) << ',' << sample.wall_seconds << ',' << sample.cpu_seconds
        << ',' << sample.cpu_seconds / sample.wall_seconds * 100.0 << ','
        << quantile(sample.release_lead, 50) << '\n';
    if (!output) throw std::runtime_error("Could not write CSV output: " + options.csv);
}
/** --------------------------------------------------------------------------------------------------------- Run
 * @brief Constructs one prestarted executor before starting the controlled-read measurements.
 */
template<typename Executor>
void run(const Options& options, size_t workers) {
    Executor executor(workers);
    LOG_INFO_STREAM << "Testing " << options.executor << '/' << options.path << " with "
        << workers << " blocking pipes; release deadline is set 20 ms ahead before signaling, "
        << "and probe timing starts after that signal immediately before enqueue";
    report(options, measure(executor, options, workers), workers);
}
} // namespace kitchen_dispatch
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs one selected isolation experiment while preserving failures as a nonzero process status.
 */
int main(int count, char** arguments) {
    try {
        const auto options = kitchen_dispatch::parse_options(count, arguments);
        const size_t workers = std::thread::hardware_concurrency();
        if (workers == 0) throw std::runtime_error("Hardware concurrency was not reported");
        if (options.executor == "kitchen")
            kitchen_dispatch::run<kitchen_dispatch::ProductionAdapter>(options, workers);
        else if (options.executor == "blocking")
            kitchen_dispatch::run<kitchen_dispatch::BlockingExecutor>(options, workers);
        else if (options.executor == "sleeping")
            kitchen_dispatch::run<kitchen_dispatch::SleepingExecutor>(options, workers);
        else if (options.executor == "spinning")
            kitchen_dispatch::run<kitchen_dispatch::SpinningExecutor>(options, workers);
        else throw std::invalid_argument("Executor must be kitchen, blocking, sleeping, or spinning");
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Pipe isolation experiment failed: " << error.what();
        return 1;
    }
}
