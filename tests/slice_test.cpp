/** --------------------------------------------------------------------------------------------------------- Slice Contract Test
 * @file slice_test.cpp
 * @brief Verifies the Slice handle contract: null state, zero fill, copy and move semantics,
 * sub-slicing, external copies, resize, and placement identity on the built-in placements.
 */
#include <buffetalligator.hpp>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool throws(void (*probe)(const buffetalligator::Slice&), const buffetalligator::Slice& slice) {
    try {
        probe(slice);
    } catch (...) {
        return true;
    }
    return false;
}
void offset_past_end(const buffetalligator::Slice& slice) {
    (void)slice.slice(slice.size_bytes(), 1);
}
void length_past_end(const buffetalligator::Slice& slice) {
    (void)slice.slice(0, slice.size_bytes() + 1);
}
}
int main() {
    buffetalligator::Slice null_slice;
    require(null_slice.is_null() && !null_slice.valid() && !null_slice, "a default Slice is not null");
    require(null_slice.placement() == nullptr, "a null Slice reported a placement");
    null_slice.free();
    require(null_slice.is_null(), "freeing a null Slice changed its state");
    buffetalligator::Slice bytes(4096);
    require(bytes.valid() && bytes.size_bytes() == 4096, "a fresh claim has the wrong size");
    require(bytes.size<uint32_t>() == 1024 && bytes.size<uint64_t>() == 512, "typed size is wrong");
    require(std::all_of(bytes.data<uint8_t>(), bytes.data<uint8_t>() + 4096,
        [](uint8_t value) { return value == 0; }), "a fresh claim was not zero filled");
    require(bytes.placement() == buffetalligator::Slice::default_placement(),
        "a default claim did not come from the default placement");
    bytes.data<uint8_t>()[10] = 0x5A;
    buffetalligator::Slice copy(bytes);
    require(copy.raw() == bytes.raw() && copy.size_bytes() == 4096, "a copy does not share the memory");
    bytes.free();
    require(bytes.is_null() && copy.data<uint8_t>()[10] == 0x5A, "a copy did not keep the slab view alive");
    buffetalligator::Slice assigned;
    assigned = copy;
    require(assigned.raw() == copy.raw(), "copy assignment does not share the memory");
    buffetalligator::Slice moved(std::move(copy));
    require(copy.is_null() && moved.raw() == assigned.raw(), "move construction did not transfer the view");
    buffetalligator::Slice move_assigned;
    move_assigned = std::move(moved);
    require(moved.is_null() && move_assigned.data<uint8_t>()[10] == 0x5A,
        "move assignment did not transfer the view");
    buffetalligator::Slice window = move_assigned.slice(1024, 256);
    require(window.size_bytes() == 256, "a sub-slice has the wrong size");
    require(window.raw() == move_assigned.data<uint8_t>() + 1024, "a sub-slice has the wrong offset");
    window.data<uint8_t>()[0] = 0x77;
    require(move_assigned.data<uint8_t>()[1024] == 0x77, "a sub-slice does not alias its parent");
    buffetalligator::Slice tail = move_assigned.slice(4000);
    require(tail.size_bytes() == 96, "an open-ended sub-slice has the wrong size");
    buffetalligator::Slice whole = move_assigned.slice();
    require(whole.raw() == move_assigned.raw() && whole.size_bytes() == 4096, "a full view differs from its parent");
    require(move_assigned.slice(16, 0).is_null(), "a zero-length sub-slice is not null");
    require(throws(&offset_past_end, move_assigned), "a sub-slice offset past the end did not throw");
    require(throws(&length_past_end, move_assigned), "a sub-slice length past the end did not throw");
    move_assigned.free();
    assigned.free();
    whole.free();
    tail.free();
    require(window.data<uint8_t>()[0] == 0x77, "a sub-slice did not keep the slab view alive");
    const char text[] = "buffet alligator";
    buffetalligator::Slice external(text, sizeof(text));
    require(external.size_bytes() == sizeof(text) && std::memcmp(external.raw(), text, sizeof(text)) == 0,
        "the external copy constructor did not copy the bytes");
    buffetalligator::Slice from_nothing(nullptr, 64);
    require(from_nothing.is_null(), "copying from a null pointer produced a claim");
    buffetalligator::Slice grown(256);
    std::memset(grown.raw(), 0xAB, 256);
    grown.resize(512);
    require(grown.size_bytes() == 512, "resize did not grow the slice");
    require(grown.data<uint8_t>()[0] == 0xAB && grown.data<uint8_t>()[255] == 0xAB,
        "resize did not preserve the existing bytes");
    void* before_shrink = grown.raw();
    grown.resize(128);
    require(grown.size_bytes() == 128 && grown.raw() == before_shrink, "a preserving shrink did not stay in place");
    grown.resize(64, false);
    require(grown.size_bytes() == 64, "a discarding resize has the wrong size");
    buffetalligator::Slice revived;
    revived.resize(96);
    require(revived.valid() && revived.size_bytes() == 96, "resize on a null slice did not allocate");
    buffetalligator::Slice heap(100, buffetalligator::BuffetMenu::get(0));
    buffetalligator::Slice heap_next(16, buffetalligator::BuffetMenu::get(0));
    require(heap.placement() == buffetalligator::BuffetMenu::get(0), "an explicit placement was not honored");
    require(reinterpret_cast<uintptr_t>(heap_next.raw()) % 16 == 0, "the heap placement ignored its 16 byte alignment");
    buffetalligator::Slice aligned_odd(100, buffetalligator::BuffetMenu::get(1));
    buffetalligator::Slice aligned_next(16, buffetalligator::BuffetMenu::get(1));
    require(reinterpret_cast<uintptr_t>(aligned_next.raw()) % 64 == 0,
        "the aligned heap placement ignored its 64 byte alignment");
    return 0;
}
