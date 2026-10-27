/** --------------------------------------------------------------------------------------------------------- Memory Tracker Test
 * @file memory_tracker_test.cpp
 * @brief Checks placement counters, retained views, failed allocations, and completed releases.
 */
#include <alligator.hpp>
#include <alligator/containers.hpp>
#include <memory/tracker.hpp>
#include "functional_support.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
#include <semaphore>
#include <span>
#include <thread>
#include <utility>

using namespace buffetalligator;
using functional::require;
namespace {
std::atomic<size_t> allocated_bytes{0};
std::atomic<size_t> freed_bytes{0};
std::atomic<bool> reject_allocation{false};
std::atomic<bool> hold_deallocation{false};
std::binary_semaphore deallocation_entered{0};
std::binary_semaphore release_deallocation{0};
/** --------------------------------------------------------------------------------------------------------- Tracker Placement
 * @brief Real aligned storage whose factory can reject once and whose deleter can pause once.
 */
struct alignas(16) TrackerPlacement {
    AlignedHeapBuffer memory;
    TrackerPlacement(size_t bytes = 0) : memory(bytes) {
        if (bytes != 0) allocated_bytes.fetch_add(memory.size(), std::memory_order_relaxed);
    }
    TrackerPlacement(const TrackerPlacement&) = delete;
    TrackerPlacement& operator=(const TrackerPlacement&) = delete;
    TrackerPlacement(TrackerPlacement&& other) noexcept : memory(std::move(other.memory)) {}
    TrackerPlacement& operator=(TrackerPlacement&& other) noexcept {
        memory = std::move(other.memory);
        return *this;
    }
    ~TrackerPlacement() {
        if (memory) freed_bytes.fetch_add(memory.size(), std::memory_order_release);
    }
    operator bool() const { return bool(memory); }
    bool operator==(std::nullptr_t) const { return memory == nullptr; }
    static void deleter_impl(void* buffer) {
        if (hold_deallocation.exchange(false)) {
            deallocation_entered.release();
            release_deallocation.acquire();
        }
        delete static_cast<TrackerPlacement*>(buffer);
    }
    static void (*deleter())(void*) { return deleter_impl; }
    static void* host_ptr(void* buffer) { return static_cast<TrackerPlacement*>(buffer)->memory.raw(); }
    static size_t size_of(void* buffer) { return static_cast<TrackerPlacement*>(buffer)->memory.size(); }
    static void* factory(size_t bytes) {
        if (reject_allocation.exchange(false)) throw std::bad_alloc();
        return new TrackerPlacement(bytes);
    }
    static size_t default_size() { return functional::SLAB_BYTES; }
    static size_t* type_idx() { static size_t idx = 0; return &idx; }
    static const char* type_name() { return "TrackerContract"; }
    static uint64_t device_address(void* buffer) { return reinterpret_cast<uint64_t>(host_ptr(buffer)); }
    size_t size() const { return memory.size(); }
    void* raw() { return memory.raw(); }
};
static_assert(IsABuffetType<TrackerPlacement>, "TrackerPlacement must satisfy IsABuffetType");
/// @brief Queue handlers for a queue that is destroyed before any consumer runs.
void ignore_single(void*, Slice&&) {}
void ignore_bulk(void*, std::span<Slice>) {}
/// @brief Releases a retained Slice on a thread whose placement deleter can be paused.
void release_slice(Slice* slice) { slice->free(); }
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs tracking contracts with the selected location-tracking configuration.
 */
int main() {
    require(setenv("ALLIGATOR_GPU_BACKEND", "cpu", 1) == 0, "CPU selection failed");
    LOG_INFO_STREAM << "Checking placement counters with code-location tracking "
        << (BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING ? "enabled" : "disabled");
    constexpr size_t novel_bytes = 8192;
    const BuffetDescriptor* placement = BuffetDescriptors::descriptor<TrackerPlacement>();
    Slice warmup(64, placement);
    const size_t initial_allocations = allocated_bytes.load(std::memory_order_relaxed);
    require(initial_allocations >= placement->default_size_method(), "chain initialization allocated no backing");
    const size_t global_allocations = Memory::total_allocations();
    const size_t global_freed = Memory::total_freed();
    require(Memory::placement_allocations(*placement) == initial_allocations,
        "tracker missed or duplicated chain backing");
    reject_allocation.store(true);
    bool rejected = false;
    try { Slice failed(novel_bytes, true, placement); }
    catch (const std::bad_alloc&) { rejected = true; }
    require(rejected && allocated_bytes.load() == initial_allocations
        && Memory::total_allocations() == global_allocations,
        "failed allocation did not preserve counters and its exception");
    Slice novel(novel_bytes, true, placement);
    const void* handle = Alligator::buffet_of(novel);
    require(Memory::allocation_info(handle).has_value() == bool(BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING),
        "location tracking did not follow its build configuration");
    Slice retained = novel.slice(1, novel_bytes - 1);
    require(allocated_bytes.load() == initial_allocations + novel_bytes, "Slice view allocated new backing");
    hold_deallocation.store(true);
    novel.free();
    require(freed_bytes.load() == 0 && retained.valid(), "view failed to retain backing");
    std::thread releaser(release_slice, &retained);
    require(deallocation_entered.try_acquire_for(std::chrono::seconds(2)),
        "last owner did not enter its backing deleter");
    release_deallocation.release();
    releaser.join();
    require(freed_bytes.load(std::memory_order_acquire) == novel_bytes && !Memory::allocation_info(handle),
        "completed deallocation leaked or duplicated backing");
    require(Memory::total_freed() == global_freed + novel_bytes
        && Memory::placement_freed(*placement) == novel_bytes
        && Memory::placement_usage(*placement) == initial_allocations,
        "completed deallocation did not update tracker totals");
    {
        SliceQueue queue(1, 1, 64, 64, &ignore_single, &ignore_bulk);
        auto producer = queue.producer(0);
        producer.push(Slice(64, true, placement));
    }
    require(freed_bytes.load(std::memory_order_acquire) == novel_bytes + 64,
        "queue destruction did not release its undelivered backing");
    require(Memory::placement_allocations(*placement) == allocated_bytes.load()
        && Memory::placement_freed(*placement) == freed_bytes.load(),
        "tracker counters differ from completed callbacks");
    Memory::set_placement_available(*placement, functional::SLAB_BYTES);
    require(Memory::placement_available(*placement) == functional::SLAB_BYTES,
        "available capacity reporting differs");
    LOG_INFO_STREAM << "Allocation tracking contracts passed";
}
