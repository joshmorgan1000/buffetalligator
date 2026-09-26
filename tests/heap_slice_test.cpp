/** --------------------------------------------------------------------------------------------------------- Heap Slice Test
 * @file heap_slice_test.cpp
 * @brief Checks HeapSlice ordering against a reference multiset, eviction, key encodings, Slice
 * ownership, positional removal, and adopted storage.
 */
#include <alligator.hpp>
#include <memory/tracker.hpp>
#include "functional_support.hpp"
#include <algorithm>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

using namespace buffetalligator;
using functional::require;
using functional::require_throws;
namespace {
/** --------------------------------------------------------------------------------------------------------- Frees Reached
 * @brief Waits up to five seconds for deferred allocator teardown to reach the expected total.
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
 */
int descending(const int32_t* left, const int32_t* right) {
    return (*left < *right) - (*left > *right);
}
/** --------------------------------------------------------------------------------------------------------- Next Key
 * @brief Deterministic key sequence spread across the 32-bit range.
 */
uint32_t next_key(uint64_t& state) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<uint32_t>(state >> 32);
}
/** --------------------------------------------------------------------------------------------------------- Reference Order
 * @brief Drives a heap and a bounded multiset through the same pushes and pops and compares them.
 */
void reference_order() {
    LOG_INFO_STREAM << "Checking heap order against a bounded multiset under pushes and pops";
    for (size_t capacity : {size_t(1), size_t(2), size_t(7), size_t(64), size_t(1000)}) {
        HeapSliceT<uint32_t, uint32_t> heap(capacity);
        std::multiset<std::pair<uint32_t, uint32_t>> reference;
        uint64_t state = 42 + capacity;
        for (uint32_t step = 0; step < 20000; ++step) {
            if (step % 5 == 4) {
                uint32_t key = 0;
                uint32_t value = 0;
                const bool popped = heap.pop(key, value);
                require(popped == !reference.empty(), "heap and reference disagree on emptiness");
                if (popped) {
                    require(std::make_pair(key, value) == *reference.begin(),
                        "heap popped the wrong entry");
                    reference.erase(reference.begin());
                }
                continue;
            }
            const uint32_t key = next_key(state);
            const bool entered = heap.push(key, step);
            if (reference.size() < capacity) {
                require(entered, "heap rejected a push with room left");
                reference.emplace(key, step);
            } else if (std::make_pair(key, step) < *reference.rbegin()) {
                require(entered, "heap rejected a push that beat its worst entry");
                reference.erase(std::prev(reference.end()));
                reference.emplace(key, step);
            } else {
                require(!entered, "heap accepted a push that did not beat its worst entry");
            }
            require(heap.size() == reference.size(), "heap size drifted from the reference");
            uint32_t worst = 0;
            require(heap.worst(worst) == (reference.size() == capacity),
                "worst availability is wrong");
            if (reference.size() == capacity) {
                require(worst == reference.rbegin()->first, "heap worst key is wrong");
            }
        }
        std::vector<std::pair<uint32_t, uint32_t>> drained;
        uint32_t key = 0;
        uint32_t value = 0;
        while (heap.pop(key, value)) drained.emplace_back(key, value);
        require(std::is_sorted(drained.begin(), drained.end()), "drain was not ascending");
        require(drained.size() == reference.size(), "drain count differs from the reference");
    }
}
/** --------------------------------------------------------------------------------------------------------- Comparator Order
 * @brief Checks that a comparator flips the heap into a max-heap over signed keys.
 */
void comparator_order() {
    LOG_INFO_STREAM << "Checking a descending comparator over signed keys";
    HeapSliceT<int32_t, uint32_t, &descending> heap(5);
    const int32_t keys[12] = {3, -7, 12, 0, -1, 44, 9, -30, 12, 5, 2, 100};
    for (uint32_t index = 0; index < 12; ++index) heap.push(keys[index], index);
    int32_t worst = 0;
    require(heap.worst(worst) && worst == 9, "worst of the max-heap is not the fifth largest");
    require(!heap.push(9, 50) && !heap.accepts(9) && heap.accepts(10),
        "ties or accepts are wrong");
    const int32_t expected[5] = {100, 44, 12, 12, 9};
    for (int32_t want : expected) {
        int32_t key = 0;
        uint32_t value = 0;
        require(heap.pop(key, value) && key == want && keys[value] == key,
            "max-heap popped out of order");
    }
    int32_t key = 0;
    uint32_t value = 0;
    require(!heap.pop(key, value) && heap.empty(), "max-heap popped a phantom entry");
}
/** --------------------------------------------------------------------------------------------------------- Key Encodings
 * @brief Checks sortable encodings for signed and float keys through the shared typed layer.
 */
void key_encodings() {
    LOG_INFO_STREAM << "Checking int16 and float key encodings on the heap";
    HeapSliceT<int16_t, uint32_t> narrow(6);
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
    HeapSliceT<float, uint32_t> floats(4);
    const float float_keys[6] = {2.5f, -1.5f, 0.0f, -100.0f, 1e30f, 3.0f};
    for (uint32_t index = 0; index < 6; ++index) floats.push(float_keys[index], index);
    const float expected[4] = {-100.0f, -1.5f, 0.0f, 2.5f};
    for (float want : expected) {
        float key = 0.0f;
        uint32_t value = 0;
        require(floats.pop(key, value) && key == want, "float keys popped out of order");
    }
}
/** --------------------------------------------------------------------------------------------------------- Slice Ownership
 * @brief Checks that evicted, rejected, and cleared Slices are released exactly once while
 * popped ones transfer.
 */
void slice_ownership() {
    LOG_INFO_STREAM << "Checking Slice ownership across eviction, pop, clear, and destruction";
    Slice warmup(64);
    const size_t before = Memory::total_freed();
    {
        HeapSliceT<float, Slice> heap(4);
        for (uint64_t id = 0; id < 6; ++id) {
            Slice entry(64, true);
            entry.get_as<uint64_t>() = id;
            const bool entered = heap.push(static_cast<float>(id), std::move(entry));
            require(entry.is_null() && entered == (id < 4), "eviction accepted a worse Slice");
        }
        require(frees_reached(before + 128) == before + 128, "rejected Slices were not released");
        float key = 0.0f;
        Slice popped;
        require(heap.pop(key, popped) && key == 0.0f && popped.get_as<uint64_t>() == 0,
            "pop handed over the wrong Slice");
        popped.free();
        require(frees_reached(before + 192) == before + 192, "the popped Slice was not released");
        heap.clear();
        require(heap.empty() && frees_reached(before + 384) == before + 384,
            "clear did not release the held Slices");
        Slice last(64, true);
        heap.push(9.0f, std::move(last));
    }
    require(frees_reached(before + 448) == before + 448,
        "destruction did not release the held Slice");
}
/** --------------------------------------------------------------------------------------------------------- Erased Words
 * @brief Checks the raw word contract, positional removal, adopted storage, and rejections.
 */
void erased_words() {
    LOG_INFO_STREAM << "Checking raw words, positional removal, and adopted storage";
    HeapSlice heap(3);
    require(heap.push(HeapSlice::pack(5, 1)) == HeapSlice::EMPTY, "first push was not an insert");
    require(heap.push(HeapSlice::pack(9, 2)) == HeapSlice::EMPTY, "second push was not an insert");
    require(heap.push(HeapSlice::pack(1, 3)) == HeapSlice::EMPTY, "third push was not an insert");
    require(heap.full() && heap.worst() == HeapSlice::pack(9, 2)
        && heap.best() == HeapSlice::pack(1, 3),
        "peeks are wrong");
    require(heap.push(HeapSlice::pack(9, 3)) == HeapSlice::pack(9, 3),
        "a worse word entered a full heap");
    require(heap.push(HeapSlice::pack(4, 4)) == HeapSlice::pack(9, 2),
        "a better word did not evict the worst");
    require(heap.take(0) == HeapSlice::pack(1, 3) && heap.size() == 2,
        "take did not remove the root");
    require(heap.pop() == HeapSlice::pack(4, 4) && heap.pop() == HeapSlice::pack(5, 1),
        "pops after take are out of order");
    require(heap.pop() == HeapSlice::EMPTY && heap.take(0) == HeapSlice::EMPTY,
        "an empty heap yielded a word");
    Slice storage(HeapSlice::HEADER_BYTES + 4 * sizeof(uint64_t));
    {
        HeapSlice filler(storage.slice());
        filler.push(HeapSlice::pack(7, 7));
        filler.push(HeapSlice::pack(3, 3));
    }
    HeapSlice adopted(storage.slice());
    require(adopted.capacity() == 4 && adopted.size() == 2
        && adopted.pop() == HeapSlice::pack(3, 3),
        "adopted storage lost its heap");
    require_throws([] { HeapSlice zero(0); }, "a zero capacity was accepted");
    require_throws([] {
        Slice header(HeapSlice::HEADER_BYTES);
        HeapSlice tiny(std::move(header));
    }, "storage without a position was accepted");
}
} // namespace

int main() {
    reference_order();
    comparator_order();
    key_encodings();
    slice_ownership();
    erased_words();
    LOG_INFO_STREAM << "HeapSlice contracts hold";
    return 0;
}
