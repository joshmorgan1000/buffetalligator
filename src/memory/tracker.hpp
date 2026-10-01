#pragma once
/** --------------------------------------------------------------------------------------------------------- Memory Tracker
 * @file tracker.hpp
 * @brief Header for the BuffetAlligator Memory Tracker class, a dumb reporting system for
 * completed allocations. Allocation and deallocation calls happen elsewhere and simply report
 * their results here.
 */
#include <alligator.hpp>
#include <alligator/atomics.hpp>
#include <memory/lifetime.hpp>
#include <optional>
#include <source_location>
#ifndef BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING
#define BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING 0
#endif

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Memory
 * @class Memory
 * @brief Tracks completed buffet allocations and deallocations per buffet type. This class
 * performs no allocation itself; the descriptor factory and deleter hooks report here.
 */
class Memory {
public:
    /** ------------------------------------------------------------------------------------------- Allocation Info
     * @brief Describes one completed backing allocation without owning its storage.
     */
    struct AllocationInfo {
        uint64_t timestamp;
        size_t size;
        uint16_t placement;
        std::source_location location;
    };
private:
    /** ------------------------------------------------------------------------------------------- PlacementDetails
     * @struct PlacementDetails
     * @brief Running allocation totals for one buffet type.
     */
    struct PlacementDetails {
        AtomicContainer* total_allocations_;  ///< Bytes allocated through this placement.
        AtomicContainer* total_freed_;  ///< Bytes freed through this placement.
        AtomicContainer* total_available_;  ///< Reported capacity for this placement.
    };
    /// @brief AtomicContainer to track total memory allocations across every placement.
    AtomicContainer* total_allocations_;
    /// @brief AtomicContainer to track total memory freed across every placement.
    AtomicContainer* total_freed_;
    /// @brief Per-type allocation totals, indexed by BuffetDescriptor::type_idx.
    std::vector<PlacementDetails> placement_details_;
    std::array<std::unique_ptr<AtomicContainer>, 26> retired_counters_;
#if BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING
    /// @brief Live backing allocations keyed by their stable placement handles.
    std::unordered_map<const void*, AllocationInfo> allocations_;
    /// @brief Serializes location records shared by allocating threads and the teardown worker.
    AtomicMutex allocations_mutex_;
#endif
    /** ------------------------------------------------------------------------------------------- Constructor - Private
     * @brief Private constructor for singleton pattern.
     */
    Memory() {
        total_allocations_ = AtomicRegistry::create_global<uint64_t>("buffetalligator_allocations", uint64_t(0));
        total_freed_ = AtomicRegistry::create_global<uint64_t>("buffetalligator_freed", uint64_t(0));
        constexpr size_t placement_count = 8;
        placement_details_.reserve(placement_count);
        for (size_t i = 0; i < placement_count; ++i) {
            const std::string prefix = "buffetalligator_placement_" + std::to_string(i) + "_";
            placement_details_.push_back(PlacementDetails{
                AtomicRegistry::create_global<uint64_t>(prefix + "allocations", uint64_t(0)),
                AtomicRegistry::create_global<uint64_t>(prefix + "freed", uint64_t(0)),
                AtomicRegistry::create_global<uint64_t>(prefix + "available", uint64_t(0))
            });
        }
    }
    /** ------------------------------------------------------------------------------------------- Get Instance
     * @brief Returns the singleton instance of the Memory tracker.
     * @return Reference to the Memory tracker instance.
     */
    static Memory& instance() {
        static RuntimeFinalizer lifetime(new Memory, &RuntimeFinalizer::delete_owner<Memory>);
        static RuntimeFinalizer counters(lifetime.object, &Memory::retain_counters,
            RuntimeFinalizer::Phase::TrackerDetach);
        return *static_cast<Memory*>(lifetime.object);
    }
    /** ------------------------------------------------------------------------------------------- Retain Counters
     * @brief Transfers accounting ownership before global values release their arena-backed payloads.
     */
    static void retain_counters(void* context) {
        auto& counters = static_cast<Memory*>(context)->retired_counters_;
        counters[0] = AtomicRegistry::remove_global("buffetalligator_allocations");
        counters[1] = AtomicRegistry::remove_global("buffetalligator_freed");
        for (size_t index = 0; index < 8; ++index) {
            const std::string prefix = "buffetalligator_placement_" + std::to_string(index) + "_";
            counters[2 + index * 3] = AtomicRegistry::remove_global(prefix + "allocations");
            counters[3 + index * 3] = AtomicRegistry::remove_global(prefix + "freed");
            counters[4 + index * 3] = AtomicRegistry::remove_global(prefix + "available");
        }
    }
public:
    /** ------------------------------------------------------------------------------------------- Deleted Copy/Move
     * @brief Deleted copy and move constructors and assignment operators to prevent copying or
     * moving of the Memory tracker instance.
     */
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    Memory(Memory&&) = delete;
    Memory& operator=(Memory&&) = delete;
    ~Memory() = default;
    /** ------------------------------------------------------------------------------------------- Record Code Location
     * @brief Records one live allocation's source location when detailed tracking is enabled.
     * @param placement The placement owning the backing allocation.
     * @param size The backing allocation size in bytes.
     * @param handle The buffet handle identifying the allocation.
     * @param location The completed allocation's source location.
     */
    static void record_code_location(
        const BuffetDescriptor& placement,
        size_t size,
        const void* handle,
        const std::source_location& location = std::source_location::current()
    ) {
#if BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING
        Memory& tracker = instance();
        const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<AtomicMutex> lock(tracker.allocations_mutex_);
        tracker.allocations_.emplace(handle, AllocationInfo{
            static_cast<uint64_t>(timestamp), size, placement.type_idx, location
        });
#else
        (void)placement;
        (void)size;
        (void)handle;
        (void)location;
#endif
    }
    /** ------------------------------------------------------------------------------------------- Forget Code Location
     * @brief Retires a completed deallocation's location record when detailed tracking is enabled.
     * @param handle The buffet handle whose backing allocation has been released.
     */
    static void forget_code_location(const void* handle) {
#if BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING
        Memory& tracker = instance();
        std::lock_guard<AtomicMutex> lock(tracker.allocations_mutex_);
        tracker.allocations_.erase(handle);
#else
        (void)handle;
#endif
    }
    /** ------------------------------------------------------------------------------------------- Allocation Info
     * @brief Returns a live allocation record or no value when absent or detailed tracking is
     * disabled.
     * @param handle The buffet handle identifying the allocation.
     * @return A snapshot of the allocation's recorded details.
     */
    static std::optional<AllocationInfo> allocation_info(const void* handle) {
#if BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING
        Memory& tracker = instance();
        std::lock_guard<AtomicMutex> lock(tracker.allocations_mutex_);
        const auto found = tracker.allocations_.find(handle);
        if (found == tracker.allocations_.end()) return std::nullopt;
        return found->second;
#else
        (void)handle;
        return std::nullopt;
#endif
    }
    /** ------------------------------------------------------------------------------------------- Record Allocation
     * @brief Records one completed slab allocation for the owning placement.
     * @param placement The placement that allocated the slab.
     * @param size The page-rounded slab size.
     */
    static void record_allocation(const BuffetDescriptor& placement, size_t size) {
        instance().total_allocations_->fetch_add<uint64_t>(
            static_cast<uint64_t>(size),
            std::memory_order_relaxed
        );
        instance().placement_details_[placement.type_idx].total_allocations_->fetch_add<uint64_t>(
            static_cast<uint64_t>(size),
            std::memory_order_relaxed
        );
    }
    /** ------------------------------------------------------------------------------------------- Record Deallocation
     * @brief Records one completed slab deallocation for the owning placement.
     * @param placement The placement that deallocated the slab.
     * @param size The page-rounded slab size.
     */
    static void record_deallocation(const BuffetDescriptor& placement, size_t size) {
        instance().total_freed_->fetch_add<uint64_t>(
            static_cast<uint64_t>(size),
            std::memory_order_relaxed
        );
        instance().placement_details_[placement.type_idx].total_freed_->fetch_add<uint64_t>(
            static_cast<uint64_t>(size),
            std::memory_order_relaxed
        );
    }
    /** ------------------------------------------------------------------------------------------- Total Allocations
     * @brief Returns all slab bytes allocated through buffet types.
     * @return Total allocated bytes.
     */
    static size_t total_allocations() {
        return instance().total_allocations_->load<uint64_t>(std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Total Freed
     * @brief Returns all slab bytes returned through buffet types.
     * @return Total freed bytes.
     */
    static size_t total_freed() {
        return instance().total_freed_->load<uint64_t>(std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Placement Allocations
     * @brief Returns all slab bytes ever allocated through one buffet type.
     * @param placement The placement to query.
     * @return Allocated bytes for the placement.
     */
    static size_t placement_allocations(const BuffetDescriptor& placement) {
        return instance().placement_details_[placement.type_idx].total_allocations_->load<uint64_t>(
            std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Placement Freed
     * @brief Returns all slab bytes ever freed through one buffet type.
     * @param placement The placement to query.
     * @return Freed bytes for the placement.
     */
    static size_t placement_freed(const BuffetDescriptor& placement) {
        return instance().placement_details_[placement.type_idx].total_freed_->load<uint64_t>(
            std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Placement Usage
     * @brief Returns live slab bytes owned by one buffet type.
     * @param placement The placement to query.
     * @return Live bytes for the placement.
     */
    static size_t placement_usage(const BuffetDescriptor& placement) {
        return placement_allocations(placement) - placement_freed(placement);
    }
    /** ------------------------------------------------------------------------------------------- Placement Available
     * @brief Returns the last reported capacity for one buffet type.
     * @param placement The placement to query.
     * @return Reported available bytes for the placement.
     */
    static size_t placement_available(const BuffetDescriptor& placement) {
        return instance().placement_details_[placement.type_idx].total_available_->load<uint64_t>(
            std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Set Placement Available
     * @brief Records the reported capacity for one buffet type.
     * @param placement The placement being reported on.
     * @param size The reported available bytes.
     */
    static void set_placement_available(const BuffetDescriptor& placement, size_t size) {
        instance().placement_details_[placement.type_idx].total_available_->store<uint64_t>(
            static_cast<uint64_t>(size), std::memory_order_release);
    }
};
} // namespace buffetalligator
