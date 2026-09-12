/** --------------------------------------------------------------------------------------------------------- Builtin Default Test
 * @file builtin_default_test.cpp
 * @brief Verifies that a process which registers nothing gets the heap and aligned-heap
 * placements at their stable identifiers with aligned heap as the default.
 */
#include "test_support.hpp"
#include <alligator.hpp>
#include <cstring>
#include <stdexcept>

namespace {
}
int main() {
    test_support::start(__FILE__);
    buffetalligator::Slice bytes(1024);
    TEST_REQUIRE(bytes.valid(), "a claim with no registrations failed");
    TEST_EQUAL(buffetalligator::BuffetMenu::count(), 2, "the built-ins were not registered on first use");
    TEST_EQUAL(std::strcmp(buffetalligator::BuffetMenu::get(0)->name(), "heap"), 0, "heap is not placement 0");
    TEST_EQUAL(std::strcmp(buffetalligator::BuffetMenu::get(1)->name(), "aligned_heap"), 0,
        "aligned_heap is not placement 1");
    TEST_EQUAL(bytes.placement()->type(), 1, "aligned heap is not the default placement");
    TEST_EQUAL(reinterpret_cast<uintptr_t>(bytes.raw()) % 64, 0, "the default claim is not 64 byte aligned");
    buffetalligator::Slice heap(1024, buffetalligator::BuffetMenu::get(0));
    TEST_EQUAL(heap.placement()->type(), 0, "an explicit heap claim came from the wrong placement");
    TEST_REQUIRE(heap.raw() != bytes.raw(), "two placements handed out the same memory");
    return 0;
}
