/** --------------------------------------------------------------------------------------------------------- Memory Leak Test
 * @file memory_leak_test.cpp
 * @brief Churns chain claims, views, copies, novel buffers and resizes across every host-visible
 * placement from every hardware thread, then requires slab bytes, pool slots and process residency
 * to return to their baselines once the deferred frees land.
 */
#include <alligator.hpp>
#include <memory/tracker.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__)
#include <malloc.h>
#endif

using namespace buffetalligator;
namespace {
constexpr size_t slab_bytes = 64ull << 20;  ///< The built-in placements' slab size.
constexpr size_t rounds = 4;                ///< Churn rounds per placement.
constexpr size_t ring_length = 4;           ///< Slices each thread keeps alive at once.
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Stops the test on a violated contract.
 */
void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAILED: %s\n", message); std::abort(); }
}
/** --------------------------------------------------------------------------------------------------------- Occupied Slots
 * @brief Counts pool slots holding a host pointer, which every live Slice and plate base slot does.
 */
size_t occupied_slots() {
    const HostPtr* table = Alligator::host_table();
    size_t count = 0;
    for (size_t index = 0; index < (size_t(1) << POOL_BITS); ++index) count += table[index].ptr != nullptr;
    return count;
}
/** --------------------------------------------------------------------------------------------------------- Settle
 * @brief Waits up to ten seconds for the worker's deferred frees to bring usage and slots back.
 */
bool settle(const Placemat& placement, size_t usage, size_t slots) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (Memory::placement_usage(placement) != usage || occupied_slots() != slots) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}
/** --------------------------------------------------------------------------------------------------------- Malloc In Use
 * @brief Bytes the C allocator has handed out and not had back, excluding the freed blocks it caches.
 */
size_t malloc_in_use() {
#if defined(__APPLE__)
    malloc_statistics_t statistics{};
    malloc_zone_statistics(nullptr, &statistics);
    return statistics.size_in_use;
#elif defined(__linux__)
    const struct mallinfo2 info = mallinfo2();
    return info.uordblks + info.hblkhd;
#endif
}
/** --------------------------------------------------------------------------------------------------------- Settled Residency
 * @brief Process residency after the C allocator returns the freed blocks it caches.
 */
size_t settled_residency() {
#if defined(__APPLE__)
    malloc_zone_pressure_relief(nullptr, 0);
#elif defined(__linux__)
    malloc_trim(0);
#endif
    return BuffetMenu::memory_usage().resident_bytes;
}
/** --------------------------------------------------------------------------------------------------------- Churn
 * @brief One thread's claim, share, view, novel and every resize path, with a bounded live ring.
 */
void churn(const Placemat* placement, size_t max_claim, size_t claims, size_t seed) {
    std::array<Slice, ring_length> ring;
    for (size_t claim = 0; claim < claims; ++claim) {
        const size_t mixed = (seed * 2654435761u + claim * 40503u) % max_claim + 1;
        Slice& slot = ring[claim % ring_length];
        const uint8_t marker = static_cast<uint8_t>(claim ^ 0x5a);
        switch (claim % 16) {
        case 0:  // Sole novel owner grows in place where the placement can realloc.
            slot = Slice(mixed, true, placement);
            slot.data<uint8_t>()[0] = marker;
            slot.resize(mixed + 4096, true, true, placement);
            require(slot.size_bytes() == mixed + 4096 && slot.data<uint8_t>()[0] == marker,
                "novel growth lost its size or payload");
            break;
        case 1:  // Shrinking keeps the backing and narrows to a view.
            slot = Slice(mixed + 64, true, placement);
            slot.data<uint8_t>()[0] = marker;
            slot.resize(mixed / 2 + 1, true, true, placement);
            require(slot.size_bytes() == mixed / 2 + 1 && slot.data<uint8_t>()[0] == marker,
                "shrink lost its size or payload");
            break;
        case 2: {  // A shared novel buffer must grow by copy and leave the other owner intact.
            slot = Slice(mixed, true, placement);
            slot.data<uint8_t>()[0] = marker;
            Slice other_owner = slot;
            slot.resize(mixed + 4096, true, true, placement);
            require(slot.size_bytes() == mixed + 4096 && slot.data<uint8_t>()[0] == marker &&
                other_owner.size_bytes() == mixed && other_owner.data<uint8_t>()[0] == marker,
                "shared growth disturbed an owner");
            break;
        }
        case 3:  // A chain claim grows by copy out of its slab.
            slot = Slice(mixed, false, placement);
            slot.data<uint8_t>()[0] = marker;
            slot.resize(mixed + 4096, true, false, placement);
            require(slot.size_bytes() == mixed + 4096 && slot.data<uint8_t>()[0] == marker,
                "chain growth lost its size or payload");
            break;
        case 4:  // Discarding resize reallocates without carrying the payload.
            slot = Slice(mixed, false, placement);
            slot.resize(mixed + 4096, false, false, placement);
            require(slot.size_bytes() == mixed + 4096, "discarding resize returned the wrong size");
            break;
        case 5:  // Resizing a null Slice claims fresh storage.
            slot = Slice();
            slot.resize(mixed, true, false, placement);
            require(slot.size_bytes() == mixed, "null resize returned the wrong size");
            break;
        default:
            slot = Slice(mixed, false, placement);
            require(slot.size_bytes() == mixed, "chain claim returned the wrong size");
        }
        slot.data<uint8_t>()[0] = static_cast<uint8_t>(claim);
        Slice shared = slot;
        Slice view = shared.slice(0, std::max<size_t>(1, mixed / 2));
        require(view.data<uint8_t>()[0] == static_cast<uint8_t>(claim), "view lost its backing");
    }
}
/** --------------------------------------------------------------------------------------------------------- Exercise
 * @brief Churns one placement and requires its slab bytes, pool slots and C heap to recover; device
 * placements, whose memory lives outside malloc, also require process residency to recover.
 */
void exercise(const Placemat* placement, size_t live_budget, size_t churn_budget, bool device_memory) {
    const size_t threads = std::thread::hardware_concurrency();
    const size_t max_claim = std::min<size_t>(8ull << 20, live_budget / (threads * ring_length * 2));
    const size_t claims = churn_budget / ((rounds + 1) * threads * (max_claim / 2));  // A total leak stays inside the budget.
    { Slice warm(64, false, placement); }
    require(settle(*placement, Memory::placement_usage(*placement), occupied_slots()), "warmup never settled");
    const size_t usage_before = Memory::placement_usage(*placement);
    const size_t slots_before = occupied_slots();
    const size_t allocated_before = Memory::placement_allocations(*placement);
    size_t resident_before = 0;
    size_t malloc_before = 0;
    for (size_t round = 0; round <= rounds; ++round) {
        if (round == 1) {
            require(settle(*placement, usage_before, slots_before), "warmup round leaked");
            resident_before = settled_residency();  // The warmup round has touched the live chain.
            malloc_before = malloc_in_use();
        }
        std::vector<std::thread> team;
        for (size_t thread = 0; thread < threads; ++thread) {
            team.emplace_back(&churn, placement, max_claim, claims, round * threads + thread);
        }
        for (std::thread& worker : team) worker.join();
    }
    const size_t churned = Memory::placement_allocations(*placement) - allocated_before;
    require(churned > 4 * slab_bytes, "churn did not cycle enough slabs to expose a leak");
    if (!settle(*placement, usage_before, slots_before)) {
        std::fprintf(stderr, "%s: live %zu -> %zu MiB, slots %zu -> %zu\n", placement->name(), usage_before >> 20,
            Memory::placement_usage(*placement) >> 20, slots_before, occupied_slots());
        require(false, "slab bytes or pool slots leaked");
    }
    const size_t malloc_after = malloc_in_use();
    const size_t resident_after = settled_residency();
    const long long malloc_growth = static_cast<long long>(malloc_after) - static_cast<long long>(malloc_before);
    const long long resident_growth = static_cast<long long>(resident_after) - static_cast<long long>(resident_before);
    std::printf("%s: churned %zu MiB, live %zu MiB, slots %zu, malloc %+lld KiB, residency %+lld MiB\n",
        placement->name(), churned >> 20, usage_before >> 20, slots_before, malloc_growth >> 10, resident_growth >> 20);
    require(malloc_growth < (1ll << 20), "the C heap kept memory the churn released");
    require(!device_memory || resident_growth < static_cast<long long>(slab_bytes),
        "device memory residency grew past the live chain");
}
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Sizes the live and total churn budgets from available memory, then churns each host-visible
 * placement; even a total leak of every churned slab stays under a quarter of available memory.
 */
int main() {
    const HostMemoryUsage host = BuffetMenu::memory_usage();
    const size_t live_budget = std::min<size_t>(1ull << 30, host.available_bytes / 8);
    const size_t churn_budget = std::min<size_t>(8ull << 30, host.available_bytes / 4);
    require(live_budget >= 4 * slab_bytes, "not enough available memory to churn safely");
    exercise(BuffetMenu::get("heap"), live_budget, churn_budget, false);
    exercise(BuffetMenu::get("aligned_heap"), live_budget, churn_budget, false);
    if (GPU::exists()) {
        exercise(VulkanContext::buffer_placement(), live_budget, churn_budget, true);
    }
    std::printf("memory leak test passed\n");
    return 0;
}
