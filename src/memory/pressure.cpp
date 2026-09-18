/** --------------------------------------------------------------------------------------------------------- Memory Pressure
 * @file pressure.cpp
 * @brief Applies hysteresis and accounts for heap allocations overlapping a host-memory sample.
 */
#include <memory/pressure.hpp>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Instance
 * @brief Returns the allocation policy shared by slab producers.
 */
MemoryPressure& MemoryPressure::instance() {
    static MemoryPressure policy;
    return policy;
}
/** --------------------------------------------------------------------------------------------------------- Configure
 * @brief Validates and installs the policy before any arena allocation can start.
 */
void MemoryPressure::configure(
    const Placemat* mapped, uint64_t reserve_bytes, uint64_t resume_bytes,
    HostMemoryUsage (*query)()
) {
    MemoryPressure& policy = instance();
    if (policy.started_) ALLIGATOR_THROW("Enable mmap spillover before creating the first Slice");
    if (policy.mapped_) ALLIGATOR_THROW("Mmap spillover is already enabled");
    if (!mapped) ALLIGATOR_THROW("Register the mmap scratch directory before enabling spillover");
    if (resume_bytes <= reserve_bytes) {
        ALLIGATOR_THROW("The heap recovery threshold must exceed the available-memory reserve");
    }
    policy.reserve_bytes_ = reserve_bytes;
    policy.resume_bytes_ = resume_bytes;
    policy.query_ = query;
    policy.mapped_ = mapped;
}
/** --------------------------------------------------------------------------------------------------------- Start
 * @brief Seals the startup-only configuration before publishing it to the worker.
 */
void MemoryPressure::start() { instance().started_ = true; }
/** --------------------------------------------------------------------------------------------------------- Reservation
 * @brief Samples memory outside the policy lock and reserves headroom for one backing allocation.
 */
MemoryPressure::Reservation::Reservation(const Placemat* requested, size_t bytes)
: placement_(requested) {
    MemoryPressure& policy = instance();
    if (!policy.mapped_ || requested->type() > 1) return;
    uint64_t admitted_before;
    uint64_t pending_before;
    {
        std::lock_guard<std::mutex> lock(policy.mutex_);
        admitted_before = policy.admitted_bytes_;
        pending_before = policy.pending_bytes_;
    }
    const HostMemoryUsage usage = policy.query_();
    bool changed;
    bool spilling;
    {
        std::lock_guard<std::mutex> lock(policy.mutex_);
        const uint64_t admitted_during = policy.admitted_bytes_ - admitted_before;
        uint64_t available = usage.available_bytes > pending_before
            ? usage.available_bytes - pending_before : 0;
        available = available > admitted_during ? available - admitted_during : 0;
        const uint64_t threshold = policy.spilling_ ? policy.resume_bytes_ : policy.reserve_bytes_;
        spilling = bytes > available || available - bytes < threshold;
        changed = spilling != policy.spilling_;
        policy.spilling_ = spilling;
        if (spilling) {
            placement_ = policy.mapped_;
        } else {
            policy.admitted_bytes_ += bytes;
            policy.pending_bytes_ += bytes;
            reserved_bytes_ = bytes;
        }
    }
    if (changed) {
        if (spilling) {
            LOG_INFO_STREAM << "Host memory reserve reached; new heap slabs will use mmap scratch storage";
        } else {
            LOG_INFO_STREAM << "Host memory recovered; new slabs will use heap storage again";
        }
    }
}
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Retires a heap reservation after successful allocation or exception unwinding.
 */
MemoryPressure::Reservation::~Reservation() {
    if (reserved_bytes_) {
        MemoryPressure& policy = instance();
        std::lock_guard<std::mutex> lock(policy.mutex_);
        policy.pending_bytes_ -= reserved_bytes_;
    }
}
} // namespace buffetalligator
