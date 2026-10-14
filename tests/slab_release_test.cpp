/** --------------------------------------------------------------------------------------------------------- Slab Release Test
 * @file slab_release_test.cpp
 * @brief Checks that chain slabs the chain has moved past are parked once their Slices are gone,
 * and that later refills reuse them instead of allocating.
 */
#include <alligator.hpp>
#include <memory/tracker.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

using buffetalligator::Memory;
using buffetalligator::BuffetDescriptor;
using buffetalligator::Slice;
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Stops the test on a violated public contract.
 */
static void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::abort(); }
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Claims and frees 16 MiB at a time across sixteen 64 MiB slab boundaries with no Slice
 * alive; refills past the first few must come from parked slabs.
 */
int main() {
    constexpr size_t slab = 64u << 20;
    constexpr size_t claim = 16u << 20;
    const BuffetDescriptor* placement = Slice::default_placement();
    { Slice warm(64); }
    const size_t usage_before = Memory::placement_usage(*placement);
    const size_t allocated_before = Memory::placement_allocations(*placement);
    for (int index = 0; index < 64; ++index) {
        Slice payload(claim);
        require(payload.size_bytes() == claim && payload.get_as<uint8_t>() == 0,
            "chain claim returned the wrong size or unzeroed bytes");
        payload.get_as<uint8_t>() = static_cast<uint8_t>(index | 1);
    }
    require(Memory::placement_allocations(*placement) - allocated_before < 4 * slab,
        "refills allocated new slabs instead of reusing parked ones");
    require(Memory::placement_usage(*placement) < usage_before + 8 * slab,
        "placement usage grew without bound");
    std::printf("slab release test passed\n");
    return 0;
}
