/** --------------------------------------------------------------------------------------------------------- Chomp Contract Test
 * @file chomp_test.cpp
 * @brief Verifies the Chomp handle contract: null state, zero fill, copy and move semantics,
 * sub-slicing, external copies, resize, and placement identity on the built-in placements.
 */
#include <alligator.hpp>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool throws(void (*probe)(const buffetalligator::Chomp&), const buffetalligator::Chomp& chomp) {
    try {
        probe(chomp);
    } catch (...) {
        return true;
    }
    return false;
}
void offset_past_end(const buffetalligator::Chomp& chomp) {
    (void)chomp.chomp(chomp.size_bytes(), 1);
}
void length_past_end(const buffetalligator::Chomp& chomp) {
    (void)chomp.chomp(0, chomp.size_bytes() + 1);
}
}
int main() {
    buffetalligator::Chomp null_chomp;
    require(null_chomp.is_null() && !null_chomp.valid() && !null_chomp, "a default Chomp is not null");
    require(null_chomp.placement() == nullptr, "a null Chomp reported a placement");
    null_chomp.free();
    require(null_chomp.is_null(), "freeing a null Chomp changed its state");
    buffetalligator::Chomp bytes(4096);
    require(bytes.valid() && bytes.size_bytes() == 4096, "a fresh claim has the wrong size");
    require(bytes.size<uint32_t>() == 1024 && bytes.size<uint64_t>() == 512, "typed size is wrong");
    require(std::all_of(bytes.data<uint8_t>(), bytes.data<uint8_t>() + 4096,
        [](uint8_t value) { return value == 0; }), "a fresh claim was not zero filled");
    require(bytes.placement() == buffetalligator::Chomp::default_placement(),
        "a default claim did not come from the default placement");
    bytes.data<uint8_t>()[10] = 0x5A;
    buffetalligator::Chomp copy(bytes);
    require(copy.raw() == bytes.raw() && copy.size_bytes() == 4096, "a copy does not share the memory");
    bytes.free();
    require(bytes.is_null() && copy.data<uint8_t>()[10] == 0x5A, "a copy did not keep the slab view alive");
    buffetalligator::Chomp assigned;
    assigned = copy;
    require(assigned.raw() == copy.raw(), "copy assignment does not share the memory");
    buffetalligator::Chomp moved(std::move(copy));
    require(copy.is_null() && moved.raw() == assigned.raw(), "move construction did not transfer the view");
    buffetalligator::Chomp move_assigned;
    move_assigned = std::move(moved);
    require(moved.is_null() && move_assigned.data<uint8_t>()[10] == 0x5A,
        "move assignment did not transfer the view");
    buffetalligator::Chomp window = move_assigned.chomp(1024, 256);
    require(window.size_bytes() == 256, "a sub-chomp has the wrong size");
    require(window.raw() == move_assigned.data<uint8_t>() + 1024, "a sub-chomp has the wrong offset");
    window.data<uint8_t>()[0] = 0x77;
    require(move_assigned.data<uint8_t>()[1024] == 0x77, "a sub-chomp does not alias its parent");
    buffetalligator::Chomp tail = move_assigned.chomp(4000);
    require(tail.size_bytes() == 128 && tail.raw() == move_assigned.data<uint8_t>() + 3968,
        "an open-ended sub-chomp did not round its bounds outward");
    buffetalligator::Chomp whole = move_assigned.chomp();
    require(whole.raw() == move_assigned.raw() && whole.size_bytes() == 4096, "a full view differs from its parent");
    require(move_assigned.chomp(16, 0).is_null(), "a zero-length sub-chomp is not null");
    require(throws(&offset_past_end, move_assigned), "a sub-chomp offset past the end did not throw");
    require(throws(&length_past_end, move_assigned), "a sub-chomp length past the end did not throw");
    move_assigned.free();
    assigned.free();
    whole.free();
    tail.free();
    require(window.data<uint8_t>()[0] == 0x77, "a sub-chomp did not keep the slab view alive");
    const char text[] = "buffet alligator";
    buffetalligator::Chomp external(text, sizeof(text));
    require(external.size_bytes() == 64 && std::memcmp(external.raw(), text, sizeof(text)) == 0,
        "the external copy constructor did not copy the bytes");
    bool rejected_null_source = false;
    try {
        buffetalligator::Chomp from_nothing(nullptr, 64);
    } catch (const buffetalligator::AlligatorException&) {
        rejected_null_source = true;
    }
    require(rejected_null_source, "copying nonempty bytes from a null pointer was accepted");
    buffetalligator::Chomp empty_copy(nullptr, 0);
    require(empty_copy.is_null(), "copying zero bytes from a null pointer produced a claim");
    buffetalligator::Chomp grown(256);
    std::memset(grown.raw(), 0xAB, 256);
    grown.resize(512);
    require(grown.size_bytes() == 512, "resize did not grow the chomp");
    require(grown.data<uint8_t>()[0] == 0xAB && grown.data<uint8_t>()[255] == 0xAB,
        "resize did not preserve the existing bytes");
    void* before_shrink = grown.raw();
    grown.resize(128);
    require(grown.size_bytes() == 128 && grown.raw() == before_shrink, "a preserving shrink did not stay in place");
    grown.resize(64, false);
    require(grown.size_bytes() == 64, "a discarding resize has the wrong size");
    buffetalligator::Chomp revived;
    revived.resize(96);
    require(revived.valid() && revived.size_bytes() == 128, "resize on a null chomp did not allocate");
    buffetalligator::Chomp heap(100, buffetalligator::BuffetDescriptors::get(0));
    buffetalligator::Chomp heap_next(16, buffetalligator::BuffetDescriptors::get(0));
    require(heap.placement() == buffetalligator::BuffetDescriptors::get(0), "an explicit placement was not honored");
    require(reinterpret_cast<uintptr_t>(heap_next.raw()) % 16 == 0, "the heap placement ignored its 16 byte alignment");
    buffetalligator::Chomp aligned_odd(100, buffetalligator::BuffetDescriptors::get(0));
    buffetalligator::Chomp aligned_next(16, buffetalligator::BuffetDescriptors::get(0));
    require(reinterpret_cast<uintptr_t>(aligned_next.raw()) % 64 == 0,
        "the aligned heap placement ignored its 64 byte alignment");
    return 0;
}
