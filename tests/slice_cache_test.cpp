/** --------------------------------------------------------------------------------------------------------- Slice Cache Test
 * @file slice_cache_test.cpp
 * @brief Checks that SliceCache budgets by bytes, evicts least recently used Slices, releases
 * them exactly once, and keeps shared views alive.
 */
#include <alligator.hpp>
#include <memory/tracker.hpp>
#include "functional_support.hpp"
#include <chrono>
#include <string>
#include <thread>

using namespace buffetalligator;
using functional::require;
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
/** --------------------------------------------------------------------------------------------------------- Payload
 * @brief Creates a novel payload of the given size stamped with an identifier.
 */
Slice payload(uint64_t id, size_t bytes) {
    Slice slice(bytes, true);
    slice.get_as<uint64_t>() = id;
    return slice;
}
/** --------------------------------------------------------------------------------------------------------- Byte Budget
 * @brief Checks eviction by bytes in least recently used order, promotion on get, and release on
 * destruction.
 */
void byte_budget() {
    LOG_INFO_STREAM << "Checking byte budgets, LRU eviction, promotion, and destruction";
    Slice warmup(64);
    const size_t before = Memory::total_freed();
    {
        SliceCache<int64_t> cache(256);
        require(cache.capacity() == 256 && cache.empty() && cache.bytes() == 0,
            "a fresh cache is not empty");
        cache.set(1, payload(1, 64));
        cache.set(2, payload(2, 64));
        cache.set(3, payload(3, 64));
        require(cache.size() == 3 && cache.bytes() == 192,
            "three entries did not weigh their bytes");
        cache.set(4, payload(4, 128));
        require(!cache.exists(1) && cache.exists(2) && cache.exists(3) && cache.exists(4),
            "the cache did not evict the least recently used entry");
        require(cache.bytes() == 256 && cache.size() == 3, "eviction did not restore the budget");
        require(frees_reached(before + 64) == before + 64, "the evicted Slice was not released");
        const Slice* second = cache.get(2);
        require(second != nullptr && second->get_as<uint64_t>() == 2,
            "get returned the wrong Slice");
        cache.set(5, payload(5, 64));
        require(cache.exists(2) && !cache.exists(3),
            "get did not promote its entry ahead of eviction");
        require(cache.get(9) == nullptr && cache.peek(9) == nullptr,
            "a missing key produced a Slice");
        const Slice* fourth = cache.peek(4);
        require(fourth != nullptr && fourth->get_as<uint64_t>() == 4,
            "peek returned the wrong Slice");
        cache.set(6, payload(6, 64));
        require(!cache.exists(4) && cache.exists(2), "peek promoted its entry");
    }
    require(frees_reached(before + 448) == before + 448,
        "destruction did not release the cached Slices");
}
/** --------------------------------------------------------------------------------------------------------- Ownership
 * @brief Checks that erase, resize, clear, and destruction release Slices while views keep them.
 */
void ownership() {
    LOG_INFO_STREAM << "Checking erase, views, resize, oversize entries, clear, and destruction";
    Slice warmup(64);
    const size_t before = Memory::total_freed();
    {
        SliceCache<std::string> cache(512);
        cache.set("alpha", payload(1, 128));
        cache.set("beta", payload(2, 128));
        Slice view = cache.view("alpha");
        require(view.valid() && view.get_as<uint64_t>() == 1, "view did not share the entry");
        cache.erase("alpha");
        require(!cache.exists("alpha") && cache.bytes() == 128, "erase did not drop the entry");
        require(Memory::total_freed() == before, "erase released a Slice a view still holds");
        view.free();
        require(frees_reached(before + 128) == before + 128,
            "the last view did not release the Slice");
        cache.erase("missing");
        cache.set("gamma", payload(3, 1024));
        require(cache.size() == 1 && cache.exists("gamma") && cache.bytes() == 1024,
            "an oversize entry was not kept alone");
        require(frees_reached(before + 256) == before + 256,
            "the entry it displaced was not released");
        cache.set("delta", payload(4, 64));
        require(!cache.exists("gamma") && cache.bytes() == 64,
            "the oversize entry survived a new set");
        cache.set("epsilon", payload(5, 64));
        cache.resize(64);
        require(cache.size() == 1 && cache.exists("epsilon") && cache.capacity() == 64,
            "resize did not prune to the new budget");
        cache.clear();
        require(cache.empty() && cache.bytes() == 0, "clear left entries");
        require(frees_reached(before + 1408) == before + 1408,
            "clear and pruning did not release every Slice");
        cache.set("zeta", payload(6, 64));
    }
    require(frees_reached(before + 1472) == before + 1472,
        "destruction did not release the remaining Slice");
}
} // namespace

int main() {
    byte_budget();
    ownership();
    LOG_INFO_STREAM << "SliceCache contracts hold";
    return 0;
}
