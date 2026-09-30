/** --------------------------------------------------------------------------------------------------------- Slice Map Growth Test
 * @file slice_map_growth_test.cpp
 * @brief Forces an overlapping resize before splitting and verifies exact row ownership at teardown.
 */
#include <alligator.hpp>
#include <alligator/containers.hpp>
#include "functional_support.hpp"
#include <array>
#include <atomic>
#include <bit>
#include <thread>

using namespace buffetalligator;
using functional::require;
namespace {
size_t overlap_checks = 0;
/** --------------------------------------------------------------------------------------------------------- Overlapping Growth
 * @brief Runs another production resize while the published table still contains marked buckets.
 */
template<typename State, typename Table>
void overlapping_growth(State& state, Table& table) {
    if (table.bucket_count != 4) return;
    require((table.buckets[1].load(std::memory_order_acquire) & State::kMark) != 0,
        "controlled growth did not stop before bucket splitting");
    std::thread contender(&State::try_resize, &state, &table);
    contender.join();
    require(state.table_.load(std::memory_order_acquire) == &table,
        "overlapping resize copied a table before its marked buckets were split");
    ++overlap_checks;
}
} // namespace
#define BUFFETALLIGATOR_TEST_MAP_GROWTH(state, table) overlapping_growth(state, table)
#include "../src/containers/slicemap.cpp"
#undef BUFFETALLIGATOR_TEST_MAP_GROWTH
namespace {
/** --------------------------------------------------------------------------------------------------------- Owned Value
 * @brief Records each row's identity and independently counts its final destruction.
 */
struct OwnedValue {
    const size_t identity;
    std::atomic<size_t>& destructions;
    OwnedValue(size_t identifier, std::atomic<size_t>& released)
    : identity(identifier), destructions(released) {}
    ~OwnedValue() { destructions.fetch_add(1, std::memory_order_relaxed); }
};
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Checks the real resize interleaving and every payload through growth and reclamation.
 */
int main() {
    LOG_INFO_STREAM << "Checking overlapping SliceMap growth and exact payload reclamation";
    constexpr uint64_t inverse = UINT64_C(17428512612931826493);
    constexpr std::array<uint64_t, 9> hashes{
        UINT64_C(0x1000000000000000), UINT64_C(0x9000000000000000),
        UINT64_C(0x5000000000000000), UINT64_C(0xd000000000000000),
        UINT64_C(0x3000000000000000), UINT64_C(0xb000000000000000),
        UINT64_C(0x7000000000000000), UINT64_C(0xf000000000000000),
        UINT64_C(0x2000000000000000)};
    std::array<std::atomic<size_t>, hashes.size()> destructions{};
    {
        SliceMapT<OwnedValue> map(2);
        for (size_t index = 0; index < hashes.size(); ++index) {
            const int64_t identifier = std::bit_cast<int64_t>(hashes[index] * inverse);
            map.emplace(identifier, index, destructions[index]);
            for (size_t expected = 0; expected <= index; ++expected) {
                const int64_t key = std::bit_cast<int64_t>(hashes[expected] * inverse);
                require(map.find(key) == static_cast<int64_t>(expected),
                    "growth changed an exact lookup's stable position");
                require(map.id<int64_t>(expected) == key && map.published(expected),
                    "growth lost a published row's identifier");
                require(map.get_slice(key).get_as<OwnedValue>().identity == expected,
                    "growth selected another row's payload");
                require(map.slice_at(expected).get_as<OwnedValue>().identity == expected,
                    "growth selected another position's payload");
                require(destructions[expected].load(std::memory_order_relaxed) == 0,
                    "growth released a live payload");
            }
        }
        require(map.size() == hashes.size() && overlap_checks == 1,
            "controlled overlapping growth did not run exactly once");
    }
    SliceMap::gc();
    for (const auto& destroyed : destructions) {
        require(destroyed.load(std::memory_order_relaxed) == 1,
            "grown map did not release each payload exactly once");
    }
    LOG_INFO_STREAM << "Overlapping SliceMap growth and exact payload reclamation passed";
}
