/** --------------------------------------------------------------------------------------------------------- Slab Lifecycle Test
 * @file slab_lifecycle_test.cpp
 * @brief Verifies bounded slab ownership after repeated rollovers and thread-exit sealing.
 */
#include <alligator.hpp>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {
/** --------------------------------------------------------------------------------------------------------- Rollovers
 * @brief Claims across eight slab capacities and seals its final plate on thread exit.
 */
void rollovers(const buffetalligator::Placemat* placement, size_t slab_bytes) {
    for (size_t count = 0; count < 8 * slab_bytes / 1024; ++count) {
        buffetalligator::Slice claim(1024, placement);
    }
}
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Checks current and prepared slab ownership after the worker settles.
 */
int main() {
    using namespace buffetalligator;
    const Placemat* placement = Slice::default_placement();
    const size_t slab_bytes = Memory::placement_slab_size(*placement);
    std::thread(rollovers, placement, slab_bytes).join();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        const size_t allocated = Memory::placement_allocations(*placement);
        const size_t freed = Memory::placement_freed(*placement);
        const size_t bound = (Memory::placement_runway_target(*placement) + 1) * slab_bytes;
        if (allocated - freed <= bound) return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::fprintf(stderr, "Slabs retained: allocated %zu, freed %zu, slab %zu\n",
        Memory::placement_allocations(*placement), Memory::placement_freed(*placement), slab_bytes);
    return 1;
}
