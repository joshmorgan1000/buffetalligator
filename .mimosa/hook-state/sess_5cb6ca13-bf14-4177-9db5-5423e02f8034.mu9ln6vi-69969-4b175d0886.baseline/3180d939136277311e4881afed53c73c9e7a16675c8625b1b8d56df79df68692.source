/** --------------------------------------------------------------------------------------------------------- Slice Map Concurrency Test
 * @file slice_map_concurrency_test.cpp
 * @brief Checks replacement ownership, bucket growth, shared-key publication, and reclamation.
 */
#include <alligator.hpp>
#include <memory/slicefriend.hpp>
#include <memory/tracker.hpp>
#include "functional_support.hpp"
#include <array>
#include <barrier>
#include <chrono>
#include <bit>
#include <memory>
#include <thread>
#include <vector>

using namespace buffetalligator;
using functional::require;
namespace {
/** --------------------------------------------------------------------------------------------------------- Value
 * @brief Tags an immutable payload with its key and an independently checked version.
 */
struct Value { uint64_t key; uint64_t version; uint64_t complement; };
/** --------------------------------------------------------------------------------------------------------- Payload
 * @brief Creates a payload whose fields detect wrong-key reads and torn replacements.
 */
Slice payload(uint64_t key, uint64_t version, bool novel = false) {
    Slice slice(sizeof(Value), novel);
    slice.get_as<Value>() = {key, version, ~version};
    return slice;
}
/** --------------------------------------------------------------------------------------------------------- Verify
 * @brief Validates the retained immutable payload after its map hazard has been released.
 */
void verify(const Slice& slice, uint64_t key) {
    const Value& value = slice.get_as<Value>();
    require(value.key == key && value.complement == ~value.version,
        "replacement returned a torn or wrong-key payload");
}
/** --------------------------------------------------------------------------------------------------------- Completed Frees
 * @brief Observes completed allocator teardown after earlier release tasks have drained.
 */
size_t completed_frees() { return Memory::total_freed(); }
/** --------------------------------------------------------------------------------------------------------- Replacement Ownership
 * @brief Checks stable-position overwrite, immediate unshared release, retained views, and finalizers.
 */
void replacement_ownership() {
    LOG_INFO_STREAM << "Checking stable replacement and old Slice deallocation";
    Slice warmup(64);
    SliceMap map(1);
    const size_t before = SliceFriend::execute_async<size_t>(completed_frees).get();
    map.add_slice(7, payload(7, 1, true));
    map.add_slice(7, payload(7, 2, true));
    require(SliceFriend::execute_async<size_t>(completed_frees).get() == before + sizeof(Value),
        "replacement did not release the previous unshared Slice");
    require(map.size() == 1 && map.find(7) == 0, "replacement consumed a second position");
    Slice retained = map.get_slice(7);
    map.add_slice(7, payload(7, 3, true));
    require(SliceFriend::execute_async<size_t>(completed_frees).get() == before + sizeof(Value),
        "replacement released backing still held by a retained Slice");
    require(retained.get_as<Value>().version == 2 && map.slice_at(0).get_as<Value>().version == 3,
        "replacement changed a retained payload or missed its stable slot");
    retained.free();
    require(SliceFriend::execute_async<size_t>(completed_frees).get() == before + 2 * sizeof(Value),
        "last retained view did not release its old backing");
    map.add_slice(7, Slice());
    require(map.size() == 1 && map.find(7) == 0 && !map.get_slice(7),
        "null replacement lost the existing key");
    require(SliceFriend::execute_async<size_t>(completed_frees).get() == before + 3 * sizeof(Value),
        "null replacement retained the old payload");
    SliceMapT<std::shared_ptr<int>> typed(1), source(1);
    auto object = std::make_shared<int>(10);
    std::weak_ptr<int> first = object;
    typed.emplace(4, std::move(object));
    object = std::make_shared<int>(20);
    std::weak_ptr<int> second = object;
    typed.emplace(4, std::move(object));
    require(first.expired() && !second.expired() && typed.size() == 1,
        "typed replacement failed to finalize exactly the old object");
    source.emplace(4, std::make_shared<int>(30));
    typed.merge(source);
    require(second.expired() && *typed.as(0) == 30 && typed.size() == 1 && source.size() == 0,
        "overlapping merge did not replace and finalize at the existing position");
}
/** --------------------------------------------------------------------------------------------------------- Growth
 * @brief Forces deep hash-prefix collisions and verifies every position survives splitting and reset.
 */
void growth() {
    LOG_INFO_STREAM << "Checking growth from zero reservation and deep bucket splits";
    constexpr size_t count = 4097;
    constexpr uint64_t inverse = UINT64_C(17428512612931826493);
    static_assert(inverse * UINT64_C(11400714819323198485) == 1);
    SliceMap map(0);
    for (size_t index = 0; index < count; ++index) {
        const int64_t key = std::bit_cast<int64_t>(index * inverse);
        map.add_slice(key, payload(index, index));
        require(map.find(key) == static_cast<int64_t>(index), "bucket split moved an existing position");
    }
    require(map.size() == count && map.capacity() >= count, "initial reservation became a size limit");
    void** pointers = map.data();
    for (size_t index = 0; index < count; ++index) {
        const int64_t key = std::bit_cast<int64_t>(index * inverse);
        require(map.published(index) && map.id<int64_t>(index) == key, "growth lost position metadata");
        verify(map.get_slice(key), index);
        require(static_cast<Value*>(pointers[index])->key == index, "pointer view lost row order");
    }
    map.add_slice(0, payload(0, 999));
    require(map.data<Value>()[0]->version == 999, "pointer view did not refresh after replacement");
    map.reset();
    require(map.size() == 0 && map.find(0) == -1, "reset retained a grown bucket");
    map.add_slice(-1, payload(1, 1));
    require(map.find(-1) == 0, "reset did not restart dense positions");
}
/** --------------------------------------------------------------------------------------------------------- Contention
 * @brief Runs oversubscribed writers and readers against shared first insertions and repeated updates.
 */
struct Contention {
    static constexpr size_t writers = 16, readers = 16, keys = 257, iterations = 8192;
    SliceMap map{1};
    std::barrier<> gate{writers + readers};
    std::atomic<size_t> reads{0};
    static void run(Contention* work, size_t worker) {
        work->gate.arrive_and_wait();
        size_t observed = 0;
        for (size_t index = 0; index < iterations; ++index) {
            const size_t key = index % keys;
            if (worker < writers) {
                work->map.add_slice(key, payload(key, worker * iterations + index));
            } else {
                Slice found = work->map.get_slice(key);
                if (found) { verify(found, key); ++observed; }
                else if (index + 1 == iterations) {
                    // A reader that saw nothing may simply have outlasted the writers' per-thread
                    // allocator warmup, so keep polling until concurrent publication lands.
                    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(5);
                    size_t probe = 0;
                    while (observed == 0 && std::chrono::steady_clock::now() < deadline) {
                        const size_t candidate = probe++ % keys;
                        Slice extra = work->map.get_slice(static_cast<int64_t>(candidate));
                        if (extra) { verify(extra, candidate); ++observed; }
                    }
                }
            }
        }
        work->gate.arrive_and_wait();
        for (size_t key = 0; key < keys; ++key) verify(work->map.get_slice(key), key);
        Value** pointers = work->map.data<Value>();
        for (size_t position = 0; position < keys; ++position) {
            require(pointers[position]->key == work->map.id<uint64_t>(position),
                "concurrent pointer-view construction lost position order");
        }
        work->gate.arrive_and_wait();
        if (worker == 0) work->map.add_slice(0, payload(0, UINT64_MAX));
        work->gate.arrive_and_wait();
        require(work->map.data<Value>()[work->map.find(0)]->version == UINT64_MAX,
            "concurrent pointer-view refresh retained an obsolete payload");
        work->reads.fetch_add(observed, std::memory_order_relaxed);
    }
};
/** --------------------------------------------------------------------------------------------------------- Shared Keys
 * @brief Checks distinct-key accounting and dense stable positions after concurrent first publication.
 */
void shared_keys() {
    LOG_INFO_STREAM << "Checking 16 writers and 16 readers sharing first insertions and replacements";
    Contention work;
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < Contention::writers + Contention::readers; ++worker) {
        workers.emplace_back(&Contention::run, &work, worker);
    }
    for (auto& worker : workers) worker.join();
    require(work.map.size() == Contention::keys && work.reads.load() != 0,
        "concurrent first insertion duplicated a key or readers made no progress");
    std::array<bool, Contention::keys> positions{};
    for (size_t key = 0; key < Contention::keys; ++key) {
        const int64_t position = work.map.find(key);
        require(position >= 0 && position < static_cast<int64_t>(positions.size()) && !positions[position],
            "concurrent insertion assigned a duplicated or non-dense position");
        positions[position] = true;
        require(work.map.id<int64_t>(position) == static_cast<int64_t>(key), "position names another key");
    }
    SliceMap::gc();
}
/** --------------------------------------------------------------------------------------------------------- Reset Readers
 * @brief Keeps retained lookups active while one owner exchanges entire map states.
 */
struct ResetReaders {
    SliceMap map{1};
    std::atomic<bool> stop{false};
    std::barrier<> gate{17};
    static void run(ResetReaders* work) {
        work->gate.arrive_and_wait();
        while (!work->stop.load(std::memory_order_acquire)) {
            Slice retained = work->map.get_slice(42);
            if (retained) verify(retained, 42);
            const int64_t position = work->map.find(42);
            require(position == 0 || position == -1, "reset lookup returned an invalid position");
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Concurrent Reset
 * @brief Exercises retirement of complete states while protected and retained readers overlap.
 */
void concurrent_reset() {
    LOG_INFO_STREAM << "Checking state reclamation across 1,024 resets with 16 active readers";
    ResetReaders work;
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < 16; ++worker) workers.emplace_back(&ResetReaders::run, &work);
    work.gate.arrive_and_wait();
    for (size_t round = 0; round < 1024; ++round) {
        work.map.reset();
        work.map.add_slice(42, payload(42, round));
    }
    work.stop.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    SliceMap::gc();
}
/** --------------------------------------------------------------------------------------------------------- Waiters
 * @brief Tests hook reentrancy and repeated waiter notification without counting replacements as rows.
 */
struct Waiters {
    SliceMap map{64};
    std::atomic<size_t> callbacks{0};
    static void hook(void* pointer, void* context, const size_t& index) {
        auto& map = *static_cast<SliceMap*>(pointer);
        auto& work = *static_cast<Waiters*>(context);
        const int64_t key = map.id<int64_t>(index);
        require(map.find(key) == static_cast<int64_t>(index), "hook observed an unpublished position");
        verify(map.get_slice(key), static_cast<uint64_t>(key));
        work.callbacks.fetch_add(1, std::memory_order_relaxed);
    }
    static void wait(Waiters* work) {
        work->map.wait();
        require(work->callbacks.load() >= 64, "wait completed before publication hooks");
        verify(work->map.get_slice(0), 0);
    }
};
/** --------------------------------------------------------------------------------------------------------- Wait And Reuse
 * @brief Verifies multiple waiters and more lifetime thread registrations than the domain's active limit.
 */
void wait_and_reuse() {
    LOG_INFO_STREAM << "Checking publish hooks, multiple waiters, and hazard-record reuse";
    Waiters work;
    work.map.on_publish(&Waiters::hook, &work);
    std::thread first(&Waiters::wait, &work), second(&Waiters::wait, &work);
    for (size_t key = 0; key < 64; ++key) work.map.add_slice(key, payload(key, key));
    first.join();
    second.join();
    for (size_t key = 0; key < 64; ++key) work.map.add_slice(key, payload(key, key + 1));
    require(work.map.size() == 64 && work.callbacks.load() == 128, "replacement hook or count differs");
    for (size_t wave = 0; wave < 20; ++wave) {
        std::vector<std::thread> workers;
        for (size_t worker = 0; worker < 16; ++worker) workers.emplace_back(&Waiters::wait, &work);
        for (auto& worker : workers) worker.join();
    }
}
/** --------------------------------------------------------------------------------------------------------- Wait Race
 * @brief Races waiter registration with publication across repeated reset boundaries.
 */
struct WaitRace {
    static constexpr size_t rounds = 256, readers = 16;
    SliceMap map{1};
    std::barrier<> gate{readers + 1};
    static void run(WaitRace* work) {
        for (size_t round = 0; round < rounds; ++round) {
            work->gate.arrive_and_wait();
            work->map.wait();
            require(work->map.get_slice(0).get_as<Value>().version == round,
                "wait observed an earlier publication generation");
            work->gate.arrive_and_wait();
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Wait Registration
 * @brief Checks that conditional notification cannot miss a newly registered waiter.
 */
void wait_registration() {
    LOG_INFO_STREAM << "Checking waiter registration against publication across 256 generations";
    WaitRace work;
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < WaitRace::readers; ++worker) {
        workers.emplace_back(&WaitRace::run, &work);
    }
    for (size_t round = 0; round < WaitRace::rounds; ++round) {
        work.map.reset();
        work.gate.arrive_and_wait();
        work.map.add_slice(0, payload(0, round));
        work.gate.arrive_and_wait();
    }
    for (auto& worker : workers) worker.join();
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs production SliceMap ownership and concurrency contracts without copied implementations.
 */
int main() {
    replacement_ownership();
    growth();
    shared_keys();
    concurrent_reset();
    wait_and_reuse();
    wait_registration();
    LOG_INFO_STREAM << "SliceMap replacement, growth, and concurrent reclamation contracts passed";
}
