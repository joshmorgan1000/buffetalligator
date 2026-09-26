/** --------------------------------------------------------------------------------------------------------- Priority Slice Test
 * @file priority_slice_test.cpp
 * @brief Checks PrioritySlice ordering, key encodings, eviction, Slice ownership, adoption, and
 * concurrent exact-once accounting.
 */
#include <alligator.hpp>
#include <memory/tracker.hpp>
#include "functional_support.hpp"
#include <algorithm>
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
 * @return The payload Slice.
 */
Slice payload(uint64_t id) {
    Slice slice(64, true);
    slice.get_as<uint64_t>() = id;
    return slice;
}
/** --------------------------------------------------------------------------------------------------------- Natural Order
 * @brief Checks that a float queue keeps the smallest keys, reports its worst, and pops ascending.
 */
void natural_order() {
    LOG_INFO_STREAM << "Checking natural float order, trimming, and ascending pops";
    PrioritySliceT<float, uint32_t> queue(8);
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
    PrioritySliceT<int32_t, uint32_t, &descending> queue(5);
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
    PrioritySliceT<int16_t, uint32_t> narrow(6);
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
    PrioritySliceT<uint8_t, uint32_t> bytes(4);
    const uint8_t byte_keys[4] = {255, 0, 128, 1};
    for (uint32_t index = 0; index < 4; ++index) bytes.push(byte_keys[index], index);
    const uint8_t byte_order[4] = {0, 1, 128, 255};
    for (uint8_t want : byte_order) {
        uint8_t key = 0;
        uint32_t value = 0;
        require(bytes.pop(key, value) && key == want, "uint8 keys popped out of order");
    }
    enum class Level : uint16_t { low = 1, mid = 500, high = 60000 };
    PrioritySliceT<Level, uint32_t> levels(3);
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
    PrioritySliceT<float, uint32_t> floats(8);
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
/** --------------------------------------------------------------------------------------------------------- Slice Ownership
 * @brief Checks that evicted, rejected, cleared, and destroyed Slices are released exactly once
 * while popped and viewed ones stay alive.
 */
void slice_ownership() {
    LOG_INFO_STREAM << "Checking Slice ownership across eviction, pop, clear, and destruction";
    Slice warmup(64);
    const size_t before = Memory::total_freed();
    {
        PrioritySliceT<float, Slice> queue(4);
        for (uint64_t id = 0; id < 6; ++id) {
            Slice entry = payload(id);
            const bool entered = queue.push(static_cast<float>(id), std::move(entry));
            require(entry.is_null(), "push left the caller holding the Slice");
            require(entered == (id < 4), "eviction accepted a worse Slice");
        }
        require(frees_reached(before + 128) == before + 128, "rejected Slices were not released");
        float key = 0.0f;
        require(queue.best(key) && key == 0.0f, "best key is wrong");
        Slice popped;
        require(queue.pop(key, popped) && key == 0.0f && popped.get_as<uint64_t>() == 0,
            "pop handed over the wrong Slice");
        require(queue.size() == 3 && Memory::total_freed() == before + 128,
            "pop released the popped Slice");
        popped.free();
        require(frees_reached(before + 192) == before + 192,
            "the popped Slice was not released by its owner");
        PrioritySliceT<float, Slice> moved(std::move(queue));
        require(queue.capacity() == 0 && moved.size() == 3, "move did not transfer the entries");
        queue.clear();
        moved.clear();
        require(moved.empty(), "clear left entries queued");
        require(frees_reached(before + 384) == before + 384,
            "clear did not release the queued Slices");
        moved.push(9.0f, payload(9));
    }
    require(frees_reached(before + 448) == before + 448,
        "destruction did not release the queued Slice");
}
/** --------------------------------------------------------------------------------------------------------- Erased Words
 * @brief Checks the raw word contract, displaced-word returns, storage adoption, and rejections.
 */
void erased_words() {
    LOG_INFO_STREAM << "Checking raw slot words, displaced returns, and adopted storage";
    require(PrioritySlice::key_of(PrioritySlice::pack(0xABCDu, 7)) == 0xABCDu
        && PrioritySlice::value_of(PrioritySlice::pack(0xABCDu, 7)) == 7,
        "pack does not round-trip");
    PrioritySlice queue(2);
    require(queue.push(PrioritySlice::pack(5, 1)) == PrioritySlice::EMPTY,
        "first push did not take a free slot");
    require(queue.push(PrioritySlice::pack(9, 2)) == PrioritySlice::EMPTY,
        "second push did not take a free slot");
    require(queue.push(PrioritySlice::pack(9, 3)) == PrioritySlice::pack(9, 3),
        "an equal key with a larger value entered");
    require(queue.push(PrioritySlice::pack(9, 1)) == PrioritySlice::pack(9, 2),
        "an equal key with a smaller value lost");
    require(queue.push(PrioritySlice::pack(1, 8)) == PrioritySlice::pack(9, 1),
        "a better key did not evict the worst");
    require(queue.worst() == PrioritySlice::pack(5, 1)
        && queue.best() == PrioritySlice::pack(1, 8),
        "peeks are wrong");
    require(queue.take(0) == PrioritySlice::pack(5, 1) && queue.take(0) == PrioritySlice::EMPTY,
        "take did not free slot zero");
    require(queue.size() == 1 && queue.worst() == PrioritySlice::EMPTY,
        "a freed slot is not reported free");
    queue.clear();
    require(queue.empty() && queue.pop() == PrioritySlice::EMPTY, "clear left an entry");
    PrioritySlice gaps(3);
    for (uint32_t key = 1; key <= 3; ++key) gaps.push(PrioritySlice::pack(key, key));
    require(gaps.take(0) == PrioritySlice::pack(1, 1)
        && gaps.take(1) == PrioritySlice::pack(2, 2), "taking two entries lost a slot");
    require(gaps.push(PrioritySlice::pack(4, 4)) == PrioritySlice::EMPTY
        && gaps.push(PrioritySlice::pack(5, 5)) == PrioritySlice::EMPTY
        && gaps.full(), "two freed slots were not reused");
    Slice words(PrioritySlice::header_bytes(4) + 4 * sizeof(uint64_t));
    uint64_t* raw = words.data<uint64_t>() + PrioritySlice::header_bytes(4) / sizeof(uint64_t);
    raw[0] = PrioritySlice::EMPTY;
    raw[1] = PrioritySlice::pack(5, 1);
    raw[2] = PrioritySlice::EMPTY;
    raw[3] = PrioritySlice::pack(2, 9);
    PrioritySliceT<uint32_t, uint32_t> adopted(words.slice());
    require(adopted.capacity() == 4 && adopted.size() == 2,
        "adopted storage was not read in place");
    require(adopted.storage().raw() == words.raw(), "storage view does not share the words");
    uint32_t key = 0;
    uint32_t value = 0;
    require(adopted.pop(key, value) && key == 2 && value == 9,
        "adopted words popped out of order");
    require(raw[3] == PrioritySlice::EMPTY, "pop did not free the adopted slot in place");
    require_throws([] { PrioritySlice zero(0); }, "a zero capacity was accepted");
    require_throws([] {
        Slice header(PrioritySlice::HEADER_BYTES);
        PrioritySlice tiny(std::move(header));
    },
        "storage without a slot word was accepted");
}
/** --------------------------------------------------------------------------------------------------------- Threshold Reset
 * @brief Checks that the constant-time rejection tracks evictions and reopens after pops and
 * takes.
 */
void threshold_reset() {
    LOG_INFO_STREAM << "Checking the cached threshold across evictions, pops, and takes";
    PrioritySliceT<uint32_t, uint32_t> queue(4);
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
    require(queue.take(0) == PrioritySlice::pack(10, 10) && queue.accepts(5000),
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
    PrioritySliceT<uint32_t, uint32_t> queue(64);
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
    PrioritySliceT<uint32_t, uint32_t> queue(capacity);
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
    PrioritySlice queue(64);
    std::vector<std::atomic<uint8_t>> seen(total);
    std::atomic<size_t> producers_done{0};
    std::barrier<> gate{static_cast<std::ptrdiff_t>(producers + consumers)};
    std::vector<std::thread> workers;
    for (size_t producer = 0; producer < producers; ++producer) {
        workers.emplace_back([&, producer] {
            gate.arrive_and_wait();
            for (size_t step = 0; step < per_producer; ++step) {
                const uint32_t id = static_cast<uint32_t>(producer * per_producer + step);
                const uint64_t word = PrioritySlice::pack(id * 2654435761u, id);
                const uint64_t displaced = queue.push(word);
                if (displaced == PrioritySlice::EMPTY) continue;
                seen[PrioritySlice::value_of(displaced)].fetch_add(1);
            }
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }
    for (size_t consumer = 0; consumer < consumers; ++consumer) {
        workers.emplace_back([&] {
            gate.arrive_and_wait();
            while (true) {
                const uint64_t word = queue.pop();
                if (word != PrioritySlice::EMPTY) {
                    seen[PrioritySlice::value_of(word)].fetch_add(1);
                    continue;
                }
                if (producers_done.load(std::memory_order_acquire) == producers) break;
                std::this_thread::yield();
            }
        });
    }
    for (std::thread& worker : workers) worker.join();
    size_t remaining = 0;
    for (uint64_t word = queue.pop(); word != PrioritySlice::EMPTY; word = queue.pop()) {
        seen[PrioritySlice::value_of(word)].fetch_add(1);
        ++remaining;
    }
    require(remaining <= 64, "more entries drained than the queue holds");
    for (size_t id = 0; id < total; ++id) {
        require(seen[id].load() == 1, "a pushed word was lost or delivered twice");
    }
}
/** --------------------------------------------------------------------------------------------------------- Concurrent Slices
 * @brief Checks that owned Slices are released exactly once across concurrent pushes and pops.
 */
void concurrent_slices() {
    const size_t producers = std::clamp<size_t>(std::thread::hardware_concurrency() / 2, 2, 8);
    const size_t consumers = producers;
    constexpr size_t per_producer = 2000;
    const size_t total = producers * per_producer;
    LOG_INFO_STREAM << "Checking Slice release accounting under "
        << producers << " pushers and " << consumers << " poppers";
    Slice warmup(64);
    const size_t before = Memory::total_freed();
    std::atomic<size_t> producers_done{0};
    {
        PrioritySliceT<uint32_t, Slice> queue(32);
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
                    Slice entry;
                    if (queue.pop(key, entry)) {
                        const uint32_t id = static_cast<uint32_t>(entry.get_as<uint64_t>());
                        require(id * 2654435761u == key,
                            "popped Slice does not match its key");
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
        "concurrent Slice entries were leaked or released twice");
}
} // namespace

int main() {
    natural_order();
    comparator_order();
    key_encodings();
    slice_ownership();
    erased_words();
    threshold_reset();
    threshold_blocks();
    concurrent_push_only();
    concurrent_mixed();
    concurrent_slices();
    LOG_INFO_STREAM << "PrioritySlice contracts hold";
    return 0;
}
