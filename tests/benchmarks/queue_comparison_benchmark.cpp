/** --------------------------------------------------------------------------------------------------------- Queue Comparison Benchmark
 * @file queue_comparison_benchmark.cpp
 * @brief Runs the high-water-mark consumer contract on SliceQueue and on moodycamel: every push is
 * visible at once, consumers dispatch singles below the mark and bulk batches at or above it.
 * Producers alternate back-to-back bursts with spaced trickles, and each payload carries its push
 * time so the handlers record push-to-handler latency.
 */
#include "benchmark_support.hpp"
#include <moodycamel/blockingconcurrentqueue.h>
#include <atomic>
#include <memory>
#include <span>

namespace {
using benchmarks::Options;
using benchmarks::Shape;
using benchmarks::Slice;
using benchmarks::require;
using benchmarks::Clock;
using buffetalligator::SliceQueue;
/** --------------------------------------------------------------------------------------------------------- Now
 * @brief Steady-clock nanoseconds, the unit stamped into payloads.
 */
inline uint64_t now() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count());
}
/** --------------------------------------------------------------------------------------------------------- Pacing
 * @struct Pacing
 * @brief Spaces trickled pushes and stamps each payload with its push time.
 */
struct Pacing {
    size_t burst;
    size_t trickle;
    uint64_t gap_ns;
    uint64_t next = 0;
    /** ------------------------------------------------------------------------------------------- Before Push
     * @brief Waits out the trickle gap for this position, then stamps the payload.
     */
    void before_push(Slice& slice, size_t position) {
        uint64_t stamp = now();
        if (trickle && position % (burst + trickle) >= burst) {
            while (stamp < next) stamp = now();
            next = stamp + gap_ns;
        }
        static_cast<uint64_t*>(slice.raw())[1] = stamp;
    }
};
/** --------------------------------------------------------------------------------------------------------- Receipts
 * @struct Receipts
 * @brief One consumer's delivered identifiers and dispatch mix, on its own cache lines.
 */
struct alignas(128) Receipts {
    std::vector<uint64_t> identifiers;
    std::vector<uint64_t> latencies;  ///< Push-to-handler nanoseconds per item.
    size_t single_items = 0;
    size_t bulk_items = 0;
    size_t bulk_calls = 0;
    /** ------------------------------------------------------------------------------------------- Single
     * @brief The single handler both arms call.
     */
    static void single(void* context, Slice&& slice) {
        Receipts& receipts = *static_cast<Receipts*>(context);
        receipts.identifiers.push_back(slice.get_as<uint64_t>());
        receipts.latencies.push_back(now() - static_cast<const uint64_t*>(slice.raw())[1]);
        ++receipts.single_items;
        slice.free();
    }
    /** ------------------------------------------------------------------------------------------- Bulk
     * @brief The bulk handler both arms call.
     */
    static void bulk(void* context, std::span<Slice> slices) {
        Receipts& receipts = *static_cast<Receipts*>(context);
        const uint64_t arrived = now();
        for (Slice& slice : slices) {
            receipts.identifiers.push_back(slice.get_as<uint64_t>());
            receipts.latencies.push_back(arrived - static_cast<const uint64_t*>(slice.raw())[1]);
            slice.free();
        }
        receipts.bulk_items += slices.size();
        ++receipts.bulk_calls;
    }
};
/** --------------------------------------------------------------------------------------------------------- Alligator Arm
 * @brief SliceQueue with its built-in watermark dispatch.
 */
class AlligatorArm {
private:
    SliceQueue queue_;
    std::atomic<size_t> producers_remaining_{0};
    size_t producers_;
public:
    static constexpr const char* name = "slicequeue";
    AlligatorArm(const Shape& shape, const Options& options)
    : queue_(shape.producers, shape.consumers, options.high_water, options.max_batch,
        &Receipts::single, &Receipts::bulk), producers_(shape.producers) {}
    void prepare() {
        queue_.reset();
        producers_remaining_.store(producers_, std::memory_order_relaxed);
    }
    void produce(size_t index, std::span<Slice> input, Pacing pacing) {
        {
            auto producer = queue_.producer(index);
            for (size_t position = 0; position < input.size(); ++position) {
                pacing.before_push(input[position], position);
                producer.push(std::move(input[position]));
            }
        }
        if (producers_remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) queue_.close();
    }
    void consume(size_t index, Receipts& receipts) {
        auto consumer = queue_.consumer(index);
        consumer.run(&receipts);
    }
};
/** --------------------------------------------------------------------------------------------------------- Moody Arm
 * @brief Blocking moodycamel with tokens; consumers read size_approx against the mark each step.
 */
class MoodyArm {
private:
    moodycamel::BlockingConcurrentQueue<Slice> queue_;
    std::vector<moodycamel::ProducerToken> tokens_;
    std::atomic<size_t> producers_done_{0};
    size_t producers_;
    size_t high_water_;
    size_t max_batch_;
public:
    static constexpr const char* name = "moodycamel";
    MoodyArm(const Shape& shape, const Options& options)
    : queue_(options.items + 64 * shape.producers, shape.producers, 0), producers_(shape.producers),
      high_water_(options.high_water), max_batch_(options.max_batch) {
        tokens_.reserve(shape.producers);
        for (size_t index = 0; index < shape.producers; ++index) tokens_.emplace_back(queue_);
    }
    void prepare() { producers_done_.store(0, std::memory_order_relaxed); }
    void produce(size_t index, std::span<Slice> input, Pacing pacing) {
        for (size_t position = 0; position < input.size(); ++position) {
            pacing.before_push(input[position], position);
            if (!queue_.enqueue(tokens_[index], std::move(input[position]))) throw std::bad_alloc();
        }
        producers_done_.fetch_add(1, std::memory_order_release);
    }
    void consume(size_t, Receipts& receipts) {
        moodycamel::ConsumerToken token(queue_);
        std::unique_ptr<Slice[]> batch(new Slice[max_batch_]);
        Slice item;
        for (;;) {
            if (queue_.size_approx() >= high_water_) {
                const size_t count = queue_.try_dequeue_bulk(token, batch.get(), max_batch_);
                if (count) {
                    Receipts::bulk(&receipts, std::span<Slice>(batch.get(), count));
                    continue;
                }
            }
            if (queue_.wait_dequeue_timed(token, item, 200)) {
                Receipts::single(&receipts, std::move(item));
                continue;
            }
            if (producers_done_.load(std::memory_order_acquire) == producers_
                && !queue_.try_dequeue(token, item)) return;
            if (!item.is_null()) Receipts::single(&receipts, std::move(item));
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Tuned Moody Arm
 * @brief Plain moodycamel with no per-push semaphore, depth rescanned every quarter mark like
 * SliceQueue, and consumers that yield rather than sleep when idle.
 */
class TunedMoodyArm {
private:
    moodycamel::ConcurrentQueue<Slice> queue_;
    std::vector<moodycamel::ProducerToken> tokens_;
    std::atomic<size_t> producers_done_{0};
    size_t producers_;
    size_t high_water_;
    size_t max_batch_;
    size_t refresh_;
public:
    static constexpr const char* name = "moodycamel-tuned";
    TunedMoodyArm(const Shape& shape, const Options& options)
    : queue_(options.items + 64 * shape.producers, shape.producers, 0), producers_(shape.producers),
      high_water_(options.high_water), max_batch_(options.max_batch),
      refresh_(std::max<size_t>(1, options.high_water / 4)) {
        tokens_.reserve(shape.producers);
        for (size_t index = 0; index < shape.producers; ++index) tokens_.emplace_back(queue_);
    }
    void prepare() { producers_done_.store(0, std::memory_order_relaxed); }
    void produce(size_t index, std::span<Slice> input, Pacing pacing) {
        for (size_t position = 0; position < input.size(); ++position) {
            pacing.before_push(input[position], position);
            if (!queue_.enqueue(tokens_[index], std::move(input[position]))) throw std::bad_alloc();
        }
        producers_done_.fetch_add(1, std::memory_order_release);
    }
    void consume(size_t, Receipts& receipts) {
        moodycamel::ConsumerToken token(queue_);
        std::unique_ptr<Slice[]> batch(new Slice[max_batch_]);
        for (;;) {
            const bool done = producers_done_.load(std::memory_order_acquire) == producers_;
            const size_t depth = queue_.size_approx();
            if (depth >= high_water_) {
                const size_t count = queue_.try_dequeue_bulk(token, batch.get(), max_batch_);
                if (count) Receipts::bulk(&receipts, std::span<Slice>(batch.get(), count));
                continue;
            }
            size_t dispatched = 0;
            Slice item;
            while (dispatched < refresh_ && queue_.try_dequeue(token, item)) {
                Receipts::single(&receipts, std::move(item));
                ++dispatched;
            }
            if (dispatched) continue;
            if (done && queue_.size_approx() == 0 && !queue_.try_dequeue(token, item)) return;
            if (!item.is_null()) Receipts::single(&receipts, std::move(item));
            std::this_thread::yield();
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Workload
 * @brief Splits preallocated payloads across producers and checks exact-once delivery each sample.
 */
template<typename Arm>
class Workload {
private:
    const Options& options_;
    Shape shape_;
    Arm arm_;
    std::vector<Slice> input_;
    std::vector<Receipts> receipts_;
public:
    Workload(const Options& options, Shape shape)
    : options_(options), shape_(shape), arm_(shape, options), receipts_(shape.consumers) {
        input_.reserve(options.items);
        for (auto& receipt : receipts_) {
            receipt.identifiers.reserve(options.items);
            receipt.latencies.reserve(options.items);
        }
    }
    void prepare(const std::vector<Slice>& payloads) {
        arm_.prepare();
        input_ = payloads;
        for (auto& receipt : receipts_) {
            receipt.identifiers.clear();
            receipt.latencies.clear();
            receipt.single_items = receipt.bulk_items = receipt.bulk_calls = 0;
        }
    }
    void run(size_t worker) {
        if (worker >= shape_.producers) {
            arm_.consume(worker - shape_.producers, receipts_[worker - shape_.producers]);
            return;
        }
        const size_t first = benchmarks::partition(options_.items, worker, shape_.producers);
        const size_t last = benchmarks::partition(options_.items, worker + 1, shape_.producers);
        arm_.produce(worker, std::span<Slice>(input_.data() + first, last - first),
            Pacing{options_.burst, options_.trickle, options_.gap_ns});
    }
    void validate(size_t& single_items, size_t& bulk_items, size_t& bulk_calls,
        std::vector<uint64_t>& latencies) const {
        std::vector<unsigned char> seen(options_.items, 0);
        size_t delivered = 0;
        for (const auto& receipt : receipts_) {
            delivered += receipt.identifiers.size();
            single_items += receipt.single_items;
            bulk_items += receipt.bulk_items;
            bulk_calls += receipt.bulk_calls;
            latencies.insert(latencies.end(), receipt.latencies.begin(), receipt.latencies.end());
            for (const uint64_t identifier : receipt.identifiers) {
                require(identifier < options_.items && seen[identifier] == 0,
                    "Queue lost or duplicated.");
                seen[identifier] = 1;
            }
        }
        require(delivered == options_.items, "Queue lost a message.");
    }
};
/** --------------------------------------------------------------------------------------------------------- Percentile
 * @brief Reads one fraction of sorted nanosecond latencies, in microseconds.
 */
double percentile(const std::vector<uint64_t>& sorted, double fraction) {
    const size_t index = std::min(sorted.size() - 1, static_cast<size_t>(fraction * double(sorted.size())));
    return double(sorted[index]) / 1000.0;
}
/** --------------------------------------------------------------------------------------------------------- Measure Arm
 * @brief Runs warmups and samples for one arm and reports the median and the dispatch mix.
 */
template<typename Arm>
void measure_arm(const Options& options, const Shape& shape, const std::vector<Slice>& payloads,
    benchmarks::Reporter& reporter) {
    Workload<Arm> workload(options, shape);
    benchmarks::Team<Workload<Arm>> team(workload, shape.producers + shape.consumers);
    std::vector<double> samples;
    size_t single_items = 0, bulk_items = 0, bulk_calls = 0;
    std::vector<uint64_t> latencies;
    std::vector<uint64_t> discarded;
    for (size_t sample = 0; sample < options.warmup + options.repetitions; ++sample) {
        workload.prepare(payloads);
        const double seconds = team.measure();
        size_t singles = 0, bulks = 0, calls = 0;
        discarded.clear();
        workload.validate(singles, bulks, calls, sample < options.warmup ? discarded : latencies);
        if (sample < options.warmup) continue;
        samples.push_back(seconds);
        single_items += singles;
        bulk_items += bulks;
        bulk_calls += calls;
    }
    reporter.report(Arm::name, "watermark", shape, options, options.items, samples);
    LOG_INFO_STREAM << Arm::name << ' ' << shape.label() << ": " << 100.0 * bulk_items
        / double(single_items + bulk_items) << "% of items in bulk calls averaging "
        << (bulk_calls ? double(bulk_items) / double(bulk_calls) : 0.0) << " items";
    std::sort(latencies.begin(), latencies.end());
    LOG_INFO_STREAM << Arm::name << ' ' << shape.label() << ": latency us p50="
        << percentile(latencies, 0.5) << " p99=" << percentile(latencies, 0.99) << " p99.9="
        << percentile(latencies, 0.999) << " max="
        << double(latencies.back()) / 1000.0;
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Compares both arms on each requested producer/consumer shape.
 */
int main(int count, char** arguments) {
    try {
        const Options options = benchmarks::parse(count, arguments, true);
        if (options.help) {
            benchmarks::help(true);
            return 0;
        }
        require(options.producers && options.consumers && options.consumers <= 2,
            "Specify --producers and --consumers (1 or 2).");
        const Shape shape{options.producers, options.consumers};
        const auto payloads = benchmarks::payloads(options.items);
        benchmarks::Reporter reporter(options);
        LOG_INFO_STREAM << "High-water mark " << options.high_water << ", max batch "
            << options.max_batch << ", bursts of " << options.burst << ", trickles of "
            << options.trickle << " spaced " << options.gap_ns << " ns";
        benchmarks::Progress progress("Queue comparison " + shape.label(), options.timeout);
        measure_arm<AlligatorArm>(options, shape, payloads, reporter);
        measure_arm<MoodyArm>(options, shape, payloads, reporter);
        measure_arm<TunedMoodyArm>(options, shape, payloads, reporter);
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Queue comparison failed: " << error.what();
        return 1;
    }
}
