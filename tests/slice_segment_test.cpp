/** --------------------------------------------------------------------------------------------------------- Slice Realm Test
 * @file slice_segment_test.cpp
 * @brief Exercises realm rollover, shared copies across a realm boundary, and slot reuse.
 */
#include <alligator.hpp>
#include <memory/region.hpp>
#include <memory/tracker.hpp>
#include "functional_support.hpp"
#include <cstdlib>
#include <vector>

using namespace buffetalligator;
using functional::require;
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Verifies a view published in another realm outlives every other owner of its buffer.
 */
int main() {
    require(setenv("ALLIGATOR_GPU_BACKEND", "cpu", 1) == 0, "CPU selection failed");
    const BuffetDescriptor* heap = BuffetDescriptors::get(AlignedHeapBuffer::type_idx());
    Slice source(size_t{64 * 1024}, true, heap);
    const size_t first_realm = pool_region(source.id());
    std::vector<Slice> views;
    views.reserve(REGION_SIZE);
    Slice retained;
    size_t retained_offset = 0;
    LOG_INFO_STREAM << "Filling a Slice realm with distinct views to force realm rollover";
    while (retained.is_null()) {
        const size_t offset = (views.size() % 1023 + 1) * 64;
        Slice view = source.slice(offset, 64);
        if (pool_region(view.id()) != first_realm) {
            retained = std::move(view);
            retained_offset = offset;
        } else {
            views.push_back(std::move(view));
        }
        require(views.size() < REGION_SIZE, "a realm exceeded its slot capacity");
        if ((views.size() & 16383) == 0) LOG_INFO_STREAM << "Published " << views.size();
    }
    require(retained.raw() == source.data<uint8_t>() + retained_offset
        && retained.size_bytes() == 64,
        "a view in the next realm lost its pointer or granule length");
    retained.get_as<uint64_t>() = UINT64_C(0x413115A70);
    Slice copy(retained);
    const SliceEntry& entry = *SliceEntry::from_slice(retained);
    require(copy.id() == retained.id() && entry.owners() == 2,
        "a copy in the next realm did not share its slot");
    copy.free();
    views.clear();
    source.free();
    require(retained.get_as<uint64_t>() == UINT64_C(0x413115A70),
        "the next-realm view lost its buffer after every other owner retired");
    Slice reused(size_t{64}, heap);
    require(pool_region(reused.id()) == first_realm
        || pool_region(reused.id()) == pool_region(retained.id()),
        "a freed realm was not reused before growing another realm");
    reused.free();
    const size_t freed = Memory::placement_freed(*heap);
    retained.free();
    require(Memory::placement_freed(*heap) == freed + 64 * 1024,
        "the novel buffer did not release exactly once with its last view");
    LOG_INFO_STREAM << "Realm rollover, shared copies, and slot reuse passed";
}
