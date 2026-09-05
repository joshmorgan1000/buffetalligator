/** --------------------------------------------------------------------------------------------------------- Memory Contract Test
 * @file memory_test.cpp
 * @brief Verifies placement registration, worker preallocation, runway replenishment, chain
 * rollover, Slice lifetime, novel buffers, and tracking.
 */
#include <buffetalligator.hpp>
#include <memory/slicefriend.hpp>
#include <memory/tracker.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::atomic<size_t> allocations{0};
std::atomic<size_t> deallocations{0};
std::atomic<size_t> startup_thread_allocations{0};
std::atomic<bool> background_allocation{false};
std::thread::id startup_thread = std::this_thread::get_id();
buffetalligator::Placemat::Handle* test_allocate(size_t size, void*) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (std::this_thread::get_id() == startup_thread) {
        startup_thread_allocations.fetch_add(1, std::memory_order_relaxed);
    } else {
        background_allocation.store(true, std::memory_order_relaxed);
    }
    void* memory = std::aligned_alloc(64, size);
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    std::memset(memory, 0, size);
    return new buffetalligator::Placemat::Handle{memory, nullptr};
}
void test_deallocate(buffetalligator::Placemat::Handle* handle, void*) {
    deallocations.fetch_add(1, std::memory_order_relaxed);
    if (handle != nullptr && handle->substrate_handle != nullptr) {
        std::free(handle->substrate_handle);
    }
}
void* test_host_ptr(buffetalligator::Placemat::Handle* handle) {
    return handle->substrate_handle;
}
void* test_context() {
    return nullptr;
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
    const uint16_t type = buffetalligator::BuffetMenu::register_type(
        "test_placement",
        64ull * 1024 * 1024,
        64,
        &test_allocate,
        &test_deallocate,
        &test_host_ptr,
        &test_context,
        true
    );
    require(type == 2, "custom placement did not receive the expected stable identifier");
    buffetalligator::Slice bytes(4096);
    require(sizeof(bytes) == 16, "Slice is not 16 bytes");
    require(bytes.placement()->type() == type, "default strategy did not select the custom placement");
    require(background_allocation.load(std::memory_order_relaxed),
        "the dedicated allocator thread did not preallocate the successor");
    require(std::all_of(bytes.data<uint8_t>(), bytes.data<uint8_t>() + bytes.size_bytes(),
        [](uint8_t value) { return value == 0; }), "fresh slice was not zero initialized");
    buffetalligator::Slice probe(1024);
    require(static_cast<char*>(probe.raw()) - static_cast<char*>(bytes.raw()) == 4096,
        "the bump cursor did not advance by the exact claim size");
    bytes.data<uint8_t>()[128] = 91;
    buffetalligator::Slice shared = bytes.slice();
    bytes.free();
    require(shared.data<uint8_t>()[128] == 91, "shared Slice did not retain the slab");
    buffetalligator::Slice view = shared.slice(64, 256);
    require(view.size_bytes() == 256, "sub-slice size is incorrect");
    require(static_cast<char*>(view.raw()) - static_cast<char*>(shared.raw()) == 64,
        "sub-slice offset is incorrect");
    buffetalligator::Slice almost_full(64ull * 1024 * 1024 - 8192);
    buffetalligator::Slice rollover(16384);
    require(almost_full.valid(), "large bump-pointer claim failed");
    require(rollover.valid(), "chain rollover claim failed");
    std::atomic<bool> concurrent_claim_failed{false};
    std::array<std::thread, 8> workers;
    for (std::thread& worker : workers) {
        worker = std::thread(&claim_worker, &concurrent_claim_failed);
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    require(!concurrent_claim_failed.load(std::memory_order_relaxed),
        "concurrent claims overlapped or failed");
    const size_t caller_allocations_before_novel =
        startup_thread_allocations.load(std::memory_order_relaxed);
    const size_t deallocations_before_novel = deallocations.load(std::memory_order_relaxed);
    {
        buffetalligator::Slice novel(8192, true);
        require(novel.placement()->type() == type, "novel Slice used the wrong placement");
        require(startup_thread_allocations.load(std::memory_order_relaxed) ==
            caller_allocations_before_novel + 1, "novel Slice was not allocated on the calling thread");
        require(deallocations.load(std::memory_order_relaxed) == deallocations_before_novel,
            "novel Slice was torn down before the end of its scope");
    }
    const auto teardown_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (deallocations.load(std::memory_order_relaxed) == deallocations_before_novel) {
        if (std::chrono::steady_clock::now() > teardown_deadline) {
            require(false, "the worker did not tear down the retired novel slab");
        }
        std::this_thread::yield();
    }
    buffetalligator::Placemat* aligned_placement = buffetalligator::BuffetMenu::get(1);
    buffetalligator::Slice aligned(1024, aligned_placement);
    require(aligned.placement() == aligned_placement, "aligned placement identity failed");
    require(reinterpret_cast<uintptr_t>(aligned.raw()) % 64 == 0,
        "aligned heap Slice is not 64-byte aligned");
    require(buffetalligator::Memory::total_allocations() >= 64ull * 1024 * 1024,
        "Memory tracker did not record placement allocations");
    require(buffetalligator::Memory::placement_usage(
        *buffetalligator::BuffetMenu::get(type)) >= 64ull * 1024 * 1024,
        "Memory tracker did not retain per-placement usage");
    buffetalligator::AtomicRegistry registry;
    buffetalligator::AtomicContainer* counter = registry.create<uint64_t>("counter", uint64_t(4));
    require(counter->fetch_add<uint64_t>(3, std::memory_order_relaxed) == 4,
        "AtomicContainer fetch_add returned the wrong prior value");
    require(counter->load<uint64_t>(std::memory_order_relaxed) == 7,
        "AtomicRegistry did not preserve the AtomicContainer value");
    return 0;
}
