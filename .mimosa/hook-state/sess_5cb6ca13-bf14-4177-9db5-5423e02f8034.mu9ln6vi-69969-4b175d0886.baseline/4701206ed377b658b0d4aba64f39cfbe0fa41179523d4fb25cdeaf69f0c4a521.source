/** --------------------------------------------------------------------------------------------------------- Slice Queue Benchmark
 * @file slice_queue_benchmark.cpp
 * @brief Compares SliceQueue ownership transfers with bounded mutex-protected deque transfers.
 */
#include "benchmark_support.hpp"
#include <array>
#include <atomic>
#include <deque>
#include <span>

namespace benchmarks {
/** --------------------------------------------------------------------------------------------------------- Deque Queue
 * @brief Gives std::deque the same blocking, draining, and ownership-transfer interface.
 */
template<bool Synchronized>
class DequeQueue {
private:
    std::deque<Slice> values_;
    size_t capacity_;
    bool closed_ = false;
    std::mutex mutex_;
    std::condition_variable readable_;
    std::condition_variable writable_;
    /** ------------------------------------------------------------------------------------------- Push
     * @brief Moves a batch while holding the deque's single container mutex.
     */
    void push(std::span<Slice> input) {
        while (!input.empty()) {
            std::unique_lock lock(mutex_, std::defer_lock);
            if constexpr (Synchronized) {
                lock.lock();
                while (values_.size() == capacity_) writable_.wait(lock);
            }
            const size_t count = std::min(input.size(), capacity_ - values_.size());
            for (size_t index = 0; index < count; ++index) {
                values_.push_back(std::move(input[index]));
            }
            input = input.subspan(count);
            if constexpr (Synchronized) {
                lock.unlock();
                readable_.notify_one();
            }
        }
    }
    /** ------------------------------------------------------------------------------------------- Pop
     * @brief Waits for work or closure and releases replaced claims outside the mutex.
     */
    size_t pop(std::span<Slice> output, std::span<Slice> received) {
        size_t count;
        {
            std::unique_lock lock(mutex_, std::defer_lock);
            if constexpr (Synchronized) {
                lock.lock();
                while (values_.empty() && !closed_) readable_.wait(lock);
            }
            count = std::min(output.size(), values_.size());
            for (size_t index = 0; index < count; ++index) {
                received[index] = std::move(values_.front());
                values_.pop_front();
            }
        }
        if constexpr (Synchronized) {
            if (count == 1) writable_.notify_one();
            else if (count > 1) writable_.notify_all();
        }
        for (size_t index = 0; index < count; ++index) output[index] = std::move(received[index]);
        return count;
    }
public:
    /** ------------------------------------------------------------------------------------------- Producer
     * @brief Provides statically dispatched single and bulk ownership transfers.
     */
    class Producer {
    private:
        DequeQueue& queue_;
    public:
        explicit Producer(DequeQueue& queue) : queue_(queue) {}
        void push(Slice&& value) { queue_.push(std::span<Slice>(&value, 1)); }
        void push(std::span<Slice> values) { queue_.push(values); }
    };
    /** ------------------------------------------------------------------------------------------- Consumer
     * @brief Provides blocking reads that report closure only after draining the deque.
     */
    class Consumer {
    private:
        DequeQueue& queue_;
        std::array<Slice, buffetalligator::SliceQueue::block_size> received_;
    public:
        explicit Consumer(DequeQueue& queue) : queue_(queue) {}
        bool pop(Slice& value) { return queue_.pop(std::span<Slice>(&value, 1), received_) != 0; }
        size_t pop(std::span<Slice> values) { return queue_.pop(values, received_); }
    };
    DequeQueue(size_t producers, size_t, size_t capacity) : capacity_(producers * capacity) {}
    Producer producer(size_t) { return Producer(*this); }
    Consumer consumer(size_t) { return Consumer(*this); }
    /** ------------------------------------------------------------------------------------------- Close
     * @brief Publishes producer completion to every sleeping consumer.
     */
    void close() {
        {
            std::unique_lock lock(mutex_, std::defer_lock);
            if constexpr (Synchronized) lock.lock();
            closed_ = true;
        }
        if constexpr (Synchronized) readable_.notify_all();
    }
    /** ------------------------------------------------------------------------------------------- Reset
     * @brief Reopens the drained queue between samples after every worker has stopped accessing it.
     */
    void reset() {
        require(values_.empty(), "Deque retained undelivered messages.");
        closed_ = false;
    }
};
/** --------------------------------------------------------------------------------------------------------- Receipts
 * @brief Records one consumer's identifiers without a shared per-message counter.
 */
struct alignas(128) Receipts {
    std::vector<uint64_t> identifiers;
};
/** --------------------------------------------------------------------------------------------------------- Queue Workload
 * @brief Transfers preallocated messages with identical worker loops for both implementations.
 */
template<typename Queue>
class QueueWorkload {
private:
    const Options& options_;
    Shape shape_;
    Queue queue_;
    std::vector<Slice> input_;
    std::vector<Receipts> receipts_;
    std::atomic<size_t> producers_remaining_{0};
    /** ------------------------------------------------------------------------------------------- Produce
     * @brief Flushes its bound producer before the final producer publishes closure.
     */
    void produce(size_t worker) {
        {
            auto producer = queue_.producer(worker);
            const size_t first = partition(options_.items, worker, shape_.producers);
            const size_t last = partition(options_.items, worker + 1, shape_.producers);
            if (options_.batch == 1) {
                for (size_t index = first; index < last; ++index) {
                    producer.push(std::move(input_[index]));
                }
            } else {
                for (size_t index = first; index < last; index += options_.batch) {
                    producer.push(std::span<Slice>(input_.data() + index,
                        std::min(options_.batch, last - index)));
                }
            }
        }
        if (producers_remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) queue_.close();
    }
    /** ------------------------------------------------------------------------------------------- Consume
     * @brief Records delivered identifiers locally while retaining ordinary Slice release costs.
     */
    void consume(size_t worker) {
        auto consumer = queue_.consumer(worker);
        auto& identifiers = receipts_[worker].identifiers;
        if (options_.batch == 1) {
            Slice received;
            while (consumer.pop(received)) identifiers.push_back(received.get_as<uint64_t>());
        } else {
            std::array<Slice, buffetalligator::SliceQueue::block_size> received;
            const std::span<Slice> output(received.data(), options_.batch);
            for (;;) {
                const size_t count = consumer.pop(output);
                if (count == 0) break;
                for (size_t index = 0; index < count; ++index) {
                    identifiers.push_back(received[index].get_as<uint64_t>());
                }
            }
        }
    }
public:
    QueueWorkload(const Options& options, Shape shape)
    : options_(options), shape_(shape), queue_(shape.producers, shape.consumers, options.capacity),
      receipts_(shape.consumers) {
        input_.reserve(options.items);
        for (auto& receipt : receipts_) receipt.identifiers.reserve(options.items);
    }
    /** ------------------------------------------------------------------------------------------- Prepare
     * @brief Resets drained storage and shares all payload claims before starting the timer.
     */
    void prepare(const std::vector<Slice>& payloads) {
        queue_.reset();
        input_ = payloads;
        for (auto& receipt : receipts_) receipt.identifiers.clear();
        producers_remaining_.store(shape_.producers, std::memory_order_relaxed);
    }
    /** ------------------------------------------------------------------------------------------- Run
     * @brief Executes one role, or a complete fill-and-drain cycle in the single-thread case.
     */
    void run(size_t worker) {
        if (shape_.serial) {
            produce(0);
            consume(0);
        } else if (worker < shape_.producers) {
            produce(worker);
        } else {
            consume(worker - shape_.producers);
        }
    }
    /** ------------------------------------------------------------------------------------------- Validate
     * @brief Checks exact-once delivery and moved-from sources after timing has stopped.
     */
    void validate() const {
        std::vector<unsigned char> seen(options_.items, 0);
        size_t delivered = 0;
        for (const auto& receipt : receipts_) {
            delivered += receipt.identifiers.size();
            for (const uint64_t identifier : receipt.identifiers) {
                require(identifier < options_.items, "Queue delivered an invalid identifier.");
                require(seen[identifier] == 0, "Queue duplicated a message.");
                seen[identifier] = 1;
            }
        }
        require(delivered == options_.items, "Queue lost a message.");
        for (const Slice& source : input_) require(source.is_null(), "Push retained its source.");
    }
};
/** --------------------------------------------------------------------------------------------------------- Compare Queues
 * @brief Alternates paired measurements while keeping both worker teams alive across samples.
 */
template<bool Synchronized>
void compare_queues(Options options, Shape shape, Reporter& reporter) {
    if (shape.serial) {
        constexpr size_t block = buffetalligator::SliceQueue::block_size;
        options.capacity = ((options.items + block - 1) / block) * block;
    }
    require(options.capacity <= SIZE_MAX / shape.producers, "Queue capacity overflow.");
    LOG_INFO_STREAM << "Queue " << shape.label() << ": " << options.items
        << " messages, batch=" << options.batch << ", capacity/producer=" << options.capacity;
    Progress progress("Queue " + shape.label(), options.timeout);
    const auto input = payloads(options.items);
    QueueWorkload<buffetalligator::SliceQueue> slices(options, shape);
    QueueWorkload<DequeQueue<Synchronized>> deque(options, shape);
    const size_t workers = shape.serial ? 1 : shape.producers + shape.consumers;
    Team slice_team(slices, workers);
    Team deque_team(deque, workers);
    std::vector<double> slice_samples, deque_samples;
    for (size_t sample = 0; sample < options.warmup + options.repetitions; ++sample) {
        LOG_INFO_STREAM << "Queue " << shape.label() << ' '
            << (sample < options.warmup ? "warmup " : "sample ")
            << (sample < options.warmup ? sample + 1 : sample - options.warmup + 1);
        double slice_seconds = 0, deque_seconds = 0;
        for (size_t order = 0; order < 2; ++order) {
            if ((sample + order) % 2 == 0) {
                slices.prepare(input);
                slice_seconds = slice_team.measure();
                slices.validate();
            } else {
                deque.prepare(input);
                deque_seconds = deque_team.measure();
                deque.validate();
            }
        }
        if (sample >= options.warmup) {
            slice_samples.push_back(slice_seconds);
            deque_samples.push_back(deque_seconds);
        }
    }
    const double slice_median = reporter.report("SliceQueue", "transfer", shape,
        options, options.items, slice_samples);
    const double deque_median = reporter.report(Synchronized ? "deque+mutex" : "deque",
        "transfer", shape, options, options.items, deque_samples);
    LOG_INFO_STREAM << "SliceQueue relative throughput: " << deque_median / slice_median
        << "x deque (greater than 1 means SliceQueue is faster).";
}
} // namespace benchmarks
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs deterministic single-thread and bounded producer/consumer queue comparisons.
 */
int main(int count, char** arguments) {
    using namespace benchmarks;
    try {
        const Options options = parse(count, arguments, true);
        if (options.help) { help(true); return 0; }
        Reporter reporter(options);
        LOG_INFO_STREAM << "SliceQueue uses block-level mutex/semaphore handoffs; "
            << "deque uses a container mutex and condition variables with the same total bound.";
        for (const Shape shape : shapes(options)) {
            if (shape.serial) compare_queues<false>(options, shape, reporter);
            else compare_queues<true>(options, shape, reporter);
        }
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Queue benchmark failed: " << error.what();
        return 1;
    }
}
