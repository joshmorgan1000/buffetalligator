#pragma once
/** --------------------------------------------------------------------------------------------------------- Memory Tracker
 * @file tracker.hpp
 * @brief Header for the BuffetAlligator Memory Tracker class, a dumb reporting system for
 * completed allocations. Allocation and deallocation calls happen elsewhere and simply report
 * their results here.
 */
#include <buffetalligator.hpp>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Memory
 * @class Memory
 * @brief Tracks completed slab allocations and deallocations per registered Placemat. This class
 * performs no allocation itself; call sites report their own results here.
 */
class Memory {
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
    /** ------------------------------------------------------------------------------------------- Constructor - Private
     * @brief Private constructor for singleton pattern.
     */
    Memory() {
        total_allocations_ = AtomicRegistry::create_global<uint64_t>("buffetalligator_allocations", uint64_t(0));
        total_freed_ = AtomicRegistry::create_global<uint64_t>("buffetalligator_freed", uint64_t(0));
        const size_t placement_count = BuffetMenu::count();
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
        static Memory instance;
        return instance;
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
