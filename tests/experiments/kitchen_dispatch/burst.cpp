/** --------------------------------------------------------------------------------------------------------- Kitchen Burst Experiment
 * @file burst.cpp
 * @brief Measures completed single and bulk submissions without a shared per-task completion counter.
 */
#include "executors.hpp"
#include <logging.hpp>
#include <algorithm>
#include <atomic>
#include <barrier>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace {
using namespace kitchen_dispatch;
using Clock = std::chrono::steady_clock;
/** --------------------------------------------------------------------------------------------------------- Options
 * @brief Selects the executor and identical producer topology outside measured execution.
 */
struct Options {
    std::string executor = "kitchen";
    std::string csv;
    size_t tasks = 131072;
    size_t producers = 1;
    size_t batch = 1;
    size_t repetitions = 3;
    size_t warmup = 1;
};
/** --------------------------------------------------------------------------------------------------------- Parse
 * @brief Validates experiment options before creating worker teams.
 */
Options parse(int count, char** arguments) {
    Options options;
    for (int index = 1; index < count; ++index) {
        if (index + 1 == count) throw std::runtime_error("Missing option value");
        const std::string_view name(arguments[index]);
        const std::string_view value(arguments[++index]);
        if (name == "--executor") options.executor = value;
        else if (name == "--csv") options.csv = value;
        else {
            size_t number = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                throw std::runtime_error("Expected unsigned decimal option");
            if (name == "--tasks") options.tasks = number;
            else if (name == "--producers") options.producers = number;
            else if (name == "--batch") options.batch = number;
            else if (name == "--repetitions") options.repetitions = number;
            else if (name == "--warmup") options.warmup = number;
            else throw std::runtime_error("Unknown option");
        }
    }
    if (!options.tasks || !options.producers || options.producers > options.tasks
        || !options.batch || !options.repetitions || !std::thread::hardware_concurrency())
        throw std::runtime_error("Invalid experiment topology");
    if (options.executor != "kitchen" && options.executor != "blocking"
        && options.executor != "sleeping" && options.executor != "spinning")
        throw std::runtime_error("Unknown executor");
    return options;
}
/** --------------------------------------------------------------------------------------------------------- Record
 * @brief Publishes one independently identifiable completion on its own cache line.
 */
struct Record {
    std::atomic<unsigned> complete{0};
    size_t identifier = 0;
    size_t result = 0;
    /** ------------------------------------------------------------------------------------------- Run
     * @brief Writes the request identity before its final release publication.
     */
    static void run(void* context) {
        auto* record = static_cast<Record*>(context);
        record->result = record->identifier ^ size_t(0x6e8b472d);
        record->complete.store(1, std::memory_order_release);
    }
};
/** --------------------------------------------------------------------------------------------------------- Record Storage
 * @brief Separates completion records using the cache-line size reported by the running hardware.
 */
class RecordStorage {
private:
    size_t stride_ = 0;
    size_t count_;
    void* memory_;
public:
    explicit RecordStorage(size_t count) : count_(count) {
#if defined(__APPLE__)
        size_t bytes = sizeof(stride_);
        if (sysctlbyname("hw.cachelinesize", &stride_, &bytes, nullptr, 0))
            throw std::runtime_error("Cannot probe hardware cache-line size");
#elif defined(__linux__)
        const long reported = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
        if (reported <= 0) throw std::runtime_error("Cannot probe hardware cache-line size");
        stride_ = static_cast<size_t>(reported);
#else
#error "The cache-line experiment requires a macOS or Linux hardware probe"
#endif
        if (stride_ < sizeof(Record) || (stride_ & (stride_ - 1)))
            throw std::runtime_error("Unsupported reported cache-line geometry");
        memory_ = ::operator new(count_ * stride_, std::align_val_t(stride_));
        for (size_t index = 0; index < count_; ++index)
            new (static_cast<unsigned char*>(memory_) + index * stride_) Record;
        LOG_INFO_STREAM << "Completion record stride: " << stride_ << " hardware-probed bytes";
    }
    ~RecordStorage() {
        for (size_t index = 0; index < count_; ++index) (*this)[index].~Record();
        ::operator delete(memory_, std::align_val_t(stride_));
    }
    RecordStorage(const RecordStorage&) = delete;
    RecordStorage& operator=(const RecordStorage&) = delete;
    Record& operator[](size_t index) {
        return *reinterpret_cast<Record*>(static_cast<unsigned char*>(memory_) + index * stride_);
    }
};
/** --------------------------------------------------------------------------------------------------------- CPU Seconds
 * @brief Reads process CPU consumption outside the task submission loop.
 */
double cpu_seconds();
/** --------------------------------------------------------------------------------------------------------- Team
 * @brief Reuses producers and verifies every task through disjoint completion slots.
 */
template<typename Executor>
class Team {
private:
    const Options& options_;
    Executor& executor_;
    RecordStorage records_;
    std::vector<Order> tasks_;
    std::barrier<> gate_;
    std::vector<std::thread> producers_;
    bool stop_ = false;
    /** ------------------------------------------------------------------------------------------- Produce
     * @brief Submits a contiguous share with one private queue token per producer thread.
     */
    static void produce(Team* team, size_t producer_index) {
        typename Executor::Producer producer(team->executor_);
        const size_t first = team->options_.tasks * producer_index / team->options_.producers;
        const size_t last = team->options_.tasks * (producer_index + 1) / team->options_.producers;
        for (;;) {
            team->gate_.arrive_and_wait();
            if (team->stop_) return;
            if (team->options_.batch == 1) {
                for (size_t index = first; index < last; ++index)
                    producer.compute(std::move(team->tasks_[index]));
            } else {
                for (size_t index = first; index < last; index += team->options_.batch)
                    producer.compute_bulk(team->tasks_.data() + index,
                        std::min(team->options_.batch, last - index));
            }
            team->gate_.arrive_and_wait();
        }
    }
public:
    Team(const Options& options, Executor& executor)
        : options_(options), executor_(executor), records_(options.tasks),
          tasks_(options.tasks), gate_(options.producers + 1) {
        for (size_t index = 0; index < options.tasks; ++index) {
            records_[index].identifier = index;
        }
        for (size_t index = 0; index < options.producers; ++index)
            producers_.emplace_back(&Team::produce, this, index);
    }
    ~Team() {
        stop_ = true;
        gate_.arrive_and_wait();
        for (auto& producer : producers_) producer.join();
    }
    /** ------------------------------------------------------------------------------------------- Measure
     * @brief Includes dispatch and observation of every completion without a global task counter.
     */
    double measure(double& cpu) {
        for (size_t index = 0; index < options_.tasks; ++index) {
            records_[index].complete.store(0, std::memory_order_relaxed);
            tasks_[index] = Order{&Record::run, &records_[index]};
        }
        const double initial_cpu = cpu_seconds();
        const auto began = Clock::now();
        gate_.arrive_and_wait();
        for (size_t index = 0; index < options_.tasks; ++index) {
            while (!records_[index].complete.load(std::memory_order_acquire))
                std::this_thread::yield();
        }
        gate_.arrive_and_wait();
        const double seconds = std::chrono::duration<double>(Clock::now() - began).count();
        cpu = cpu_seconds() - initial_cpu;
        for (size_t index = 0; index < options_.tasks; ++index) {
            if (records_[index].result != (index ^ size_t(0x6e8b472d)))
                throw std::runtime_error("Incorrect task identity");
        }
        return seconds;
    }
};
/** --------------------------------------------------------------------------------------------------------- CPU Seconds
 * @brief Reads process CPU consumption outside the task submission loop.
 */
double cpu_seconds() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage)) throw std::runtime_error("Cannot read CPU usage");
    return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec
        + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
}
/** --------------------------------------------------------------------------------------------------------- Run
 * @brief Retains each optimized repetition and completes worker shutdown before process exit.
 */
template<typename Executor>
int run(const Options& options) {
    Executor executor(std::thread::hardware_concurrency());
    Team<Executor> team(options, executor);
    std::ofstream csv(options.csv);
    if (!csv) throw std::runtime_error("Cannot open CSV output");
    csv << "executor,producers,batch,tasks,sample,seconds,tasks_per_second,cpu_seconds\n";
    for (size_t round = 0; round < options.warmup + options.repetitions; ++round) {
        LOG_INFO_STREAM << "Burst " << options.executor << ", producers=" << options.producers
            << ", batch=" << options.batch << ", round=" << round;
        double cpu = 0;
        const double seconds = team.measure(cpu);
        if (round < options.warmup) continue;
        csv << options.executor << ',' << options.producers << ',' << options.batch << ','
            << options.tasks << ',' << round - options.warmup + 1 << ',' << std::setprecision(12)
            << seconds << ',' << options.tasks / seconds << ',' << cpu << '\n';
        csv.flush();
        if (!csv) throw std::runtime_error("Cannot write CSV output");
        LOG_INFO_STREAM << "Completed " << options.tasks / seconds << " tasks/s; CPU="
            << cpu << " seconds";
    }
    return 0;
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Selects the executor before constructing any measured workload.
 */
int main(int count, char** arguments) {
    try {
        const Options options = parse(count, arguments);
        if (options.executor == "kitchen") return run<ProductionAdapter>(options);
        if (options.executor == "blocking") return run<BlockingExecutor>(options);
        if (options.executor == "sleeping") return run<SleepingExecutor>(options);
        return run<SpinningExecutor>(options);
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Kitchen burst experiment failed: " << error.what();
        return 1;
    }
}
