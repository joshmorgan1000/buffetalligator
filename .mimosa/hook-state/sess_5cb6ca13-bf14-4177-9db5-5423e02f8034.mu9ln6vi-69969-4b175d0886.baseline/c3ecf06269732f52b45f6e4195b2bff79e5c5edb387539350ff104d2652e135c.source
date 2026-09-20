/** --------------------------------------------------------------------------------------------------------- Map Functional Test
 * @file map_functional_test.cpp
 * @brief Exercises row lookup, transfer, typed cleanup, concurrent appends, and queue integration.
 */
#include <alligator.hpp>
#include "functional_support.hpp"
#include <array>
#include <barrier>
#include <memory>
#include <thread>
#include <vector>

using namespace buffetalligator;
using functional::require;
/** --------------------------------------------------------------------------------------------------------- Payload
 * @brief Creates an owned payload tagged with its expected row value.
 */
static Slice payload(uint64_t value) {
    Slice bytes(sizeof(value));
    bytes.get_as<uint64_t>() = value;
    return bytes;
}
/** --------------------------------------------------------------------------------------------------------- Rows
 * @brief Checks missing and boundary IDs, raw views, shared payloads, moves, and reset reuse.
 */
static void rows() {
    constexpr std::array<int64_t, 11> identifiers{
        0, 7, 8, 15, 16, 31, -2, INT64_MAX, UINT32_MAX, -1, INT64_MIN
    };
    SliceMap map(identifiers.size());
    require(map.size() == 0 && map.find(99) == -1 && map.get_slice(99).is_null(),
        "empty map lookup did not miss");
    require(!map.published(0) && map.id<int64_t>(0) == -1 && map.as<uint64_t>(0) == nullptr
        && map.data<uint64_t>()[0] == nullptr, "unpublished position lost its empty views");
    for (size_t slot = 0; slot < identifiers.size(); ++slot) {
        require(map.size() == slot, "append changed the entry count unexpectedly");
        map.add_slice(identifiers[slot], payload(slot + 100));
    }
    for (size_t slot = 0; slot < identifiers.size(); ++slot) {
        require(map.find(identifiers[slot]) == static_cast<int64_t>(slot), "ID lookup found wrong slot");
        require(map.id<int64_t>(slot) == identifiers[slot],
            "stored ID differs");
        require(*map.as<uint64_t>(slot) == slot + 100 && *map.data<uint64_t>()[slot] == slot + 100,
            "map raw payload view differs");
    }
    Slice retained = map.slice_at(0);
    SliceMap moved(std::move(map));
    require(map.size() == 0 && map.capacity() == 0 && map.wait_threshold() == 0,
        "move retained source ownership or wait expectation");
    require(moved.get_slice(identifiers.back()).get_as<uint64_t>() == 110, "move lost a row");
    moved.reset();
    require(moved.size() == 0 && moved.find(0) == -1 && retained.get_as<uint64_t>() == 100,
        "reset lost shared storage or retained a row");
    moved.add_slice(55, payload(77));
    require(moved.size() == 1 && moved.get_slice(55).get_as<uint64_t>() == 77,
        "reset did not permit reuse");
}
/** --------------------------------------------------------------------------------------------------------- Merge
 * @brief Checks partially filled merges transfer owned entries and leave capacity for later inserts.
 */
static void merge() {
    SliceMap source(8), destination(4);
    source.add_slice(20, payload(200));
    source.add_slice(30, payload(300));
    destination.add_slice(10, payload(100));
    destination.merge(source);
    destination.add_slice(40, payload(400));
    require(source.size() == 0 && source.find(20) == -1, "merge did not drain source");
    for (uint64_t value = 1; value <= 4; ++value) {
        Slice found = destination.get_slice(value * 10);
        require(found && found.get_as<uint64_t>() == value * 100,
            "partial merge lost a row or consumed unused source capacity");
    }
    source.add_slice(50, payload(500));
    require(source.get_slice(50).get_as<uint64_t>() == 500, "drained source could not be reused");
}
/** --------------------------------------------------------------------------------------------------------- Lifetime
 * @brief Checks typed row destructors run once after transfer, reset, and map destruction.
 */
static void lifetime() {
    std::weak_ptr<int> first, second;
    {
        SliceMapT<std::shared_ptr<int>> source(2), destination(2);
        auto object = std::make_shared<int>(17);
        first = object;
        source.emplace(1, std::move(object));
        require(!first.expired() && *source.as(0) == 17, "typed emplacement lost its object");
        destination.merge(source);
        require(!first.expired(), "typed merge destroyed its object");
        destination.reset();
        require(first.expired(), "typed reset did not finalize its object");
        object = std::make_shared<int>(19);
        second = object;
        destination.emplace(2, std::move(object));
    }
    require(second.expired(), "typed map destruction did not finalize its object");
}
/** --------------------------------------------------------------------------------------------------------- Container Boundaries
 * @brief Checks empty containers, null payloads, stable replacement, and move-assignment ownership.
 */
static void container_boundaries() {
    SliceMap empty(0), another_empty(0);
    require(empty.size() == 0 && empty.find(1) == -1, "zero-capacity map did not remain empty");
    empty.merge(another_empty);
    empty.reset();
    SliceMap values(3);
    values.add_slice(10, Slice());
    values.add_slice(20, payload(21));
    values.add_slice(20, payload(22));
    require(values.find(10) == 0 && values.get_slice(10).is_null(), "null entry lost its identity");
    require(values.size() == 2 && values.find(20) == 1
        && values.get_slice(20).get_as<uint64_t>() == 22,
        "duplicate replacement changed position, count, or value");
    std::weak_ptr<int> released;
    SliceMapT<std::shared_ptr<int>> destination(1);
    auto object = std::make_shared<int>(9);
    released = object;
    destination.emplace(9, std::move(object));
    static_cast<SliceMap&>(destination) = std::move(values);
    require(released.expired() && values.size() == 0, "move assignment lost ownership cleanup");
    require(destination.get_slice(20).get_as<uint64_t>() == 22, "move assignment lost entries");
    destination.reset();
    destination.reset();
    require(destination.size() == 0, "repeated reset changed empty ownership");
}
/** --------------------------------------------------------------------------------------------------------- Concurrent Map
 * @brief Checks independent appends and reads synchronized by the caller's worker barrier.
 */
static void concurrent_map() {
    constexpr size_t workers = 8, rows_per_worker = 137, total = workers * rows_per_worker;
    SliceMap map(total);
    std::barrier phase(static_cast<std::ptrdiff_t>(workers));
    std::vector<std::thread> team;
    for (size_t worker = 0; worker < workers; ++worker) {
        team.emplace_back([&, worker] {
            phase.arrive_and_wait();
            for (size_t row = 0; row < rows_per_worker; ++row) {
                const uint64_t identifier = worker * rows_per_worker + row;
                map.add_slice(identifier, payload(identifier));
            }
            phase.arrive_and_wait();
            for (size_t row = 0; row < rows_per_worker; ++row) {
                const uint64_t identifier = ((worker + 1) % workers) * rows_per_worker + row;
                Slice found = map.get_slice(identifier);
                require(found && found.get_as<uint64_t>() == identifier,
                    "caller-synchronized map lookup lost another worker's entry");
            }
        });
    }
    for (auto& worker : team) worker.join();
    require(map.size() == total, "concurrent appends lost an entry");
    for (uint64_t identifier = 0; identifier < total; ++identifier) {
        Slice found = map.get_slice(identifier);
        require(found && found.get_as<uint64_t>() == identifier, "map payload differs");
    }
}
/** --------------------------------------------------------------------------------------------------------- Shared Ownership
 * @brief Checks retained Slice handles keep payloads alive while the caller resets and reuses a map.
 */
static void shared_ownership() {
    constexpr size_t workers = 8, rounds = 40;
    SliceMap map(workers);
    std::barrier phase(static_cast<std::ptrdiff_t>(workers + 1));
    std::vector<std::thread> team;
    for (size_t worker = 0; worker < workers; ++worker) {
        team.emplace_back([&, worker] {
            for (size_t round = 0; round < rounds; ++round) {
                phase.arrive_and_wait();
                Slice retained = map.get_slice(worker);
                require(retained.is_novel(), "map changed the allocation's backing identity");
                phase.arrive_and_wait();
                phase.arrive_and_wait();
                require(retained.get_as<uint64_t>() == round * workers + worker,
                    "map reset invalidated memory still owned by another Slice");
                phase.arrive_and_wait();
            }
        });
    }
    for (size_t round = 0; round < rounds; ++round) {
        map.reset();
        for (size_t worker = 0; worker < workers; ++worker) {
            Slice memory(sizeof(uint64_t), true);
            memory.get_as<uint64_t>() = round * workers + worker;
            map.add_slice(worker, std::move(memory));
        }
        phase.arrive_and_wait();
        phase.arrive_and_wait();
        map.reset();
        for (size_t worker = 0; worker < workers; ++worker) {
            map.add_slice(worker, payload(UINT64_MAX));
        }
        phase.arrive_and_wait();
        phase.arrive_and_wait();
    }
    for (auto& worker : team) worker.join();
    map.reset();
    require(map.size() == 0, "reset retained container ownership");
}
/** --------------------------------------------------------------------------------------------------------- Pipeline
 * @brief Sends borrowed input through owned slices and a bounded queue into a typed result map.
 */
static void pipeline() {
    constexpr size_t count = 1027;
    SliceQueue queue(1, 1, SliceQueue::block_size);
    SliceMapT<uint64_t> results(count);
    AtomicRegistry registry;
    auto* completed = registry.create("completed", uint64_t(0));
    std::thread consumer([&] {
        auto input = queue.consumer(0);
        Slice received;
        while (input.pop(received)) {
            SliceT<uint64_t> typed(std::move(received));
            const uint64_t value = typed.get_as();
            results.emplace(value, value * value);
            completed->fetch_add<uint64_t>(1, std::memory_order_relaxed);
        }
    });
    {
        auto output = queue.producer(0);
        for (uint64_t value = 0; value < count; ++value) {
            WeakSlice input(&value, sizeof(value));
            output.push(static_cast<Slice>(input));
        }
    }
    queue.close();
    consumer.join();
    require(completed->load<uint64_t>() == count, "pipeline lost a message");
    for (uint64_t value = 0; value < count; ++value) {
        Slice found = results.get_slice(value);
        require(found && found.get_as<uint64_t>() == value * value,
            "pipeline output differs from its input transformation");
    }
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Dispatches independent row storage and cross-component pipeline cases.
 */
int main(int count, char** arguments) {
    return functional::run(count, arguments, {
        {"map_rows", rows}, {"map_merge", merge}, {"map_lifetime", lifetime},
        {"map_boundaries", container_boundaries},
        {"concurrent_map", concurrent_map}, {"map_shared_ownership", shared_ownership}, {"pipeline", pipeline}
    });
}
