/** --------------------------------------------------------------------------------------------------------- PotentialSlice
 * @file potentialslice.cpp
 * @brief Implementation of the PotentialSlice container.
 */
#include <alligator.hpp>
#include <alligator/kitchen.hpp>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- PotentialSlice Details
 * @struct PotentialSlice::Details
 * @brief Internal details structure for the PotentialSlice container.
 */
struct PotentialSlice::Details {
    /** ------------------------------------------------------------------------------------------- Shared Context
     * @struct PotentialSlice::Details::SharedContext
     * @brief Shared context structure for the fulfillment method.
     */
    struct SharedContext {
        /// @brief Fulfillment method for the shared context.
        Slice (*fulfillment_method)(std::shared_ptr<void> context);
        /// @brief Context for the fulfillment method.
        std::shared_ptr<void> context = nullptr;
    };
    /** ------------------------------------------------------------------------------------------- Raw Context
     * @struct PotentialSlice::Details::RawContext
     * @brief Raw context structure for the fulfillment method.
     */
    struct RawContext {
        /// @brief Fulfillment method for the raw context.
        Slice(*fulfillment_method)(void* context);
        /// @brief Context for the fulfillment method.
        void* context;
        /// @brief Deleter for the context.
        void (*context_deleter)(void* context);
    };
    /// @brief Union of shared and raw contexts.
    union {
        /// @brief Shared context.
        SharedContext shared;
        /// @brief Raw context.
        RawContext raw;
    };
    /// @brief Indicates whether the context is shared or raw.
    bool shared_ = false;
    /// @brief Result of the fulfillment method.
    Slice result_;
    /// @brief Indicates whether the fulfillment is in flight.
    std::atomic<uint32_t> in_flight{0};
    /** ------------------------------------------------------------------------------------------- Fulfill Slice
     * @brief Fulfills the slice by invoking the appropriate fulfillment method.
     * @return The resulting Slice after fulfillment.
     */
    Slice fulfill() {
        if (shared_) {
            if (in_flight.fetch_or(1)) {
                while (in_flight.load(std::memory_order_acquire) != 3u) {
                    std::this_thread::yield();
                }
                return result_;
            }
            result_ = shared.fulfillment_method(shared.context);
            in_flight.store(3u, std::memory_order_release);
            return result_;
        } else {
            if (in_flight.fetch_or(1)) {
                while (in_flight.load(std::memory_order_acquire) != 3u) {
                    std::this_thread::yield();
                }
                return result_;
            }
            result_ = raw.fulfillment_method(raw.context);
            if (raw.context_deleter) raw.context_deleter(raw.context);
            in_flight.store(3u, std::memory_order_release);
            return result_;
        }
    }
    /** ------------------------------------------------------------------------------------------- Constructor for Raw Details
     * @brief Constructs a Details object for a raw fulfillment method.
     * @param fulfillment_method The raw fulfillment method function pointer.
     * @param context The context pointer to be passed to the fulfillment method.
     * @param context_deleter The deleter function for the context pointer.
     */
    Details(
        Slice (*fulfillment_method)(void* context),
        void* context,
        void (*context_deleter)(void* context)
    ) : raw{fulfillment_method, context, context_deleter}, shared_(false) {}
    /** ------------------------------------------------------------------------------------------- Constructor for Shared Details
     * @brief Constructs a Details object for a shared fulfillment method.
     * @param fulfillment_method The shared fulfillment method function pointer.
     * @param context The shared context pointer to be passed to the fulfillment method.
     */
    Details(
        Slice (*fulfillment_method)(std::shared_ptr<void> context),
        std::shared_ptr<void> context
    ) : shared{fulfillment_method, context}, shared_(true) {}
    /** ------------------------------------------------------------------------------------------- No Copy/Move */
    Details(const Details&) = delete;
    Details& operator=(const Details&) = delete;
    Details(Details&&) = delete;
    Details& operator=(Details&&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Destroys the Details object, releasing any associated resources.
     */
    ~Details() {
        if (!shared_ && raw.context_deleter) raw.context_deleter(raw.context);
    }
};
/** ------------------------------------------------------------------------------------------- Constructor
 * @brief Default constructor for PotentialSlice.
 * @param fulfillment_method The method to fulfill the slice.
 * @param context The context for the fulfillment method.
 * @param context_deleter The deleter for the context.
 */
PotentialSlice::PotentialSlice(
    Slice (*fulfillment_method)(void* context),
    void* context,
    void (*context_deleter)(void* context)
) {
    details_ = std::make_unique<Details>(fulfillment_method, context, context_deleter);
}
/** ------------------------------------------------------------------------------------------- Constructor with shared context
 * @brief Constructor for PotentialSlice with a shared context.
 * @param fulfillment_method The method to fulfill the slice.
 * @param context The shared context for the fulfillment method.
 */
PotentialSlice::PotentialSlice(
    Slice (*fulfillment_method)(std::shared_ptr<void> context),
    std::shared_ptr<void> context
) {
    details_ = std::make_unique<Details>(fulfillment_method, context);
}
/** ------------------------------------------------------------------------------------------- Check Fulfillment
 * @brief Checks if the slice has been fulfilled.
 * @return `true` if the slice has been fulfilled, `false` otherwise.
 */
bool PotentialSlice::is_fulfilled() const {
    return (details_ && (details_->in_flight.load(std::memory_order_acquire) == 3u));
}
/** ------------------------------------------------------------------------------------------- Fulfill Slice
 * @brief Fulfills the slice by invoking its fulfillment method if it has not been fulfilled
 * yet.
 */
void PotentialSlice::fulfill() const {
    if (details_ && (details_->in_flight.fetch_or(1u, std::memory_order_acq_rel) == 0u)) {
        Order order(+[](PotentialSlice::Details* details) {
            details->fulfill();
        }, details_.get());
        Kitchen::submit(std::move(order));
    }
}
/** ------------------------------------------------------------------------------------------- Get Raw Slice
 * @brief Returns a pointer to the underlying raw Slice object, which may be nullptr if the
 * slice has not been fulfilled yet.
 * @return A pointer to the underlying raw Slice object.
 */
Slice* PotentialSlice::raw_slice() const {
    if (details_ && (details_->in_flight.load(std::memory_order_acquire) == 3u)) {
        return &details_->result_;
    }
    return nullptr;
}
/** ------------------------------------------------------------------------------------------- Get Subslice
 * @brief Returns a subslice of the current slice, starting at the specified offset and with
 * the specified length. This will fulfill the current slice if it has not been fulfilled yet.
 * @param offset The starting offset of the subslice.
 * @param length The length of the subslice.
 * @return A new Slice representing the subslice.
 */
Slice PotentialSlice::slice(size_t offset, size_t length) const {
    fulfill();
    if (details_ && (details_->in_flight.load(std::memory_order_acquire) == 3u)) {
        return details_->result_.slice(offset, length);
    }
    return Slice();
}
/** ------------------------------------------------------------------------------------------- Get Size in Bytes
 * @brief Returns the size of the slice in bytes.
 * @return The size of the slice in bytes.
 */
size_t PotentialSlice::size_bytes() const {
    fulfill();
    if (details_ && (details_->in_flight.load(std::memory_order_acquire) == 3u)) {
        return details_->result_.size_bytes();
    }
    return 0;
}
/** ------------------------------------------------------------------------------------------- Free Slice
 * @brief Frees the underlying memory of the slice if it has been allocated.
 */
void PotentialSlice::free() {
    if (details_ && (details_->in_flight.load(std::memory_order_acquire) == 3u)) {
        details_->result_.free();
    }
}
/** ------------------------------------------------------------------------------------------- Null Slice
 * @brief Checks if the slice is null.
 * @return True if the slice is null, false otherwise.
 */
bool PotentialSlice::is_null() const {
    return !(details_ && (details_->in_flight.load(std::memory_order_acquire) == 3u)
        && !details_->result_.is_null());
}
/** ------------------------------------------------------------------------------------------- Valid Slice
 * @brief Checks if the slice is valid.
 * @return True if the slice is valid, false otherwise.
 */
bool PotentialSlice::valid() const {
    fulfill();
    return (details_ && (details_->in_flight.load(std::memory_order_acquire) == 3u)
        && details_->result_.valid());
}
/** ------------------------------------------------------------------------------------------- Bool Conversion
 * @brief Converts the slice to a boolean value, indicating whether it is valid.
 * @return True if the slice is valid, false otherwise.
 */
PotentialSlice::operator bool() const {
    return valid();
}
/** ------------------------------------------------------------------------------------------- Adopt Slice
 * @brief Adopts the given Slice, becoming another view of the same underlying memory. This
 * will complete the fulfillment contract for the PotentialSlice, overriding any fufillment
 * methods, and clean up the context if a deleter is set for it.
 * @param slice The Slice to adopt.
 */
void PotentialSlice::adopt(Slice slice) {
    if (!details_) return;
    details_->result_ = std::move(slice);
    details_->in_flight.store(3u, std::memory_order_release);
}
} // namespace buffetalligator