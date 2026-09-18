/** --------------------------------------------------------------------------------------------------------- Slice Map Test
 * @file slice_map_test.cpp
 * @brief Checks that SliceMap releases row claims and that SliceMapT destroys emplaced payloads
 * on destruction, reset, merge, move, and reset with a pinned lookup.
 */
#include <alligator.hpp>
#include <memory/tracker.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>

using buffetalligator::Memory;
using buffetalligator::Slice;
using buffetalligator::SliceMap;
using buffetalligator::SliceMapT;
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Stops the test on a violated public contract.
 */
static void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::abort(); }
}
/** --------------------------------------------------------------------------------------------------------- Tracked
 * @brief A payload with a heap-owning member and a live-instance counter, so a skipped destructor
 * shows up both as a stale count and as a leak under a sanitizer.
 */
struct Tracked {
    static inline std::atomic<int> live{0};
    static inline std::atomic<uint64_t> live_tags{0};
    std::string name;
    uint64_t tag;
    Tracked(uint64_t value)
    : name("row-" + std::to_string(value) + "-padding-to-defeat-small-string-optimization"), tag(value) {
        live.fetch_add(1, std::memory_order_relaxed);
        live_tags.fetch_or(uint64_t{1} << value, std::memory_order_relaxed);
    }
    ~Tracked() {
        live.fetch_sub(1, std::memory_order_relaxed);
        live_tags.fetch_and(~(uint64_t{1} << tag), std::memory_order_relaxed);
    }
};
static_assert(!std::is_trivially_destructible_v<Tracked>, "Tracked must exercise the finalizer path");
/** --------------------------------------------------------------------------------------------------------- Untyped Rows Release
 * @brief Checks that destroying a SliceMap releases the novel backing of every row it still holds.
 */
static void untyped_rows_release() {
    const size_t before = Memory::total_freed();
    {
        SliceMap map(4);
        for (size_t index = 0; index < 4; ++index) {
            Slice payload(64, true);
            payload.get_as<uint64_t>() = index;
            map.add_slice(index, std::move(payload));
        }
        require(map.size() == 4, "untyped rows did not all land");
        require(map.published(3) && map.id<int64_t>(3) == 3, "untyped row did not publish its ID");
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (Memory::total_freed() < before + 4 * 64) {
        require(std::chrono::steady_clock::now() < deadline, "SliceMap leaked row backing on destruction");
        std::this_thread::yield();
    }
}
/** --------------------------------------------------------------------------------------------------------- Typed Rows Destroy
 * @brief Checks that destroying a SliceMapT runs the destructor of every emplaced payload.
 */
static void typed_rows_destroy() {
    require(Tracked::live.load() == 0, "test started with live payloads");
    {
        SliceMapT<Tracked> map(4);
        for (uint64_t index = 0; index < 4; ++index) map.emplace(index, index);
        require(map.size() == 4, "typed rows did not all land");
        require(Tracked::live.load() == 4, "emplace did not construct every payload");
        require(map.as(2).tag == 2, "typed payload read back the wrong value");
        require(map.as(2).name.rfind("row-2-", 0) == 0, "typed payload heap member was not constructed");
    }
    require(Tracked::live.load() == 0, "SliceMapT destruction leaked emplaced payloads");
}
/** --------------------------------------------------------------------------------------------------------- Typed Reset
 * @brief Checks that reset destroys emplaced payloads and that the channel is reusable afterward.
 */
static void typed_reset() {
    SliceMapT<Tracked> map(4);
    for (uint64_t index = 0; index < 4; ++index) map.emplace(index, index);
    map.reset();
    require(Tracked::live.load() == 0, "reset leaked emplaced payloads");
    require(map.size() == 0, "reset did not rewind the landed count");
    for (uint64_t index = 0; index < 4; ++index) map.emplace(index + 10, index + 10);
    require(Tracked::live.load() == 4, "reused channel did not construct payloads");
    require(map.as(2).tag == 12, "reused channel did not publish rows");
    map.reset();
    require(Tracked::live.load() == 0, "second reset leaked emplaced payloads");
}
/** --------------------------------------------------------------------------------------------------------- Typed Merge
 * @brief Checks that rows drained by merge carry their finalizer to the destination.
 */
static void typed_merge() {
    {
        SliceMapT<Tracked> destination(8);
        {
            SliceMapT<Tracked> source(4);
            for (uint64_t index = 0; index < 4; ++index) source.emplace(index, index);
            destination.merge(source);
            require(source.size() == 0, "merge left rows in the source");
        }
        require(Tracked::live.load() == 4, "merge destroyed or duplicated payloads");
        require(destination.size() == 4, "merge did not land rows in the destination");
        require(destination.as(1).tag == 1, "merged payload read back the wrong value");
    }
    require(Tracked::live.load() == 0, "merged rows leaked on destination destruction");
}
/** --------------------------------------------------------------------------------------------------------- Typed Move
 * @brief Checks that a moved map transfers row ownership exactly once.
 */
static void typed_move() {
    {
        SliceMapT<Tracked> moved(1);
        {
            SliceMapT<Tracked> map(4);
            for (uint64_t index = 0; index < 4; ++index) map.emplace(index, index);
            moved = std::move(map);
        }
        require(Tracked::live.load() == 4, "move destroyed payloads with the moved-from map");
        require(moved.as(3).tag == 3, "moved map lost its rows");
    }
    require(Tracked::live.load() == 0, "moved map leaked payloads on destruction");
}
/** --------------------------------------------------------------------------------------------------------- Typed Find Pins Row
 * @brief Checks that a row located by `find` survives reset on the looking thread until its next
 * lookup, and is finalized once the lookup moves on and the retired state is collected.
 */
static void typed_find_pins_row() {
    {
        SliceMapT<Tracked> map(4);
        for (uint64_t index = 0; index < 4; ++index) map.emplace(index, index);
        require(map.find(2) == 2, "find missed a landed row");
        map.reset();
        require(map.size() == 0, "reset did not rewind the landed count");
        require((Tracked::live_tags.load() & (uint64_t{1} << 2)) != 0,
            "reset finalized a row pinned by find");
        map.emplace(uint64_t{2}, uint64_t{20});
        require(map.find(2) == 0, "republished row did not land in the first position");
        SliceMap::gc();
        require(Tracked::live.load() == 1 && Tracked::live_tags.load() == (uint64_t{1} << 20),
            "released lookup pin did not let the retired rows finalize");
        require(map.as(0).tag == 20, "republished payload read back the wrong value");
    }
    require(Tracked::live.load() == 0, "destroying the map on the looking thread deferred finalizers");
}
/** --------------------------------------------------------------------------------------------------------- Trivial Payloads
 * @brief Checks that trivially destructible payloads still emplace and read back through the plain path.
 */
static void trivial_payloads() {
    SliceMapT<uint64_t> map(3);
    for (uint64_t index = 0; index < 3; ++index) map.emplace(index, index * 7);
    require(map.size() == 3, "trivial rows did not all land");
    require(map.as(2) == 14, "trivial payload read back the wrong value");
    require(map.get_slice(1).get_as<uint64_t>() == 7, "get_slice returned the wrong payload");
}
/** --------------------------------------------------------------------------------------------------------- Main */
int main() {
    untyped_rows_release();
    typed_rows_destroy();
    typed_reset();
    typed_merge();
    typed_move();
    typed_find_pins_row();
    trivial_payloads();
    std::printf("slice map tests passed\n");
    return 0;
}
