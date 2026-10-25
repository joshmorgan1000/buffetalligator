/** --------------------------------------------------------------------------------------------------------- Slice Queue Test
 * @file slice_queue_test.cpp
 * @brief Checks immediate visibility, the high-water switch to bulk dispatch, ownership, bindings,
 * and exactly-once delivery across producers and one or two consumers.
 */
#include <alligator.hpp>
#include <alligator/containers.hpp>
#include <memory/tracker.hpp>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

using buffetalligator::Memory;
using buffetalligator::Slice;
using buffetalligator::SliceQueue;
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Stops the test on a violated public contract.
 */
static void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::abort(); }
}
/** --------------------------------------------------------------------------------------------------------- Record
 * @struct Record
 * @brief What the handlers saw, in order.
 */
struct Record {
    std::vector<uint64_t> singles;  ///< Payload values handed to the single handler.
    std::vector<uint64_t> bulks;  ///< Payload values handed to the bulk handler.
    std::vector<size_t> batches;  ///< Size of each bulk call.
    size_t nulls = 0;  ///< Null Slices handed to the single handler.
    std::atomic<size_t> delivered{0};  ///< Items seen by either handler.
};
/** --------------------------------------------------------------------------------------------------------- Record Single
 * @brief Records one item from the single handler.
 */
static void record_single(void* context, Slice&& slice) {
    Record& record = *static_cast<Record*>(context);
    if (slice.is_null()) ++record.nulls;
    else record.singles.push_back(slice.get_as<uint64_t>());
    record.delivered.fetch_add(1, std::memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Record Bulk
 * @brief Records one batch from the bulk handler.
 */
static void record_bulk(void* context, std::span<Slice> slices) {
    Record& record = *static_cast<Record*>(context);
    record.batches.push_back(slices.size());
    for (Slice& slice : slices) record.bulks.push_back(slice.get_as<uint64_t>());
    record.delivered.fetch_add(slices.size(), std::memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Payload
 * @brief Claims an eight-byte Slice holding value.
 */
static Slice payload(uint64_t value, bool novel = false) {
    Slice slice(sizeof(uint64_t), novel);
    slice.get_as<uint64_t>() = value;
    return slice;
}
/** --------------------------------------------------------------------------------------------------------- Run Consumer
 * @brief Thread entry that runs one consumer index to completion.
 */
static void run_consumer(SliceQueue* queue, size_t index, void* context) {
    auto consumer = queue->consumer(index);
    consumer.run(context);
}
/** --------------------------------------------------------------------------------------------------------- Undelivered
 * @brief Checks that destroying a queue releases the Slices nobody consumed.
 */
static void undelivered() {
    const size_t before = Memory::total_freed();
    {
        SliceQueue queue(1, 1, 16, 8, &record_single, &record_bulk);
        auto producer = queue.producer(0);
        Slice novel(64, true);
        producer.push(std::move(novel));
        require(!novel, "push did not move ownership");
    }
    require(Memory::total_freed() >= before + 64, "queue leaked undelivered novel backing");
}
/** --------------------------------------------------------------------------------------------------------- Immediate
 * @brief A single push reaches a running consumer with no flush, block fill, or close.
 */
static void immediate() {
    Record record;
    SliceQueue queue(1, 1, 1024, 64, &record_single, &record_bulk);
    std::thread reader(&run_consumer, &queue, size_t(0), &record);
    {
        auto producer = queue.producer(0);
        for (uint64_t value = 0; value < 3; ++value) {
            producer.push(payload(value));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (record.delivered.load(std::memory_order_acquire) != value + 1)
                require(std::chrono::steady_clock::now() < deadline, "a pushed item was not delivered");
        }
    }
    queue.close();
    reader.join();
    require(record.singles == std::vector<uint64_t>({0, 1, 2}) && record.batches.empty(),
        "items below the high-water mark were not dispatched one at a time");
}
/** --------------------------------------------------------------------------------------------------------- Watermark
 * @brief A backlog at or above the high-water mark drains in max_batch bulk calls until it falls
 * below the mark, then one at a time, in producer order.
 */
static void watermark() {
    for (size_t round = 0; round < 2; ++round) {
        Record record;
        SliceQueue queue(1, 1, 100, 32, &record_single, &record_bulk);
        {
            auto producer = queue.producer(0);
            for (uint64_t value = 0; value < 1000; ++value) producer.push(payload(value, value % 7 == 0));
            Slice empty;
            producer.push(std::move(empty));
        }
        queue.close();
        run_consumer(&queue, 0, &record);
        // 1001 items: 29 bulks of 32 leave 73 below the mark of 100.
        require(record.batches == std::vector<size_t>(29, 32),
            "bulk calls did not take max_batch while at the mark");
        require(record.singles.size() == 72 && record.nulls == 1, "the tail below the mark was not single");
        for (uint64_t value = 0; value < 928; ++value)
            require(record.bulks[value] == value, "bulk dispatch changed producer order");
        for (uint64_t value = 0; value < 72; ++value)
            require(record.singles[value] == 928 + value, "single dispatch changed producer order");
        queue.reset();
    }
}
/** --------------------------------------------------------------------------------------------------------- Rejects
 * @brief Whether construction with this geometry throws.
 */
static bool rejects(size_t producers, size_t consumers, size_t high_water, size_t batch, bool handlers) {
    try {
        SliceQueue invalid(producers, consumers, high_water, batch, handlers ? &record_single : nullptr,
            &record_bulk);
    } catch (const std::exception&) { return true; }
    return false;
}
/** --------------------------------------------------------------------------------------------------------- Bindings
 * @brief Rejects invalid geometry and duplicate or out-of-range bindings.
 */
static void bindings() {
    require(rejects(0, 1, 1, 1, true) && rejects(1, 0, 1, 1, true) && rejects(1, 3, 1, 1, true)
        && rejects(1, 1, 0, 1, true) && rejects(1, 1, 1, 0, true) && rejects(1, 1, 1, 1, false),
        "invalid geometry or a missing handler was accepted");
    SliceQueue queue(1, 2, 8, 8, &record_single, &record_bulk);
    bool rejected = false;
    {
        auto producer = queue.producer(0);
        try { auto duplicate = queue.producer(0); } catch (const std::exception&) { rejected = true; }
        require(rejected, "duplicate producer binding was accepted");
    }
    rejected = false;
    try { auto invalid = queue.consumer(2); } catch (const std::exception&) { rejected = true; }
    require(rejected, "out-of-range consumer binding was accepted");
    auto consumer = queue.consumer(1);
    rejected = false;
    try { auto duplicate = queue.consumer(1); } catch (const std::exception&) { rejected = true; }
    require(rejected, "duplicate consumer binding was accepted");
    auto rebound = queue.producer(0);
}
/** --------------------------------------------------------------------------------------------------------- Exact Once
 * @brief Marks each delivered identifier and fails on a duplicate.
 */
struct ExactOnce {
    std::unique_ptr<std::atomic<unsigned>[]> seen;
    size_t total;
    std::atomic<size_t> singles{0};
    std::atomic<size_t> bulks{0};
    static void mark(ExactOnce& state, Slice& slice) {
        const uint64_t identifier = slice.get_as<uint64_t>();
        require(identifier < state.total, "queue returned an invalid message");
        require(state.seen[identifier].fetch_add(1, std::memory_order_relaxed) == 0,
            "queue duplicated a message");
    }
    static void single(void* context, Slice&& slice) {
        ExactOnce& state = *static_cast<ExactOnce*>(context);
        mark(state, slice);
        state.singles.fetch_add(1, std::memory_order_relaxed);
    }
    static void bulk(void* context, std::span<Slice> slices) {
        ExactOnce& state = *static_cast<ExactOnce*>(context);
        for (Slice& slice : slices) mark(state, slice);
        state.bulks.fetch_add(slices.size(), std::memory_order_relaxed);
    }
};
/** --------------------------------------------------------------------------------------------------------- Produce
 * @brief Pushes one producer's identifiers singly or in spans of 37.
 */
static void produce(SliceQueue* queue, size_t producer, size_t items, bool spans, std::barrier<>* start) {
    auto writer = queue->producer(producer);
    std::array<Slice, 37> input;
    start->arrive_and_wait();
    for (size_t offset = 0; offset < items;) {
        const size_t count = std::min(spans ? input.size() : size_t(1), items - offset);
        for (size_t index = 0; index < count; ++index)
            input[index] = payload(producer * items + offset + index);
        if (spans) writer.push(std::span<Slice>(input.data(), count));
        else writer.push(std::move(input[0]));
        for (size_t index = 0; index < count; ++index) require(!input[index], "push retained its input");
        offset += count;
    }
}
/** --------------------------------------------------------------------------------------------------------- Consume
 * @brief Thread entry that waits for the start barrier, then runs one consumer.
 */
static void consume(SliceQueue* queue, size_t index, ExactOnce* state, std::barrier<>* start) {
    auto consumer = queue->consumer(index);
    start->arrive_and_wait();
    consumer.run(state);
}
/** --------------------------------------------------------------------------------------------------------- Concurrent
 * @brief Delivers every identifier exactly once across segment boundaries and both dispatch paths.
 */
static void concurrent(size_t producers, size_t consumers, bool spans) {
    constexpr size_t items = 5003;
    ExactOnce state;
    state.total = producers * items;
    state.seen = std::make_unique<std::atomic<unsigned>[]>(state.total);
    SliceQueue queue(producers, consumers, 64, 37, &ExactOnce::single, &ExactOnce::bulk);
    std::barrier<> start(static_cast<std::ptrdiff_t>(producers + consumers));
    std::vector<std::thread> writers, readers;
    for (size_t consumer = 0; consumer < consumers; ++consumer)
        readers.emplace_back(&consume, &queue, consumer, &state, &start);
    for (size_t producer = 0; producer < producers; ++producer)
        writers.emplace_back(&produce, &queue, producer, items, spans, &start);
    for (auto& writer : writers) writer.join();
    queue.close();
    for (auto& reader : readers) reader.join();
    for (size_t index = 0; index < state.total; ++index)
        require(state.seen[index].load(std::memory_order_relaxed) == 1, "queue omitted a message");
    require(state.singles + state.bulks == state.total, "handler counts differ from the pushed total");
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs the public queue contract.
 */
int main() {
    static_assert(sizeof(Slice) == 64);
    static_assert(!std::is_copy_constructible_v<SliceQueue::Producer>);
    static_assert(!std::is_move_constructible_v<SliceQueue::Consumer>);
    undelivered();
    immediate();
    watermark();
    bindings();
    for (const auto shape : {std::array<size_t, 2>{1, 1}, {8, 1}, {24, 1}, {1, 2}, {8, 2}, {24, 2}}) {
        concurrent(shape[0], shape[1], false);
        concurrent(shape[0], shape[1], true);
    }
    std::printf("slice queue test passed\n");
}
