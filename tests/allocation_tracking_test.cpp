/** --------------------------------------------------------------------------------------------------------- Allocation Tracking Test
 * @file allocation_tracking_test.cpp
 * @brief Checks that allocation tracking defaults on, turns off before the arena starts, and then
 * leaves every tracker counter untouched.
 */
#include <alligator.hpp>
#include <memory/tracker.hpp>
#include "functional_support.hpp"
#include <cstdlib>

using namespace buffetalligator;
using functional::require;
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Turns tracking off, then allocates and frees buffers and checks that nothing counted.
 */
int main() {
    require(setenv("ALLIGATOR_GPU_BACKEND", "cpu", 1) == 0, "CPU selection failed");
    require(BuffetDescriptors::allocation_tracking(), "allocation tracking did not default on");
    BuffetDescriptors::set_allocation_tracking(false);
    require(!BuffetDescriptors::allocation_tracking(), "allocation tracking did not turn off");
    const BuffetDescriptor& heap = *BuffetDescriptors::get(AlignedHeapBuffer::type_idx());
    {
        Slice claimed(size_t{256}, &heap);
        Slice novel(size_t{8192}, true, &heap);
        SharedBuffet shared(size_t{4096});
        require(claimed && novel && shared, "untracked allocations failed");
    }
    require(Memory::total_allocations() == 0 && Memory::total_freed() == 0
        && Memory::placement_allocations(heap) == 0 && Memory::placement_freed(heap) == 0,
        "untracked allocations or releases reached the tracker");
    bool rejected = false;
    try { BuffetDescriptors::set_allocation_tracking(true); }
    catch (const AlligatorException&) { rejected = true; }
    require(rejected && !BuffetDescriptors::allocation_tracking(),
        "tracking changed after the arena started, which would unbalance its counters");
    LOG_INFO_STREAM << "Allocation tracking switch passed";
}
