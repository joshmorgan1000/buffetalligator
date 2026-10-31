/** --------------------------------------------------------------------------------------------------------- Refcount Lanes
 * @file refcount_lanes.cpp
 * @brief Measures whether one thread that owns plain counts keeps up with the reference-count drops many
 * producers send it over moodycamel queues, next to atomic and shared_ptr counting on the callers' threads.
 *
 * - A drop is a 4-byte message: a 24-bit entry index and a 5-bit weight exponent, as in weighted counting,
 *   where a copy splits its handle's weight locally and only the drop sends a message.
 * - Each consumer owns its entries outright and subtracts weights from plain uint32_t counts.
 * - Every run presets each entry to the weight it will receive, so a lost or doubled drop fails the run.
 * - A consumer keeps up when it finishes within 1 ms, or 2% of the producers' time, of the last enqueue.
 */
#include <logging.hpp>
#include <moodycamel/blockingconcurrentqueue.h>
#include <moodycamel/concurrentqueue.h>
#include <algorithm>
#include <atomic>
#include <barrier>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
constexpr uint32_t ENTRIES = 1u << 20;  ///< Counted entries, one per pool record.
constexpr uint32_t HOT_ENTRIES = 64;    ///< Entries every producer shares in the hot pattern.
constexpr size_t BULK = 256;            ///< Drops the bulk consumer takes per dequeue.
/** --------------------------------------------------------------------------------------------------------- Options
 * @struct Options
 * @brief Work sizes and the optional CSV path.
 */
struct Options {
    size_t messages = 1u << 20;  ///< Drops, or copy-and-drop pairs, each producer makes per run.
    size_t repetitions = 3;      ///< Runs per configuration; the median run is reported.
    std::string csv;             ///< Every run is written here when set.
};
/** --------------------------------------------------------------------------------------------------------- Drop
 * @struct Drop
 * @brief One message: the entry, and the exponent of the weight the dropped handle returns.
 */
struct Drop {
    uint32_t index : 24;    ///< Entry index.
    uint32_t exponent : 5;  ///< The returned weight is `1 << exponent`.
};
static_assert(sizeof(Drop) == 4, "a drop is one 32-bit word");
enum class Pattern { Spread, Hot };       ///< Spread hashes over every entry; hot shares 64 entries.
enum class Consumer { Single, Bulk };     ///< One drop per dequeue, or up to BULK.
enum class Producer { Token, Implicit };  ///< Enqueue with a ProducerToken, or without one.
enum class Idle { Spin, Sleep, Block };   ///< An empty queue: poll again, sleep 50 us, or wait on its semaphore.
enum class Baseline { SharedAtomic, PrivateAtomic, SharedPointer };  ///< Counting on the callers' threads.
/** --------------------------------------------------------------------------------------------------------- Config
 * @struct Config
 * @brief One lane configuration.
 */
struct Config {
    size_t producers;   ///< Producer threads.
    size_t lanes;       ///< Consumer threads, one or two; entry `index` belongs to lane `index & (lanes - 1)`.
    Pattern pattern;    ///< Which entries the drops land on.
    Consumer consumer;  ///< How each consumer dequeues.
    Producer producer;  ///< How each producer enqueues.
    Idle idle;          ///< What each consumer does when its queue is empty.
    size_t work;        ///< Xorshift steps each producer runs before every drop, standing in for its own work.
};
/** --------------------------------------------------------------------------------------------------------- Result
 * @struct Result
 * @brief One run's timings, measured from the first thread to leave the start barrier.
 */
struct Result {
    double produce_seconds = 0;  ///< To the last producer's last enqueue, or last count for a baseline.
    double consume_seconds = 0;  ///< To the last consumer's last drop.
    size_t peak_backlog = 0;     ///< Largest queue depth a consumer sampled.
};
/** --------------------------------------------------------------------------------------------------------- Lane
 * @struct Lane
 * @brief One consumer's queue and the entries it owns.
 * @tparam Queue The moodycamel queue type.
 */
template<typename Queue>
struct Lane {
    Queue queue;                   ///< Drops for this lane's entries.
    std::vector<uint32_t> counts;  ///< Outstanding weight per owned entry, touched by this lane alone.
    uint32_t shift = 0;            ///< Entry `index` sits at `counts[index >> shift]`.
    size_t expected = 0;           ///< Drops this lane will receive.
    size_t touched = 0;            ///< Owned entries that receive at least one drop.
    size_t zeros = 0;              ///< Owned entries that reached zero.
    size_t peak = 0;               ///< Largest queue depth sampled.
    Clock::time_point started;     ///< When the consumer left the start barrier.
    Clock::time_point done;        ///< When the last drop was applied.
};
/** --------------------------------------------------------------------------------------------------------- Padded Counter
 * @struct PaddedCounter
 * @brief An atomic count on its own cache lines.
 */
struct alignas(128) PaddedCounter {
    std::atomic<uint64_t> value{0};  ///< The count.
};
/** --------------------------------------------------------------------------------------------------------- Prefer Performance Cores
 * @brief Asks macOS to run the calling thread on performance cores; other systems keep their default.
 */
void prefer_performance_cores() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}
/** --------------------------------------------------------------------------------------------------------- Seconds
 * @brief Seconds between two clock readings.
 */
double seconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double>(end - begin).count();
}
/** --------------------------------------------------------------------------------------------------------- Entry For
 * @brief The entry a producer's drop lands on, the same sequence every run.
 * @param producer The producer.
 * @param sequence The drop's position in that producer's run.
 * @param pattern Spread or hot.
 * @return The entry index.
 */
uint32_t entry_for(size_t producer, size_t sequence, Pattern pattern) {
    if (pattern == Pattern::Hot) return static_cast<uint32_t>((sequence + producer) & (HOT_ENTRIES - 1));
    return static_cast<uint32_t>((producer * 0x9E3779B1ull + sequence * 2654435761ull) & (ENTRIES - 1));
}
/** --------------------------------------------------------------------------------------------------------- Exponent For
 * @brief The weight exponent a drop returns, cycling through weights 1 to 128.
 */
uint32_t exponent_for(size_t sequence) { return static_cast<uint32_t>(sequence & 7); }
/** --------------------------------------------------------------------------------------------------------- Churn
 * @brief Runs `work` dependent xorshift steps, the producer's own work between drops.
 * @param state The running state.
 * @param work The number of steps.
 * @return The new state.
 */
uint64_t churn(uint64_t state, size_t work) {
    for (size_t step = 0; step < work; ++step) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
    }
    return state;
}
/** --------------------------------------------------------------------------------------------------------- Take (polled)
 * @brief Takes up to `most` drops from a polled queue without waiting.
 */
size_t take(moodycamel::ConcurrentQueue<Drop>& queue, Drop* items, size_t most) {
    return queue.try_dequeue_bulk(items, most);
}
/** --------------------------------------------------------------------------------------------------------- Take (blocking)
 * @brief Takes up to `most` drops from a blocking queue, waiting at most a millisecond.
 */
size_t take(moodycamel::BlockingConcurrentQueue<Drop>& queue, Drop* items, size_t most) {
    return queue.wait_dequeue_bulk_timed(items, most, std::chrono::milliseconds(1));
}
/** --------------------------------------------------------------------------------------------------------- Consume
 * @brief Applies every drop meant for one lane, sampling the queue depth as it goes.
 * @tparam Queue The moodycamel queue type.
 * @param lane The lane.
 * @param most Drops per dequeue.
 * @param sleeping Whether an empty queue sends the consumer to sleep for 50 us.
 * @param start The barrier every thread leaves together.
 */
template<typename Queue>
void consume(Lane<Queue>* lane, size_t most, bool sleeping, std::barrier<>* start) {
    prefer_performance_cores();
    Drop items[BULK];
    size_t received = 0;
    size_t rounds = 0;
    start->arrive_and_wait();
    lane->started = Clock::now();
    while (received < lane->expected) {
        const size_t taken = take(lane->queue, items, most);
        if (taken == 0 && sleeping) std::this_thread::sleep_for(std::chrono::microseconds(50));
        for (size_t item = 0; item < taken; ++item) {
            uint32_t& count = lane->counts[items[item].index >> lane->shift];
            count -= 1u << items[item].exponent;
            lane->zeros += count == 0;
        }
        received += taken;
        if ((++rounds & 63) == 0) lane->peak = std::max(lane->peak, lane->queue.size_approx());
    }
    lane->done = Clock::now();
}
/** --------------------------------------------------------------------------------------------------------- Produce
 * @brief Sends one producer's drops to the lanes that own their entries.
 * @tparam Queue The moodycamel queue type.
 * @param lanes The lanes.
 * @param config The configuration.
 * @param producer This producer's number.
 * @param messages Drops to send.
 * @param start The barrier every thread leaves together.
 * @param started Where the time this producer left the barrier goes.
 * @param done Where the time of the last enqueue goes.
 * @param sink Keeps the churn from being optimized away.
 */
template<typename Queue>
void produce(
    std::vector<std::unique_ptr<Lane<Queue>>>* lanes,
    const Config* config,
    size_t producer,
    size_t messages,
    std::barrier<>* start,
    Clock::time_point* started,
    Clock::time_point* done,
    std::atomic<uint64_t>* sink
) {
    prefer_performance_cores();
    const bool token = config->producer == Producer::Token;
    std::vector<moodycamel::ProducerToken> tokens;
    if (token) for (std::unique_ptr<Lane<Queue>>& lane : *lanes) tokens.emplace_back(lane->queue);
    const uint32_t mask = static_cast<uint32_t>(lanes->size() - 1);
    uint64_t state = producer + 1;
    start->arrive_and_wait();
    *started = Clock::now();
    for (size_t sequence = 0; sequence < messages; ++sequence) {
        state = churn(state, config->work);
        const uint32_t index = entry_for(producer, sequence, config->pattern);
        const Drop drop{index, exponent_for(sequence)};
        Queue& queue = (*lanes)[index & mask]->queue;
        if (token) queue.enqueue(tokens[index & mask], drop);
        else queue.enqueue(drop);
    }
    *done = Clock::now();
    sink->fetch_add(state, std::memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Run Lanes
 * @brief Presets the entries, runs the producers and consumers once, and checks every entry came back to zero.
 * @tparam Queue The moodycamel queue type.
 * @param config The configuration.
 * @param messages Drops per producer.
 * @param sink Keeps the churn from being optimized away.
 * @return The run's timings.
 */
template<typename Queue>
Result run_lanes(const Config& config, size_t messages, std::atomic<uint64_t>& sink) {
    std::vector<std::unique_ptr<Lane<Queue>>> lanes;
    for (size_t lane = 0; lane < config.lanes; ++lane) {
        lanes.push_back(std::make_unique<Lane<Queue>>());
        lanes.back()->counts.assign(ENTRIES / config.lanes, 0);
        lanes.back()->shift = config.lanes == 2 ? 1 : 0;
    }
    const uint32_t mask = static_cast<uint32_t>(config.lanes - 1);
    for (size_t producer = 0; producer < config.producers; ++producer) {
        for (size_t sequence = 0; sequence < messages; ++sequence) {
            const uint32_t index = entry_for(producer, sequence, config.pattern);
            Lane<Queue>& lane = *lanes[index & mask];
            uint32_t& count = lane.counts[index >> lane.shift];
            lane.touched += count == 0;
            count += 1u << exponent_for(sequence);
            ++lane.expected;
        }
    }
    std::barrier<> start(static_cast<std::ptrdiff_t>(config.producers + config.lanes + 1));
    std::vector<Clock::time_point> started(config.producers);
    std::vector<Clock::time_point> done(config.producers);
    std::vector<std::thread> threads;
    const size_t most = config.consumer == Consumer::Bulk ? BULK : 1;
    const bool sleeping = config.idle == Idle::Sleep;
    for (std::unique_ptr<Lane<Queue>>& lane : lanes) {
        threads.emplace_back(&consume<Queue>, lane.get(), most, sleeping, &start);
    }
    for (size_t producer = 0; producer < config.producers; ++producer) {
        threads.emplace_back(&produce<Queue>, &lanes, &config, producer, messages, &start, &started[producer],
            &done[producer], &sink);
    }
    start.arrive_and_wait();
    for (std::thread& thread : threads) thread.join();
    Clock::time_point begin = *std::min_element(started.begin(), started.end());
    for (const std::unique_ptr<Lane<Queue>>& lane : lanes) begin = std::min(begin, lane->started);
    Result result;
    for (const std::unique_ptr<Lane<Queue>>& lane : lanes) {
        for (uint32_t count : lane->counts) {
            if (count != 0) throw std::runtime_error("A lane lost or doubled a drop");
        }
        if (lane->zeros != lane->touched) throw std::runtime_error("An entry reached zero other than once");
        result.consume_seconds = std::max(result.consume_seconds, seconds(begin, lane->done));
        result.peak_backlog = std::max(result.peak_backlog, lane->peak);
    }
    for (Clock::time_point finished : done) {
        result.produce_seconds = std::max(result.produce_seconds, seconds(begin, finished));
    }
    return result;
}
/** --------------------------------------------------------------------------------------------------------- Count On Caller
 * @brief One thread's copy-and-drop pairs counted on its own thread, the way the baselines count.
 * @param baseline Which count.
 * @param shared The one atomic every thread shares.
 * @param mine This thread's own atomic.
 * @param pointer This thread's copy of the one shared_ptr every thread shares.
 * @param work Xorshift steps before every pair.
 * @param messages Pairs to make.
 * @param start The barrier every thread leaves together.
 * @param started Where the time this thread left the barrier goes.
 * @param done Where the time of the last pair goes.
 * @param sink Keeps the churn from being optimized away.
 */
void count_on_caller(
    Baseline baseline,
    std::atomic<uint64_t>* shared,
    PaddedCounter* mine,
    std::shared_ptr<uint64_t> pointer,
    size_t work,
    size_t messages,
    std::barrier<>* start,
    Clock::time_point* started,
    Clock::time_point* done,
    std::atomic<uint64_t>* sink
) {
    prefer_performance_cores();
    uint64_t state = reinterpret_cast<uintptr_t>(mine) | 1;
    start->arrive_and_wait();
    *started = Clock::now();
    if (baseline == Baseline::SharedAtomic) {
        for (size_t pair = 0; pair < messages; ++pair) {
            state = churn(state, work);
            shared->fetch_add(1, std::memory_order_relaxed);
            shared->fetch_sub(1, std::memory_order_acq_rel);
        }
    } else if (baseline == Baseline::PrivateAtomic) {
        for (size_t pair = 0; pair < messages; ++pair) {
            state = churn(state, work);
            mine->value.fetch_add(1, std::memory_order_relaxed);
            mine->value.fetch_sub(1, std::memory_order_acq_rel);
        }
    } else {
        for (size_t pair = 0; pair < messages; ++pair) {
            state = churn(state, work);
            std::shared_ptr<uint64_t> copy = pointer;
            state += *copy;
        }
    }
    *done = Clock::now();
    sink->fetch_add(state, std::memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Run Baseline
 * @brief Runs one baseline across `threads` threads once.
 * @return The run's timings; only the produce time is set.
 */
Result run_baseline(Baseline baseline, size_t threads, size_t work, size_t messages, std::atomic<uint64_t>& sink) {
    std::atomic<uint64_t> shared{0};
    std::vector<PaddedCounter> mine(threads);
    std::shared_ptr<uint64_t> pointer = std::make_shared<uint64_t>(1);
    std::barrier<> start(static_cast<std::ptrdiff_t>(threads + 1));
    std::vector<Clock::time_point> started(threads);
    std::vector<Clock::time_point> done(threads);
    std::vector<std::thread> workers;
    for (size_t thread = 0; thread < threads; ++thread) {
        workers.emplace_back(&count_on_caller, baseline, &shared, &mine[thread], pointer, work, messages, &start,
            &started[thread], &done[thread], &sink);
    }
    start.arrive_and_wait();
    for (std::thread& worker : workers) worker.join();
    const Clock::time_point begin = *std::min_element(started.begin(), started.end());
    Result result;
    for (Clock::time_point finished : done) {
        result.produce_seconds = std::max(result.produce_seconds, seconds(begin, finished));
    }
    return result;
}
/** --------------------------------------------------------------------------------------------------------- Fixed
 * @brief Formats a number with a fixed count of decimals.
 */
std::string fixed(double value, int digits) {
    char text[48];
    const std::to_chars_result written = std::to_chars(text, text + sizeof(text), value, std::chars_format::fixed,
        digits);
    return std::string(text, written.ptr);
}
/** --------------------------------------------------------------------------------------------------------- Keeps Up
 * @brief Whether the consumers finished within 1 ms, or 2% of the producers' time, of the last enqueue.
 */
bool keeps_up(const Result& result) {
    return result.consume_seconds - result.produce_seconds <= std::max(0.001, 0.02 * result.produce_seconds);
}
/** --------------------------------------------------------------------------------------------------------- Slower Side Less
 * @brief Orders runs by the slower of their two sides.
 */
bool slower_side_less(const Result& left, const Result& right) {
    return std::max(left.consume_seconds, left.produce_seconds)
        < std::max(right.consume_seconds, right.produce_seconds);
}
/** --------------------------------------------------------------------------------------------------------- Median
 * @brief The middle run by the slower of the two sides.
 */
Result median(std::vector<Result> results) {
    std::sort(results.begin(), results.end(), &slower_side_less);
    return results[results.size() / 2];
}
/** --------------------------------------------------------------------------------------------------------- Idle Name
 * @brief The word for an idle style.
 */
std::string_view idle_name(Idle idle) {
    static constexpr std::string_view names[] = {"spin ", "sleep", "block"};
    return names[static_cast<int>(idle)];
}
/** --------------------------------------------------------------------------------------------------------- Describe
 * @brief One configuration in words.
 */
std::string describe(const Config& config) {
    std::string text = std::to_string(config.lanes) + (config.lanes == 1 ? " lane " : " lanes");
    text += config.consumer == Consumer::Bulk ? " | bulk  " : " | single";
    text += config.producer == Producer::Token ? " | token   " : " | implicit";
    text += " | " + std::string(idle_name(config.idle));
    text += config.pattern == Pattern::Spread ? " | spread" : " | hot   ";
    text += " | " + std::to_string(config.producers) + " producers | work " + std::to_string(config.work);
    return text;
}
/** --------------------------------------------------------------------------------------------------------- Run Config
 * @brief Runs one lane configuration `repetitions` times, logs the median, and writes every run to the CSV.
 * @return The median run.
 */
Result run_config(const Config& config, const Options& options, std::ofstream& csv, std::atomic<uint64_t>& sink) {
    std::vector<Result> results;
    for (size_t run = 0; run < options.repetitions; ++run) {
        const Result result = config.idle == Idle::Block
            ? run_lanes<moodycamel::BlockingConcurrentQueue<Drop>>(config, options.messages, sink)
            : run_lanes<moodycamel::ConcurrentQueue<Drop>>(config, options.messages, sink);
        if (csv.is_open()) {
            csv << "lanes," << config.lanes << ',' << (config.consumer == Consumer::Bulk ? "bulk" : "single") << ','
                << (config.producer == Producer::Token ? "token" : "implicit") << ',' << idle_name(config.idle)
                << ',' << (config.pattern == Pattern::Spread ? "spread" : "hot") << ',' << config.producers << ','
                << config.work << ',' << result.produce_seconds << ',' << result.consume_seconds << ','
                << result.peak_backlog << '\n';
        }
        results.push_back(result);
    }
    const Result result = median(results);
    const double drops = static_cast<double>(config.producers * options.messages);
    LOG_INFO_STREAM << describe(config) << " | callers " << fixed(drops / result.produce_seconds / 1e6, 1)
        << " M drops/s (" << fixed(result.produce_seconds * 1e9 / static_cast<double>(options.messages), 1)
        << " ns per drop per thread) | consumer " << fixed(drops / result.consume_seconds / 1e6, 1)
        << " M/s | lag " << fixed((result.consume_seconds - result.produce_seconds) * 1e3, 2)
        << " ms | peak backlog " << result.peak_backlog << (keeps_up(result) ? " | keeps up" : " | FALLS BEHIND");
    return result;
}
/** --------------------------------------------------------------------------------------------------------- Run Baseline Config
 * @brief Runs one baseline `repetitions` times and logs the median.
 */
void run_baseline_config(
    Baseline baseline,
    size_t threads,
    size_t work,
    const Options& options,
    std::ofstream& csv,
    std::atomic<uint64_t>& sink
) {
    static constexpr std::string_view names[] = {"shared atomic ", "private atomic", "shared_ptr    "};
    std::vector<Result> results;
    for (size_t run = 0; run < options.repetitions; ++run) {
        const Result result = run_baseline(baseline, threads, work, options.messages, sink);
        if (csv.is_open()) {
            csv << "caller," << names[static_cast<int>(baseline)] << ",,,," << threads << ',' << work << ','
                << result.produce_seconds << ",,\n";
        }
        results.push_back(result);
    }
    const Result result = median(results);
    const double pairs = static_cast<double>(threads * options.messages);
    LOG_INFO_STREAM << "caller | " << names[static_cast<int>(baseline)] << " | " << threads << " threads | work "
        << work << " | " << fixed(pairs / result.produce_seconds / 1e6, 1) << " M copy+drop pairs/s ("
        << fixed(result.produce_seconds * 1e9 / static_cast<double>(options.messages), 1)
        << " ns per pair per thread)";
}
/** --------------------------------------------------------------------------------------------------------- Parse
 * @brief Reads `--messages`, `--repetitions`, and `--csv`.
 */
Options parse(int count, char** arguments) {
    Options options;
    for (int index = 1; index + 1 < count; index += 2) {
        const std::string_view name(arguments[index]);
        const std::string_view text(arguments[index + 1]);
        if (name == "--csv") {
            options.csv = std::string(text);
            continue;
        }
        size_t value = 0;
        if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{} || value == 0) {
            throw std::runtime_error("Options take positive integers, e.g. --messages 1048576");
        }
        if (name == "--messages") options.messages = value;
        else if (name == "--repetitions") options.repetitions = value;
        else throw std::runtime_error("Unknown option " + std::string(name));
    }
    return options;
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Sweeps producers, lanes, dequeue, enqueue and idle styles, and producer work, then the caller baselines.
 */
int main(int count, char** arguments) {
    const Options options = parse(count, arguments);
    std::ofstream csv;
    if (!options.csv.empty()) {
        csv.open(options.csv);
        csv << "section,lanes_or_baseline,consumer,producer,idle,pattern,threads,work,produce_s,consume_s,"
            << "peak_backlog\n";
    }
    std::atomic<uint64_t> sink{0};
    const size_t cores = std::max<size_t>(std::thread::hardware_concurrency(), 4);
    const size_t most = cores - 2;
    std::vector<size_t> producers;
    for (size_t threads = 1; threads < most; threads <<= 1) producers.push_back(threads);
    producers.push_back(most);
    LOG_INFO_STREAM << "Refcount lanes: " << cores << " hardware threads, " << options.messages
        << " drops per producer, median of " << options.repetitions << " runs per line";
    LOG_INFO_STREAM << "Sweep 1 of 4: producers against one and two consumer lanes";
    size_t single_lane_spread = 0;
    size_t single_lane_hot = 0;
    for (Pattern pattern : {Pattern::Spread, Pattern::Hot}) {
        for (size_t lanes : {size_t{1}, size_t{2}}) {
            for (size_t threads : producers) {
                const Config config{threads, lanes, pattern, Consumer::Bulk, Producer::Token, Idle::Spin, 0};
                const bool kept = keeps_up(run_config(config, options, csv, sink));
                if (lanes == 1 && kept && pattern == Pattern::Spread) single_lane_spread = threads;
                if (lanes == 1 && kept && pattern == Pattern::Hot) single_lane_hot = threads;
            }
        }
    }
    LOG_INFO_STREAM << "Sweep 2 of 4: dequeue, enqueue, and idle styles at " << most << " producers, one lane";
    for (Idle idle : {Idle::Spin, Idle::Sleep, Idle::Block}) {
        for (Producer producer : {Producer::Token, Producer::Implicit}) {
            for (Consumer consumer : {Consumer::Bulk, Consumer::Single}) {
                run_config(Config{most, 1, Pattern::Spread, consumer, producer, idle, 0}, options, csv, sink);
            }
        }
    }
    LOG_INFO_STREAM << "Sweep 3 of 4: producer work between drops at " << most << " producers, one lane";
    for (Idle idle : {Idle::Spin, Idle::Sleep}) {
        for (size_t work : {size_t{16}, size_t{64}, size_t{256}}) {
            run_config(Config{most, 1, Pattern::Spread, Consumer::Bulk, Producer::Token, idle, work}, options, csv,
                sink);
        }
    }
    LOG_INFO_STREAM << "Sweep 4 of 4: counting on the callers' threads instead";
    for (Baseline baseline : {Baseline::SharedAtomic, Baseline::PrivateAtomic, Baseline::SharedPointer}) {
        for (size_t threads : {size_t{1}, most}) run_baseline_config(baseline, threads, 0, options, csv, sink);
    }
    LOG_INFO_STREAM << "One lane kept up through " << single_lane_spread << " producers on spread entries and "
        << single_lane_hot << " on hot entries (sink " << (sink.load() & 0xFF) << ")";
    return 0;
}
