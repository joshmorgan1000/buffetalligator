/** --------------------------------------------------------------------------------------------------------- Memory Tracker Test
 * @file memory_tracker_test.cpp
 * @brief Checks allocation counters and optional code-location records in both build modes.
 */
#include <alligator.hpp>
#include <memory/tracker.hpp>
#include "functional_support.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <semaphore>

using namespace buffetalligator;
using functional::require;
namespace {
/** --------------------------------------------------------------------------------------------------------- Allocation
 * @brief Owns one test placement allocation and its byte count.
 */
struct Allocation {
    void* memory = nullptr;
    size_t bytes = 0;
    ~Allocation() { std::free(memory); }
};
std::atomic<size_t> allocated_bytes{0};
std::atomic<size_t> freed_bytes{0};
std::atomic<bool> reject_allocation{false};
std::atomic<bool> hold_deallocation{false};
std::binary_semaphore deallocation_entered{0};
std::binary_semaphore release_deallocation{0};
/** --------------------------------------------------------------------------------------------------------- Allocate
 * @brief Allocates backing storage and records successful placement callbacks independently.
 */
std::pair<void*, void*> allocate(size_t bytes, void*) {
    if (reject_allocation.exchange(false)) throw std::bad_alloc();
    auto allocation = std::make_unique<Allocation>();
    allocation->bytes = bytes;
    allocation->memory = std::calloc(1, bytes);
    if (!allocation->memory) throw std::bad_alloc();
    void* substrate = allocation.release();
    allocated_bytes.fetch_add(bytes, std::memory_order_relaxed);
    return {static_cast<Allocation*>(substrate)->memory, substrate};
}
/** --------------------------------------------------------------------------------------------------------- Deallocate
 * @brief Pauses selected teardown callbacks before recording completed backing releases.
 */
std::pair<void*, void*> deallocate(void* host_ptr, void* substrate_handle) {
    static_cast<void>(host_ptr);
    if (hold_deallocation.exchange(false)) {
        deallocation_entered.release();
        release_deallocation.acquire();
    }
    auto* allocation = static_cast<Allocation*>(substrate_handle);
    const size_t bytes = allocation->bytes;
    delete allocation;
    freed_bytes.fetch_add(bytes, std::memory_order_relaxed);
    return {nullptr, nullptr};
}
/** --------------------------------------------------------------------------------------------------------- Context
 * @brief Returns the unused placement context.
 */
void* context() { return nullptr; }
/** --------------------------------------------------------------------------------------------------------- Completed Frees
 * @brief Reads placement totals after previously queued teardown callbacks have returned.
 */
size_t completed_frees() { return freed_bytes.load(std::memory_order_relaxed); }
/** --------------------------------------------------------------------------------------------------------- Tracking
 * @brief Exercises successful allocations, failed allocations, retained views, and queued frees.
 */
void tracking() {
    constexpr size_t slab_bytes = 64ull * 1024 * 1024;
    constexpr size_t novel_bytes = 8192;
    const uint16_t type = BuffetMenu::register_type(
        "tracker_test", slab_bytes, 64, allocate, deallocate, context, true
    );
    const Placemat& placement = *BuffetMenu::get(type);
    Slice warmup(64);
    const size_t initial_allocations = allocated_bytes.load(std::memory_order_relaxed);
    require(initial_allocations == 3 * slab_bytes, "initial slab allocation count differs");
    const size_t global_allocations = Memory::total_allocations();
    const size_t global_freed = Memory::total_freed();
    require(Memory::placement_allocations(placement) == initial_allocations,
        "tracker missed or duplicated an initial slab");
    require(Memory::placement_freed(placement) == 0, "tracker reported an unfreed slab");
    reject_allocation.store(true);
    bool rejected = false;
    try { Slice failed(novel_bytes, true); }
    catch (const std::bad_alloc&) { rejected = true; }
    require(rejected, "failed allocation did not propagate");
    require(allocated_bytes.load() == initial_allocations, "failed allocation changed totals");
    require(Memory::total_allocations() == global_allocations,
        "tracker counted a failed allocation");
    Slice novel(novel_bytes, true);
    const Placemat::Plate* plate = Alligator::plate_for(novel);
    const auto details = Memory::allocation_info(plate);
    if constexpr (BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING) {
        require(details.has_value(), "enabled tracking omitted a live allocation record");
        require(details->size == novel_bytes && details->placement == type &&
            details->timestamp > 0 && details->location.line() > 0 &&
            std::string_view(details->location.file_name()).ends_with("buffet.cpp") &&
            std::string_view(details->location.function_name()).find("Buffet") !=
                std::string_view::npos, "allocation location details differ");
    } else {
        require(!details, "disabled location tracking returned an allocation record");
    }
    Slice retained = novel.slice(1, novel_bytes - 1);
    require(allocated_bytes.load() == initial_allocations + novel_bytes,
        "Slice view allocated new backing");
    require(Memory::total_allocations() == global_allocations + novel_bytes &&
        Memory::placement_allocations(placement) == initial_allocations + novel_bytes,
        "novel allocation was missed or counted more than once");
    hold_deallocation.store(true);
    novel.free();
    require(freed_bytes.load() == 0 && retained.valid(), "view did not retain its backing");
    retained.free();
    require(deallocation_entered.try_acquire_for(std::chrono::seconds(2)),
        "last Slice did not schedule its backing deallocation");
    require(Memory::total_freed() == global_freed && Memory::placement_freed(placement) == 0,
        "tracker reported deallocation before the callback completed");
    require(Memory::allocation_info(plate).has_value() ==
        bool(BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING),
        "location record retired before the deallocation callback completed");
    release_deallocation.release();
    require(std::async(std::launch::async, completed_frees).get() == novel_bytes,
        "backing was not deallocated exactly once");
    require(!Memory::allocation_info(plate), "completed deallocation retained a location record");
    require(Memory::total_freed() == global_freed + novel_bytes &&
        Memory::placement_freed(placement) == novel_bytes &&
        Memory::placement_usage(placement) == initial_allocations,
        "completed deallocation did not update tracker totals");
    {
        SliceQueue queue(1, 1);
        auto producer = queue.producer(0);
        producer.push(Slice(64, true));
    }
    require(std::async(std::launch::async, completed_frees).get() == novel_bytes + 64,
        "queue destruction leaked its undelivered backing");
    require(Memory::placement_allocations(placement) == allocated_bytes.load() &&
        Memory::placement_freed(placement) == freed_bytes.load(),
        "tracker totals differ from completed placement callbacks");
    Memory::set_placement_available(placement, slab_bytes);
    require(Memory::placement_available(placement) == slab_bytes,
        "capacity reporting depends on optional location tracking");
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs the tracker contract using the library's selected build configuration.
 */
int main() {
    LOG_INFO_STREAM << "Checking allocation counters with code-location tracking "
        << (BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING ? "enabled" : "disabled");
    tracking();
    LOG_INFO_STREAM << "Allocation tracking contracts passed";
}
