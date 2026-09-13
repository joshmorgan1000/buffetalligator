/** --------------------------------------------------------------------------------------------------------- Slab Release Test
 * @file slab_release_test.cpp
 * @brief Checks that chain slabs are returned to their placement once the chain has moved past them
 * and every Slice claimed from them is gone. Guards the slab reference ledger: the root Slice is
 * the slab's only self-reference, and deleting it at eviction must let the last claim free the slab.
 */
#include <alligator.hpp>
#include <memory/tracker.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

using buffetalligator::Memory;
using buffetalligator::Placemat;
using buffetalligator::Slice;
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Stops the test on a violated public contract.
 */
static void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::abort(); }
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Claims and immediately frees enough to cross several slab boundaries with no Slice alive,
 * then requires that evicted slabs were handed back. Slabs are at least 64 MiB, so sixteen 16 MiB
 * claims fill four of them; the chain keeps the current slab, its prefetched successors, and the one
 * parked in the previous-pool slot, and everything older must be freed.
 */
int main() {
    constexpr size_t slab = 64u << 20;
    constexpr size_t claim = 16u << 20;
    const Placemat* placement = Slice::default_placement();
    { Slice warm(64); }
    const size_t usage_before = Memory::placement_usage(*placement);
    const size_t freed_before = Memory::total_freed();
    for (int index = 0; index < 16; ++index) {
        Slice payload(claim);
        payload.get_as<uint8_t>() = static_cast<uint8_t>(index);
        require(payload.size_bytes() == claim, "chain claim returned the wrong size");
    }
    /// Slab release runs on the arena worker, so give it a bounded window to land.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (Memory::total_freed() < freed_before + 2 * slab) {
        require(std::chrono::steady_clock::now() < deadline, "evicted chain slabs were never released");
        std::this_thread::yield();
    }
    require(Memory::placement_usage(*placement) < usage_before + 8 * slab, "placement usage grew without bound");
    std::printf("slab release test passed\n");
    return 0;
}
