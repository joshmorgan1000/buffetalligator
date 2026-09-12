/** --------------------------------------------------------------------------------------------------------- Resource Test
 * @file resource_test.cpp
 * @brief Verifies public placement policy, novel caching, budgets, probes, and trimming.
 */
#include "test_support.hpp"
#include <alligator.hpp>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace {
std::atomic<unsigned> novel_allocations{0};
/** --------------------------------------------------------------------------------------------------------- Allocate
 * @brief Returns a zeroed test allocation with a framework-owned C++ handle.
 */
buffetalligator::Placemat::Handle* allocate(size_t bytes, void*) {
    if (bytes == 8ull * 1024 * 1024) novel_allocations.fetch_add(1);
    void* memory = std::aligned_alloc(64, bytes);
    if (!memory) return nullptr;
    std::memset(memory, 0, bytes);
    return new buffetalligator::Placemat::Handle{memory, nullptr};
}
/** --------------------------------------------------------------------------------------------------------- Deallocate
 * @brief Releases the substrate while the framework owns handle deletion.
 */
void deallocate(buffetalligator::Placemat::Handle* handle, void*) { std::free(handle->substrate_handle); }
/** --------------------------------------------------------------------------------------------------------- Host
 * @brief Resolves the test allocation host address.
 */
void* host(buffetalligator::Placemat::Handle* handle) { return handle->substrate_handle; }
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Exercises resource awareness entirely through the installed public interface.
 */
int main() {
    test_support::start(__FILE__);
    using namespace buffetalligator;
    PlacementDescription description;
    description.name = "resources";
    description.slab_bytes = 16ull * 1024 * 1024;
    description.novel_cache_bytes = 64ull * 1024 * 1024;
    description.allocate = allocate; description.deallocate = deallocate; description.get_host_ptr = host;
    const uint16_t type = BuffetMenu::register_type(description);
    const Placemat* placement = BuffetMenu::get(type);
    {
        Slice first(8ull * 1024 * 1024, true, placement);
        std::memset(first.raw(), 0x6c, first.size_bytes());
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (Memory::placement_novel_cached(*placement) == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    TEST_EQUAL(Memory::placement_novel_cached(*placement), 8ull * 1024 * 1024, "novel cache not prepared");
    {
        Slice second(8ull * 1024 * 1024, true, placement);
        TEST_EQUAL(novel_allocations.load(), 1, "novel allocation was not reused");
        for (size_t index = 0; index < second.size_bytes(); ++index) TEST_EQUAL(second.data<uint8_t>()[index], 0, "cached allocation dirty");
    }
    Memory::trim(*placement);
    TEST_REQUIRE(Memory::placement_reserved(*placement) == 0 && Memory::placement_novel_cached(*placement) == 0, "trim retained idle capacity");
    const size_t slab = Memory::placement_slab_size(*placement);
    Memory::set_placement_budget(*placement, 2 * slab);
    {
        Slice first(slab / 2 + 64, placement);
        Slice second(slab / 2 + 64, placement);
        bool threw = false;
        try { Slice third(slab / 2 + 64, placement); }
        catch (const AlligatorException&) { threw = true; }
        TEST_REQUIRE(threw, "third slab exceeded placement budget");
    }
    TEST_REQUIRE(Memory::system_physical() > 0, "physical capacity missing");
    const size_t page = Memory::page_size();
    TEST_REQUIRE(page && !(page & (page - 1)), "invalid page size");
    TEST_REQUIRE(Memory::hardware_threads() >= 1, "invalid hardware thread count");
    TEST_EQUAL(Memory::placement_budget(*placement), 2 * slab, "budget update missing");
    Memory::trim(*placement);
    TEST_EQUAL(Memory::placement_reserved(*placement), 0, "final trim retained slabs");
    description.name = "invalid_alignment"; description.base_alignment = 16;
    bool threw = false;
    try { BuffetMenu::register_type(description); }
    catch (const AlligatorException&) { threw = true; }
    TEST_REQUIRE(threw, "sub-cache-line placement accepted");
    BuffetMenu::shutdown();
}
