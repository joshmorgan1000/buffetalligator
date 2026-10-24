/** --------------------------------------------------------------------------------------------------------- Slab Release Test
 * @file slab_release_test.cpp
 * @brief Checks that drained chain backings release after their last Chomp retires.
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
    const BuffetDescriptor* placement = Chomp::default_placement();
    { Chomp warm(64); }
    const size_t usage_before = Memory::placement_usage(*placement);
    const size_t freed_before = Memory::placement_freed(*placement);
    LOG_INFO_STREAM << "Checking reclamation across sixteen drained slab boundaries";
    for (size_t index = 0; index < 64; ++index) {
        Chomp payload(claim);
        require(payload.size_bytes() == claim && payload.get_as<uint8_t>() == 0,
            "chain claim returned the wrong size or unzeroed bytes");
        payload.get_as<uint8_t>() = static_cast<uint8_t>(index | 1);
        if ((index & 7) == 0) LOG_INFO_STREAM << "Checked slab release claim " << index;
    }
    /// A drained link frees one Kitchen hop after its last release, so the count is given a moment to land.
    for (int wait = 0; wait < 1000 && Memory::placement_freed(*placement) < freed_before + 15 * slab; ++wait)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(Memory::placement_freed(*placement) >= freed_before + 15 * slab,
        "drained slabs retained backing after every Chomp released it");
    require(Memory::placement_usage(*placement) <= usage_before + 2 * slab,
        "placement retained more than the current slab and its prepared successors");
    LOG_INFO_STREAM << "Drained slab reclamation passed";
}
