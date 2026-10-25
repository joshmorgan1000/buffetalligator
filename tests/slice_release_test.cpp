/** --------------------------------------------------------------------------------------------------------- Slab Release Test
 * @file slab_release_test.cpp
 * @brief Checks that drained chain backings release after their last Slice retires.
 */
#include <alligator.hpp>
#include <memory/tracker.hpp>
#include "functional_support.hpp"
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

using namespace buffetalligator;
using functional::require;
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Exercises repeated slab retirement while checking bounded backing retention.
 */
int main() {
    require(setenv("ALLIGATOR_GPU_BACKEND", "cpu", 1) == 0, "CPU selection failed");
    constexpr size_t slab = 64u << 20;
    constexpr size_t claim = 16u << 20;
    const BuffetDescriptor* placement = Slice::default_placement();
    { Slice warm(64); }
    const size_t usage_before = Memory::placement_usage(*placement);
    const size_t allocated_before = Memory::placement_allocations(*placement);
    LOG_INFO_STREAM << "Checking reclamation across drained slab boundaries";
    for (size_t index = 0; index < 64; ++index) {
        Slice payload(claim);
        require(payload.size_bytes() == claim && payload.get_as<uint8_t>() == 0,
            "chain claim returned the wrong size or unzeroed bytes");
        payload.get_as<uint8_t>() = static_cast<uint8_t>(index | 1);
        if ((index & 7) == 0) LOG_INFO_STREAM << "Checked slab release claim " << index;
    }
    /// Three claims and their records fill a slab, so the loop drains over twenty slabs; a drained slab parks as
    /// the placement's spare and becomes the next link, so far fewer than that are ever allocated.
    require(Memory::placement_allocations(*placement) - allocated_before < 12 * slab,
        "drained slabs were not recycled into the chain");
    require(Memory::placement_usage(*placement) <= usage_before + 2 * slab,
        "placement retained more than the current slab, its prepared successor and the parked spare");
    LOG_INFO_STREAM << "Drained slab reclamation passed";
}
