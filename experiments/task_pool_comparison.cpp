/** --------------------------------------------------------------------------------------------------------- Task Pool Comparison
 * @file task_pool_comparison.cpp
 * @brief Runs one set of workloads through the Kitchen, a mutex+condvar pool, GCD, and OpenMP so
 * the Kitchen's queue design is settled by measurement. Every approach runs the same 32-byte
 * Task records and completes through the same TaskCountdown unless its native idiom joins itself.
 */
#include <alligator/kitchen.hpp>
#include <logging.hpp>
#include <algorithm>
#include <barrier>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#if defined(__APPLE__) || defined(TASK_POOL_HAS_DISPATCH)
#include <dispatch/dispatch.h>
#define TASK_POOL_DISPATCH 1
#endif
#if defined(_OPENMP)
#include <omp.h>
#endif

namespace {
using buffetalligator::Kitchen;
using buffetalligator::Task;
using buffetalligator::TaskCountdown;
using Clock = std::chrono::steady_clock;
/** --------------------------------------------------------------------------------------------------------- Options
 * @struct Options
 * @brief Work sizes; thread counts are probed from the machine.
 */
struct Options {
    size_t tasks = 1u << 18;         ///< Tasks per throughput repetition.
    size_t repetitions = 5;          ///< Timed repetitions after one warmup.
    size_t batch = 256;              ///< Tasks per bulk submit in the fork-join workload.
    size_t latency_samples = 20000;  ///< Warm round trips per approach.
};
/** --------------------------------------------------------------------------------------------------------- Parse
 * @brief Reads `--tasks`, `--repetitions`, `--batch`, and `--latency-samples`.
 */
Options parse(int count, char** arguments) {
    Options options;
    for (int index = 1; index + 1 < count; index += 2) {
        const std::string_view name(arguments[index]);
        const std::string_view text(arguments[index + 1]);
        size_t value = 0;
        if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{} || value == 0) {
            throw std::runtime_error("Options take positive integers, e.g. --tasks 262144");
        }
        if (name == "--tasks") options.tasks = value;
        else if (name == "--repetitions") options.repetitions = value;
        else if (name == "--batch") options.batch = value;
        else if (name == "--latency-samples") options.latency_samples = value;
        else throw std::runtime_error("Unknown option; use --tasks, --repetitions, --batch, --latency-samples");
    }
    return options;
}
/** --------------------------------------------------------------------------------------------------------- Task Body
 * @brief The measured work: spins for the nanoseconds its context names, or returns at once for 0.
 * @param context A `const size_t` holding the spin length in nanoseconds.
 */
void task_body(void* context) {
    const size_t work_nanoseconds = *static_cast<const size_t*>(context);
    if (work_nanoseconds == 0) return;
    const Clock::time_point deadline = Clock::now() + std::chrono::nanoseconds(work_nanoseconds);
    while (Clock::now() < deadline) {}
}
/** --------------------------------------------------------------------------------------------------------- Run Record
 * @brief Runs one Task record the way every pool's worker does: run, then done when set.
 * @param record The Task.
 */
void run_record(const Task& record) {
    record.run(record.context);
    if (record.done != nullptr) record.done(record.done_context);
}
/** --------------------------------------------------------------------------------------------------------- Median
 * @brief The median of a sample set, reordering it in place.
 */
double median(std::vector<double>& samples) {
    std::nth_element(samples.begin(), samples.begin() + samples.size() / 2, samples.end());
    return samples[samples.size() / 2];
}
/** --------------------------------------------------------------------------------------------------------- Kitchen Approach
 * @struct KitchenApproach
 * @brief The Kitchen as it stands: moodycamel blocking queue, producer and consumer tokens.
 */
struct KitchenApproach {
    static constexpr const char* name = "Kitchen";
    static void submit(const Task* task) { Kitchen::inst().submit(*task); }
    static void fork_join(Task* records, size_t count, size_t batch, TaskCountdown* finished) {
        for (size_t first = 0; first < count; first += batch) {
            Kitchen::inst().submit_bulk(records + first, std::min(batch, count - first));
        }
        finished->wait();
    }
};
/** --------------------------------------------------------------------------------------------------------- Mutex Pool
 * @class MutexPool
 * @brief The baseline every queue has to beat: one deque, one mutex, one condvar, hardware-1 workers.
 */
class MutexPool {
private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Task> tasks_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
    /** ------------------------------------------------------------------------------------------- Work
     * @brief Pops under the lock, runs outside it, and exits once stopping with nothing queued.
     */
    void work() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            while (!stopping_ && tasks_.empty()) ready_.wait(lock);
            if (tasks_.empty()) return;
            const Task task = tasks_.front();
            tasks_.pop_front();
            lock.unlock();
            run_record(task);
            lock.lock();
        }
    }
public:
    explicit MutexPool(size_t workers) {
        workers_.reserve(workers);
        for (size_t index = 0; index < workers; ++index) workers_.emplace_back(&MutexPool::work, this);
    }
    ~MutexPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        for (std::thread& worker : workers_) worker.join();
    }
    void push(const Task& task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push_back(task);
        }
        ready_.notify_one();
    }
    void push_bulk(const Task* tasks, size_t count) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.insert(tasks_.end(), tasks, tasks + count);
        }
        ready_.notify_all();
    }
    static MutexPool& inst() {
        static MutexPool pool(std::max<size_t>(1u, std::thread::hardware_concurrency() - 1));
        return pool;
    }
};
/** --------------------------------------------------------------------------------------------------------- Mutex Approach
 * @struct MutexApproach
 * @brief Adapts MutexPool to the shared workloads.
 */
struct MutexApproach {
    static constexpr const char* name = "Mutex+condvar";
    static void submit(const Task* task) { MutexPool::inst().push(*task); }
    static void fork_join(Task* records, size_t count, size_t batch, TaskCountdown* finished) {
        for (size_t first = 0; first < count; first += batch) {
            MutexPool::inst().push_bulk(records + first, std::min(batch, count - first));
        }
        finished->wait();
    }
};
#if defined(TASK_POOL_DISPATCH)
/** --------------------------------------------------------------------------------------------------------- Dispatch Trampoline
 * @brief dispatch_async_f entry: the context is the caller's Task record, which outlives the dispatch.
 */
void dispatch_trampoline(void* record) { run_record(*static_cast<const Task*>(record)); }
/** --------------------------------------------------------------------------------------------------------- Apply Trampoline
 * @brief dispatch_apply_f entry: runs record `index` of the caller's Task array.
 */
void apply_trampoline(void* records, size_t index) { run_record(static_cast<const Task*>(records)[index]); }
/** --------------------------------------------------------------------------------------------------------- GCD Approach
 * @struct GcdApproach
 * @brief libdispatch's global high-priority concurrent queue: async_f per task, apply_f for a batch.
 */
struct GcdApproach {
    static constexpr const char* name = "GCD";
    static dispatch_queue_t queue() {
        static dispatch_queue_t global = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0);
        return global;
    }
    static void submit(const Task* task) {
        dispatch_async_f(queue(), const_cast<Task*>(task), &dispatch_trampoline);
    }
    static void fork_join(Task* records, size_t count, size_t, TaskCountdown*) {
        dispatch_apply_f(count, queue(), records, &apply_trampoline);
    }
};
#endif
#if defined(_OPENMP)
/** --------------------------------------------------------------------------------------------------------- OpenMP Approach
 * @struct OpenMpApproach
 * @brief OpenMP's static parallel-for; it has no way to accept tasks from an outside thread, so it
 * only runs the fork-join workload.
 */
struct OpenMpApproach {
    static constexpr const char* name = "OpenMP";
    static void fork_join(Task* records, size_t count, size_t, TaskCountdown*) {
        const long total = static_cast<long>(count);
#pragma omp parallel for schedule(static)
        for (long index = 0; index < total; ++index) run_record(records[index]);
    }
};
#endif
/** --------------------------------------------------------------------------------------------------------- Produce
 * @brief One producer thread's share of a single-submit repetition.
 */
template<typename Approach>
void produce(const Task* task, std::barrier<>* start, size_t count) {
    start->arrive_and_wait();
    for (size_t index = 0; index < count; ++index) Approach::submit(task);
}
/** --------------------------------------------------------------------------------------------------------- Single Submit
 * @brief Splits `tasks` single submits across `producers` threads; first submit to last completion.
 * @return Median nanoseconds per task.
 */
template<typename Approach>
double single_submit(const Options& options, size_t producers, const size_t* work) {
    std::vector<double> samples;
    for (size_t repetition = 0; repetition <= options.repetitions; ++repetition) {
        TaskCountdown finished(static_cast<uint32_t>(options.tasks));
        const Task task{&task_body, const_cast<size_t*>(work), &TaskCountdown::arrive, &finished};
        std::barrier start(static_cast<std::ptrdiff_t>(producers + 1));
        std::vector<std::thread> threads;
        for (size_t producer = 0; producer < producers; ++producer) {
            const size_t share = options.tasks * (producer + 1) / producers - options.tasks * producer / producers;
            threads.emplace_back(&produce<Approach>, &task, &start, share);
        }
        start.arrive_and_wait();
        const Clock::time_point began = Clock::now();
        finished.wait();
        const double elapsed = std::chrono::duration<double, std::nano>(Clock::now() - began).count();
        for (std::thread& thread : threads) thread.join();
        if (repetition > 0) samples.push_back(elapsed / static_cast<double>(options.tasks));
    }
    return median(samples);
}
/** --------------------------------------------------------------------------------------------------------- Fork Join
 * @brief One caller runs `tasks` records through the approach's native batch idiom and waits.
 * @return Median nanoseconds per task.
 */
template<typename Approach>
double fork_join(const Options& options, const size_t* work) {
    std::vector<double> samples;
    std::vector<Task> records(options.tasks);
    for (size_t repetition = 0; repetition <= options.repetitions; ++repetition) {
        TaskCountdown finished(static_cast<uint32_t>(options.tasks));
        std::fill(records.begin(), records.end(),
            Task{&task_body, const_cast<size_t*>(work), &TaskCountdown::arrive, &finished});
        const Clock::time_point began = Clock::now();
        Approach::fork_join(records.data(), records.size(), options.batch, &finished);
        const double elapsed = std::chrono::duration<double, std::nano>(Clock::now() - began).count();
        finished.wait();
        if (repetition > 0) samples.push_back(elapsed / static_cast<double>(options.tasks));
    }
    return median(samples);
}
/** --------------------------------------------------------------------------------------------------------- Latency
 * @brief Times single-task round trips, optionally letting the pool park before each one.
 * @return Median round trip in nanoseconds; `p99` receives the 99th percentile.
 */
template<typename Approach>
double latency(size_t samples, bool idle_first, const size_t* work, double& p99) {
    std::vector<double> round_trips;
    round_trips.reserve(samples);
    TaskCountdown finished(1);
    const Task task{&task_body, const_cast<size_t*>(work), &TaskCountdown::arrive, &finished};
    for (size_t sample = 0; sample < samples; ++sample) {
        if (idle_first) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        finished.rearm(1);
        const Clock::time_point began = Clock::now();
        Approach::submit(&task);
        finished.wait();
        round_trips.push_back(std::chrono::duration<double, std::nano>(Clock::now() - began).count());
    }
    std::sort(round_trips.begin(), round_trips.end());
    p99 = round_trips[std::min(round_trips.size() - 1, round_trips.size() * 99 / 100)];
    return round_trips[round_trips.size() / 2];
}
/** --------------------------------------------------------------------------------------------------------- Row
 * @struct Row
 * @brief One workload's result per approach, NaN-free: approaches that cannot run it print a dash.
 */
struct Row {
    std::string workload;
    std::vector<std::string> cells;
};
/** --------------------------------------------------------------------------------------------------------- Format
 * @brief Renders a nanosecond figure with one decimal place.
 */
std::string format(double nanoseconds) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", nanoseconds);
    return text;
}
/** --------------------------------------------------------------------------------------------------------- Measure Single
 * @brief Logs progress, runs one single-submit measurement, and returns its cell.
 */
template<typename Approach>
std::string measure_single(const Options& options, size_t producers, const size_t* work) {
    LOG_INFO_STREAM << "  " << Approach::name << ": single submits from " << producers << " producer(s)...";
    return format(single_submit<Approach>(options, producers, work));
}
/** --------------------------------------------------------------------------------------------------------- Measure Fork Join
 * @brief Logs progress, runs one fork-join measurement, and returns its cell.
 */
template<typename Approach>
std::string measure_fork_join(const Options& options, const size_t* work) {
    LOG_INFO_STREAM << "  " << Approach::name << ": fork-join batch...";
    return format(fork_join<Approach>(options, work));
}
/** --------------------------------------------------------------------------------------------------------- Measure Latency
 * @brief Logs progress, runs one latency measurement, and returns "median / p99".
 */
template<typename Approach>
std::string measure_latency(size_t samples, bool idle_first, const size_t* work) {
    LOG_INFO_STREAM << "  " << Approach::name << ": " << (idle_first ? "parked" : "warm") << " round trips...";
    double p99 = 0;
    const double middle = latency<Approach>(samples, idle_first, work, p99);
    return format(middle) + " / " + format(p99);
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs every workload at 0 ns and 1000 ns of work per task and prints one table per work size.
 */
int main(int count, char** arguments) {
    try {
        const Options options = parse(count, arguments);
        const size_t cores = std::max<size_t>(1u, std::thread::hardware_concurrency());
        std::vector<size_t> producer_counts{1, 2, cores};
        producer_counts.erase(std::unique(producer_counts.begin(), producer_counts.end()), producer_counts.end());
        std::vector<std::string> header{"Kitchen", "Mutex+condvar"};
#if defined(TASK_POOL_DISPATCH)
        header.push_back("GCD");
#endif
#if defined(_OPENMP)
        header.push_back("OpenMP");
#endif
        LOG_INFO_STREAM << "Task pool comparison: " << options.tasks << " tasks x " << options.repetitions
            << " repetitions on " << cores << " hardware threads";
        for (const size_t work : {size_t{0}, size_t{1000}}) {
            LOG_INFO_STREAM << "Workloads at " << work << " ns of work per task:";
            std::vector<Row> rows;
            for (const size_t producers : producer_counts) {
                Row row{"single submit, " + std::to_string(producers) + " producer(s), ns/task", {}};
                row.cells.push_back(measure_single<KitchenApproach>(options, producers, &work));
                row.cells.push_back(measure_single<MutexApproach>(options, producers, &work));
#if defined(TASK_POOL_DISPATCH)
                row.cells.push_back(measure_single<GcdApproach>(options, producers, &work));
#endif
#if defined(_OPENMP)
                row.cells.push_back("-");
#endif
                rows.push_back(row);
            }
            Row batch{"fork-join batch from one caller, ns/task", {}};
            batch.cells.push_back(measure_fork_join<KitchenApproach>(options, &work));
            batch.cells.push_back(measure_fork_join<MutexApproach>(options, &work));
#if defined(TASK_POOL_DISPATCH)
            batch.cells.push_back(measure_fork_join<GcdApproach>(options, &work));
#endif
#if defined(_OPENMP)
            batch.cells.push_back(measure_fork_join<OpenMpApproach>(options, &work));
#endif
            rows.push_back(batch);
            const size_t parked_samples = std::max<size_t>(1u, options.latency_samples / 100);
            for (const bool idle_first : {false, true}) {
                const size_t samples = idle_first ? parked_samples : options.latency_samples;
                Row row{std::string(idle_first ? "parked" : "warm") + " round trip, median / p99 ns", {}};
                row.cells.push_back(measure_latency<KitchenApproach>(samples, idle_first, &work));
                row.cells.push_back(measure_latency<MutexApproach>(samples, idle_first, &work));
#if defined(TASK_POOL_DISPATCH)
                row.cells.push_back(measure_latency<GcdApproach>(samples, idle_first, &work));
#endif
#if defined(_OPENMP)
                row.cells.push_back("-");
#endif
                rows.push_back(row);
            }
            std::string table = "\n| Workload (" + std::to_string(work) + " ns work) |";
            for (const std::string& column : header) table += " " + column + " |";
            table += "\n|---|";
            for (size_t column = 0; column < header.size(); ++column) table += "---|";
            for (const Row& row : rows) {
                table += "\n| " + row.workload + " |";
                for (const std::string& cell : row.cells) table += " " + cell + " |";
            }
            LOG_INFO_STREAM << table;
        }
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Task pool comparison failed: " << error.what();
        return 1;
    }
}
