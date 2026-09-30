/** --------------------------------------------------------------------------------------------------------- Slice Region Test
 * @file slice_region_test.cpp
 * @brief Exercises region-construction rollback and retained copies across a region boundary.
 */
#include <alligator.hpp>
#include <memory/tracker.hpp>
#include "functional_support.hpp"
#include <atomic>
#include <cstdlib>
#include <stdexcept>
#include <vector>

using namespace buffetalligator;
using functional::require;
namespace {
std::atomic<bool> fail_next_region{true};
/** --------------------------------------------------------------------------------------------------------- Region Construction Failure
 * @brief Injects one failure after the second region's backing allocation succeeds.
 */
void fail_region_construction(uint8_t index) {
    if (index == 1 && fail_next_region.exchange(false, std::memory_order_relaxed)) {
        throw std::runtime_error("Controlled region construction failure");
    }
}
} // namespace
#define BUFFETALLIGATOR_TEST_REGION_CONSTRUCTION(index) fail_region_construction(index)
#include "../src/memory/alligator.cpp"
#undef BUFFETALLIGATOR_TEST_REGION_CONSTRUCTION
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Verifies failed publication can retry without losing allocation or Slice ownership.
 */
int main() {
    require(setenv("ALLIGATOR_GPU_BACKEND", "cpu", 1) == 0, "CPU selection failed");
    const BuffetDescriptor* heap = BuffetDescriptors::get(AlignedHeapBuffer::type_idx());
    Slice source(size_t{64}, true, heap);
    source.get_as<uint64_t>() = UINT64_C(0x413115A70);
    require(((source.id() >> 3) & 63) == 0, "the first Slice did not start in region zero");
    std::vector<Slice> copies;
    copies.reserve(REGION_SIZE);
    LOG_INFO_STREAM << "Filling Slice region zero for a controlled publication retry";
    for (size_t index = 1; index < REGION_SIZE; ++index) {
        copies.emplace_back(source);
        if ((index & 65535) == 0) LOG_INFO_STREAM << "Published " << index << " retained copies";
    }
    const uint64_t before = Memory::total_allocations() - Memory::total_freed();
    bool rejected = false;
    try {
        copies.emplace_back(source);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected && !fail_next_region.load(std::memory_order_relaxed),
        "controlled second-region construction did not fail");
    require(Memory::total_allocations() - Memory::total_freed() == before,
        "failed region construction retained its backing allocation");
    copies.emplace_back(source);
    Slice retained(std::move(copies.back()));
    require(((retained.id() >> 3) & 63) == 1, "region construction did not retry its failed slot");
    require(retained.raw() == source.raw() && retained.size_bytes() == source.size_bytes(),
        "cross-region copy lost its pointer or granule length");
    const GPUBuf original = *Alligator::gpubuf_for(source);
    const GPUBuf copied = *Alligator::gpubuf_for(retained);
    require(copied.address == original.address && copied.size == original.size
        && copied.offset == original.offset, "cross-region copy changed its GPU record");
    copies.clear();
    source.free();
    require(retained.get_as<uint64_t>() == UINT64_C(0x413115A70),
        "cross-region copy lost its backing after other owners retired");
    retained.free();
    LOG_INFO_STREAM << "Region construction retry and cross-region copy ownership passed";
}
