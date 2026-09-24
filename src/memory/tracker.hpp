#pragma once
/** --------------------------------------------------------------------------------------------------------- Memory Tracker
 * @file tracker.hpp
 * @brief Header for the BuffetAlligator Memory Tracker class, a dumb reporting system for
 * completed allocations. Allocation and deallocation calls happen elsewhere and simply report
 * their results here.
 */
#include <alligator.hpp>
#include <optional>
#include <source_location>
#ifndef BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING
#define BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING 0
#endif

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Memory
 * @class Memory
 * @brief Tracks completed slab allocations and deallocations per registered Placemat. This class
 * performs no allocation itself; call sites report their own results here.
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
    /** ------------------------------------------------------------------------------------------- PlacematDetails
     * @struct PlacematDetails
     * @brief Running allocation totals for one registered Placemat.
     */
    struct PlacematDetails {
        AtomicContainer* total_allocations_;  ///< Bytes allocated through this placement.
        AtomicContainer* total_freed_;  ///< Bytes freed through this placement.
        AtomicContainer* total_available_;  ///< Reported capacity for this placement.
    };
    /// @brief AtomicContainer to track total memory allocations across every placement.
    AtomicContainer* total_allocations_;
    /// @brief AtomicContainer to track total memory freed across every placement.
    AtomicContainer* total_freed_;
    /// @brief Per-placement allocation totals, indexed by Placemat::type().
    std::vector<PlacematDetails> placemat_details_;
#if BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING
    /// @brief Live backing allocations keyed by their stable placement handles.
    std::unordered_map<const Plate*, AllocationInfo> allocations_;
    /// @brief Serializes location records shared by allocating threads and the teardown worker.
    AtomicMutex allocations_mutex_;
#endif
    /** ------------------------------------------------------------------------------------------- Constructor - Private
     * @brief Private constructor for singleton pattern.
     */
    Memory() {
        total_allocations_ = AtomicRegistry::create_global<uint64_t>("buffetalligator_allocations", uint64_t(0));
        total_freed_ = AtomicRegistry::create_global<uint64_t>("buffetalligator_freed", uint64_t(0));
        constexpr size_t placement_count = 256;
        placemat_details_.reserve(placement_count);
        for (size_t i = 0; i < placement_count; ++i) {
            const std::string prefix = "buffetalligator_placemat_" + std::to_string(i) + "_";
            placemat_details_.push_back(PlacematDetails{
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
        static Memory* const instance = new Memory();
        return *instance;
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
     * @param handle The stable placement handle identifying the allocation.
     * @param location The completed allocation's source location.
     */
    static void record_code_location(
        const Placemat& placement,
        size_t size,
        const Plate* plate,
        const std::source_location& location = std::source_location::current()
    ) {
#if BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING
        Memory& tracker = instance();
        const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<AtomicMutex> lock(tracker.allocations_mutex_);
        tracker.allocations_.emplace(plate, AllocationInfo{
            static_cast<uint64_t>(timestamp), size, placement.type(), location
        });
#else
        (void)placement;
        (void)size;
        (void)plate;
        (void)location;
#endif
    }
    /** ------------------------------------------------------------------------------------------- Forget Code Location
     * @brief Retires a completed deallocation's location record when detailed tracking is enabled.
     * @param handle The placement handle whose backing allocation has been released.
     */
    static void forget_code_location(const Plate* plate) {
#if BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING
        Memory& tracker = instance();
        std::lock_guard<AtomicMutex> lock(tracker.allocations_mutex_);
        tracker.allocations_.erase(plate);
#else
        (void)plate;
#endif
    }
    /** ------------------------------------------------------------------------------------------- Allocation Info
     * @brief Returns a live allocation record or no value when absent or detailed tracking is disabled.
     * @param handle The placement handle identifying the allocation.
     * @return A snapshot of the allocation's recorded details.
     */
    static std::optional<AllocationInfo> allocation_info(const Plate* plate) {
#if BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING
        Memory& tracker = instance();
        std::lock_guard<AtomicMutex> lock(tracker.allocations_mutex_);
        const auto found = tracker.allocations_.find(plate);
        if (found == tracker.allocations_.end()) return std::nullopt;
        return found->second;
#else
        (void)plate;
        return std::nullopt;
#endif
    }
    /** ------------------------------------------------------------------------------------------- Record Allocation
     * @brief Records one completed slab allocation for the owning placement.
     * @param placement The placement that allocated the slab.
     * @param size The page-rounded slab size.
     */
    static void record_allocation(const Placemat& placement, size_t size) {
        instance().total_allocations_->fetch_add<uint64_t>(static_cast<uint64_t>(size), std::memory_order_relaxed);
        instance().placemat_details_[placement.type()].total_allocations_->fetch_add<uint64_t>(
            static_cast<uint64_t>(size), std::memory_order_relaxed);
    }
    /** ------------------------------------------------------------------------------------------- Record Deallocation
     * @brief Records one completed slab deallocation for the owning placement.
     * @param placement The placement that deallocated the slab.
     * @param size The page-rounded slab size.
     */
    static void record_deallocation(const Placemat& placement, size_t size) {
        instance().total_freed_->fetch_add<uint64_t>(static_cast<uint64_t>(size), std::memory_order_relaxed);
        instance().placemat_details_[placement.type()].total_freed_->fetch_add<uint64_t>(
            static_cast<uint64_t>(size), std::memory_order_relaxed);
    }
    /** ------------------------------------------------------------------------------------------- Total Allocations
     * @brief Returns all slab bytes allocated through registered placements.
     * @return Total allocated bytes.
     */
    static size_t total_allocations() {
        return instance().total_allocations_->load<uint64_t>(std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Total Freed
     * @brief Returns all slab bytes returned through registered placements.
     * @return Total freed bytes.
     */
    static size_t total_freed() {
        return instance().total_freed_->load<uint64_t>(std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Placemat Allocations
     * @brief Returns all slab bytes ever allocated through one registered placement.
     * @param placement The placement to query.
     * @return Allocated bytes for the placement.
     */
    static size_t placement_allocations(const Placemat& placement) {
        return instance().placemat_details_[placement.type()].total_allocations_->load<uint64_t>(
            std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Placemat Freed
     * @brief Returns all slab bytes ever freed through one registered placement.
     * @param placement The placement to query.
     * @return Freed bytes for the placement.
     */
    static size_t placement_freed(const Placemat& placement) {
        return instance().placemat_details_[placement.type()].total_freed_->load<uint64_t>(
            std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Placemat Usage
     * @brief Returns live slab bytes owned by one registered placement.
     * @param placement The placement to query.
     * @return Live bytes for the placement.
     */
    static size_t placement_usage(const Placemat& placement) {
        return placement_allocations(placement) - placement_freed(placement);
    }
    /** ------------------------------------------------------------------------------------------- Placemat Available
     * @brief Returns the last reported capacity for one registered placement.
     * @param placement The placement to query.
     * @return Reported available bytes for the placement.
     */
    static size_t placement_available(const Placemat& placement) {
        return instance().placemat_details_[placement.type()].total_available_->load<uint64_t>(
            std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Set Placemat Available
     * @brief Records the reported capacity for one registered placement.
     * @param placement The placement being reported on.
     * @param size The reported available bytes.
     */
    static void set_placement_available(const Placemat& placement, size_t size) {
        instance().placemat_details_[placement.type()].total_available_->store<uint64_t>(
            static_cast<uint64_t>(size), std::memory_order_release);
    }
};
} // namespace buffetalligator
