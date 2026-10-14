/** --------------------------------------------------------------------------------------------------------- Slice Segment Test
 * @file slice_segment_test.cpp
 * @brief Exercises slot-segment growth, shared copies across a segment boundary, and slot reuse.
 */
#include <alligator.hpp>
#include <memory/arena.hpp>
#include <memory/tracker.hpp>
#include "functional_support.hpp"
#include <cstdlib>
#include <vector>

using namespace buffetalligator;
using functional::require;
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Verifies a view published in a grown segment outlives every other owner of its buffer.
 */
int main() {
    require(setenv("ALLIGATOR_GPU_BACKEND", "cpu", 1) == 0, "CPU selection failed");
    const BuffetDescriptor* heap = BuffetDescriptors::get(AlignedHeapBuffer::type_idx());
    Slice source(size_t{64 * 1024}, true, heap);
    require((source.id() >> SEGMENT_BITS) == 0, "the first Slice did not land in segment zero");
    std::vector<Slice> views;
    views.reserve(SEGMENT_SLOTS);
    Slice retained;
    size_t retained_offset = 0;
    LOG_INFO_STREAM << "Filling Slice segment zero with distinct views to force segment growth";
    while (retained.is_null()) {
        const size_t offset = (views.size() % 1023 + 1) * 64;
        Slice view = source.slice(offset, 64);
        if ((view.id() >> SEGMENT_BITS) != 0) {
            retained = std::move(view);
            retained_offset = offset;
        } else {
            views.push_back(std::move(view));
        }
        if ((views.size() & 16383) == 0) LOG_INFO_STREAM << "Published " << views.size();
    }
    require(Arena::segments.load(std::memory_order_acquire) == 2,
        "segment zero did not grow into one more");
    require(retained.raw() == source.data<uint8_t>() + retained_offset
        && retained.size_bytes() == 64,
        "a view in the grown segment lost its pointer or granule length");
    retained.get_as<uint64_t>() = UINT64_C(0x413115A70);
    Slice copy(retained);
    const uint32_t slot = retained.id();
    const SlotState& state = Arena::states[slot >> SEGMENT_BITS][slot & SEGMENT_MASK];
    require(copy.id() == retained.id() && state.references == 2,
        "a copy in the grown segment did not share its slot");
    copy.free();
    views.clear();
    source.free();
    require(retained.get_as<uint64_t>() == UINT64_C(0x413115A70),
        "the grown-segment view lost its buffer after every other owner retired");
    Slice reused(size_t{64}, heap);
    require((reused.id() >> SEGMENT_BITS) < 2
        && Arena::segments.load(std::memory_order_acquire) == 2,
        "a freed slot was not reused before growing another segment");
    reused.free();
    const size_t freed = Memory::placement_freed(*heap);
    retained.free();
    require(Memory::placement_freed(*heap) == freed + 64 * 1024,
        "the novel buffer did not release exactly once with its last view");
    LOG_INFO_STREAM << "Segment growth, shared copies, and slot reuse passed";
}
