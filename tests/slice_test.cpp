/** --------------------------------------------------------------------------------------------------------- Slice Contract Test
 * @file slice_test.cpp
 * @brief Verifies the Slice handle contract: null state, zero fill, copy and move semantics,
 * sub-slicing, external copies, resize, and placement identity on the built-in placements.
 */
#include "test_support.hpp"
#include <alligator.hpp>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace {
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
    test_support::start(__FILE__);
    buffetalligator::Slice null_slice;
    TEST_REQUIRE(null_slice.is_null() && !null_slice.valid() && !null_slice, "a default Slice is not null");
    TEST_EQUAL(null_slice.placement(), nullptr, "a null Slice reported a placement");
    TEST_REQUIRE(!null_slice.is_novel(), "a null Slice reported novel backing");
    null_slice.free();
    TEST_REQUIRE(null_slice.is_null(), "freeing a null Slice changed its state");
    buffetalligator::Slice bytes(4096);
    TEST_REQUIRE(!bytes.is_novel(), "a slab claim reported novel backing");
    TEST_REQUIRE(bytes.valid() && bytes.size_bytes() == 4096, "a fresh claim has the wrong size");
    TEST_REQUIRE(bytes.size<uint32_t>() == 1024 && bytes.size<uint64_t>() == 512, "typed size is wrong");
    TEST_REQUIRE(std::all_of(bytes.data<uint8_t>(), bytes.data<uint8_t>() + 4096,
        [](uint8_t value) { return value == 0; }), "a fresh claim was not zero filled");
    TEST_EQUAL(bytes.placement(), buffetalligator::Slice::default_placement(),
        "a default claim did not come from the default placement");
    bytes.data<uint8_t>()[10] = 0x5A;
    buffetalligator::Slice copy(bytes);
    TEST_REQUIRE(copy.raw() == bytes.raw() && copy.size_bytes() == 4096, "a copy does not share the memory");
    bytes.free();
    TEST_REQUIRE(bytes.is_null() && copy.data<uint8_t>()[10] == 0x5A, "a copy did not keep the slab view alive");
    buffetalligator::Slice assigned;
    assigned = copy;
    TEST_EQUAL(assigned.raw(), copy.raw(), "copy assignment does not share the memory");
    buffetalligator::Slice moved(std::move(copy));
    TEST_REQUIRE(copy.is_null() && moved.raw() == assigned.raw(), "move construction did not transfer the view");
    buffetalligator::Slice move_assigned;
    move_assigned = std::move(moved);
    TEST_REQUIRE(moved.is_null() && move_assigned.data<uint8_t>()[10] == 0x5A,
        "move assignment did not transfer the view");
    buffetalligator::Slice window = move_assigned.slice(1024, 256);
    TEST_EQUAL(window.size_bytes(), 256, "a sub-slice has the wrong size");
    TEST_EQUAL(window.raw(), move_assigned.data<uint8_t>() + 1024, "a sub-slice has the wrong offset");
    window.data<uint8_t>()[0] = 0x77;
    TEST_EQUAL(move_assigned.data<uint8_t>()[1024], 0x77, "a sub-slice does not alias its parent");
    buffetalligator::Slice tail = move_assigned.slice(4000);
    TEST_EQUAL(tail.size_bytes(), 96, "an open-ended sub-slice has the wrong size");
    buffetalligator::Slice whole = move_assigned.slice();
    TEST_REQUIRE(whole.raw() == move_assigned.raw() && whole.size_bytes() == 4096, "a full view differs from its parent");
    TEST_REQUIRE(move_assigned.slice(16, 0).is_null(), "a zero-length sub-slice is not null");
    TEST_REQUIRE(throws(&offset_past_end, move_assigned), "a sub-slice offset past the end did not throw");
    TEST_REQUIRE(throws(&length_past_end, move_assigned), "a sub-slice length past the end did not throw");
    move_assigned.free();
    assigned.free();
    whole.free();
    tail.free();
    TEST_EQUAL(window.data<uint8_t>()[0], 0x77, "a sub-slice did not keep the slab view alive");
    const char text[] = "buffet alligator";
    buffetalligator::Slice external(text, sizeof(text));
    TEST_REQUIRE(external.size_bytes() == sizeof(text) && std::memcmp(external.raw(), text, sizeof(text)) == 0,
        "the external copy constructor did not copy the bytes");
    buffetalligator::Slice from_nothing(nullptr, 64);
    TEST_REQUIRE(from_nothing.is_null(), "copying from a null pointer produced a claim");
    buffetalligator::Slice grown(256);
    std::memset(grown.raw(), 0xAB, 256);
    grown.resize(512);
    TEST_EQUAL(grown.size_bytes(), 512, "resize did not grow the slice");
    TEST_REQUIRE(grown.data<uint8_t>()[0] == 0xAB && grown.data<uint8_t>()[255] == 0xAB,
        "resize did not preserve the existing bytes");
    void* before_shrink = grown.raw();
    grown.resize(128);
    TEST_REQUIRE(grown.size_bytes() == 128 && grown.raw() == before_shrink, "a preserving shrink did not stay in place");
    grown.resize(64, false);
    TEST_EQUAL(grown.size_bytes(), 64, "a discarding resize has the wrong size");
    buffetalligator::Slice revived;
    revived.resize(96);
    TEST_REQUIRE(revived.valid() && revived.size_bytes() == 96, "resize on a null slice did not allocate");
    buffetalligator::Slice heap(100, buffetalligator::BuffetMenu::get(0));
    buffetalligator::Slice heap_next(16, buffetalligator::BuffetMenu::get(0));
    TEST_EQUAL(heap.placement(), buffetalligator::BuffetMenu::get(0), "an explicit placement was not honored");
    TEST_EQUAL(reinterpret_cast<uintptr_t>(heap_next.raw()) % 16, 0, "the heap placement ignored its 16 byte alignment");
    buffetalligator::Slice aligned_odd(100, buffetalligator::BuffetMenu::get(1));
    buffetalligator::Slice aligned_next(16, buffetalligator::BuffetMenu::get(1));
    TEST_EQUAL(reinterpret_cast<uintptr_t>(aligned_next.raw()) % 64, 0,
        "the aligned heap placement ignored its 64 byte alignment");
    const buffetalligator::Slice novel(4096, true);
    TEST_REQUIRE(novel.is_novel() && sizeof(novel) == 16, "novel identity changed the Slice layout");
    auto novel_view = novel.slice(64, 128);
    auto novel_copy = novel_view;
    TEST_REQUIRE(novel_view.is_novel() && novel_copy.is_novel(), "a view lost its novel identity");
    auto novel_moved = std::move(novel_copy);
    TEST_REQUIRE(novel_moved.is_novel() && !novel_copy.is_novel(), "move changed novel ownership");
    novel_moved.resize(32);
    TEST_REQUIRE(novel_moved.is_novel(), "a preserving shrink changed backing identity");
    novel_moved.resize(64, false, false);
    TEST_REQUIRE(!novel_moved.is_novel(), "a new slab claim retained stale novel identity");
    novel_view.free();
    TEST_REQUIRE(!novel_view.is_novel(), "a released Slice retained novel identity");
    return 0;
}
