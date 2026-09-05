/** --------------------------------------------------------------------------------------------------------- Memory Contract Test
 * @file memory_test.cpp
 * @brief Verifies placement registration, preallocation, chain rollover, Slice lifetime, and tracking.
 */
#include <buffetalligator.hpp>
#include <memory/tracker.hpp>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
class TestPlacemat final : public buffetalligator::Placemat {
private:
    std::thread::id startup_thread_ = std::this_thread::get_id();
public:
    std::atomic<size_t> allocations{0};
    std::atomic<size_t> deallocations{0};
    std::atomic<size_t> startup_thread_allocations{0};
    std::atomic<bool> background_allocation{false};
    void* allocate_impl(size_t size, void*) override {
        allocations.fetch_add(1, std::memory_order_relaxed);
        if (std::this_thread::get_id() == startup_thread_) {
            startup_thread_allocations.fetch_add(1, std::memory_order_relaxed);
        } else {
            background_allocation.store(true, std::memory_order_relaxed);
        }
        void* allocation = std::aligned_alloc(64, size);
        if (!allocation) throw std::bad_alloc();
        std::memset(allocation, 0, size);
        return allocation;
    }
    void deallocate(void* handle, void*) override {
        deallocations.fetch_add(1, std::memory_order_relaxed);
        std::free(handle);
    }
    void* context() override { return nullptr; }
    void* host_ptr(void* handle) override { return handle; }
    void* handle(const buffetalligator::Slice& slice) const { return allocation_handle(slice); }
    size_t offset(const buffetalligator::Slice& slice) const { return allocation_offset(slice); }
};
TestPlacemat* test_placement = nullptr;
buffetalligator::Placemat& test_placement_strategy() {
    return *test_placement;
}
bool is_zero(uint8_t value) {
    return value == 0;
}
void claim_worker(std::atomic<bool>* failed) {
    try {
        for (size_t claim = 0; claim < 1000; ++claim) {
            buffetalligator::Slice slice(1024);
            std::fill_n(slice.data<uint8_t>(), slice.size_bytes(), static_cast<uint8_t>(claim));
            if (slice.data<uint8_t>()[0] != static_cast<uint8_t>(claim) ||
                slice.data<uint8_t>()[slice.size_bytes() - 1] != static_cast<uint8_t>(claim)) {
                failed->store(true, std::memory_order_relaxed);
            }
        }
    } catch (...) {
        failed->store(true, std::memory_order_relaxed);
    }
}
}
int main() {
    test_placement = new TestPlacemat();
    const uint16_t type = buffetalligator::buffettypes::BuffetTypeRegistry::register_type(test_placement);
    require(type == 2, "custom placement did not receive the expected stable identifier");
    buffetalligator::Slice::set_default_placement_strategy(&test_placement_strategy);
    buffetalligator::Slice bytes(4096);
    require(sizeof(bytes) == 16, "Slice is not 16 bytes");
    require(test_placement->owns(bytes), "default strategy did not select the custom placement");
    require(test_placement->handle(bytes), "custom placement could not resolve its allocation handle");
    require(test_placement->offset(bytes) == 0, "root allocation offset is not zero");
    require(test_placement->background_allocation.load(std::memory_order_relaxed),
        "the dedicated allocator thread did not preallocate the successor");
    require(std::all_of(bytes.data<uint8_t>(), bytes.data<uint8_t>() + bytes.size_bytes(), &is_zero),
        "fresh slice was not zero initialized");
    bytes.data<uint8_t>()[128] = 91;
    buffetalligator::Slice shared = bytes.slice();
    bytes.free();
    require(shared.data<uint8_t>()[128] == 91, "shared Slice did not retain the slab");
    buffetalligator::Slice view = shared.slice(64, 256);
    require(view.size_bytes() == 256, "sub-slice size is incorrect");
    require(test_placement->offset(view) == 64, "sub-slice allocation offset is incorrect");
    const size_t caller_allocations_before_rollover =
        test_placement->startup_thread_allocations.load(std::memory_order_relaxed);
    buffetalligator::Slice almost_full(64ull * 1024 * 1024 - 8192);
    buffetalligator::Slice rollover(16384);
    require(almost_full.valid(), "large bump-pointer claim failed");
    require(rollover.valid(), "chain rollover claim failed");
    require(test_placement->startup_thread_allocations.load(std::memory_order_relaxed) ==
        caller_allocations_before_rollover, "chain rollover allocated on the calling thread");
    std::atomic<bool> concurrent_claim_failed{false};
    std::array<std::thread, 8> workers;
    for (std::thread& worker : workers) worker = std::thread(&claim_worker, &concurrent_claim_failed);
    for (std::thread& worker : workers) worker.join();
    require(!concurrent_claim_failed.load(std::memory_order_relaxed), "concurrent claims overlapped or failed");
    const size_t caller_allocations_before_novel =
        test_placement->startup_thread_allocations.load(std::memory_order_relaxed);
    const size_t deallocations_before_novel = test_placement->deallocations.load(std::memory_order_relaxed);
    {
        buffetalligator::Slice novel(8192, true);
        require(test_placement->owns(novel), "novel Slice used the wrong placement");
        require(test_placement->startup_thread_allocations.load(std::memory_order_relaxed) ==
            caller_allocations_before_novel + 1, "novel Slice was not allocated on the calling thread");
    }
    require(test_placement->deallocations.load(std::memory_order_relaxed) == deallocations_before_novel + 1,
        "novel Slice did not immediately release its dedicated Buffer");
    buffetalligator::Slice aligned(1024, buffetalligator::buffettypes::aligned_heap());
    require(buffetalligator::buffettypes::aligned_heap().owns(aligned), "aligned placement identity failed");
    require(reinterpret_cast<uintptr_t>(aligned.raw()) % 64 == 0, "aligned heap Slice is not 64-byte aligned");
    require(buffetalligator::Memory::total_allocations() >= 64ull * 1024 * 1024,
        "Memory tracker did not record placement allocations");
    require(buffetalligator::Memory::placement_usage(*test_placement) >= 64ull * 1024 * 1024,
        "Memory tracker did not retain per-placement usage");
    require(buffetalligator::Memory::total_heap_allocations() >= 64ull * 1024 * 1024,
        "Memory tracker did not classify built-in heap allocations");
    buffetalligator::AtomicRegistry registry;
    buffetalligator::AtomicContainer* counter = registry.create<uint64_t>("counter", uint64_t(4));
    require(counter->fetch_add<uint64_t>(3, std::memory_order_relaxed) == 4,
        "AtomicContainer fetch_add returned the wrong prior value");
    require(counter->load<uint64_t>(std::memory_order_relaxed) == 7,
        "AtomicRegistry did not preserve the AtomicContainer value");
    return 0;
}
