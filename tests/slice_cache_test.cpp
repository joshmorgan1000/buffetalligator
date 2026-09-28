/** --------------------------------------------------------------------------------------------------------- Slice Cache Test
 * @file slice_cache_test.cpp
 * @brief Checks eviction policies, byte budgets, recency, ownership, moves, and cache keys.
 */
#include <alligator.hpp>
#include <memory/tracker.hpp>
#include "functional_support.hpp"
#include <chrono>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

using namespace buffetalligator;
using functional::require;
namespace cache_test {
/** --------------------------------------------------------------------------------------------------------- Collision Key
 * @brief Identifies distinct cache entries that deliberately share the same hash.
 */
struct CollisionKey {
    int64_t value;
    bool operator==(const CollisionKey&) const = default;
};
/** --------------------------------------------------------------------------------------------------------- Exceptional Key
 * @brief Raises controlled errors while copying, hashing, or comparing a selected cache key.
 */
struct ExceptionalKey {
    inline static int64_t failed_copy = -1;
    inline static int64_t failed_hash = -1;
    inline static int64_t failed_equality = -1;
    int64_t value;
    explicit ExceptionalKey(int64_t identifier) : value(identifier) {}
    ExceptionalKey(const ExceptionalKey& other) : value(other.value) {
        if (value == failed_copy) throw std::runtime_error("key copy failed");
    }
    bool operator==(const ExceptionalKey& other) const {
        if (value == failed_equality && other.value == failed_equality) {
            throw std::runtime_error("key equality failed");
        }
        return value == other.value;
    }
};
}
namespace std {
/** --------------------------------------------------------------------------------------------------------- Collision Hash
 * @brief Places every collision-test key in the same hash bucket.
 */
template<>
struct hash<cache_test::CollisionKey> {
    size_t operator()(const cache_test::CollisionKey&) const noexcept { return 7; }
};
/** --------------------------------------------------------------------------------------------------------- Exceptional Hash
 * @brief Hashes test identifiers while allowing a selected key to reject hashing.
 */
template<>
struct hash<cache_test::ExceptionalKey> {
    size_t operator()(const cache_test::ExceptionalKey& key) const {
        if (key.value == cache_test::ExceptionalKey::failed_hash) {
            throw std::runtime_error("key hash failed");
        }
        return static_cast<size_t>(key.value);
    }
};
}
namespace {
static_assert(!std::is_copy_constructible_v<SliceCache<>>);
static_assert(!std::is_copy_assignable_v<SliceCache<>>);
static_assert(std::is_nothrow_move_constructible_v<SliceCache<>>);
static_assert(std::is_nothrow_move_assignable_v<SliceCache<>>);
static_assert(std::bidirectional_iterator<SliceCache<>::const_iterator>);
/** --------------------------------------------------------------------------------------------------------- Select Most Recent
 * @brief Selects the most recently used eligible entry for eviction.
 */
template<typename Key>
SliceCacheIterator<Key> select_most_recent(
    SliceCacheIterator<Key> first,
    SliceCacheIterator<Key>
) noexcept {
    return first;
}
/** --------------------------------------------------------------------------------------------------------- Boundary Eviction
 * @brief Selects a marked newest candidate or the oldest candidate from immutable entry data.
 */
struct BoundaryEviction {
    inline static size_t selections = 0;
    /** ------------------------------------------------------------------------------------------- Select
     * @brief Chooses the newest entry only when both its key and payload carry the marker.
     */
    static SliceCacheIterator<int64_t> select(
        SliceCacheIterator<int64_t> first,
        SliceCacheIterator<int64_t> last
    ) noexcept {
        ++selections;
        if (first->first == 7 && first->second.get_as<uint64_t>() == 70) return first;
        return --last;
    }
};
using MostRecentCache = SliceCache<int64_t, select_most_recent<int64_t>>;
static_assert(std::is_nothrow_move_constructible_v<MostRecentCache>);
static_assert(std::is_nothrow_move_assignable_v<MostRecentCache>);
static_assert(std::is_same_v<MostRecentCache::const_iterator, SliceCacheIterator<int64_t>>);
/** --------------------------------------------------------------------------------------------------------- Require Order
 * @brief Checks the public iteration order against the expected most-to-least-recent keys.
 */
template<typename Key, auto Evict>
void require_order(const SliceCache<Key, Evict>& cache, std::initializer_list<Key> expected) {
    auto entry = cache.begin();
    for (const Key& key : expected) {
        require(entry != cache.end(), "iteration ended before every expected entry");
        require(entry->first == key, "iteration did not follow most-recent-first order");
        ++entry;
    }
    require(entry == cache.end(), "iteration included an unexpected entry");
}
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
/** --------------------------------------------------------------------------------------------------------- Replacement
 * @brief Checks replacement weights and promotion while preserving unaffected entries.
 */
void replacement() {
    LOG_INFO_STREAM << "Checking replacement weights, promotion, and oversize replacements";
    SliceCache<> cache(256);
    cache.set(1, payload(1, 64));
    cache.set(2, payload(2, 64));
    cache.set(3, payload(3, 64));
    cache.set(1, payload(11, 128));
    require(cache.size() == 3 && cache.bytes() == 256,
        "a larger replacement counted its previous weight");
    require(cache.peek(1)->get_as<uint64_t>() == 11, "replacement retained its old payload");
    require_order<int64_t>(cache, {1, 3, 2});
    cache.set(4, payload(4, 64));
    require(!cache.exists(2) && cache.exists(1) && cache.bytes() == 256,
        "replacement did not protect its entry through the next eviction");
    cache.set(1, payload(12, 64));
    require(cache.size() == 3 && cache.bytes() == 192,
        "a smaller replacement failed to subtract its previous weight");
    require_order<int64_t>(cache, {1, 4, 3});
    cache.set(5, payload(5, 64));
    require(cache.size() == 4 && cache.bytes() == 256,
        "a smaller replacement did not make its bytes available");
    cache.set(6, payload(6, 64));
    require(!cache.exists(3) && cache.exists(1), "replacement did not promote its entry");
    require_order<int64_t>(cache, {6, 5, 1, 4});
    cache.set(4, payload(44, 512));
    require(cache.size() == 1 && cache.bytes() == 512 && cache.exists(4),
        "an oversize replacement was not kept alone");
    cache.set(4, payload(45, 64));
    require(cache.size() == 1 && cache.bytes() == 64,
        "shrinking an oversize replacement left excess weight");
}
/** --------------------------------------------------------------------------------------------------------- Iteration And Recency
 * @brief Checks iterator pairs, promotion, and observation without changing recency.
 */
void iteration_and_recency() {
    LOG_INFO_STREAM << "Checking cache iteration and observing entries without promotion";
    SliceCache<> cache(192);
    cache.set(1, payload(1, 64));
    cache.set(2, payload(2, 64));
    cache.set(3, payload(3, 64));
    const SliceCache<>& observed = cache;
    require_order<int64_t>(observed, {3, 2, 1});
    auto current = observed.begin();
    const auto previous = current++;
    require(previous->first == 3 && current->first == 2,
        "postfix iteration did not retain the previous entry");
    require((*previous).second.get_as<uint64_t>() == 3,
        "iteration did not expose the cached Slice");
    auto last = observed.end();
    --last;
    require(last->first == 1, "reverse iteration did not reach the least-recent entry");
    require(observed.peek(1)->get_as<uint64_t>() == 1 && observed.exists(2),
        "observation lost a cached entry");
    require(cache.get(9) == nullptr && observed.peek(9) == nullptr && !observed.exists(9),
        "an absent key appeared in the cache");
    require(!cache.view(9).valid(), "an absent key returned an owning view");
    require_order<int64_t>(observed, {3, 2, 1});
    cache.set(4, payload(4, 64));
    require(!cache.exists(1), "peek or exists promoted an entry");
    require(cache.get(2)->get_as<uint64_t>() == 2, "get returned an unrelated entry");
    require_order<int64_t>(observed, {2, 4, 3});
    Slice shared = cache.view(3);
    require(shared.valid() && shared.get_as<uint64_t>() == 3,
        "view returned an unrelated entry");
    require_order<int64_t>(observed, {3, 2, 4});
    cache.erase(2);
    cache.erase(9);
    require_order<int64_t>(observed, {3, 4});
    cache.clear();
    require(observed.begin() == observed.end(), "empty iteration retained an entry");
}
/** --------------------------------------------------------------------------------------------------------- Zero Budget And Resize
 * @brief Checks zero-weight entries and strict resizing of protected oversize entries.
 */
void zero_budget_and_resize() {
    LOG_INFO_STREAM << "Checking zero budgets, zero-weight entries, and strict resize pruning";
    SliceCache<> cache(0);
    cache.set(1, payload(1, 64));
    require(cache.capacity() == 0 && cache.size() == 1 && cache.bytes() == 64,
        "a zero budget did not protect its newly set entry");
    cache.set(2, payload(2, 128));
    require(!cache.exists(1) && cache.exists(2) && cache.bytes() == 128,
        "a zero budget retained an older oversize entry");
    cache.resize(0);
    require(cache.empty() && cache.bytes() == 0,
        "resize did not remove an oversize entry at the unchanged budget");
    cache.set(1, Slice());
    cache.set(2, Slice());
    cache.resize(0);
    require(cache.size() == 2 && cache.bytes() == 0 && cache.exists(1) && cache.exists(2),
        "a zero budget removed zero-weight entries");
    require(cache.peek(1) != nullptr && !cache.peek(1)->valid(),
        "a cached null Slice was mistaken for a missing key");
    cache.set(3, payload(3, 64));
    require(cache.size() == 1 && cache.exists(3),
        "an oversize entry did not evict older zero-weight entries");
    cache.resize(128);
    cache.set(4, payload(4, 64));
    cache.resize(256);
    require(cache.capacity() == 256 && cache.bytes() == 128 && cache.size() == 2,
        "increasing the budget lost entries or changed their weight");
    require_order<int64_t>(cache, {4, 3});
    cache.resize(32);
    require(cache.capacity() == 32 && cache.empty() && cache.bytes() == 0,
        "resize protected an entry larger than the new budget");
}
/** --------------------------------------------------------------------------------------------------------- Moves
 * @brief Checks ownership transfer, recency, borrowed pointers, and reuse after moving.
 */
void moves() {
    LOG_INFO_STREAM << "Checking cache moves, self-move, and reuse of moved-from caches";
    SliceCache<> source(128);
    source.set(1, payload(1, 64));
    source.set(2, payload(2, 64));
    const Slice* borrowed = source.get(1);
    SliceCache<> moved(std::move(source));
    require(moved.capacity() == 128 && moved.bytes() == 128 && moved.size() == 2,
        "move construction lost the budget or entries");
    require_order<int64_t>(moved, {1, 2});
    require(moved.peek(1) == borrowed && borrowed->get_as<uint64_t>() == 1,
        "move construction invalidated a borrowed Slice");
    require(source.empty() && source.bytes() == 0 && source.capacity() == 0,
        "move construction left the source owning entries or a budget");
    source.set(3, payload(3, 64));
    require(source.size() == 1 && source.bytes() == 64 && source.exists(3),
        "a move-constructed source could not be reused");
    SliceCache<> destination(256);
    destination.set(8, payload(8, 64));
    Slice displaced = destination.view(8);
    destination = std::move(moved);
    require(destination.capacity() == 128 && destination.bytes() == 128,
        "move assignment retained the destination budget or weight");
    require(!destination.exists(8) && destination.peek(1) == borrowed,
        "move assignment retained old entries or invalidated a borrowed Slice");
    require(displaced.valid() && displaced.get_as<uint64_t>() == 8,
        "move assignment invalidated a view of its previous contents");
    require_order<int64_t>(destination, {1, 2});
    require(moved.empty() && moved.bytes() == 0 && moved.capacity() == 0,
        "move assignment left its source owning entries or a budget");
    moved.set(4, payload(4, 64));
    moved.resize(64);
    require(moved.size() == 1 && moved.bytes() == 64 && moved.exists(4),
        "a move-assigned source could not be reused");
    SliceCache<>& same = destination;
    destination = std::move(same);
    require(destination.capacity() == 128 && destination.bytes() == 128,
        "self-move changed the budget or weight");
    require_order<int64_t>(destination, {1, 2});
    destination.set(5, payload(5, 64));
    require(destination.exists(1) && !destination.exists(2),
        "moves changed the eviction order");
    SliceCache<> empty(16);
    SliceCache<> empty_moved(std::move(empty));
    require(empty_moved.empty() && empty_moved.capacity() == 16
        && empty_moved.begin() == empty_moved.end(),
        "moving an empty cache created entries or lost its budget");
    require(empty.empty() && empty.capacity() == 0 && empty.begin() == empty.end(),
        "moving an empty cache left an invalid source");
    destination = std::move(empty_moved);
    require(destination.empty() && destination.bytes() == 0 && destination.capacity() == 16
        && destination.begin() == destination.end(),
        "assigning an empty cache retained entries or its previous budget");
    destination.set(6, payload(6, 64));
    require(destination.size() == 1 && destination.bytes() == 64 && destination.exists(6),
        "an empty move-assigned cache could not be reused");
}
/** --------------------------------------------------------------------------------------------------------- Collisions And Growth
 * @brief Checks colliding key lookup and borrowed Slice stability as the cache grows.
 */
void collisions_and_growth() {
    LOG_INFO_STREAM << "Checking colliding keys and borrowed Slices across cache growth";
    using cache_test::CollisionKey;
    SliceCache<CollisionKey> cache(512 * 64);
    cache.set(CollisionKey{0}, payload(0, 64));
    const Slice* borrowed = cache.peek(CollisionKey{0});
    for (int64_t key = 1; key < 512; ++key) {
        cache.set(CollisionKey{key}, payload(static_cast<uint64_t>(key), 64));
    }
    require(cache.size() == 512 && cache.bytes() == 512 * 64,
        "hash collisions lost distinct entries");
    require(cache.peek(CollisionKey{0}) == borrowed && borrowed->get_as<uint64_t>() == 0,
        "cache growth invalidated a borrowed Slice");
    for (int64_t key = 0; key < 512; ++key) {
        const Slice* found = cache.peek(CollisionKey{key});
        require(found != nullptr && found->get_as<uint64_t>() == static_cast<uint64_t>(key),
            "colliding key lookup returned the wrong entry");
    }
    cache.set(CollisionKey{256}, payload(999, 64));
    require(cache.size() == 512 && cache.peek(CollisionKey{256})->get_as<uint64_t>() == 999,
        "replacing a colliding key inserted a duplicate or replaced a neighbor");
    cache.erase(CollisionKey{255});
    cache.erase(CollisionKey{1024});
    require(!cache.exists(CollisionKey{255}) && cache.exists(CollisionKey{254})
        && cache.exists(CollisionKey{256}) && cache.size() == 511,
        "erasing a colliding key removed a neighbor");
    require(cache.get(CollisionKey{0}) == borrowed,
        "erasing or replacing another key invalidated a borrowed Slice");
    cache.set(CollisionKey{512}, payload(512, 64));
    cache.set(CollisionKey{513}, payload(513, 64));
    require(cache.peek(CollisionKey{0}) == borrowed && !cache.exists(CollisionKey{1}),
        "collision handling broke promotion or eviction");
}
/** --------------------------------------------------------------------------------------------------------- Key Exceptions
 * @brief Checks failed insertions preserve ownership and eviction bypasses victim key callbacks.
 */
void key_exceptions() {
    LOG_INFO_STREAM << "Checking key exceptions preserve cache accounting and eviction";
    using cache_test::ExceptionalKey;
    SliceCache<ExceptionalKey> cache(64);
    const ExceptionalKey original(1);
    const ExceptionalKey replacement(2);
    cache.set(original, payload(1, 64));
    const Slice* borrowed = cache.peek(original);
    ExceptionalKey::failed_copy = replacement.value;
    bool rejected = false;
    try {
        cache.set(replacement, payload(2, 64));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    ExceptionalKey::failed_copy = -1;
    require(rejected && cache.size() == 1 && cache.bytes() == 64,
        "a failed key copy changed cache size or weight");
    require(cache.peek(original) == borrowed && !cache.exists(replacement),
        "a failed key copy changed the cached entries");
    ExceptionalKey::failed_hash = replacement.value;
    rejected = false;
    try {
        cache.set(replacement, payload(2, 64));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    ExceptionalKey::failed_hash = -1;
    require(rejected && cache.size() == 1 && cache.bytes() == 64,
        "a failed key hash changed cache size or weight");
    require(cache.peek(original) == borrowed && borrowed->get_as<uint64_t>() == 1
        && !cache.exists(replacement), "a failed key hash changed the cached entries");
    ExceptionalKey::failed_hash = original.value;
    rejected = false;
    try {
        cache.set(replacement, payload(2, 64));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    ExceptionalKey::failed_hash = -1;
    require(!rejected && cache.size() == 1 && cache.bytes() == 64,
        "eviction hashed its victim or left incorrect accounting");
    require(!cache.exists(original) && cache.peek(replacement)->get_as<uint64_t>() == 2,
        "eviction with a throwing victim hash retained the wrong entry");
    ExceptionalKey::failed_equality = replacement.value;
    rejected = false;
    try {
        cache.set(original, payload(3, 64));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    ExceptionalKey::failed_equality = -1;
    require(!rejected && cache.size() == 1 && cache.bytes() == 64,
        "eviction compared its victim key or left incorrect accounting");
    require(!cache.exists(replacement) && cache.peek(original)->get_as<uint64_t>() == 3,
        "eviction with throwing victim equality retained the wrong entry");
    ExceptionalKey::failed_hash = original.value;
    ExceptionalKey::failed_equality = original.value;
    rejected = false;
    try {
        cache.resize(0);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    ExceptionalKey::failed_hash = -1;
    ExceptionalKey::failed_equality = -1;
    require(!rejected && cache.empty() && cache.bytes() == 0 && cache.capacity() == 0,
        "resize invoked a victim key callback or left incorrect accounting");
}
/** --------------------------------------------------------------------------------------------------------- Views Survive
 * @brief Checks that owning views retain their original payload through replacement and eviction.
 */
void views_survive() {
    LOG_INFO_STREAM << "Checking owning views through replacement, eviction, and destruction";
    Slice replaced;
    Slice evicted;
    {
        SliceCache<> cache(64);
        cache.set(1, payload(1, 64));
        replaced = cache.view(1);
        require(replaced.raw() == cache.peek(1)->raw(), "view copied its cached payload");
        cache.set(1, payload(2, 64));
        require(replaced.get_as<uint64_t>() == 1 && cache.peek(1)->get_as<uint64_t>() == 2,
            "replacement changed the payload of an existing owning view");
        evicted = cache.view(1);
        require(evicted.raw() == cache.peek(1)->raw(), "replacement view copied its payload");
        cache.set(2, payload(3, 64));
        require(!cache.exists(1) && evicted.get_as<uint64_t>() == 2,
            "eviction invalidated an owning view");
        cache.clear();
    }
    require(replaced.valid() && replaced.get_as<uint64_t>() == 1,
        "cache destruction invalidated the view of a replaced entry");
    require(evicted.valid() && evicted.get_as<uint64_t>() == 2,
        "cache destruction invalidated the view of an evicted entry");
}
/** --------------------------------------------------------------------------------------------------------- Custom Eviction
 * @brief Checks templated selection, protected writes, repeated eviction, and strict resizing.
 */
void custom_eviction() {
    LOG_INFO_STREAM << "Checking templated eviction through insert, replacement, and resize";
    MostRecentCache cache(256);
    cache.set(1, payload(1, 64));
    cache.set(2, payload(2, 64));
    cache.set(3, payload(3, 64));
    cache.get(1);
    const Slice* borrowed = cache.peek(2);
    cache.set(4, payload(4, 128));
    require(!cache.exists(1) && cache.peek(2) == borrowed && cache.bytes() == 256,
        "custom insertion did not select the most recent eligible entry");
    require_order<int64_t>(cache, {4, 3, 2});
    cache.set(2, payload(22, 192));
    require(!cache.exists(4) && cache.peek(2) == borrowed && cache.bytes() == 256
        && borrowed->get_as<uint64_t>() == 22,
        "custom replacement evicted its protected entry or miscounted its weight");
    require_order<int64_t>(cache, {2, 3});
    cache.set(5, payload(5, 256));
    require(cache.size() == 1 && cache.bytes() == 256 && cache.exists(5),
        "custom selection did not repeat until multiple victims restored the budget");
    cache.set(5, payload(55, 512));
    require(cache.size() == 1 && cache.bytes() == 512 && cache.exists(5),
        "custom selection did not protect a sole oversize replacement");
    cache.set(6, payload(6, 1024));
    require(cache.size() == 1 && cache.bytes() == 1024 && cache.exists(6),
        "custom selection did not protect a new oversize entry");
    cache.resize(256);
    require(cache.empty() && cache.bytes() == 0,
        "custom resize protected an oversize entry");
    cache.set(7, payload(7, 64));
    cache.set(8, payload(8, 64));
    cache.set(9, payload(9, 64));
    cache.resize(64);
    require(cache.size() == 1 && cache.bytes() == 64 && cache.exists(7),
        "custom resize did not include its newest entry or repeat selection");
    cache.resize(0);
    cache.set(10, Slice());
    cache.set(11, Slice());
    cache.set(12, payload(12, 64));
    require(cache.size() == 1 && cache.exists(12) && cache.bytes() == 64,
        "custom oversize insertion retained older zero-weight entries");
    cache.resize(0);
    require(cache.empty() && cache.bytes() == 0,
        "custom zero-budget resize retained its final entry");
}
/** --------------------------------------------------------------------------------------------------------- Static Eviction
 * @brief Checks static selection using candidate keys, Slice contents, and both range boundaries.
 */
void static_eviction() {
    LOG_INFO_STREAM << "Checking a static eviction selector inspecting keys and Slices";
    BoundaryEviction::selections = 0;
    SliceCache<int64_t, BoundaryEviction::select> cache(256);
    cache.set(1, payload(1, 64));
    cache.set(2, payload(2, 64));
    cache.set(3, payload(3, 64));
    cache.set(7, payload(70, 64));
    cache.resize(256);
    require(BoundaryEviction::selections == 0,
        "the selector ran without a budget excess");
    cache.set(8, payload(8, 64));
    require_order<int64_t>(cache, {8, 3, 2, 1});
    require(BoundaryEviction::selections == 1,
        "insertion did not invoke the static selector once");
    cache.resize(192);
    require_order<int64_t>(cache, {8, 3, 2});
    cache.set(7, payload(71, 64));
    require_order<int64_t>(cache, {7, 8, 3});
    cache.set(9, payload(9, 64));
    require_order<int64_t>(cache, {9, 7, 8});
    require(BoundaryEviction::selections == 4 && cache.bytes() == 192,
        "static selection ignored Slice contents or produced incorrect accounting");
}
/** --------------------------------------------------------------------------------------------------------- Custom Moves And Views
 * @brief Checks custom eviction after moves while borrowed addresses and owning views survive.
 */
void custom_moves_and_views() {
    LOG_INFO_STREAM << "Checking custom eviction after cache moves and retained owning views";
    Slice evicted;
    Slice replaced;
    Slice displaced;
    {
        MostRecentCache source(128);
        source.set(1, payload(1, 64));
        source.set(2, payload(2, 64));
        evicted = source.view(1);
        const Slice* borrowed = source.peek(2);
        MostRecentCache moved(std::move(source));
        moved.set(3, payload(3, 64));
        require_order<int64_t>(moved, {3, 2});
        require(moved.peek(2) == borrowed && evicted.get_as<uint64_t>() == 1,
            "custom eviction after move construction invalidated a borrow or owning view");
        MostRecentCache destination(256);
        destination.set(9, payload(9, 64));
        displaced = destination.view(9);
        destination = std::move(moved);
        destination.set(4, payload(4, 64));
        require_order<int64_t>(destination, {4, 2});
        require(destination.capacity() == 128 && destination.peek(2) == borrowed,
            "move assignment changed the custom policy or borrowed Slice address");
        require(source.empty() && source.capacity() == 0
            && moved.empty() && moved.capacity() == 0,
            "custom moves left entries or budgets in their sources");
        source.set(5, payload(5, 64));
        source.set(6, payload(6, 64));
        require(source.size() == 1 && source.exists(6) && source.bytes() == 64,
            "a custom move-constructed source could not be reused");
        moved.set(7, payload(7, 64));
        moved.resize(0);
        require(moved.empty(), "a custom move-assigned source could not be reused");
        replaced = destination.view(2);
        require(replaced.raw() == borrowed->raw(), "a custom cache view copied its payload");
        destination.set(2, payload(22, 64));
        require(replaced.get_as<uint64_t>() == 2 && borrowed->get_as<uint64_t>() == 22,
            "custom replacement changed an existing view or invalidated its borrowed Slice");
        destination.set(8, payload(8, 64));
        require_order<int64_t>(destination, {8, 4});
    }
    require(evicted.valid() && evicted.get_as<uint64_t>() == 1,
        "custom eviction or destruction invalidated an owning view");
    require(replaced.valid() && replaced.get_as<uint64_t>() == 2,
        "custom replacement or destruction invalidated an owning view");
    require(displaced.valid() && displaced.get_as<uint64_t>() == 9,
        "custom move assignment or destruction invalidated a displaced owning view");
}
} // namespace
int main() {
    byte_budget();
    ownership();
    replacement();
    iteration_and_recency();
    zero_budget_and_resize();
    moves();
    collisions_and_growth();
    key_exceptions();
    views_survive();
    custom_eviction();
    static_eviction();
    custom_moves_and_views();
    LOG_INFO_STREAM << "SliceCache contracts hold";
    return 0;
}
