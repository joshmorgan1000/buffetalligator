/** --------------------------------------------------------------------------------------------------------- Public Memory Contracts
 * @file memory_contracts.cpp
 * @brief Tests aligned claims, prepared backing, typed sizes, and shared lifetimes through the public header.
 */
#include "test_support.hpp"
#include <alligator.hpp>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace buffetalligator;
thread_local size_t caller_allocations = 0;
/** --------------------------------------------------------------------------------------------------------- Allocate
 * @brief Supplies aligned, zeroed backing while recording calls on the claiming thread.
 */
Placemat::Handle* allocate(size_t bytes, void*) {
    ++caller_allocations;
    void* memory = std::aligned_alloc(64, bytes);
    if (!memory) return nullptr;
    std::memset(memory, 0, bytes);
    return new Placemat::Handle{memory, nullptr};
}
/** --------------------------------------------------------------------------------------------------------- Deallocate
 * @brief Releases backing before the library deletes the public placement handle.
 */
void deallocate(Placemat::Handle* handle, void*) {
    std::free(handle->substrate_handle);
}
/** --------------------------------------------------------------------------------------------------------- Host Address
 * @brief Resolves the test placement's actual backing address.
 */
void* host_address(Placemat::Handle* handle) {
    return handle->substrate_handle;
}
/** --------------------------------------------------------------------------------------------------------- Placement
 * @brief Registers a bounded one-megabyte slab fixture through the public API.
 */
const Placemat* placement() {
    PlacementDescription description;
    description.name = "public_memory_contract";
    description.slab_bytes = 1024 * 1024;
    description.budget_bytes = 32 * 1024 * 1024;
    description.allocate = allocate;
    description.deallocate = deallocate;
    description.get_host_ptr = host_address;
    return BuffetMenu::get(BuffetMenu::register_type(description));
}
/** --------------------------------------------------------------------------------------------------------- Alignment and Zeroing
 * @brief Verifies odd-sized live claims on both built-ins and explicit novel backing.
 */
void alignment_and_zeroing() {
    constexpr size_t sizes[] = {1, 2, 3, 7, 63, 64, 65, 127, 128, 129, 4095, 4096, 4097, 65537};
    std::vector<Slice> held;
    for (uint16_t type = 0; type < 2; ++type) {
        for (bool novel : {false, true}) {
            for (size_t bytes : sizes) {
                Slice slice(bytes, novel, BuffetMenu::get(type));
                TEST_EQUAL(slice.size_bytes(), bytes, "An odd-sized claim must preserve its requested byte count");
                TEST_EQUAL(reinterpret_cast<uintptr_t>(slice.raw()) % 64, uintptr_t{0},
                    "Each built-in Slice must begin at a 64-byte boundary");
                const auto* dirty = std::find_if(slice.data(), slice.data() + bytes,
                    [](uint8_t value) { return value != 0; });
                if (dirty != slice.data() + bytes)
                    TEST_EQUAL(*dirty, uint8_t{0}, "Every byte in a fresh claim must be zero");
                std::memset(slice.raw(), static_cast<int>(held.size() + 1), bytes);
                held.push_back(std::move(slice));
            }
        }
    }
    for (size_t index = 0; index < held.size(); ++index) {
        const auto& slice = held[index];
        const auto expected = static_cast<uint8_t>(index + 1);
        const auto* damaged = std::find_if(slice.data(), slice.data() + slice.size_bytes(),
            [expected](uint8_t value) { return value != expected; });
        if (damaged != slice.data() + slice.size_bytes())
            TEST_EQUAL(*damaged, expected, "Later claims must not overwrite any byte in an earlier live Slice");
    }
}
/** --------------------------------------------------------------------------------------------------------- Prepared Claims
 * @brief Checks that claims within an already prepared slab never call its backing allocator.
 */
void prepared_claims() {
    const auto* owner = placement();
    { Slice warmup(64, owner); }
    const size_t before = caller_allocations;
    for (size_t index = 0; index < 4096; ++index) {
        Slice slice(64, owner);
        TEST_EQUAL(slice.size_bytes(), size_t{64}, "A prepared claim must retain its byte count");
        slice.data()[0] = 17;
    }
    TEST_EQUAL(caller_allocations, before,
        "4096 claims inside prepared capacity must not invoke the backing allocator on the claiming thread");
}
/** --------------------------------------------------------------------------------------------------------- Typed Count
 * @brief Checks element-count construction without depending on the private Slice representation.
 */
template<class Element> void typed_count() {
    for (size_t count : {size_t{0}, size_t{1}, size_t{3}, size_t{19}, size_t{257}}) {
        SliceT<Element> values(count);
        TEST_EQUAL(values.size_bytes(), sizeof(Element) * count,
            "SliceT<T>(count) must expose exactly sizeof(T) times count bytes");
    }
    bool rejected = false;
    try { SliceT<Element> oversized(SIZE_MAX / sizeof(Element) + 1); }
    catch (const AlligatorException&) { rejected = true; }
    TEST_REQUIRE(rejected, "An overflowing typed element count must be rejected before claiming memory");
}
/** --------------------------------------------------------------------------------------------------------- Typed Sizes
 * @brief Tests multiple element widths around the allocator's alignment boundaries.
 */
void typed_sizes() {
    typed_count<uint16_t>();
    typed_count<uint32_t>();
    typed_count<uint64_t>();
}
/** --------------------------------------------------------------------------------------------------------- Views Across Rollover
 * @brief Keeps shared views alive through owner-thread exit, slab rollover, and release on other threads.
 */
void views_across_rollover() {
    const auto* owner = placement();
    std::array<Slice, 8> views;
    std::array<void*, 8> addresses;
    std::thread producer([&] {
        test_support::step("Creating shared views before their owner thread exits");
        Slice parent(4096, owner);
        for (size_t index = 0; index < views.size(); ++index) {
            views[index] = parent.slice(index * 512, 512);
            std::memset(views[index].raw(), static_cast<int>(index + 1), 512);
            addresses[index] = views[index].raw();
        }
    });
    producer.join();
    test_support::step("Advancing through sixteen slab capacities while views remain live");
    for (size_t index = 0; index < 64; ++index) {
        Slice temporary(256 * 1024, owner);
        temporary.data()[0] = 0xe1;
        temporary.data()[temporary.size_bytes() - 1] = 0x7b;
    }
    std::array<std::thread, 8> consumers;
    for (size_t index = 0; index < consumers.size(); ++index) {
        consumers[index] = std::thread([&, index] {
            TEST_EQUAL(views[index].raw(), addresses[index],
                "Moving ownership across threads and slab rollover must preserve the view address");
            for (size_t byte = 0; byte < views[index].size_bytes(); ++byte)
                TEST_EQUAL(views[index].data()[byte], static_cast<uint8_t>(index + 1),
                    "A live view must preserve every byte after its owner exits and slabs advance");
            views[index].free();
            TEST_REQUIRE(views[index].is_null(), "A view released on another thread must become null");
        });
    }
    for (auto& consumer : consumers) consumer.join();
}
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs each public memory contract in an independent process.
 */
int main(int count, char** arguments) {
    test_support::start(__FILE__);
    TEST_EQUAL(count, 2, "Select one public memory contract");
    const std::string_view selected(arguments[1]);
    test_support::step(arguments[1]);
    if (selected == "alignment_and_zeroing") alignment_and_zeroing();
    else if (selected == "prepared_claims") prepared_claims();
    else if (selected == "typed_sizes") typed_sizes();
    else if (selected == "views_across_rollover") views_across_rollover();
    else TEST_REQUIRE(false, "Unknown public memory contract");
    BuffetMenu::shutdown();
    return 0;
}
