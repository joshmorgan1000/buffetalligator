/** --------------------------------------------------------------------------------------------------------- Builtin Default Test
 * @file builtin_default_test.cpp
 * @brief Verifies that a process which registers nothing gets the heap and aligned-heap
 * placements at their stable identifiers with aligned heap as the default.
 */
#include <alligator.hpp>
#include <cstring>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}
int main() {
    buffetalligator::Slice bytes(1024);
    require(bytes.valid(), "a claim with no registrations failed");
    require(buffetalligator::BuffetMenu::count() == 2, "the built-ins were not registered on first use");
    require(std::strcmp(buffetalligator::BuffetMenu::get(0)->name(), "heap") == 0, "heap is not placement 0");
    require(std::strcmp(buffetalligator::BuffetMenu::get(1)->name(), "aligned_heap") == 0,
        "aligned_heap is not placement 1");
    require(bytes.placement()->type() == 1, "aligned heap is not the default placement");
    require(reinterpret_cast<uintptr_t>(bytes.raw()) % 64 == 0, "the default claim is not 64 byte aligned");
    buffetalligator::Slice heap(1024, buffetalligator::BuffetMenu::get(0));
    require(heap.placement()->type() == 0, "an explicit heap claim came from the wrong placement");
    require(heap.raw() != bytes.raw(), "two placements handed out the same memory");
    return 0;
}
