/** --------------------------------------------------------------------------------------------------------- Queue Benchmark
 * @file bench_queue.cpp
 * @brief Compares Slice handoffs through C TLS blocks, moodycamel, and sequence-keyed SliceMap rows.
 */
#include "../test_support.hpp"
extern "C" {
#include "core/ba_queue.h"
}
#include <concurrentqueue.h>
#include <blockingconcurrentqueue.h>
#include <alligator.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t lane_capacity = 4096;
constexpr size_t maximum_batch = BA_QUEUE_BATCH_SIZE;
constexpr uint64_t checksum_salt = 0x59a73bc48fe206d1ull;
static_assert(sizeof(ba_slice_t) == 16);
/** --------------------------------------------------------------------------------------------------------- Nanoseconds
 * @brief Returns a steady timestamp for optional delivery sampling.
 */
uint64_t nanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
}
/** --------------------------------------------------------------------------------------------------------- Payload
 * @brief Carries a unique ID and producer-written visibility and latency probes.
 */
struct Payload {
    uint64_t identifier;
    uint64_t generation;
    uint64_t sent;
};
/** --------------------------------------------------------------------------------------------------------- Fixture
 * @brief Holds real Slice views whose ownership returns to the driver after every complete drain.
 */
struct Fixture {
    ba_slice_t backing{};
    std::vector<ba_slice_t> input;
    size_t per_producer;
    Fixture(size_t producers, size_t items) : input(producers * items), per_producer(items) {
        TEST_EQUAL(ba_claim(0, input.size() * sizeof(Payload), BA_CLAIM_NOVEL, &backing), BA_OK,
            "payload backing allocation failed");
        auto* payloads = static_cast<Payload*>(backing.ptr);
        for (size_t index = 0; index < input.size(); ++index) {
            std::construct_at(payloads + index, Payload{index + 1, 0, 0});
            TEST_EQUAL(ba_view(&backing, index * sizeof(Payload), sizeof(Payload), &input[index]), BA_OK,
                "payload view creation failed");
        }
    }
    ~Fixture() {
        for (auto& slice : input) ba_release(&slice);
        ba_release(&backing);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
};
/** --------------------------------------------------------------------------------------------------------- C Queue
 * @brief Adapts the separately compiled C prototype without adding synchronization.
 */
struct CQueue {
    struct Local {
        ba_queue_local_t* state;
        explicit Local(ba_queue_local_t* local) : state(local) { TEST_REQUIRE(state != nullptr, "queue bind failed"); }
        Local(const Local&) = delete;
        Local& operator=(const Local&) = delete;
        ~Local() { ba_queue_unbind(state); }
        operator ba_queue_local_t*() const { return state; }
    };
    ba_queue_t* queue;
    CQueue(size_t producers, size_t readers, size_t capacity)
    : queue(ba_queue_create(producers, readers, capacity)) {
        TEST_REQUIRE(queue != nullptr, "C queue creation failed");
    }
    ~CQueue() { ba_queue_destroy(queue); }
    Local bind_producer(size_t producer) { return Local(ba_queue_bind_producer(queue, producer)); }
    Local bind_consumer(size_t consumer) { return Local(ba_queue_bind_consumer(queue, consumer)); }
    void start(ba_queue_local_t* local) { ba_queue_start(local); }
    void flush(ba_queue_local_t* local) { ba_queue_flush(local); }
    void close() { ba_queue_close(queue); }
    void reset() { ba_queue_reset(queue); }
    size_t push(ba_queue_local_t* local, const ba_slice_t* input, size_t count) {
        return ba_queue_push(local, input, count);
    }
    size_t pop(ba_queue_local_t* local, ba_slice_t* output, size_t count) {
        return ba_queue_pop(local, output, count);
    }
};
/** --------------------------------------------------------------------------------------------------------- Moody Traits
 * @brief Bounds each subqueue and provisions its index before timing begins.
 */
struct MoodyTraits : moodycamel::ConcurrentQueueDefaultTraits {
    static constexpr size_t MAX_SUBQUEUE_SIZE = lane_capacity;
    static constexpr size_t EXPLICIT_INITIAL_INDEX_SIZE = 256;
    inline static std::atomic<size_t> allocations{0};
    static void* malloc(size_t bytes) {
        allocations.fetch_add(1, std::memory_order_relaxed);
        return std::malloc(bytes);
    }
    static void free(void* pointer) { std::free(pointer); }
};
/** --------------------------------------------------------------------------------------------------------- Moody Queue
 * @brief Uses explicit tokens and naturally inlined nonallocating single or bulk APIs.
 */
struct MoodyQueue {
    struct alignas(128) Consumer : moodycamel::ConsumerToken {
        explicit Consumer(moodycamel::ConcurrentQueue<ba_slice_t, MoodyTraits>& owner)
        : moodycamel::ConsumerToken(owner) {}
    };
    moodycamel::ConcurrentQueue<ba_slice_t, MoodyTraits> queue;
    std::vector<std::unique_ptr<moodycamel::ProducerToken>> producers;
    std::vector<std::unique_ptr<Consumer>> consumers;
    MoodyQueue(size_t writers, size_t readers, size_t)
    : queue(writers * (lane_capacity + 2 * MoodyTraits::BLOCK_SIZE)) {
        for (size_t writer = 0; writer < writers; ++writer) {
            producers.emplace_back(std::make_unique<moodycamel::ProducerToken>(queue));
            TEST_REQUIRE(producers.back()->valid(), "moodycamel producer token allocation failed");
        }
        for (size_t reader = 0; reader < readers; ++reader) {
            consumers.emplace_back(std::make_unique<Consumer>(queue));
        }
        std::vector<ba_slice_t> warm(lane_capacity);
        for (const auto& producer : producers) {
            TEST_REQUIRE(queue.try_enqueue_bulk(*producer, warm.data(), warm.size()), "moodycamel prefill failed");
            TEST_EQUAL(queue.try_dequeue_bulk_from_producer(*producer, warm.data(), warm.size()), warm.size(),
                "moodycamel prefill drain failed");
        }
    }
    size_t bind_producer(size_t producer) { return producer; }
    size_t bind_consumer(size_t consumer) { return consumer; }
    void start(size_t) {}
    void flush(size_t) {}
    void close() {}
    void reset() {}
    size_t push(size_t producer, const ba_slice_t* input, size_t count) {
        const bool success = count == 1 ? queue.try_enqueue(*producers[producer], *input)
            : queue.try_enqueue_bulk(*producers[producer], input, count);
        return success ? count : 0;
    }
    size_t pop(size_t reader, ba_slice_t* output, size_t count) {
        return count == 1 ? size_t(queue.try_dequeue(*consumers[reader], *output))
            : queue.try_dequeue_bulk(*consumers[reader], output, count);
    }
};
/** --------------------------------------------------------------------------------------------------------- Buffered Moody Queue
 * @brief Gives moodycamel the same local block staging and semaphore-based consumer waits.
 */
struct BufferedMoodyQueue {
    using Queue = moodycamel::BlockingConcurrentQueue<ba_slice_t, MoodyTraits>;
    struct Local {
        size_t index;
        size_t count;
        size_t offset;
        std::array<ba_slice_t, BA_QUEUE_BATCH_SIZE> values;
    };
    inline static thread_local Local producer_local;
    inline static thread_local Local consumer_local;
    Queue queue;
    std::vector<std::unique_ptr<moodycamel::ProducerToken>> producers;
    std::vector<std::unique_ptr<moodycamel::ConsumerToken>> consumers;
    BufferedMoodyQueue(size_t writers, size_t readers, size_t)
    : queue(writers * (lane_capacity + 2 * MoodyTraits::BLOCK_SIZE)) {
        for (size_t writer = 0; writer < writers; ++writer) {
            producers.emplace_back(std::make_unique<moodycamel::ProducerToken>(queue));
            TEST_REQUIRE(producers.back()->valid(), "buffered moodycamel producer token failed");
        }
        for (size_t reader = 0; reader < readers; ++reader) {
            consumers.emplace_back(std::make_unique<moodycamel::ConsumerToken>(queue));
        }
        std::vector<ba_slice_t> warm(lane_capacity);
        for (const auto& producer : producers) {
            TEST_REQUIRE(queue.try_enqueue_bulk(*producer, warm.data(), warm.size()), "buffered prefill failed");
            TEST_EQUAL(queue.try_dequeue_bulk(warm.data(), warm.size()), warm.size(), "buffered prefill drain failed");
        }
    }
    Local* bind_producer(size_t producer) {
        producer_local.index = producer;
        return &producer_local;
    }
    Local* bind_consumer(size_t consumer) {
        consumer_local.index = consumer;
        return &consumer_local;
    }
    void start(Local* local) { local->count = local->offset = 0; }
    void reset() {}
    void close() {}
    void flush(Local* local) {
        if (!local->count) return;
        if (!queue.try_enqueue_bulk(*producers[local->index], local->values.data(), local->count)) {
            const auto deadline = Clock::now() + std::chrono::seconds(60);
            do {
                std::this_thread::yield();
                TEST_REQUIRE(Clock::now() < deadline, "buffered moodycamel enqueue stalled");
            } while (!queue.try_enqueue_bulk(*producers[local->index], local->values.data(), local->count));
        }
        local->count = 0;
    }
    size_t push(Local* local, const ba_slice_t* input, size_t count) {
        size_t written = 0;
        while (written < count) {
            const size_t amount = std::min(size_t(BA_QUEUE_BATCH_SIZE) - local->count, count - written);
            if (amount == 1) local->values[local->count] = input[written];
            else std::memcpy(local->values.data() + local->count, input + written, amount * sizeof(ba_slice_t));
            local->count += amount;
            written += amount;
            if (local->count == BA_QUEUE_BATCH_SIZE) flush(local);
        }
        return written;
    }
    size_t pop(Local* local, ba_slice_t* output, size_t count) {
        if (local->offset == local->count) {
            local->count = queue.wait_dequeue_bulk_timed(*consumers[local->index],
                local->values.data(), BA_QUEUE_BATCH_SIZE, std::int64_t(50));
            local->offset = 0;
            if (!local->count) return 0;
        }
        const size_t amount = std::min(count, local->count - local->offset);
        if (amount == 1) output[0] = local->values[local->offset];
        else std::memcpy(output, local->values.data() + local->offset, amount * sizeof(ba_slice_t));
        local->offset += amount;
        return amount;
    }
};
/** --------------------------------------------------------------------------------------------------------- SliceMap Queue
 * @brief Uses independent atomic producer and consumer sequences as existing SliceMap row IDs.
 */
template<bool indexed>
struct SliceMapAdapter {
    struct Local {
        size_t pending = SIZE_MAX;
        bool exhausted = false;
    };
    Fixture& fixture;
    buffetalligator::SliceMap map;
    std::vector<buffetalligator::Slice> staged;
    std::vector<size_t> expected_orders;
    alignas(128) std::atomic<size_t> producer_order{0};
    alignas(128) std::atomic<size_t> consumer_order{0};
    SliceMapAdapter(Fixture& input, bool verify)
    : fixture(input), map(input.input.size()), staged(input.input.size()),
      expected_orders(verify ? input.input.size() : 0) {}
    Local bind_producer(size_t) { return {}; }
    Local bind_consumer(size_t) { return {}; }
    void start(Local& local) { local = {}; }
    void flush(Local&) {}
    void close() {}
    /** --------------------------------------------------------------------------------------------- Reset
     * @brief Reclaims the previous drain and stages owned Slice views before workers resume.
     */
    void reset() {
        map.reset();
        producer_order.store(0, std::memory_order_relaxed);
        consumer_order.store(0, std::memory_order_relaxed);
        for (size_t index = 0; index < staged.size(); ++index) {
            TEST_REQUIRE(!staged[index], "SliceMap did not publish every staged input");
            TEST_EQUAL(ba_view(&fixture.input[index], 0, sizeof(Payload),
                reinterpret_cast<ba_slice_t*>(&staged[index])), BA_OK, "SliceMap staging failed");
        }
    }
    /** --------------------------------------------------------------------------------------------- Push
     * @brief Reserves one sequence per message and publishes it through the existing add_slice API.
     */
    size_t push(Local&, const ba_slice_t* input, size_t count) {
        for (size_t index = 0; index < count; ++index) {
            const auto* payload = static_cast<const Payload*>(input[index].ptr);
            const size_t source = payload->identifier - 1;
            const size_t order = producer_order.fetch_add(1, std::memory_order_relaxed);
            if (!expected_orders.empty()) {
                expected_orders[source] = order;
                if ((order & 31) == 0) std::this_thread::yield();
            }
            map.add_slice(static_cast<int64_t>(order), std::move(staged[source]));
        }
        return count;
    }
    /** --------------------------------------------------------------------------------------------- Pop
     * @brief Keeps an unpublished consumer ticket pending until its matching row can be copied out.
     */
    size_t pop(Local& local, ba_slice_t* output, size_t count) {
        size_t delivered = 0;
        while (delivered < count && !local.exhausted) {
            if (local.pending == SIZE_MAX) {
                local.pending = consumer_order.fetch_add(1, std::memory_order_relaxed);
                if (local.pending >= staged.size()) {
                    local.exhausted = true;
                    break;
                }
            }
            auto slice = [&] {
                if constexpr (indexed) {
                    if (!map.published(local.pending)) return buffetalligator::Slice{};
                    return map.slice_at(local.pending);
                } else {
                    return map.get_slice(static_cast<int64_t>(local.pending));
                }
            }();
            if (!slice) break;
            if (!expected_orders.empty()) {
                const auto* payload = slice.template data<Payload>();
                TEST_REQUIRE(payload->identifier && payload->identifier <= expected_orders.size(),
                    "SliceMap returned an invalid payload ID");
                const size_t order = indexed ? map.id<size_t>(local.pending) : local.pending;
                TEST_EQUAL(expected_orders[payload->identifier - 1], order,
                    "SliceMap delivered the wrong message sequence");
            }
            std::memcpy(output + delivered, &slice, sizeof(ba_slice_t));
            ++delivered;
            local.pending = SIZE_MAX;
        }
        return delivered;
    }
};
using SliceMapQueue = SliceMapAdapter<false>;
using IndexedSliceMapQueue = SliceMapAdapter<true>;
/** --------------------------------------------------------------------------------------------------------- Configuration
 * @brief Selects worker counts, batch size, and optional correctness or latency instrumentation.
 */
struct Configuration {
    size_t producers;
    size_t consumers;
    size_t batch;
    size_t rounds;
    size_t capacity = lane_capacity;
    bool verify = false;
    bool latency = false;
    bool skew = false;
};
/** --------------------------------------------------------------------------------------------------------- Statistics
 * @brief Keeps all frequently updated measurements private to one worker.
 */
struct alignas(128) Statistics {
    uint64_t count = 0;
    uint64_t sum = 0;
    uint64_t checksum = 0;
    uint64_t misses = 0;
    std::vector<uint64_t> latencies;
};
/** --------------------------------------------------------------------------------------------------------- Completion
 * @brief Publishes a consumer's delivered count only after producers finish and its queue poll misses.
 */
struct alignas(128) Completion {
    std::atomic<uint64_t> count{0};
};
/** --------------------------------------------------------------------------------------------------------- Backoff
 * @brief Yields after bounded empty polling and aborts a stalled test after sixty seconds.
 */
void backoff(uint64_t misses, Clock::time_point deadline) {
    if ((misses & 63) == 0) std::this_thread::yield();
    if ((misses & 65535) == 0) TEST_REQUIRE(Clock::now() < deadline, "queue run exceeded sixty seconds");
}
/** --------------------------------------------------------------------------------------------------------- Result
 * @brief Reports elapsed handoff time and optional sampled delivery percentiles.
 */
struct Result {
    double seconds = 0;
    uint64_t items = 0;
    uint64_t push_misses = 0;
    uint64_t pop_misses = 0;
    uint64_t minimum_consumer = UINT64_MAX;
    uint64_t maximum_consumer = 0;
    uint64_t allocations = 0;
    std::vector<uint64_t> latencies;
};
/** --------------------------------------------------------------------------------------------------------- Run
 * @brief Reuses warmed workers across complete drains while excluding setup and validation from timing.
 */
template<typename Queue>
Result run(Fixture& fixture, const Configuration& configuration) {
    auto queue = [&] {
        if constexpr (requires { Queue(fixture, configuration.verify); }) {
            return Queue(fixture, configuration.verify);
        } else {
            return Queue(configuration.producers, configuration.consumers, configuration.capacity);
        }
    }();
    const size_t total = fixture.input.size();
    std::unique_ptr<std::atomic<unsigned>[]> seen;
    if (configuration.verify) seen = std::make_unique<std::atomic<unsigned>[]>(total);
    std::vector<Statistics> statistics(configuration.producers + configuration.consumers);
    std::vector<Completion> completions(configuration.consumers);
    for (auto& record : statistics) {
        if (configuration.latency) record.latencies.reserve((total + 1023) / 1024);
    }
    alignas(128) std::atomic<size_t> finished_producers{0};
    std::barrier phase(static_cast<std::ptrdiff_t>(statistics.size() + 1));
    std::vector<std::thread> workers;
    for (size_t producer = 0; producer < configuration.producers; ++producer) {
        workers.emplace_back([&, producer] {
            auto& record = statistics[producer];
            auto local = queue.bind_producer(producer);
            for (size_t round = 0; round <= configuration.rounds; ++round) {
                queue.start(local);
                phase.arrive_and_wait();
                const auto deadline = Clock::now() + std::chrono::seconds(60);
                const size_t origin = producer * fixture.per_producer;
                for (size_t offset = 0; offset < fixture.per_producer;) {
                    const size_t count = std::min(configuration.batch, fixture.per_producer - offset);
                    if (configuration.verify || configuration.latency) {
                        for (size_t index = 0; index < count; ++index) {
                            auto* payload = static_cast<Payload*>(fixture.input[origin + offset + index].ptr);
                            if (configuration.verify) payload->generation = (round + 1) ^ checksum_salt;
                            if (configuration.latency && (payload->identifier & 1023) == 1) {
                                payload->sent = nanoseconds();
                            }
                        }
                    }
                    size_t written = 0;
                    while (written < count) {
                        const size_t pushed = queue.push(local,
                            fixture.input.data() + origin + offset + written, count - written);
                        written += pushed;
                        if (!pushed) backoff(++record.misses, deadline);
                    }
                    offset += count;
                    if (configuration.skew && producer != 0 && (offset % 256) == 0) {
                        std::this_thread::yield();
                    }
                }
                queue.flush(local);
                if (finished_producers.fetch_add(1, std::memory_order_acq_rel) + 1 == configuration.producers) {
                    queue.close();
                }
                phase.arrive_and_wait();
            }
        });
    }
    for (size_t consumer = 0; consumer < configuration.consumers; ++consumer) {
        workers.emplace_back([&, consumer] {
            auto& record = statistics[configuration.producers + consumer];
            auto local = queue.bind_consumer(consumer);
            std::array<ba_slice_t, maximum_batch> output;
            for (size_t round = 0; round <= configuration.rounds; ++round) {
                queue.start(local);
                phase.arrive_and_wait();
                const auto deadline = Clock::now() + std::chrono::seconds(60);
                for (;;) {
                    size_t count = queue.pop(local, output.data(), configuration.batch);
                    if (!count) {
                        if (finished_producers.load(std::memory_order_acquire) == configuration.producers) {
                            completions[consumer].count.store(record.count, std::memory_order_release);
                            uint64_t completed = 0;
                            for (const auto& completion : completions) {
                                completed += completion.count.load(std::memory_order_acquire);
                            }
                            if (completed == total) break;
                        }
                        backoff(++record.misses, deadline);
                        continue;
                    }
                    record.count += count;
                    for (size_t index = 0; index < count; ++index) {
                        const auto* payload = static_cast<const Payload*>(output[index].ptr);
                        const uint64_t identifier = payload->identifier;
                        record.sum += identifier;
                        record.checksum ^= identifier;
                        if (configuration.verify) {
                            TEST_REQUIRE(identifier && identifier <= total, "invalid payload ID");
                            TEST_EQUAL(output[index].meta, fixture.input[identifier - 1].meta,
                                "corrupt Slice metadata");
                            TEST_EQUAL(payload->generation, ((round + 1) ^ checksum_salt),
                                "payload publication was not visible");
                            TEST_EQUAL(seen[identifier - 1].fetch_add(1, std::memory_order_relaxed), 0,
                                "duplicate queue delivery");
                        }
                        if (configuration.latency && (identifier & 1023) == 1) {
                            record.latencies.push_back(nanoseconds() - payload->sent);
                        }
                    }
                    if (configuration.verify && (record.count & 255) == 0) std::this_thread::yield();
                }
                phase.arrive_and_wait();
            }
        });
    }
    Result result;
    for (size_t round = 0; round <= configuration.rounds; ++round) {
        queue.reset();
        finished_producers.store(0, std::memory_order_relaxed);
        for (auto& completion : completions) completion.count.store(0, std::memory_order_relaxed);
        for (auto& record : statistics) {
            record.count = record.sum = record.checksum = record.misses = 0;
            record.latencies.clear();
        }
        if (configuration.verify) {
            for (size_t index = 0; index < total; ++index) seen[index].store(0, std::memory_order_relaxed);
        }
        const size_t allocated_before = MoodyTraits::allocations.load(std::memory_order_relaxed);
        const auto begin = Clock::now();
        phase.arrive_and_wait();
        phase.arrive_and_wait();
        const double seconds = std::chrono::duration<double>(Clock::now() - begin).count();
        const size_t allocations = MoodyTraits::allocations.load(std::memory_order_relaxed) - allocated_before;
        uint64_t count = 0, sum = 0, checksum = 0;
        for (size_t consumer = configuration.producers; consumer < statistics.size(); ++consumer) {
            const auto& record = statistics[consumer];
            count += record.count;
            sum += record.sum;
            checksum ^= record.checksum;
            if (round) {
                result.pop_misses += record.misses;
                result.minimum_consumer = std::min(result.minimum_consumer, record.count);
                result.maximum_consumer = std::max(result.maximum_consumer, record.count);
                result.latencies.insert(result.latencies.end(), record.latencies.begin(), record.latencies.end());
            }
        }
        const uint64_t expected_xor = (total % 4 == 0) ? total
            : (total % 4 == 1) ? 1 : (total % 4 == 2) ? total + 1 : 0;
        if (count != total || sum != uint64_t(total) * (total + 1) / 2 || checksum != expected_xor) {
            std::fprintf(stderr, "round=%zu count=%llu expected=%zu sum=%llu xor=%llu\n", round,
                (unsigned long long)count, total, (unsigned long long)sum, (unsigned long long)checksum);
        }
        TEST_REQUIRE(count == total && sum == uint64_t(total) * (total + 1) / 2 && checksum == expected_xor,
            "queue delivery count or checksum mismatch");
        if (configuration.verify) {
            for (size_t index = 0; index < total; ++index) {
                TEST_EQUAL(seen[index].load(std::memory_order_relaxed), 1, "missing queue delivery");
            }
        }
        if (round) {
            result.seconds += seconds;
            result.items += total;
            result.allocations += allocations;
            for (size_t producer = 0; producer < configuration.producers; ++producer) {
                result.push_misses += statistics[producer].misses;
            }
        }
    }
    for (auto& worker : workers) worker.join();
    TEST_EQUAL(result.allocations, 0, "moodycamel allocated during timed handoffs");
    std::sort(result.latencies.begin(), result.latencies.end());
    return result;
}
/** --------------------------------------------------------------------------------------------------------- Boundary Test
 * @brief Checks partial-block delivery and closure without losing consumer wakeups.
 */
void boundary_test() {
    Fixture fixture(1, 275);
    CQueue queue(1, 1, BA_QUEUE_BATCH_SIZE);
    std::thread consumer([&] {
        auto local = queue.bind_consumer(0);
        queue.start(local);
        std::array<ba_slice_t, maximum_batch> output;
        size_t delivered = 0;
        for (;;) {
            const size_t count = queue.pop(local, output.data(), 17);
            if (!count) break;
            for (size_t index = 0; index < count; ++index) {
                TEST_EQUAL(output[index].ptr, fixture.input[delivered++].ptr, "buffered ordering changed");
            }
        }
        TEST_EQUAL(delivered, 275, "partial flush or wakeup lost items");
        TEST_EQUAL(queue.pop(local, output.data(), 1), 0, "closed consumer did not stay closed");
    });
    auto local = queue.bind_producer(0);
    queue.start(local);
    TEST_EQUAL(queue.push(local, fixture.input.data(), fixture.input.size()), fixture.input.size(),
        "buffered push failed");
    queue.flush(local);
    queue.close();
    consumer.join();
    CQueue empty(1, 8, BA_QUEUE_BATCH_SIZE);
    std::barrier start(9);
    std::vector<std::thread> readers;
    for (size_t reader = 0; reader < 8; ++reader) {
        readers.emplace_back([&, reader] {
            auto token = empty.bind_consumer(reader);
            empty.start(token);
            start.arrive_and_wait();
            ba_slice_t output;
            TEST_EQUAL(empty.pop(token, &output, 1), 0, "empty close lost a wakeup");
        });
    }
    start.arrive_and_wait();
    empty.close();
    for (auto& reader : readers) reader.join();
}
/** --------------------------------------------------------------------------------------------------------- Print
 * @brief Emits raw CSV with delivered items counted once rather than as two queue operations.
 */
void print(const char* engine, size_t repeat, const Configuration& configuration, const Result& result) {
    const auto percentile = [&](double fraction) {
        return result.latencies.empty() ? 0.0
            : double(result.latencies[size_t((result.latencies.size() - 1) * fraction)]) / 1000;
    };
    std::printf("%s,%s,%zu,%zu,%zu,%zu,%llu,%.9f,%.3f,%.3f,%llu,%llu,%.3f,%.3f,%.3f,%zu,%llu,%llu,%llu\n",
        configuration.latency ? "latency" : configuration.skew ? "skew" : "throughput",
        engine, repeat, configuration.producers, configuration.consumers, configuration.batch,
        (unsigned long long)result.items, result.seconds, result.items / result.seconds / 1e6,
        result.seconds * 1e9 / result.items, (unsigned long long)result.push_misses,
        (unsigned long long)result.pop_misses, percentile(0.50), percentile(0.99), percentile(0.999),
        result.latencies.size(), (unsigned long long)result.minimum_consumer,
        (unsigned long long)result.maximum_consumer, (unsigned long long)result.allocations);
    std::fflush(stdout);
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs correctness stress or alternating paired throughput and sampled-latency comparisons.
 */
int main(int argument_count, char** arguments) {
    test_support::start(__FILE__);
    bool verify = false, latency = false, skew = false, slicemap = false;
    size_t items = 1048576, rounds = 4, repeats = 5;
    size_t selected_producers = 0, selected_consumers = 0, selected_batch = 0;
    for (int argument = 1; argument < argument_count; ++argument) {
        const std::string_view option(arguments[argument]);
        if (option == "--verify") verify = true;
        else if (option == "--latency") latency = true;
        else if (option == "--skew") skew = true;
        else if (option == "--slicemap") slicemap = true;
        else if (option == "--items" || option == "--rounds" || option == "--repeats"
            || option == "--producers" || option == "--consumers" || option == "--batch") {
            TEST_REQUIRE(argument + 1 < argument_count, "missing numeric argument");
            char* end = nullptr;
            const auto number = std::strtoull(arguments[++argument], &end, 10);
            TEST_REQUIRE(end && *end == '\0' && number > 0 && number <= (1ull << 24), "invalid numeric argument");
            if (option == "--items") items = number;
            else if (option == "--rounds") rounds = number;
            else if (option == "--repeats") repeats = number;
            else if (option == "--producers") selected_producers = number;
            else if (option == "--consumers") selected_consumers = number;
            else selected_batch = number;
        } else {
            std::fprintf(stderr, "Usage: %s [--verify|--latency|--skew] [--slicemap] [--items N] [--rounds N] [--repeats N] [--producers N --consumers N] [--batch 1|32|256]\n",
                arguments[0]);
            return 2;
        }
    }
    TEST_EQUAL((selected_producers == 0), (selected_consumers == 0), "select both worker counts");
    TEST_REQUIRE(!selected_batch || selected_batch == 1 || selected_batch == 32 || selected_batch == 256,
        "batch must be 1, 32, or 256");
    ba_init();
    if (verify) {
        if (!slicemap) boundary_test();
        const std::array<std::array<size_t, 2>, 5> shapes{{{1, 1}, {1, 8}, {8, 1}, {8, 8}, {24, 24}}};
        for (const auto& shape : shapes) {
            Fixture fixture(shape[0], slicemap ? 259 : 8195);
            for (size_t batch : {size_t(1), size_t(32), size_t(256)}) {
                Configuration configuration{shape[0], shape[1], batch, 3, BA_QUEUE_BATCH_SIZE, true, false, true};
                if (slicemap) {
                    std::fprintf(stderr, "verify %zu/%zu batch=%zu SliceMap\n", shape[0], shape[1], batch);
                    run<SliceMapQueue>(fixture, configuration);
                    run<IndexedSliceMapQueue>(fixture, configuration);
                    continue;
                }
                std::fprintf(stderr, "verify %zu/%zu batch=%zu C\n", shape[0], shape[1], batch);
                run<CQueue>(fixture, configuration);
                std::fprintf(stderr, "verify %zu/%zu batch=%zu direct\n", shape[0], shape[1], batch);
                run<MoodyQueue>(fixture, configuration);
                std::fprintf(stderr, "verify %zu/%zu batch=%zu buffered\n", shape[0], shape[1], batch);
                run<BufferedMoodyQueue>(fixture, configuration);
            }
        }
        ba_shutdown();
        std::fprintf(stderr, "%s\n", slicemap
            ? "SliceMap checks passed: sequence matching, partial batches, reset/reuse, visibility, exactly-once delivery, 48 workers."
            : "Queue checks passed: partial flush, close/wakeups, reuse, visibility, exactly-once delivery, 48 workers.");
        return 0;
    }
    std::printf("mode,engine,repeat,producers,consumers,batch,items,seconds,million_items_s,ns_item,push_misses,pop_misses,p50_us,p99_us,p999_us,latency_samples,min_consumer_items,max_consumer_items,timed_moody_allocations\n");
    const std::array<std::array<size_t, 2>, 7> shapes{{{1, 1}, {2, 2}, {4, 4}, {8, 8}, {1, 8}, {8, 1}, {16, 16}}};
    size_t case_index = 0;
    for (const auto& shape : shapes) {
        if (selected_producers && (shape[0] != selected_producers || shape[1] != selected_consumers)) continue;
        if ((latency || skew) && !(shape[0] == shape[1] && shape[0] <= 8)) continue;
        Fixture fixture(shape[0], items);
        for (size_t batch : {size_t(1), size_t(32), size_t(256)}) {
            if (selected_batch && batch != selected_batch) continue;
            Configuration configuration{shape[0], shape[1], batch, rounds, lane_capacity, false, latency, skew};
            for (size_t repeat = 0; repeat < repeats; ++repeat) {
                std::fprintf(stderr, "%zup/%zuc batch=%zu pair=%zu mode=%s\n", shape[0], shape[1], batch,
                    repeat + 1, latency ? "latency" : skew ? "skew" : "throughput");
                const size_t engines = slicemap ? 5 : 3;
                for (size_t position = 0; position < engines; ++position) {
                    switch ((repeat + case_index + position) % engines) {
                    case 0: print("c_tls_semaphore", repeat, configuration, run<CQueue>(fixture, configuration)); break;
                    case 1: print("moodycamel_direct", repeat, configuration, run<MoodyQueue>(fixture, configuration)); break;
                    case 2: print("moodycamel_tls", repeat, configuration, run<BufferedMoodyQueue>(fixture, configuration)); break;
                    case 3: print("slicemap_sequence", repeat, configuration, run<SliceMapQueue>(fixture, configuration)); break;
                    case 4: print("slicemap_indexed", repeat, configuration, run<IndexedSliceMapQueue>(fixture, configuration)); break;
                    }
                }
            }
            ++case_index;
        }
    }
    ba_shutdown();
}
