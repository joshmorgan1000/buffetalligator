#pragma once
/** --------------------------------------------------------------------------------------------------------- Memory Pressure
 * @file pressure.hpp
 * @brief Selects heap or scratch backing before allocating a new slab.
 */
#include <alligator.hpp>
#include <mutex>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Memory Pressure
 * @brief Coordinates available-memory decisions and outstanding heap reservations.
 */
class MemoryPressure {
private:
    std::mutex mutex_;
    const Placemat* mapped_ = nullptr;
    HostMemoryUsage (*query_)() = &BuffetMenu::memory_usage;
    uint64_t reserve_bytes_ = 0;
    uint64_t resume_bytes_ = 0;
    uint64_t admitted_bytes_ = 0;
    uint64_t pending_bytes_ = 0;
    bool spilling_ = false;
    bool started_ = false;
    /** ------------------------------------------------------------------------------------------- Instance
     * @brief Returns the process-lifetime allocation policy.
     */
    static MemoryPressure& instance();
public:
    /** ------------------------------------------------------------------------------------------- Configure
     * @brief Installs immutable startup policy and its host-memory query.
     */
    static void configure(
        const Placemat* mapped, uint64_t reserve_bytes, uint64_t resume_bytes,
        HostMemoryUsage (*query)() = &BuffetMenu::memory_usage
    );
    /** ------------------------------------------------------------------------------------------- Start
     * @brief Closes policy configuration before the allocation worker starts.
     */
    static void start();
    /** ------------------------------------------------------------------------------------------- Reservation
     * @brief Keeps concurrent heap allocations charged until their backing allocation completes.
     */
    class Reservation {
    private:
        const Placemat* placement_;
        uint64_t reserved_bytes_ = 0;
    public:
        Reservation(const Placemat* requested, size_t bytes);
        ~Reservation();
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        /** ------------------------------------------------------------------------------- Placement
         * @brief Returns the actual backing placement selected for this allocation.
         */
        const Placemat* placement() const { return placement_; }
    };
};
} // namespace buffetalligator
