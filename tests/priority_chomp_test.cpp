/** --------------------------------------------------------------------------------------------------------- Priority Chomp Test
 * @file priority_chomp_test.cpp
 * @brief Checks PriorityChomp ordering, key encodings, eviction, Chomp ownership, adoption, and
 * concurrent exact-once accounting.
 */
#include <alligator/containers.hpp>
#include <memory/tracker.hpp>
#include "functional_support.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <limits>
#include <thread>
#include <vector>

using namespace buffetalligator;
using functional::require;
using functional::require_throws;
namespace {
/** --------------------------------------------------------------------------------------------------------- Frees Reached
 * @brief Waits up to five seconds for deferred allocator teardown to reach the expected total.
 * @param expected The freed byte total to wait for.
 * @return The observed freed byte total.
 */
size_t frees_reached(size_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    size_t observed = Memory::total_freed();
    while (observed < expected && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
        observed = Memory::total_freed();
    }
    return observed;
}
/** --------------------------------------------------------------------------------------------------------- Descending
 * @brief Three-way order that pops the largest key first.
 * @param left The left key.
 * @param right The right key.
 * @return Negative when left pops first.
 */
int descending(const int32_t* left, const int32_t* right) {
    return (*left < *right) - (*left > *right);
}
/** --------------------------------------------------------------------------------------------------------- Payload
 * @brief Creates a novel 64-byte payload stamped with its identifier.
 * @param id The identifier.
 * @return The payload Chomp.
 */
Chomp payload(uint64_t id) {
    Chomp chomp(64, true);
    chomp.get_as<uint64_t>() = id;
    return chomp;
}
/** --------------------------------------------------------------------------------------------------------- Natural Order
 * @brief Checks that a float queue keeps the smallest keys, reports its worst, and pops ascending.
 */
void natural_order() {
    LOG_INFO_STREAM << "Checking natural float order, trimming, and ascending pops";
    PriorityChompT<float, uint32_t> queue(8);
    require(queue.capacity() == 8 && queue.empty() && !queue.full() && queue.size() == 0,
        "fresh queue reported entries");
    std::vector<float> keys;
    for (uint32_t index = 0; index < 32; ++index) {
        const float key = static_cast<float>((index * 37) % 101) - 50.0f;
        keys.push_back(key);
        const bool entered = queue.push(key, index);
        require(index < 8 ? entered : true, "push into a free slot was rejected");
    }
    std::vector<float> sorted = keys;
    std::sort(sorted.begin(), sorted.end());
    require(queue.size() == 8 && queue.full() && !queue.empty(), "trimmed queue is not full");
    float worst = 0.0f;
    require(queue.worst(worst) && worst == sorted[7], "worst key is not the eighth smallest");
    require(queue.accepts(sorted[7] - 1.0f) && !queue.accepts(sorted[7] + 1.0f),
        "accepts disagrees with worst");
    require(!queue.push(sorted[7] + 1.0f, 99), "a worse key entered a full queue");
    require(queue.push(sorted[0] - 1.0f, 100), "a better key was rejected by a full queue");
    float key = 0.0f;
    uint32_t value = 0;
    require(queue.pop(key, value) && key == sorted[0] - 1.0f && value == 100,
        "best entry was not popped first");
    for (size_t rank = 0; rank < 7; ++rank) {
        require(queue.pop(key, value) && key == sorted[rank], "pops left ascending order");
        require(keys[value] == key, "popped value does not match its key");
    }
    key = 123.0f;
    value = 7;
    require(!queue.pop(key, value) && key == 123.0f && value == 7,
        "pop on an empty queue touched its outputs");
    require(queue.empty() && !queue.full(), "drained queue still reports entries");
}
/** --------------------------------------------------------------------------------------------------------- Comparator Order
 * @brief Checks that a comparator flips the queue into a max-queue over signed keys.
 */
void comparator_order() {
    LOG_INFO_STREAM << "Checking a descending comparator over signed keys";
    PriorityChompT<int32_t, uint32_t, &descending> queue(5);
    require(queue.compare() != nullptr, "comparator was not installed");
    const int32_t keys[12] = {3, -7, 12, 0, -1, 44, 9, -30, 12, 5, 2, 100};
    for (uint32_t index = 0; index < 12; ++index) queue.push(keys[index], index);
    int32_t worst = 0;
    require(queue.worst(worst) && worst == 9, "worst of the max-queue is not the fifth largest");
    require(!queue.push(9, 50), "an equal key displaced an older entry");
    require(!queue.accepts(9) && queue.accepts(10), "accepts disagrees with the comparator");
    const int32_t expected[5] = {100, 44, 12, 12, 9};
    for (int32_t want : expected) {
        int32_t key = 0;
        uint32_t value = 0;
        require(queue.pop(key, value) && key == want && keys[value] == key,
            "max-queue popped out of order");
    }
    int32_t key = 0;
    uint32_t value = 0;
    require(!queue.pop(key, value), "max-queue popped a phantom entry");
}
/** --------------------------------------------------------------------------------------------------------- Key Encodings
 * @brief Checks sortable encodings for narrow signed, unsigned, enum, and special float keys.
 */
void key_encodings() {
    LOG_INFO_STREAM << "Checking int16, uint8, enum, and special float key encodings";
    PriorityChompT<int16_t, uint32_t> narrow(6);
    const int16_t narrow_keys[6] = {-32768, 32767, 0, -1, 1, -1000};
    for (uint32_t index = 0; index < 6; ++index) narrow.push(narrow_keys[index], index);
    int16_t previous = -32768;
    for (size_t rank = 0; rank < 6; ++rank) {
        int16_t key = 0;
        uint32_t value = 0;
        require(narrow.pop(key, value) && key >= previous && narrow_keys[value] == key,
            "int16 keys popped out of order");
        previous = key;
    }
    PriorityChompT<uint8_t, uint32_t> bytes(4);
    const uint8_t byte_keys[4] = {255, 0, 128, 1};
    for (uint32_t index = 0; index < 4; ++index) bytes.push(byte_keys[index], index);
    const uint8_t byte_order[4] = {0, 1, 128, 255};
    for (uint8_t want : byte_order) {
        uint8_t key = 0;
        uint32_t value = 0;
        require(bytes.pop(key, value) && key == want, "uint8 keys popped out of order");
    }
    enum class Level : uint16_t { low = 1, mid = 500, high = 60000 };
    PriorityChompT<Level, uint32_t> levels(3);
    levels.push(Level::high, 0);
    levels.push(Level::low, 1);
    levels.push(Level::mid, 2);
    Level level = Level::mid;
    uint32_t value = 0;
    require(levels.pop(level, value) && level == Level::low,
        "enum keys did not order by underlying bits");
    require(levels.pop(level, value) && level == Level::mid,
        "enum keys popped out of order");
    require(levels.pop(level, value) && level == Level::high,
        "enum keys popped out of order");
    PriorityChompT<float, uint32_t> floats(8);
    const float float_keys[8] = {
        std::numeric_limits<float>::infinity(), -1.5f, 0.0f, -0.0f, 1e-30f, 3.0f,
        -std::numeric_limits<float>::infinity(), -1e30f
    };
    for (uint32_t index = 0; index < 8; ++index) floats.push(float_keys[index], index);
    float last = -std::numeric_limits<float>::infinity();
    for (size_t rank = 0; rank < 8; ++rank) {
        float key = 0.0f;
        require(floats.pop(key, value) && key >= last, "float keys popped out of order");
        require(std::bit_cast<uint32_t>(float_keys[value]) == std::bit_cast<uint32_t>(key),
            "float key bits changed");
        last = key;
    }
}
/** --------------------------------------------------------------------------------------------------------- Chomp Ownership
 * @brief Checks that evicted, rejected, cleared, and destroyed Chomps are released exactly once
 * while popped and viewed ones stay alive.
 */
void chomp_ownership() {
    LOG_INFO_STREAM << "Checking Chomp ownership across eviction, pop, clear, and destruction";
    Chomp warmup(64);
    const size_t before = Memory::total_freed();
    {
        PriorityChompT<float, Chomp> queue(4);
        for (uint64_t id = 0; id < 6; ++id) {
            Chomp entry = payload(id);
            const bool entered = queue.push(static_cast<float>(id), std::move(entry));
            require(entry.is_null(), "push left the caller holding the Chomp");
            require(entered == (id < 4), "eviction accepted a worse Chomp");
        }
        require(frees_reached(before + 128) == before + 128, "rejected Chomps were not released");
        float key = 0.0f;
        require(queue.best(key) && key == 0.0f, "best key is wrong");
        Chomp popped;
        require(queue.pop(key, popped) && key == 0.0f && popped.get_as<uint64_t>() == 0,
            "pop handed over the wrong Chomp");
        require(queue.size() == 3 && Memory::total_freed() == before + 128,
            "pop released the popped Chomp");
        popped.free();
        require(frees_reached(before + 192) == before + 192,
            "the popped Chomp was not released by its owner");
        PriorityChompT<float, Chomp> moved(std::move(queue));
        require(queue.capacity() == 0 && moved.size() == 3, "move did not transfer the entries");
        queue.clear();
        moved.clear();
        require(moved.empty(), "clear left entries queued");
        require(frees_reached(before + 384) == before + 384,
            "clear did not release the queued Chomps");
        moved.push(9.0f, payload(9));
    }
    require(frees_reached(before + 448) == before + 448,
        "destruction did not release the queued Chomp");
}
/** --------------------------------------------------------------------------------------------------------- Chomp Move Assignment
 * @brief Checks exact payload release through populated move assignment and self-move.
 */
template<typename Queue>
void chomp_move_assignment() {
    LOG_INFO_STREAM << "Checking populated Chomp queue moves and exact payload release";
    Chomp warmup(64);
    const size_t before = Memory::total_freed();
    {
        Queue destination(4);
        std::array<Chomp, 3> displaced;
        for (size_t index = 0; index < displaced.size(); ++index) {
            Chomp entry = payload(100 + index);
            displaced[index] = entry.chomp();
            require(destination.push(static_cast<uint32_t>(index), std::move(entry)),
                "destination rejected an initial payload");
        }
        const uint64_t* transferred_words = nullptr;
        const void* transferred_payload = nullptr;
        {
            Queue source(3);
            for (uint32_t index = 0; index < 3; ++index) {
                Chomp entry = payload(200 + index);
                if (index == 0) transferred_payload = entry.raw();
                require(source.push(index, std::move(entry)),
                    "source rejected an initial payload");
            }
            transferred_words = source.words();
            require(&(destination = std::move(source)) == &destination,
                "move assignment returned another container");
            require(source.capacity() == 0 && source.empty(),
                "move assignment left its source owning payloads");
            source.clear();
        }
        require(destination.capacity() == 3 && destination.size() == 3
            && destination.words() == transferred_words,
            "move assignment did not transfer the source storage");
        require(Memory::total_freed() == before,
            "move assignment or source destruction released a live payload");
        Queue& same = destination;
        destination = std::move(same);
        require(destination.capacity() == 3 && destination.size() == 3
            && destination.words() == transferred_words,
            "self-move changed the queue or its payloads");
        require(Memory::total_freed() == before, "self-move released a live payload");
        for (size_t index = 0; index < displaced.size(); ++index) {
            require(displaced[index].get_as<uint64_t>() == 100 + index,
                "move assignment invalidated a retained destination payload");
            displaced[index].free();
            const size_t expected = before + (index + 1) * 64;
            require(frees_reached(expected) == expected,
                "move assignment leaked or duplicated a destination payload reference");
        }
        uint32_t key = 0;
        Chomp popped;
        require(destination.pop(key, popped) && key == 0
            && popped.get_as<uint64_t>() == 200 && popped.raw() == transferred_payload,
            "move assignment or self-move invalidated the transferred payload");
        require(Memory::total_freed() == before + 192,
            "pop released a transferred payload");
        popped.free();
        require(frees_reached(before + 256) == before + 256,
            "the popped transferred payload was not released exactly once");
    }
    require(frees_reached(before + 384) == before + 384,
        "move-assigned queue destruction leaked or duplicated a payload");
}
/** --------------------------------------------------------------------------------------------------------- Erased Words
 * @brief Checks the raw word contract, displaced-word returns, storage adoption, and rejections.
 */
void erased_words() {
    LOG_INFO_STREAM << "Checking raw slot words, displaced returns, and adopted storage";
    require(PriorityChomp::key_of(PriorityChomp::pack(0xABCDu, 7)) == 0xABCDu
        && PriorityChomp::value_of(PriorityChomp::pack(0xABCDu, 7)) == 7,
        "pack does not round-trip");
    PriorityChomp queue(2);
    require(queue.push(PriorityChomp::pack(5, 1)) == PriorityChomp::EMPTY,
        "first push did not take a free slot");
    require(queue.push(PriorityChomp::pack(9, 2)) == PriorityChomp::EMPTY,
        "second push did not take a free slot");
    require(queue.push(PriorityChomp::pack(9, 3)) == PriorityChomp::pack(9, 3),
        "an equal key with a larger value entered");
    require(queue.push(PriorityChomp::pack(9, 1)) == PriorityChomp::pack(9, 2),
        "an equal key with a smaller value lost");
    require(queue.push(PriorityChomp::pack(1, 8)) == PriorityChomp::pack(9, 1),
        "a better key did not evict the worst");
    require(queue.worst() == PriorityChomp::pack(5, 1)
        && queue.best() == PriorityChomp::pack(1, 8),
        "peeks are wrong");
    require(queue.take(0) == PriorityChomp::pack(5, 1) && queue.take(0) == PriorityChomp::EMPTY,
        "take did not free slot zero");
    require(queue.size() == 1 && queue.worst() == PriorityChomp::EMPTY,
        "a freed slot is not reported free");
    queue.clear();
    require(queue.empty() && queue.pop() == PriorityChomp::EMPTY, "clear left an entry");
    PriorityChomp gaps(3);
    for (uint32_t key = 1; key <= 3; ++key) gaps.push(PriorityChomp::pack(key, key));
    require(gaps.take(0) == PriorityChomp::pack(1, 1)
        && gaps.take(1) == PriorityChomp::pack(2, 2), "taking two entries lost a slot");
    require(gaps.push(PriorityChomp::pack(4, 4)) == PriorityChomp::EMPTY
        && gaps.push(PriorityChomp::pack(5, 5)) == PriorityChomp::EMPTY
        && gaps.full(), "two freed slots were not reused");
    Chomp words(PriorityChomp::header_bytes(8) + 8 * sizeof(uint64_t));
    words.data<uint64_t>()[PriorityChomp::CAPACITY_WORD] = 8;
    uint64_t* raw = words.data<uint64_t>() + PriorityChomp::header_bytes(8) / sizeof(uint64_t);
    std::fill_n(raw, 8, PriorityChomp::EMPTY);
    raw[1] = PriorityChomp::pack(5, 1);
    raw[2] = PriorityChomp::EMPTY;
    raw[3] = PriorityChomp::pack(2, 9);
    PriorityChompT<uint32_t, uint32_t> adopted(words.chomp());
    require(adopted.capacity() == 8 && adopted.size() == 2,
        "adopted storage was not read in place");
    require(adopted.storage().raw() == words.raw(), "storage view does not share the words");
    uint32_t key = 0;
    uint32_t value = 0;
    require(adopted.pop(key, value) && key == 2 && value == 9,
        "adopted words popped out of order");
    require(raw[3] == PriorityChomp::EMPTY, "pop did not free the adopted slot in place");
    PriorityChomp odd(3);
    Chomp owned = payload(71);
    odd.push(PriorityChomp::pack(7, ChompHandle::detach(std::move(owned))));
    PriorityChompT<uint32_t, Chomp> owned_adopted(odd.storage());
    require(owned_adopted.capacity() == 3 && owned_adopted.size() == 1,
        "odd-capacity adoption interpreted padding as owned payloads");
    Chomp adopted_payload;
    require(owned_adopted.pop(key, adopted_payload) && key == 7
        && adopted_payload.get_as<uint64_t>() == 71 && owned_adopted.empty(),
        "odd-capacity storage adoption changed the owned payload");
    require_throws([] { PriorityChomp zero(0); }, "a zero capacity was accepted");
    require_throws([] {
        Chomp header(PriorityChomp::HEADER_BYTES);
        PriorityChomp tiny(std::move(header));
    },
        "storage without a slot word was accepted");
}
/** --------------------------------------------------------------------------------------------------------- Threshold Reset
 * @brief Checks that the constant-time rejection tracks evictions and reopens after pops and
 * takes.
 */
void threshold_reset() {
    LOG_INFO_STREAM << "Checking the cached threshold across evictions, pops, and takes";
    PriorityChompT<uint32_t, uint32_t> queue(4);
    for (uint32_t key : {10u, 20u, 30u, 40u}) queue.push(key, key);
    require(!queue.push(50, 50) && !queue.accepts(40),
        "a rejecting scan did not arm the threshold");
    require(queue.push(5, 5) && !queue.accepts(30) && queue.accepts(29),
        "the threshold did not follow the eviction");
    require(queue.push(25, 25) && !queue.accepts(25) && queue.accepts(24),
        "a second eviction did not tighten the threshold");
    uint32_t key = 0;
    uint32_t value = 0;
    require(queue.pop(key, value) && key == 5 && queue.accepts(1000),
        "a pop did not reopen the queue");
    require(queue.push(1000, 1000) && queue.full(),
        "the refill did not take the freed slot");
    require(!queue.push(1000, 1000) && !queue.accepts(1000) && queue.accepts(999),
        "the rejection after a refill did not re-arm");
    require(queue.push(999, 999) && !queue.accepts(999) && queue.accepts(998),
        "the eviction after a refill did not re-arm");
    require(queue.take(0) == PriorityChomp::pack(10, 10) && queue.accepts(5000),
        "a take did not reopen the queue");
    require(queue.push(5000, 5000) && queue.full(), "the taken slot was not refilled");
    const uint32_t expected[4] = {20, 25, 999, 5000};
    for (uint32_t want : expected) {
        require(queue.pop(key, value) && key == want,
            "threshold bookkeeping changed the contents");
    }
    require(!queue.pop(key, value) && queue.empty(), "a drained queue still pops");
}
/** --------------------------------------------------------------------------------------------------------- Threshold Blocks
 * @brief Checks that the block caches keep the threshold exact across fills, pops, and evictions
 * once the capacity spans more than one block.
 */
void threshold_blocks() {
    LOG_INFO_STREAM << "Checking block-cached thresholds across two blocks";
    PriorityChompT<uint32_t, uint32_t> queue(64);
    for (uint32_t key = 0; key < 64; ++key) queue.push(key, key);
    require(queue.full() && !queue.accepts(63) && queue.accepts(62),
        "filling both blocks did not arm the threshold");
    uint32_t key = 0;
    uint32_t value = 0;
    require(queue.pop(key, value) && key == 0 && queue.accepts(1000),
        "a pop did not reopen the queue");
    require(queue.push(1000, 1000) && queue.full() && !queue.accepts(1000) && queue.accepts(999),
        "the refill did not re-arm the threshold from the caches");
    require(queue.push(0, 100) && !queue.accepts(63) && queue.accepts(62),
        "the eviction did not re-arm the threshold from the other block");
    for (uint32_t expected = 0; expected < 64; ++expected) {
        require(queue.pop(key, value) && key == expected, "block caches reordered the pops");
    }
    require(!queue.pop(key, value) && queue.empty(), "a drained two-block queue still pops");
}
/** --------------------------------------------------------------------------------------------------------- Concurrent Push Only
 * @brief Checks that concurrent pushers leave exactly the smallest keys, none lost or duplicated.
 */
void concurrent_push_only() {
    const size_t threads = std::clamp<size_t>(std::thread::hardware_concurrency(), 4, 16);
    constexpr size_t per_thread = 20000;
    constexpr size_t capacity = 256;
    LOG_INFO_STREAM << "Checking exact trimming under " << threads << " concurrent pushers";
    PriorityChompT<uint32_t, uint32_t> queue(capacity);
    std::barrier<> gate{static_cast<std::ptrdiff_t>(threads)};
    std::vector<std::thread> workers;
    for (size_t thread = 0; thread < threads; ++thread) {
        workers.emplace_back([&queue, &gate, threads, thread] {
            gate.arrive_and_wait();
            for (size_t step = 0; step < per_thread; ++step) {
                const size_t rank = (step * 7919) % per_thread;
                const uint32_t key = static_cast<uint32_t>(rank * threads + thread);
                queue.push(key, key);
            }
        });
    }
    for (std::thread& worker : workers) worker.join();
    require(queue.size() == capacity && queue.full(), "concurrent pushes did not fill the queue");
    for (uint32_t expected = 0; expected < capacity; ++expected) {
        uint32_t key = 0;
        uint32_t value = 0;
        require(queue.pop(key, value) && key == expected && value == key,
            "concurrent pushes lost or reordered a key");
    }
    require(queue.empty(), "concurrent pushes left a phantom entry");
}
/** --------------------------------------------------------------------------------------------------------- Concurrent Mixed
 * @brief Checks that every pushed word ends up popped, evicted, rejected, or queued exactly once
 * under concurrent pushers and poppers.
 */
void concurrent_mixed() {
    const size_t producers = std::clamp<size_t>(std::thread::hardware_concurrency() / 2, 2, 8);
    const size_t consumers = producers;
    constexpr size_t per_producer = 50000;
    const size_t total = producers * per_producer;
    LOG_INFO_STREAM << "Checking exact-once accounting under "
        << producers << " pushers and " << consumers << " poppers";
    PriorityChomp queue(64);
    std::vector<std::atomic<uint8_t>> seen(total);
    std::atomic<size_t> producers_done{0};
    std::barrier<> gate{static_cast<std::ptrdiff_t>(producers + consumers)};
    std::vector<std::thread> workers;
    for (size_t producer = 0; producer < producers; ++producer) {
        workers.emplace_back([&, producer] {
            gate.arrive_and_wait();
            for (size_t step = 0; step < per_producer; ++step) {
                const uint32_t id = static_cast<uint32_t>(producer * per_producer + step);
                const uint64_t word = PriorityChomp::pack(id * 2654435761u, id);
                const uint64_t displaced = queue.push(word);
                if (displaced == PriorityChomp::EMPTY) continue;
                seen[PriorityChomp::value_of(displaced)].fetch_add(1);
            }
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }
    for (size_t consumer = 0; consumer < consumers; ++consumer) {
        workers.emplace_back([&] {
            gate.arrive_and_wait();
            while (true) {
                const uint64_t word = queue.pop();
                if (word != PriorityChomp::EMPTY) {
                    seen[PriorityChomp::value_of(word)].fetch_add(1);
                    continue;
                }
                if (producers_done.load(std::memory_order_acquire) == producers) break;
                std::this_thread::yield();
            }
        });
    }
    for (std::thread& worker : workers) worker.join();
    size_t remaining = 0;
    for (uint64_t word = queue.pop(); word != PriorityChomp::EMPTY; word = queue.pop()) {
        seen[PriorityChomp::value_of(word)].fetch_add(1);
        ++remaining;
    }
    require(remaining <= 64, "more entries drained than the queue holds");
    for (size_t id = 0; id < total; ++id) {
        require(seen[id].load() == 1, "a pushed word was lost or delivered twice");
    }
}
/** --------------------------------------------------------------------------------------------------------- Concurrent Chomps
 * @brief Checks that owned Chomps are released exactly once across concurrent pushes and pops.
 */
void concurrent_chomps() {
    const size_t producers = std::clamp<size_t>(std::thread::hardware_concurrency() / 2, 2, 8);
    const size_t consumers = producers;
    constexpr size_t per_producer = 2000;
    const size_t total = producers * per_producer;
    LOG_INFO_STREAM << "Checking Chomp release accounting under "
        << producers << " pushers and " << consumers << " poppers";
    Chomp warmup(64);
    const size_t before = Memory::total_freed();
    std::atomic<size_t> producers_done{0};
    {
        PriorityChompT<uint32_t, Chomp> queue(32);
        std::barrier<> gate{static_cast<std::ptrdiff_t>(producers + consumers)};
        std::vector<std::thread> workers;
        for (size_t producer = 0; producer < producers; ++producer) {
            workers.emplace_back([&, producer] {
                gate.arrive_and_wait();
                for (size_t step = 0; step < per_producer; ++step) {
                    const uint32_t id = static_cast<uint32_t>(producer * per_producer + step);
                    queue.push(id * 2654435761u, payload(id));
                }
                producers_done.fetch_add(1, std::memory_order_release);
            });
        }
        for (size_t consumer = 0; consumer < consumers; ++consumer) {
            workers.emplace_back([&] {
                gate.arrive_and_wait();
                while (true) {
                    uint32_t key = 0;
                    Chomp entry;
                    if (queue.pop(key, entry)) {
                        const uint32_t id = static_cast<uint32_t>(entry.get_as<uint64_t>());
                        require(id * 2654435761u == key,
                            "popped Chomp does not match its key");
                        continue;
                    }
                    if (producers_done.load(std::memory_order_acquire) == producers) break;
                    std::this_thread::yield();
                }
            });
        }
        for (std::thread& worker : workers) worker.join();
    }
    const size_t expected = before + total * 64;
    require(frees_reached(expected) == expected,
        "concurrent Chomp entries were leaked or released twice");
}
} // namespace

int main() {
    natural_order();
    comparator_order();
    key_encodings();
    chomp_ownership();
    chomp_move_assignment<PriorityChompT<uint32_t, Chomp>>();
    chomp_move_assignment<HeapChompT<uint32_t, Chomp>>();
    erased_words();
    threshold_reset();
    threshold_blocks();
    concurrent_push_only();
    concurrent_mixed();
    concurrent_chomps();
    LOG_INFO_STREAM << "PriorityChomp contracts hold";
    return 0;
}
