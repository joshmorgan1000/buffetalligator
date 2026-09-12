/** --------------------------------------------------------------------------------------------------------- SliceMap Test
 * @file slicemap_test.cpp
 * @brief Exercises row publication, duplicate lookup, merge, and reclamation during reuse.
 */
#include "test_support.hpp"
#include <alligator.hpp>
#include <array>
#include <atomic>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {
using buffetalligator::Slice;
using buffetalligator::SliceMap;
/** --------------------------------------------------------------------------------------------------------- Payload
 * @brief Builds a row whose bytes identify both its key and publication generation.
 */
Slice payload(int64_t identifier, uint64_t generation) {
    Slice result(64);
    result.data<int64_t>()[0] = identifier;
    result.data<uint64_t>()[1] = generation;
    return result;
}
/** --------------------------------------------------------------------------------------------------------- Contracts
 * @brief Checks ID domains, duplicate selection, move, merge, and retained ownership after reset.
 */
void contracts() {
    alignas(SliceMap::Row) std::byte row_storage[sizeof(SliceMap::Row)];
    auto* placed = new (row_storage) SliceMap::Row(17, payload(17, 1));
    TEST_EQUAL(placed->payload.data<int64_t>()[0], 17, "row placement construction failed");
    placed->~Row();
    SliceMap empty(0);
    TEST_REQUIRE(empty.find(int64_t(17)) == -1 && !empty.get_slice(int64_t(17)), "empty lookup");
    SliceMap map(8);
    const std::array<int64_t, 5> identifiers{0, INT64_MIN, UINT32_MAX, INT64_MAX, 0};
    for (size_t index = 0; index < identifiers.size(); ++index) {
        map.add_slice(identifiers[index], payload(identifiers[index], index));
    }
    TEST_EQUAL(map.find(int64_t(0)), 0, "duplicate ID did not select the first slot");
    TEST_EQUAL(map.get_slice(uint32_t(UINT32_MAX)).data<int64_t>()[0], UINT32_MAX, "32-bit ID mismatch");
    TEST_REQUIRE(!map.get_slice(int64_t(-1)) && map.find(int64_t(-1)) == -1, "sentinel matched a row");
    for (size_t index = 0; index < 4; ++index) {
        TEST_EQUAL(map.find(identifiers[index]), static_cast<int64_t>(index), "slot order changed");
        auto result = map.get_slice(identifiers[index]);
        TEST_REQUIRE(result && result.data<int64_t>()[0] == identifiers[index], "key lookup mismatch");
    }
    Slice retained = map.get_slice(int64_t(0));
    SliceMap moved(std::move(map));
    TEST_EQUAL(moved.size(), identifiers.size(), "move lost rows");
    map.reset();
    TEST_REQUIRE(map.size() == 0 && map.find(int64_t(0)) == -1, "moved-from reset failed");
    moved.merge(map);
    SliceMap destination(16);
    destination.add_slice(int64_t(101), payload(101, 1));
    destination.merge(moved);
    TEST_REQUIRE(destination.size() == 6 && moved.size() == 0, "merge lost rows");
    TEST_EQUAL(destination.find(int64_t(0)), 1, "merge changed row order");
    destination.add_slice(int64_t(202), payload(202, 2));
    TEST_EQUAL(destination.find(int64_t(202)), 6, "partial merge left unclaimed slot holes");
    destination.reset();
    TEST_EQUAL(retained.data<uint64_t>()[1], 0, "reset invalidated a copied Slice");
    TEST_REQUIRE(destination.size() == 0 && destination.find(int64_t(0)) == -1, "reset retained ID");
}
/** --------------------------------------------------------------------------------------------------------- Identifier For Hash
 * @brief Constructs keys that force fingerprint collisions without a probabilistic search.
 */
int64_t identifier_for_hash(uint64_t hash) {
    const auto inverse = [](uint64_t factor) {
        uint64_t value = factor;
        for (unsigned step = 0; step < 6; ++step) value *= 2 - factor * value;
        return value;
    };
    hash ^= (hash >> 31) ^ (hash >> 62);
    hash *= inverse(UINT64_C(0x94d049bb133111eb));
    hash ^= (hash >> 27) ^ (hash >> 54);
    hash *= inverse(UINT64_C(0xbf58476d1ce4e5b9));
    hash ^= (hash >> 30) ^ (hash >> 60);
    return static_cast<int64_t>(hash);
}
/** --------------------------------------------------------------------------------------------------------- Collisions
 * @brief Forces a wrapped probe cluster and checks concurrent duplicate publication.
 */
void collisions() {
    SliceMap fingerprints(8);
    const int64_t first = identifier_for_hash(0x200e);
    const int64_t second = identifier_for_hash(0x100f);
    const int64_t missing = identifier_for_hash(0x100e);
    fingerprints.add_slice(first, payload(first, 0));
    fingerprints.add_slice(second, payload(second, 1));
    TEST_REQUIRE(fingerprints.find(missing) == -1 && !fingerprints.get_slice(missing), "fingerprint false hit");
    fingerprints.add_slice(missing, payload(missing, 2));
    TEST_EQUAL(fingerprints.find(missing), 2, "fingerprint collision lost insertion");
    SliceMap map(64);
    std::vector<int64_t> identifiers;
    for (uint64_t candidate = 0; identifiers.size() < map.capacity(); ++candidate) {
        uint64_t hash = (candidate ^ (candidate >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        hash = (hash ^ (hash >> 27)) * UINT64_C(0x94d049bb133111eb);
        if (((hash ^ (hash >> 31)) & 127) != 127) continue;
        identifiers.push_back(static_cast<int64_t>(candidate));
        map.add_slice(static_cast<int64_t>(candidate), payload(candidate, identifiers.size()));
    }
    for (size_t index = 0; index < identifiers.size(); ++index) {
        TEST_EQUAL(map.find(identifiers[index]), static_cast<int64_t>(index), "collision lost a slot");
        TEST_EQUAL(map.get_slice(identifiers[index]).data<int64_t>()[0], identifiers[index], "collision mismatch");
    }
    map.reset();
    std::barrier start(8);
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&, worker] {
            start.arrive_and_wait();
            for (size_t index = 0; index < 8; ++index) map.add_slice(int64_t(11), payload(11, worker));
        });
    }
    for (auto& worker : workers) worker.join();
    TEST_REQUIRE(map.size() == 64 && map.find(int64_t(11)) == 0, "duplicate publication changed first slot");
    TEST_EQUAL(map.get_slice(int64_t(11)).raw(), map.slice_at(0).raw(), "duplicate selected wrong payload");
}
/** --------------------------------------------------------------------------------------------------------- Publish Hook
 * @brief Verifies slot publication and records hook completion before the landed count advances.
 */
void publish_hook(void* context, void* pointer, const size_t& slot) {
    auto& map = *static_cast<SliceMap*>(pointer);
    TEST_REQUIRE(map.published(slot), "hook ran before publication");
    auto result = map.slice_at(slot);
    TEST_EQUAL(result.data<int64_t>()[0], map.id<int64_t>(slot), "hook saw uninitialized payload");
    static_cast<std::atomic<size_t>*>(context)->fetch_add(1, std::memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Publish
 * @brief Checks concurrent append and copy-out visibility with an oversubscribed worker team.
 */
void publish(size_t threads) {
    constexpr size_t per_producer = 257;
    const size_t total = threads * per_producer;
    SliceMap map(total);
    std::atomic<size_t> hooks{0};
    map.on_publish(publish_hook, &hooks);
    std::barrier start(static_cast<std::ptrdiff_t>(threads * 2 + 1));
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < threads; ++worker) {
        workers.emplace_back([&, worker] {
            start.arrive_and_wait();
            for (size_t index = worker; index < total; index += threads) {
                map.add_slice(static_cast<int64_t>(index), payload(index, index ^ 0x91));
                if (!(index & 31)) std::this_thread::yield();
            }
        });
        workers.emplace_back([&, worker] {
            start.arrive_and_wait();
            for (size_t index = worker; index < total; index += threads) {
                Slice result;
                while (!(result = map.get_slice(static_cast<int64_t>(index)))) std::this_thread::yield();
                TEST_EQUAL(result.data<int64_t>()[0], static_cast<int64_t>(index), "wrong published key");
                TEST_EQUAL(result.data<uint64_t>()[1], (index ^ 0x91), "payload visibility failure");
            }
        });
    }
    start.arrive_and_wait();
    map.wait_until_full(total / 2);
    map.wait();
    TEST_EQUAL(hooks.load(std::memory_order_relaxed), total, "wait returned before hooks");
    for (auto& worker : workers) worker.join();
    TEST_EQUAL(map.size(), total, "concurrent append lost a row");
}
/** --------------------------------------------------------------------------------------------------------- Reuse
 * @brief Changes every key during reset while readers retain and validate reclaimed generations.
 */
void reuse() {
    constexpr size_t capacity = 31;
    SliceMap map(capacity);
    std::atomic<bool> running{true};
    std::vector<std::thread> readers;
    const size_t reader_count = std::min(size_t(8), size_t(2) * buffetalligator::Memory::hardware_threads());
    std::barrier start(static_cast<std::ptrdiff_t>(reader_count + 1));
    for (size_t worker = 0; worker < reader_count; ++worker) {
        readers.emplace_back([&, worker] {
            start.arrive_and_wait();
            size_t probe = worker;
            while (running.load(std::memory_order_relaxed)) {
                const int64_t needle = static_cast<int64_t>(probe++ % (capacity * 2));
                auto result = map.get_slice(needle);
                if (!result) continue;
                std::this_thread::yield();
                TEST_EQUAL(result.data<int64_t>()[0], needle, "reset returned another row's key");
                const uint64_t generation = result.data<uint64_t>()[1];
                TEST_EQUAL(static_cast<size_t>(needle) / capacity, generation % 2, "reset corrupted generation");
            }
        });
    }
    start.arrive_and_wait();
    for (size_t generation = 0; generation < 2000; ++generation) {
        map.reset();
        for (size_t slot = 0; slot < capacity; ++slot) {
            const int64_t needle = slot + (generation % 2) * capacity;
            map.add_slice(needle, payload(needle, generation));
        }
    }
    running.store(false, std::memory_order_relaxed);
    for (auto& reader : readers) reader.join();
    map.reset();
    SliceMap::gc();
}
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs map contracts and concurrent publication and reclamation stress.
 */
int main() {
    test_support::start(__FILE__);
    contracts();
    collisions();
    const size_t maximum = size_t(2) * buffetalligator::Memory::hardware_threads();
    for (size_t threads : {size_t(1), size_t(2), size_t(8), size_t(24)}) {
        if (threads <= maximum) publish(threads);
    }
    reuse();
}
