/** --------------------------------------------------------------------------------------------------------- Slice Queue Test
 * @file slice_queue_test.cpp
 * @brief Checks public queue ownership, flushes, closure, reuse, and concurrent exact-once delivery.
 */
#include <alligator.hpp>
extern "C" {
#include "core/ba_core.h"
}
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

using buffetalligator::Slice;
using buffetalligator::SliceQueue;
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Stops the test on a violated public contract.
 */
static void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::abort(); }
}
/** --------------------------------------------------------------------------------------------------------- Undelivered
 * @brief Checks that destroying an unused queue releases its remaining owned Slices.
 */
static void undelivered() {
    ba_stats_t before, allocated, current;
    ba_stats(ba_placement_default(), &before);
    {
        SliceQueue queue(1, 1);
        auto producer = queue.producer(0);
        Slice payload(64, true);
        producer.push(std::move(payload));
        require(!payload, "push did not move ownership");
        ba_stats(ba_placement_default(), &allocated);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
        ba_stats(ba_placement_default(), &current);
        require(std::chrono::steady_clock::now() < deadline, "queue leaked undelivered novel backing");
        std::this_thread::yield();
    } while (current.novel_bytes_freed - before.novel_bytes_freed
        < allocated.novel_bytes_allocated - before.novel_bytes_allocated);
}
/** --------------------------------------------------------------------------------------------------------- Boundaries
 * @brief Checks null payloads, automatic partial flush, destination replacement, and quiescent reset.
 */
static void boundaries() {
    SliceQueue queue(1, 1, 512);
    for (size_t round = 0; round < 3; ++round) {
        {
            auto producer = queue.producer(0);
            for (size_t index = 0; index < 275; ++index) {
                Slice payload(sizeof(uint64_t), index % 2 == 0);
                payload.get_as<uint64_t>() = index;
                producer.push(std::move(payload));
                require(payload.is_null(), "single push retained its source");
            }
            Slice empty;
            producer.push(std::move(empty));
        }
        queue.close();
        {
            auto consumer = queue.consumer(0);
            Slice output(64);
            for (size_t index = 0; index < 275; ++index) {
                require(consumer.pop(output), "closed queue lost a published message");
                require(output.get_as<uint64_t>() == index, "single-producer ordering changed");
                require(output.is_novel() == (index % 2 == 0), "handoff changed backing identity");
            }
            require(consumer.pop(output) && output.is_null(), "null payload was confused with closure");
            output = Slice(64);
            const void* retained = output.raw();
            require(!consumer.pop(output) && output.raw() == retained, "closed pop modified its destination");
        }
        queue.reset();
    }
}
/** --------------------------------------------------------------------------------------------------------- Bindings
 * @brief Rejects invalid geometry and duplicate or out-of-range thread bindings.
 */
static void bindings() {
    bool rejected = false;
    try { SliceQueue invalid(0, 1); } catch (const std::exception&) { rejected = true; }
    require(rejected, "zero producers were accepted");
    rejected = false;
    try { SliceQueue invalid(1, 1, 257); } catch (const std::exception&) { rejected = true; }
    require(rejected, "partial block capacity was accepted");
    SliceQueue queue(1, 1);
    {
        auto producer = queue.producer(0);
        rejected = false;
        try { auto duplicate = queue.producer(0); } catch (const std::exception&) { rejected = true; }
        require(rejected, "duplicate producer binding was accepted");
    }
    rejected = false;
    try { auto invalid = queue.consumer(1); } catch (const std::exception&) { rejected = true; }
    require(rejected, "out-of-range consumer binding was accepted");
    {
        auto consumer = queue.consumer(0);
        rejected = false;
        try { auto duplicate = queue.consumer(0); } catch (const std::exception&) { rejected = true; }
        require(rejected, "duplicate consumer binding was accepted");
        require(consumer.pop(std::span<Slice>{}) == 0, "an empty output span blocked");
    }
    queue.close();
}
/** --------------------------------------------------------------------------------------------------------- Concurrent
 * @brief Forces bounded block reuse with single or bulk ownership transfers across fixed workers.
 */
static void concurrent(size_t producers, size_t consumers, bool bulk) {
    constexpr size_t items = 1027;
    const size_t total = producers * items;
    auto seen = std::make_unique<std::atomic<unsigned>[]>(total);
    SliceQueue queue(producers, consumers, SliceQueue::block_size);
    std::barrier start(static_cast<std::ptrdiff_t>(producers + consumers));
    std::vector<std::thread> writers, readers;
    for (size_t consumer = 0; consumer < consumers; ++consumer) {
        readers.emplace_back([&, consumer] {
            auto reader = queue.consumer(consumer);
            std::array<Slice, 37> output;
            start.arrive_and_wait();
            for (;;) {
                const size_t count = bulk ? reader.pop(output) : size_t(reader.pop(output[0]));
                if (!count) break;
                for (size_t index = 0; index < count; ++index) {
                    const size_t identifier = output[index].get_as<uint64_t>();
                    require(identifier < total, "queue returned an invalid message");
                    require(seen[identifier].fetch_add(1, std::memory_order_relaxed) == 0,
                        "queue duplicated a message");
                    require(output[index].size_bytes() == 64, "queue changed Slice metadata");
                }
                std::this_thread::yield();
            }
        });
    }
    for (size_t producer = 0; producer < producers; ++producer) {
        writers.emplace_back([&, producer] {
            auto writer = queue.producer(producer);
            std::array<Slice, 37> input;
            start.arrive_and_wait();
            for (size_t offset = 0; offset < items;) {
                const size_t count = std::min(bulk ? input.size() : size_t(1), items - offset);
                for (size_t index = 0; index < count; ++index) {
                    input[index] = Slice(64);
                    input[index].get_as<uint64_t>() = producer * items + offset + index;
                }
                if (bulk) writer.push(std::span<Slice>(input.data(), count));
                else writer.push(std::move(input[0]));
                for (size_t index = 0; index < count; ++index) require(!input[index], "bulk push retained input");
                offset += count;
                if (offset % 37 == 0) writer.flush();
            }
        });
    }
    for (auto& writer : writers) writer.join();
    queue.close();
    for (auto& reader : readers) reader.join();
    for (size_t index = 0; index < total; ++index) {
        require(seen[index].load(std::memory_order_relaxed) == 1, "queue omitted a message");
    }
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs the public queue contract without a moodycamel dependency.
 */
int main() {
    static_assert(sizeof(Slice) == 16);
    static_assert(!std::is_copy_constructible_v<SliceQueue::Producer>);
    static_assert(!std::is_move_constructible_v<SliceQueue::Consumer>);
    undelivered();
    boundaries();
    bindings();
    for (const auto shape : {std::array<size_t, 2>{1, 1}, {1, 8}, {8, 1}, {8, 8}, {24, 24}}) {
        concurrent(shape[0], shape[1], false);
        concurrent(shape[0], shape[1], true);
    }
}
