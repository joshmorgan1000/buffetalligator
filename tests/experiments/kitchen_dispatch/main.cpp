/** --------------------------------------------------------------------------------------------------------- Matched Kitchen Benchmark
 * @file main.cpp
 * @brief Compares bounded callbacks and buffered reads across production and persistent queue experiments.
 */
#include <alligator.hpp>
#include "executors.hpp"
#include <logging.hpp>
#include <algorithm>
#include <barrier>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <future>
#include <iomanip>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using buffetalligator::AlignedHeapBuffer;
using buffetalligator::BuffetDescriptors;
using buffetalligator::Order;
using namespace kitchen_dispatch;
using buffetalligator::Slice;
using Clock = std::chrono::steady_clock;
constexpr size_t block_bytes = 64 * 1024;
constexpr size_t file_bytes = 64 * 1024 * 1024;
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Rejects invalid harness configuration or results without changing production behavior.
 */
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
/** --------------------------------------------------------------------------------------------------------- Options
 * @brief Selects one endpoint and workload per process while retaining identical sample defaults.
 */
struct Options {
    size_t requests = 4096;
    size_t depth = 1;
    size_t warmup = 1;
    size_t repetitions = 5;
    size_t timeout = 180;
    size_t idle_us = 0;
    std::string executor = "kitchen";
    std::string mode = "compute";
    std::string work = "read";
    std::string directory = "/tmp";
    std::string csv;
    bool help = false;
};
/** --------------------------------------------------------------------------------------------------------- Parse
 * @brief Validates command-line options before starting measured work.
 */
Options parse(int count, char** arguments) {
    Options result;
    for (int index = 1; index < count; ++index) {
        const std::string_view option(arguments[index]);
        if (option == "--help") { result.help = true; continue; }
        require(index + 1 < count, "Missing option value; use --help.");
        const std::string_view value(arguments[++index]);
        if (option == "--executor") result.executor = value;
        else if (option == "--mode") result.mode = value;
        else if (option == "--work") result.work = value;
        else if (option == "--csv") result.csv = value;
        else if (option == "--directory") result.directory = value;
        else {
            size_t number = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size(),
                "Numeric options require unsigned decimal integers.");
            if (option == "--requests") result.requests = number;
            else if (option == "--depth") result.depth = number;
            else if (option == "--warmup") result.warmup = number;
            else if (option == "--repetitions") result.repetitions = number;
            else if (option == "--timeout") result.timeout = number;
            else if (option == "--idle-us") result.idle_us = number;
            else throw std::runtime_error("Unknown option: " + std::string(option));
        }
    }
    require(result.executor == "kitchen" || result.executor == "blocking"
        || result.executor == "sleeping" || result.executor == "spinning",
        "--executor must be kitchen, blocking, sleeping, or spinning.");
    require(result.mode == "compute" || result.mode == "storage", "--mode must be compute or storage.");
    require(result.work == "noop" || result.work == "read", "--work must be noop or read.");
    require(result.requests > 0 && result.requests <= SIZE_MAX / block_bytes,
        "--requests must be positive and fit the workload byte count.");
    require(result.depth > 0 && result.depth <= result.requests,
        "--depth must be positive and at most --requests.");
    require(result.repetitions > 0 && result.timeout > 0
        && result.warmup <= SIZE_MAX - result.repetitions, "Invalid repetition or timeout count.");
    return result;
}
/** --------------------------------------------------------------------------------------------------------- Progress
 * @brief Logs progress every second and aborts a stalled process with its current phase.
 */
class Progress {
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool stopped_ = false;
    std::string label_;
    size_t timeout_;
    std::thread monitor_;
    /** ------------------------------------------------------------------------------------------- Monitor
     * @brief Reports elapsed setup or sample time without touching measured request state.
     */
    static void monitor(Progress* self) {
        const auto began = Clock::now();
        std::unique_lock lock(self->mutex_);
        while (!self->stopped_) {
            self->changed_.wait_for(lock, std::chrono::seconds(1));
            if (self->stopped_) break;
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                Clock::now() - began).count();
            LOG_INFO_STREAM << self->label_ << ": working (" << seconds << " s)...";
            if (seconds >= static_cast<int64_t>(self->timeout_)) {
                LOG_ERROR_STREAM << "Matched benchmark exceeded --timeout; aborting stalled process.";
                std::abort();
            }
        }
    }
public:
    Progress(std::string label, size_t timeout)
        : label_(std::move(label)), timeout_(timeout), monitor_(&Progress::monitor, this) {}
    ~Progress() {
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
        }
        changed_.notify_one();
        monitor_.join();
    }
};
/** --------------------------------------------------------------------------------------------------------- Host Buffer
 * @brief Uses the same explicit novel aligned host Slice constructor on both installed APIs.
 */
Slice host_buffer() {
    return Slice(block_bytes, true, BuffetDescriptors::get(AlignedHeapBuffer::type_idx()));
}
/** --------------------------------------------------------------------------------------------------------- Mark
 * @brief Identifies both ends of an otherwise constant deterministic file block.
 */
void mark(Slice& block, size_t identifier) {
    block.data<uint64_t>()[0] = identifier;
    block.data<uint64_t>()[block_bytes / sizeof(uint64_t) - 1] =
        static_cast<uint64_t>(identifier) ^ UINT64_C(0xd19a735ec8042fb6);
}
/** --------------------------------------------------------------------------------------------------------- Scratch File
 * @brief Prepares an immediately unlinked buffered file outside all timed samples.
 */
class ScratchFile {
private:
    int descriptor_ = -1;
public:
    explicit ScratchFile(const Options& options) {
        std::string path = options.directory + "/kitchen-matched-XXXXXX";
        descriptor_ = mkstemp(path.data());
        if (descriptor_ < 0) throw std::system_error(errno, std::generic_category(), "Creating file");
        try {
            if (unlink(path.c_str()) != 0)
                throw std::system_error(errno, std::generic_category(), "Unlinking file");
            Slice block = host_buffer();
            std::fill_n(block.data<unsigned char>(), block_bytes, 0xa5);
            for (size_t offset = 0; offset < file_bytes; offset += block_bytes) {
                mark(block, offset / block_bytes);
                const ssize_t written = pwrite(descriptor_, block.raw(), block_bytes,
                    static_cast<off_t>(offset));
                if (written < 0) throw std::system_error(errno, std::generic_category(), "Writing file");
                require(static_cast<size_t>(written) == block_bytes, "Short setup write.");
            }
            if (fsync(descriptor_) != 0)
                throw std::system_error(errno, std::generic_category(), "Flushing setup file");
        } catch (...) { close(descriptor_); descriptor_ = -1; throw; }
    }
    ~ScratchFile() { if (descriptor_ >= 0) close(descriptor_); }
    int descriptor() const { return descriptor_; }
    ScratchFile(const ScratchFile&) = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;
};
/** --------------------------------------------------------------------------------------------------------- Result
 * @brief Stores one callback's writes until its producer observes the promise publication.
 */
struct Result {
    double entry_us = 0;
    double finish_us = 0;
    ssize_t bytes = 0;
    int error = 0;
    size_t identifier = SIZE_MAX;
};
/** --------------------------------------------------------------------------------------------------------- Request
 * @brief Gives identical callback code a shared Slice and a completion state it can retain independently.
 */
struct Request {
    Result* result;
    Slice buffer;
    std::promise<void> completed;
    Clock::time_point submitted;
    size_t identifier;
    int descriptor;
    bool read;
    /** ------------------------------------------------------------------------------------------- Run
     * @brief Moves owned arguments locally before publishing results and never touches the context afterward.
     */
    static void run(void* context) noexcept {
        const auto entered = Clock::now();
        auto& request = *static_cast<Request*>(context);
        Result* result = request.result;
        Slice buffer = std::move(request.buffer);
        std::promise<void> completed = std::move(request.completed);
        const auto submitted = request.submitted;
        const size_t identifier = request.identifier;
        const int descriptor = request.descriptor;
        const bool read = request.read;
        if (read) {
            result->bytes = pread(descriptor, buffer.raw(), block_bytes,
                static_cast<off_t>((identifier % (file_bytes / block_bytes)) * block_bytes));
            result->error = result->bytes < 0 ? errno : 0;
        } else {
            result->bytes = 0;
            result->error = 0;
        }
        const auto finished = Clock::now();
        result->identifier = identifier;
        result->entry_us = std::chrono::duration<double, std::micro>(entered - submitted).count();
        result->finish_us = std::chrono::duration<double, std::micro>(finished - submitted).count();
        completed.set_value();
    }
};
/** --------------------------------------------------------------------------------------------------------- Lane
 * @brief Keeps one request outstanding with reusable buffers and disjoint latency output slots.
 */
struct Lane {
    Slice buffer = host_buffer();
    Slice expected = host_buffer();
    Result result;
    std::exception_ptr error;
    size_t completed = 0;
    Lane() { std::fill_n(expected.data<unsigned char>(), block_bytes, 0xa5); }
};
/** --------------------------------------------------------------------------------------------------------- Team
 * @brief Reuses producer threads and buffers across warmup and all measured samples.
 */
template<typename Executor>
class Team {
private:
    const Options& options_;
    Executor& executor_;
    int descriptor_;
    std::barrier<> gate_;
    std::vector<std::unique_ptr<Lane>> lanes_;
    std::vector<std::thread> producers_;
    bool stopping_ = false;
    /** ------------------------------------------------------------------------------------------- Run
     * @brief Observes completion before checking payloads or reusing the request storage.
     */
    static void run(Team* team, size_t lane_index) {
        Lane& lane = *team->lanes_[lane_index];
        typename Executor::Producer producer(team->executor_);
        const Options& options = team->options_;
        const bool read = options.work == "read";
        const bool storage = options.mode == "storage";
        for (;;) {
            team->gate_.arrive_and_wait();
            if (team->stopping_) return;
            lane.error = {};
            lane.completed = 0;
            try {
                for (size_t index = lane_index; index < options.requests; index += options.depth) {
                    if (options.idle_us)
                        std::this_thread::sleep_for(std::chrono::microseconds(options.idle_us));
                    Request request{&lane.result, lane.buffer, {}, {}, index, team->descriptor_, read};
                    std::future<void> completion = request.completed.get_future();
                    request.submitted = Clock::now();
                    Order task{&Request::run, &request};
                    if (storage) producer.storage(std::move(task));
                    else producer.compute(std::move(task));
                    completion.get();
                    const auto observed = Clock::now();
                    team->entry[index] = lane.result.entry_us;
                    team->finish[index] = lane.result.finish_us;
                    team->observed[index] = std::chrono::duration<double, std::micro>(
                        observed - request.submitted).count();
                    require(lane.result.identifier == index, "Incorrect callback completion identifier.");
                    if (read) {
                        if (lane.result.error != 0)
                            throw std::system_error(lane.result.error, std::generic_category(), "Reading file");
                        require(lane.result.bytes == static_cast<ssize_t>(block_bytes), "Short read.");
                        mark(lane.expected, index % (file_bytes / block_bytes));
                        require(std::memcmp(lane.buffer.raw(), lane.expected.raw(), block_bytes) == 0,
                            "Incorrect file block or payload byte.");
                    }
                    ++lane.completed;
                }
            } catch (...) { lane.error = std::current_exception(); }
            team->gate_.arrive_and_wait();
        }
    }
public:
    std::vector<double> entry;
    std::vector<double> finish;
    std::vector<double> observed;
    Team(const Options& options, int descriptor, Executor& executor)
        : options_(options), executor_(executor), descriptor_(descriptor), gate_(options.depth + 1),
          entry(options.requests), finish(options.requests), observed(options.requests) {
        lanes_.reserve(options.depth);
        producers_.reserve(options.depth);
        for (size_t index = 0; index < options.depth; ++index)
            lanes_.push_back(std::make_unique<Lane>());
        for (size_t index = 0; index < options.depth; ++index)
            producers_.emplace_back(&Team::run, this, index);
    }
    ~Team() {
        stopping_ = true;
        gate_.arrive_and_wait();
        for (auto& producer : producers_) producer.join();
    }
    /** ------------------------------------------------------------------------------------------- Measure
     * @brief Includes submission, future observation, and verification while excluding producer construction.
     */
    double measure() {
        const auto started = Clock::now();
        gate_.arrive_and_wait();
        gate_.arrive_and_wait();
        const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
        size_t completed = 0;
        for (const auto& lane : lanes_) {
            if (lane->error) std::rethrow_exception(lane->error);
            completed += lane->completed;
        }
        require(completed == options_.requests, "Incorrect completed request count.");
        return seconds;
    }
};
/** --------------------------------------------------------------------------------------------------------- CPU Seconds
 * @brief Captures total process user and system CPU time across all current Kitchen threads.
 */
double cpu_seconds() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        throw std::system_error(errno, std::generic_category(), "Reading process CPU time");
    return double(usage.ru_utime.tv_sec) + double(usage.ru_stime.tv_sec)
        + (double(usage.ru_utime.tv_usec) + double(usage.ru_stime.tv_usec)) / 1e6;
}
/** --------------------------------------------------------------------------------------------------------- Quantiles
 * @brief Returns empirical p50 and p99 after sorting outside measured work.
 */
std::pair<double, double> quantiles(std::vector<double>& values) {
    std::sort(values.begin(), values.end());
    const size_t rank = values.size() - 1;
    return {values[rank / 2], values[(rank / 100) * 99 + (rank % 100) * 99 / 100]};
}
/** --------------------------------------------------------------------------------------------------------- Endpoint
 * @brief Reports the actual executor semantics rather than implying identical storage implementations.
 */
std::string endpoint(const Options& options) {
    return options.executor + "_" + options.mode;
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs one endpoint workload per process and leaves library shutdown to externally measured process exit.
 */
template<typename Executor>
int run(const Options& options) {
    if (options.help) {
        LOG_INFO_STREAM << "Options: --executor kitchen|blocking|sleeping|spinning --mode compute|storage --work noop|read --depth N (1) "
            << "--requests N (4096) --warmup N (1) --repetitions N (5) --timeout SECONDS (180) "
            << "--directory PATH (/tmp) --idle-us N (0) --csv PATH";
        return 0;
    }
    LOG_INFO_STREAM << "Matched benchmark: " << endpoint(options) << ", work=" << options.work
        << ", depth=" << options.depth << ", requests=" << options.requests
        << ", hardware_threads=" << std::thread::hardware_concurrency();
    LOG_INFO_STREAM << "Persistent producer lanes; promise creation outside per-request latency; "
        << "same callback and shared Slice transfer; all requests include completion publication.";
    LOG_INFO_STREAM << "Read workload: 64 MiB buffered file, 64 KiB blocks, full payload verification; "
        << "setup/fsync excluded, no cache eviction, no cold-disk throughput claim.";
    LOG_INFO_STREAM << "Entry, work-finish and producer-observed completion start before submission; "
        << "verification is excluded from latency and included in throughput; retirement may overlap.";
#ifndef NDEBUG
    LOG_WARN_STREAM << "Assertions enabled; use the Release build before comparing performance.";
#endif
    std::ofstream csv;
    if (!options.csv.empty()) {
        csv.open(options.csv);
        require(csv.is_open(), "Cannot open CSV output.");
        csv << "endpoint,work,sample,depth,requests,file_bytes,block_bytes,hardware_threads,seconds,"
            << "requests_per_second,mib_per_second,entry_p50_us,entry_p99_us,finish_p50_us,"
            << "finish_p99_us,observed_p50_us,observed_p99_us,cpu_seconds\n";
    }
    std::unique_ptr<ScratchFile> file;
    {
        Progress progress("Preparing shared buffers and buffered file", options.timeout);
        if (options.work == "read") file = std::make_unique<ScratchFile>(options);
    }
    Executor executor(std::thread::hardware_concurrency());
    Team<Executor> team(options, file ? file->descriptor() : -1, executor);
    for (size_t round = 0; round < options.warmup + options.repetitions; ++round) {
        const bool measured = round >= options.warmup;
        const std::string label = std::string(measured ? "Measured " : "Warmup ")
            + endpoint(options) + " " + options.work + " round=" + std::to_string(round + 1);
        LOG_INFO_STREAM << label;
        Progress progress(label, options.timeout);
        const double initial_cpu = cpu_seconds();
        const double seconds = team.measure();
        const double cpu = cpu_seconds() - initial_cpu;
        const auto entry = quantiles(team.entry);
        const auto finish = quantiles(team.finish);
        const auto observed = quantiles(team.observed);
        if (!measured) continue;
        const size_t sample = round - options.warmup + 1;
        const double throughput = options.requests / seconds;
        const double mib = options.work == "read" ? throughput * block_bytes / (1024 * 1024) : 0;
        LOG_INFO_STREAM << "Sample=" << sample << ", requests/s=" << throughput
            << ", MiB/s=" << mib << ", entry p50/p99=" << entry.first << '/' << entry.second
            << " us, finish p50/p99=" << finish.first << '/' << finish.second
            << " us, observed p50/p99=" << observed.first << '/' << observed.second
            << " us, CPU=" << cpu << " s";
        if (csv.is_open()) {
            csv << endpoint(options) << ',' << options.work << ',' << sample << ','
                << options.depth << ',' << options.requests << ','
                << (options.work == "read" ? file_bytes : 0) << ','
                << (options.work == "read" ? block_bytes : 0) << ','
                << std::thread::hardware_concurrency() << ',' << std::setprecision(12)
                << seconds << ',' << throughput << ',' << mib << ',' << entry.first << ','
                << entry.second << ',' << finish.first << ',' << finish.second << ','
                << observed.first << ',' << observed.second << ',' << cpu << '\n';
            csv.flush();
            require(csv.good(), "Failed writing CSV output.");
        }
    }
    LOG_INFO_STREAM << "All payloads verified; leaving main for Kitchen shutdown; "
        << "external process timing includes deferred retirement and shutdown.";
    return 0;
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Selects an executor outside all measured work.
 */
int main(int count, char** arguments) {
    try {
        const Options options = parse(count, arguments);
        if (options.executor == "kitchen") return run<ProductionAdapter>(options);
        if (options.executor == "blocking") return run<BlockingExecutor>(options);
        if (options.executor == "sleeping") return run<SleepingExecutor>(options);
        return run<SpinningExecutor>(options);
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Kitchen dispatch experiment failed: " << error.what();
        return 1;
    }
}
