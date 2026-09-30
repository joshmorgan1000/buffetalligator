/** --------------------------------------------------------------------------------------------------------- Memory Tracker Test
 * @file memory_tracker_test.cpp
 * @brief Checks descriptor counters, retained views, failed allocations, and completed releases.
 */
#include <alligator/containers.hpp>
#include <memory/tracker.hpp>
#include "functional_support.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <semaphore>
#include <thread>

using namespace buffetalligator;
using functional::require;
namespace {
struct Allocation { void* memory; size_t bytes; };
BuffetDescriptor placement{};
std::atomic<size_t> allocated_bytes{0};
std::atomic<size_t> freed_bytes{0};
std::atomic<bool> reject_allocation{false};
std::atomic<bool> hold_deallocation{false};
std::binary_semaphore deallocation_entered{0};
std::binary_semaphore release_deallocation{0};
/** --------------------------------------------------------------------------------------------------------- Allocate
 * @brief Records a successful custom descriptor allocation.
 */
void* allocate(size_t bytes) {
    if (reject_allocation.exchange(false)) throw std::bad_alloc();
    void* memory = std::aligned_alloc(64, bytes);
    if (!memory) throw std::bad_alloc();
    std::memset(memory, 0, bytes);
    Allocation* allocation = new Allocation{memory, bytes};
    allocated_bytes.fetch_add(bytes, std::memory_order_relaxed);
    BuffetDescriptors::record_allocation(placement.type_idx, bytes, allocation);
    return allocation;
}
/** --------------------------------------------------------------------------------------------------------- Deallocate
 * @brief Pauses a selected backing release before recording its completion.
 */
void deallocate(void* handle) {
    if (hold_deallocation.exchange(false)) {
        deallocation_entered.release();
        release_deallocation.acquire();
    }
    Allocation* allocation = static_cast<Allocation*>(handle);
    const size_t bytes = allocation->bytes;
    std::free(allocation->memory);
    delete allocation;
    BuffetDescriptors::record_deallocation(placement.type_idx, bytes, handle);
    freed_bytes.fetch_add(bytes, std::memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Size
 * @brief Reports a custom allocation's exact length.
 */
size_t allocation_size(void* handle) { return static_cast<Allocation*>(handle)->bytes; }
/** --------------------------------------------------------------------------------------------------------- Host Pointer
 * @brief Resolves a byte offset into registered backing.
 */
void* host_ptr(void* handle, size_t offset) {
    return static_cast<char*>(static_cast<Allocation*>(handle)->memory) + offset;
}
/** --------------------------------------------------------------------------------------------------------- Device Address
 * @brief Supplies this CPU descriptor's address domain.
 */
uint64_t device_address(void* handle) {
    return reinterpret_cast<uint64_t>(static_cast<Allocation*>(handle)->memory);
}
/** --------------------------------------------------------------------------------------------------------- Release Slice
 * @brief Releases a retained Slice on a thread whose descriptor callback can be paused.
 */
void release_slice(Slice* slice) { slice->free(); }
/** --------------------------------------------------------------------------------------------------------- Tracking
 * @brief Exercises public descriptor tracking with a paused final-owner release.
 */
void tracking() {
    static_cast<void>(Slice::default_placement());
    constexpr size_t slab_bytes = 64ull * 1024 * 1024;
    constexpr size_t novel_bytes = 8192;
    const uint8_t type = static_cast<uint8_t>(BuffetDescriptors::count());
    placement = {"tracker_test", deallocate, host_ptr, allocation_size, allocate,
        type, slab_bytes, device_address};
    BuffetDescriptors::register_descriptor(&placement);
    Slice warmup(64, &placement);
    const size_t initial_allocations = allocated_bytes.load(std::memory_order_relaxed);
    require(initial_allocations >= slab_bytes, "chain initialization allocated no backing");
    const size_t global_allocations = Memory::total_allocations();
    const size_t global_freed = Memory::total_freed();
    require(Memory::placement_allocations(placement) == initial_allocations,
        "tracker missed or duplicated chain backing");
    reject_allocation.store(true);
    bool rejected = false;
    try { Slice failed(novel_bytes, true, &placement); }
    catch (const std::bad_alloc&) { rejected = true; }
    require(rejected && allocated_bytes.load() == initial_allocations
        && Memory::total_allocations() == global_allocations,
        "failed allocation did not preserve counters and its exception");
    Slice novel(novel_bytes, true, &placement);
    const void* handle = SliceEntry::from_slice(novel)->token()->buffet();
    const auto details = Memory::allocation_info(handle);
    if constexpr (BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING) {
        require(details.has_value() && details->size == novel_bytes
            && details->placement == type && details->timestamp > 0
            && details->location.line() > 0,
            "enabled tracking omitted allocation details");
    } else {
        require(!details, "disabled location tracking returned an allocation record");
    }
    Slice retained = novel.slice(1, novel_bytes - 1);
    require(allocated_bytes.load() == initial_allocations + novel_bytes,
        "Slice view allocated new backing");
    hold_deallocation.store(true);
    novel.free();
    require(freed_bytes.load() == 0 && retained.valid(), "view failed to retain backing");
    std::thread releaser(release_slice, &retained);
    require(deallocation_entered.try_acquire_for(std::chrono::seconds(2)),
        "last owner did not enter its backing deleter");
    require(Memory::total_freed() == global_freed && Memory::placement_freed(placement) == 0,
        "tracker reported a paused deallocation as completed");
    require(Memory::allocation_info(handle).has_value()
        == bool(BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING),
        "paused deallocation retired its location record");
    release_deallocation.release();
    releaser.join();
    require(freed_bytes.load(std::memory_order_acquire) == novel_bytes
        && !Memory::allocation_info(handle),
        "completed deallocation leaked or duplicated backing");
    require(Memory::total_freed() == global_freed + novel_bytes
        && Memory::placement_freed(placement) == novel_bytes
        && Memory::placement_usage(placement) == initial_allocations,
        "completed deallocation did not update tracker totals");
    {
        SliceQueue queue(1, 1);
        auto producer = queue.producer(0);
        producer.push(Slice(64, true, &placement));
    }
    require(freed_bytes.load(std::memory_order_acquire) == novel_bytes + 64,
        "queue destruction did not release its undelivered backing");
    require(Memory::placement_allocations(placement) == allocated_bytes.load()
        && Memory::placement_freed(placement) == freed_bytes.load(),
        "tracker counters differ from completed callbacks");
    Memory::set_placement_available(placement, slab_bytes);
    require(Memory::placement_available(placement) == slab_bytes,
        "available capacity reporting differs");
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs tracking contracts with the selected location-tracking configuration.
 */
int main() {
    LOG_INFO_STREAM << "Checking descriptor counters with code-location tracking "
        << (BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING ? "enabled" : "disabled");
    tracking();
    LOG_INFO_STREAM << "Allocation tracking contracts passed";
}
