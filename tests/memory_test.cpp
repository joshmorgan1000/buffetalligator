/** --------------------------------------------------------------------------------------------------------- Memory Contract Test
 * @file memory_test.cpp
 * @brief Verifies descriptor registration, chain rollover, Slice ownership, and tracked allocations.
 */
#include <alligator.hpp>
#include <alligator/atomics.hpp>
#include <memory/tracker.hpp>
#include "functional_support.hpp"
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace buffetalligator;
using functional::require;
namespace {
std::atomic<size_t> allocations{0};
std::atomic<size_t> deallocations{0};
BuffetDescriptor placement{};
/** --------------------------------------------------------------------------------------------------------- Test Block
 * @brief Holds one registered allocation and its length.
 */
struct TestBlock { void* memory; size_t size; };
/** --------------------------------------------------------------------------------------------------------- Allocate
 * @brief Allocates and records a block through the test descriptor.
 */
void* test_allocate(size_t size) {
    void* memory = std::aligned_alloc(64, size);
    if (memory == nullptr) throw std::bad_alloc();
    std::memset(memory, 0, size);
    TestBlock* block = new TestBlock{memory, size};
    allocations.fetch_add(1, std::memory_order_relaxed);
    BuffetDescriptors::record_allocation(placement.type_idx, size, block);
    return block;
}
/** --------------------------------------------------------------------------------------------------------- Deallocate
 * @brief Releases and records one completed descriptor allocation.
 */
void test_deallocate(void* handle) {
    TestBlock* block = static_cast<TestBlock*>(handle);
    const size_t bytes = block->size;
    std::free(block->memory);
    delete block;
    BuffetDescriptors::record_deallocation(placement.type_idx, bytes, handle);
    deallocations.fetch_add(1, std::memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Host Pointer
 * @brief Resolves a byte offset within the descriptor's allocation.
 */
void* test_host_ptr(void* handle, size_t offset) {
    return static_cast<char*>(static_cast<TestBlock*>(handle)->memory) + offset;
}
/** --------------------------------------------------------------------------------------------------------- Size
 * @brief Reports the exact backing allocation length.
 */
size_t test_size(void* handle) { return static_cast<TestBlock*>(handle)->size; }
/** --------------------------------------------------------------------------------------------------------- Device Address
 * @brief Supplies the address domain used by this CPU descriptor.
 */
uint64_t test_device_address(void* handle) {
    return reinterpret_cast<uint64_t>(static_cast<TestBlock*>(handle)->memory);
}
/** --------------------------------------------------------------------------------------------------------- Claim Worker
 * @brief Checks deterministic payload isolation across concurrent claims.
 */
void claim_worker(std::atomic<bool>* failed) {
    try {
        for (size_t claim = 0; claim < 1000; ++claim) {
            Slice slice(1024, &placement);
            std::memset(slice.raw(), static_cast<unsigned char>(claim), slice.size_bytes());
            if (slice.data<uint8_t>()[0] != static_cast<uint8_t>(claim)
                || slice.data<uint8_t>()[1023] != static_cast<uint8_t>(claim)) {
                failed->store(true, std::memory_order_relaxed);
            }
        }
    } catch (...) {
        failed->store(true, std::memory_order_relaxed);
    }
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Exercises registered descriptors through the public Slice and tracking contracts.
 */
int main() {
    const BuffetDescriptor* initial_default = Slice::default_placement();
    const uint8_t type = static_cast<uint8_t>(BuffetDescriptors::count());
    placement = {"test_placement", test_deallocate, test_host_ptr, test_size, test_allocate,
        type, 64ull * 1024 * 1024, test_device_address};
    require(BuffetDescriptors::register_descriptor(&placement) == type,
        "registration changed the descriptor's assigned type");
    require(BuffetDescriptors::get(type) == &placement,
        "registered descriptor lookup lost its identity");
    require(Slice::default_placement() == initial_default,
        "registration changed the explicitly selected default");
    Slice bytes(4096, &placement);
    require(sizeof(bytes) == 4 && bytes.placement() == &placement,
        "Slice size or explicit placement differs");
    void* backing = SliceEntry::from_slice(bytes)->token()->buffet();
    require(backing != nullptr, "live Slice lost its backing handle");
    const std::array<unsigned char, 4096> zeros{};
    require(std::memcmp(bytes.raw(), zeros.data(), zeros.size()) == 0,
        "fresh Slice was not zero initialized");
    Slice probe(1024, &placement);
    require(static_cast<char*>(probe.raw()) - static_cast<char*>(bytes.raw()) == 4096,
        "the bump cursor did not advance by the exact claim size");
    bytes.data<uint8_t>()[128] = 91;
    Slice shared = bytes.slice();
    bytes.free();
    require(shared.data<uint8_t>()[128] == 91, "shared Slice did not retain the slab");
    Slice view = shared.slice(64, 256);
    require(SliceEntry::from_slice(view)->token()->buffet() == backing,
        "subview changed its backing handle");
    require(view.size_bytes() == 256 && view.raw() == shared.data<uint8_t>() + 64,
        "subview changed its byte bounds");
    Slice almost_full(64ull * 1024 * 1024 - 8192, &placement);
    Slice rollover(16384, &placement);
    require(almost_full.valid() && rollover.valid(), "chain rollover failed");
    std::atomic<bool> failed{false};
    std::array<std::thread, 8> workers;
    for (std::thread& worker : workers) worker = std::thread(claim_worker, &failed);
    for (std::thread& worker : workers) worker.join();
    require(!failed.load(std::memory_order_relaxed), "concurrent claims overlapped or failed");
    const size_t freed_before = Memory::placement_freed(placement);
    {
        Slice novel(8192, true, &placement);
        require(novel.placement() == &placement && novel.is_novel(),
            "novel Slice lost its placement or ownership kind");
        require(Memory::placement_freed(placement) == freed_before,
            "novel Slice was freed before its owner");
    }
    require(Memory::placement_freed(placement) == freed_before + 8192,
        "novel backing was not released exactly once");
    Slice aligned(1024, initial_default);
    require(reinterpret_cast<uintptr_t>(aligned.raw()) % 64 == 0,
        "default Slice alignment differs");
    require(Memory::placement_usage(placement) >= placement.default_size,
        "tracker lost live chain backing");
    AtomicRegistry registry;
    AtomicContainer* counter = registry.create<uint64_t>("counter", uint64_t(4));
    require(counter->fetch_add<uint64_t>(3, std::memory_order_relaxed) == 4
        && counter->load<uint64_t>(std::memory_order_relaxed) == 7,
        "registered atomic arithmetic differs");
    LOG_INFO_STREAM << "Descriptor allocation and Slice ownership contracts passed";
}
