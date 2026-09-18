/** --------------------------------------------------------------------------------------------------------- BuffetAlligator Memory
 * @file slice.cpp
 * @brief Implements registered placement chains, background replenishment, and Slice lifetime.
 */
#include <alligator.hpp>
#include <memory/alligator.hpp>
#include <memory/buffet.hpp>
#include <memory/slicefriend.hpp>
#include <simd.hpp>
#include <algorithm>
#include <cstring>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Default Placement
 * @brief Returns the default placement for slices, which is determined by the system's
 * capabilities.
 * @return The default `Placement` enum value for slices.
 */
const Placemat* Slice::default_placement() {
    return BuffetMenu::default_placement();
}
/** --------------------------------------------------------------------------------------------------------- Constructor - Fresh Claim
 * @brief Claims a slice of pre-allocated memory in the buffet alligator's slab arena. The
 * slice is guaranteed to be zero-initialized.
 * @param size The size of the slice in bytes.
 */
Slice::Slice(size_t size, const Placemat* placement) {
    *this = Alligator::instance().current_for_placement(placement->type())->claim(size);
}
/** --------------------------------------------------------------------------------------------------------- Constructor - Fresh Claim
 * @brief Claims a slice of pre-allocated memory in the buffet alligator's slab arena, with
 * the option to specify whether the slice should be part of a larger slab or a novel buffer.
 * The slice is guaranteed to be zero-initialized.
 * @param size The size of the slice in bytes.
 * @param novel_buffer If true, then the slice is allocated as a novel buffer instead of being
 * a claim of a pre-allocated slab. This is ideal for slices that are long-lived to help
 * reduce fragmentation in the arena.
 */
Slice::Slice(size_t size, bool novel_buffer, const Placemat* placement) {
    *this = Alligator::instance().current_for_placement(placement->type())->claim(size, novel_buffer);
}
/** --------------------------------------------------------------------------------------------------------- Constructor - Copy from External Memory
 * @brief Copies data from an external memory location into a new slice of memory in the
 * buffet alligator. This can be used to deep-copy a slice, or load data from an external
 * source into the buffet alligator's memory management system.
 * @param copy_from Pointer to the external memory to copy from.
 * @param size The size of the data to copy in bytes.
 * @param novel_buffer If true, then the slice is allocated as a novel buffer instead of being
 * a claim of a pre-allocated slab. This is ideal for slices that are long-lived to help
 * reduce fragmentation in the arena. Default is false.
 */
Slice::Slice(
    const void* copy_from,
    size_t size,
    bool novel_buffer,
    const Placemat* placement
) {
    if (copy_from == nullptr || size == 0) {
        return;
    }
    *this = Alligator::instance().current_for_placement(
        placement->type())->claim(copy_from, size, novel_buffer
    );
}
/** --------------------------------------------------------------------------------------------------------- Copy/move semantics
 * @brief Copying a `Slice` does not actually copy the underlying memory, `Slice` objects act
 * much like `std::shared_ptr` in that they share the same reference counter and underlying
 * memory. Move semantics transfer ownership without reference-counting traffic.
 */
Slice::Slice(const Slice& other)
: meta_(other.meta_)
, cached_(other.cached_) {
    if (cached_) {
        Alligator::instance().get(meta_ & 0x1FFFFu)->ref_count_.fetch_add(
            1, std::memory_order_acq_rel
        );
    }
}
/** --------------------------------------------------------------------------------------------------------- Copy assignment operator
 * @brief Assigns the contents of one `Slice` to another, sharing the same underlying memory
 * and reference counter.
 * @param other The `Slice` to assign from.
 * @return A reference to the assigned `Slice`.
 */
Slice& Slice::operator=(const Slice& other) {
    if (this != &other) {
        free();
        meta_ = other.meta_;
        cached_ = other.cached_;
        if (cached_) {
            uint32_t slot = meta_ & 0x1FFFFu;
            Alligator::instance().get(slot)->ref_count_.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Move constructor
 * @brief Moves the contents of one `Slice` to another, transferring ownership of the
 * underlying memory.
 * @param other The `Slice` to move from.
 */
Slice::Slice(Slice&& other) noexcept
: meta_(other.meta_)
, cached_(other.cached_) {
    other.meta_ = UINT64_MAX;
    other.cached_ = nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Move assignment operator
 * @brief Moves the contents of one `Slice` to another, transferring ownership of the
 * underlying memory.
 * @param other The `Slice` to move from.
 * @return A reference to the assigned `Slice`.
 */
Slice& Slice::operator=(Slice&& other) noexcept {
    if (this != &other) {
        free();
        meta_ = other.meta_;
        cached_ = other.cached_;
        other.meta_ = UINT64_MAX;
        other.cached_ = nullptr;
    }
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Placement
 * @brief Returns the memory placement type of the slice.
 * @return The `Placement` enum value representing the slice's memory placement.
 */
const Placemat* Slice::placement() const {
    if (!cached_) {
        return nullptr;
    }
    uint32_t slot = meta_ & 0x1FFFFu;
    Buffet* buffer = Alligator::instance().get(slot);
    return buffer ? buffer->placement() : nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Create new view
 * @brief Creates a new view of the slice, which is a sub-slice of the original slice. The new view shares
 * the same underlying memory and reference counter as the original slice. Using the default parameters will
 * create a new view that is essentially identical to the original slice - a shared view that increments the
 * reference counter and will keep the underlying memory alive until all views are destroyed.
 * @param offset The offset in bytes from the start of the original slice to the start of the new view.
 * @param length The length in bytes of the new view.
 * @return A new `Slice` object that is a view of the original slice.
 */
Slice Slice::slice(size_t offset, size_t length) const {
    if (length == 0) {
        return Slice();
    }
    if (offset == 0 && length == SIZE_MAX) {
        return *this;
    }
    if (meta_ == UINT64_MAX) {
        return Slice();
    }
    uint32_t slot = meta_ & 0x1FFFFu;
    Buffet* buffet = Alligator::instance().get(slot);
    if (buffet == nullptr) {
        return Slice();
    }
    const size_t slice_size = size_bytes();
    if (offset >= slice_size) {
        ALLIGATOR_THROW("Slice::slice: offset exceeds slice size");
    }
    const size_t view_size = slice_size - offset;
    if (length != SIZE_MAX && length > view_size) {
        ALLIGATOR_THROW("Slice::slice: length exceeds slice size");
    }
    const size_t result_size = length == SIZE_MAX ? view_size : length;
    Slice result;
    result.meta_ = (static_cast<uint64_t>(result_size) << 17) | (meta_ & 0x1FFFFu);
    result.cached_ = static_cast<uint8_t*>(cached_) + offset;
    buffet->ref_count_.fetch_add(1, std::memory_order_acq_rel);
    return result;
}
/** --------------------------------------------------------------------------------------------------------- Resize
 * @brief Resizes the slice to a new size. If `preserve_data` is true, the existing data in the slice will
 * be preserved up to the minimum of the old and new sizes. If `preserve_data` is false, the existing data
 * will be discarded and the slice will be reallocated. This can be called on a freed or null slice, in
 * which case it will behave like a normal constructor and allocate a new slice of the specified size.
 * @param new_size The new size of the slice in bytes.
 * @param preserve_data Whether to preserve existing data in the slice. Default is true.
 * @param novel_buffer Whether to allocate a novel buffer even if the slice is not null. Default is false.
 * @param placement The memory placement strategy to use. Default is `default_placement()`.
 */
void Slice::resize(
    size_t new_size,
    bool preserve_data,
    bool novel_buffer,
    const Placemat* placement
) {
    if (is_null()) {
        *this = Slice(new_size, novel_buffer, placement);
        return;
    }
    if (new_size == size_bytes()) {
        return;
    }
    if (new_size < size_bytes() && preserve_data) {
        Slice shrunk = slice(0, new_size);
        *this = std::move(shrunk);
        return;
    }
    Slice grown(new_size, novel_buffer, placement);
    if (preserve_data && !is_null() && !grown.is_null()) {
        std::memcpy(grown.raw(), raw(), std::min(size_bytes(), new_size));
    }
    *this = std::move(grown);
}
/** --------------------------------------------------------------------------------------------------------- Novel Backing
 * @brief Reports whether this slice owns or views a dedicated novel buffer.
 * @return True when the backing allocation is a novel buffer.
 */
bool Slice::is_novel() const noexcept {
    if (meta_ == UINT64_MAX || cached_ == nullptr) {
        return false;
    }
    Buffet* buffet = Alligator::instance().get(meta_ & 0x1FFFFu);
    return buffet != nullptr && buffet->is_novel();
}
/** --------------------------------------------------------------------------------------------------------- Adopt
 * @brief Adopts the contents of another slice, freeing the current slice if necessary.
 * @param other The slice to adopt.
 */
void Slice::adopt(Slice other) {
    if (this == &other) {
        return;
    }
    free();
    meta_ = other.meta_;
    cached_ = other.cached_;
    other.meta_ = UINT64_MAX;
    other.cached_ = nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Free
 * @brief Frees the underlying memory of the slice. This is called automatically when the
 * slice is destroyed, but can be called manually to free the memory early. After calling this
 * method, the slice will be null.
 */
void Slice::free() {
    if (meta_ == UINT64_MAX) {
        return;
    }
    uint32_t slot = meta_ & 0x1FFFFu;
    Buffet* buffet = Alligator::instance().bufs[slot & 0x1FFFFu].load(std::memory_order_acquire);
    if (buffet != nullptr) {
        buffet->free();
    }
    meta_ = UINT64_MAX;
    cached_ = nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Do Something Fun
 * @brief A placeholder function for demonstration purposes.
 * @param ptr A void pointer parameter.
 */
void SliceFriend::do_somthing_fun(void* ptr) {
    Alligator::instance().enqueue_order(ptr);
}
/** --------------------------------------------------------------------------------------------------------- Get Placemat Handle for Slice
 * @brief Retrieves the Placemat handle associated with the given slice.
 * @param slice The slice to retrieve the handle for.
 * @return The Placemat handle associated with the slice, or nullptr if not found.
 */
Placemat::Handle* Placemat::get_for(const Slice* slice) {
    uint32_t arena_id = slice->meta_ & 0x1FFFF; // Extract the arena ID from the meta_ field
    Buffet* buffet = Alligator::instance().get(arena_id);
    return buffet ? static_cast<Placemat::Handle*>(buffet->handle()) : nullptr;
}
/** --------------------------------------------------------------------------------------------------------- PotentialSlice Details
 * @struct PotentialSlice::Details
 * @brief Internal details structure for the PotentialSlice class.
 */
struct PotentialSlice::Details {
    /// @brief Sentinel value used to indicate a swap operation in progress.
    inline static Slice* SWAP_SENTINEL = reinterpret_cast<Slice*>(-1);
    /// @brief Pointer to the underlying slice, managed atomically.
    std::atomic<Slice*> slice{nullptr};
    /// @brief Function pointer to the fulfillment method for the potential slice.
    Slice (*fullfillment_method)(void* context) = nullptr;
    /// @brief Context pointer passed to the fulfillment method.
    void* context = nullptr;
    /// @brief Function pointer to the deleter for the context.
    void (*context_deleter)(void* context) = nullptr;
};
/** --------------------------------------------------------------------------------------------------------- PotentialSlice Constructor
 * @brief Constructs a PotentialSlice with the specified fulfillment method, context, and context deleter.
 * @param fullfillment_method The function pointer to the fulfillment method.
 * @param context The context pointer passed to the fulfillment method.
 * @param context_deleter The function pointer to the deleter for the context.
 */
PotentialSlice::PotentialSlice(
    Slice (*fullfillment_method)(void* context),
    void* context,
    void (*context_deleter)(void* context)
) {
    details_->fullfillment_method = fullfillment_method;
    details_->context = context;
    details_->context_deleter = context_deleter;
}
/** ------------------------------------------------------------------------------------------- Copy Constructor
 * @brief Constructs a PotentialSlice as a copy of another PotentialSlice.
 * @param other The PotentialSlice to copy from.
 */
PotentialSlice::PotentialSlice(const PotentialSlice& other)
: details_(other.details_) {}
/** ------------------------------------------------------------------------------------------- Move Constructor
 * @brief Constructs a PotentialSlice by moving another PotentialSlice.
 * @param other The PotentialSlice to move from.
 */
PotentialSlice::PotentialSlice(PotentialSlice&& other) noexcept
: details_(std::move(other.details_)) {}
/** ------------------------------------------------------------------------------------------- Copy Assignment Operator
 * @brief Assigns the value of another PotentialSlice to this one.
 * @param other The PotentialSlice to copy from.
 * @return A reference to this PotentialSlice.
 */
PotentialSlice& PotentialSlice::operator=(const PotentialSlice& other) {
    if (this != &other) details_ = other.details_;
    return *this;
}
/** ------------------------------------------------------------------------------------------- Move Assignment Operator
 * @brief Assigns the value of another PotentialSlice to this one by moving it.
 * @param other The PotentialSlice to move from.
 * @return A reference to this PotentialSlice.
 */
PotentialSlice& PotentialSlice::operator=(PotentialSlice&& other) noexcept {
    if (this != &other) details_ = std::move(other.details_);
    return *this;
}
/** ------------------------------------------------------------------------------------------- Destructor
 * @brief Destructor for PotentialSlice. Cleans up the underlying slice and context if
 * necessary.
 */
PotentialSlice::~PotentialSlice() {
    Slice* slice = details_->slice.exchange(nullptr, std::memory_order_acquire);
    while (slice == Details::SWAP_SENTINEL) {
        std::this_thread::yield();
        slice = details_->slice.exchange(nullptr, std::memory_order_acquire);
    }
    if (slice != nullptr) {
        delete slice;
    }
    if (details_->context_deleter != nullptr && details_->context != nullptr) {
        details_->context_deleter(details_->context);
        details_->context = nullptr;
        details_->context_deleter = nullptr;
    }
}
/** ------------------------------------------------------------------------------------------- Get Raw Pointer
 * @brief Returns the raw pointer to the underlying slice's data, fulfilling the lazy
 * initialization if necessary.
 * @return The raw pointer to the underlying slice's data.
 */
void* PotentialSlice::raw() {
    Slice* slice = details_->slice.load(std::memory_order_acquire);
    while (slice == nullptr || slice == Details::SWAP_SENTINEL) {
        Slice* expected = nullptr;
        if (details_->slice.compare_exchange_strong(expected, Details::SWAP_SENTINEL)) {
            Slice fulfilled = details_->fullfillment_method(details_->context);
            details_->slice.store(new Slice(std::move(fulfilled)), std::memory_order_release);
            if (details_->context_deleter != nullptr && details_->context != nullptr) {
                details_->context_deleter(details_->context);
                details_->context = nullptr;
                details_->context_deleter = nullptr;
            }
            return details_->slice.load(std::memory_order_acquire)->raw();
        }
        std::this_thread::yield();
        slice = details_->slice.load(std::memory_order_acquire);
    }
    return slice->raw();
}
/** ------------------------------------------------------------------------------------------- Get Raw Pointer (const)
 * @brief Returns the raw pointer to the underlying slice's data, fulfilling the lazy
 * initialization if necessary.
 * @return The raw pointer to the underlying slice's data.
 */
const void* PotentialSlice::raw() const {
    Slice* slice = details_->slice.load(std::memory_order_acquire);
    while (slice == nullptr || slice == Details::SWAP_SENTINEL) {
        Slice* expected = nullptr;
        if (details_->slice.compare_exchange_strong(expected, Details::SWAP_SENTINEL)) {
            Slice fulfilled = details_->fullfillment_method(details_->context);
            details_->slice.store(new Slice(std::move(fulfilled)), std::memory_order_release);
            if (details_->context_deleter != nullptr && details_->context != nullptr) {
                details_->context_deleter(details_->context);
                details_->context = nullptr;
                details_->context_deleter = nullptr;
            }
            return details_->slice.load(std::memory_order_acquire)->raw();
        }
        std::this_thread::yield();
        slice = details_->slice.load(std::memory_order_acquire);
    }
    return slice->raw();
}
/** ------------------------------------------------------------------------------------------- Check Fulfillment
 * @brief Checks if the slice has been fulfilled.
 * @return `true` if the slice has been fulfilled, `false` otherwise.
 */
bool PotentialSlice::is_fulfilled() const {
    Slice* slice = details_->slice.load(std::memory_order_acquire);
    return slice != nullptr && slice != Details::SWAP_SENTINEL;
}
/** ------------------------------------------------------------------------------------------- Fulfill Slice
 * @brief Fulfills the slice by invoking its fulfillment method if it has not been fulfilled
 * yet.
 */
void PotentialSlice::fulfill() {
    (void)raw();
}
/** ------------------------------------------------------------------------------------------- Get Raw Slice
 * @brief Returns a pointer to the underlying raw Slice object, which may be nullptr if the
 * slice has not been fulfilled yet.
 * @return A pointer to the underlying raw Slice object.
 */
Slice* PotentialSlice::raw_slice() const {
    return details_->slice.load(std::memory_order_acquire);
}
/** ------------------------------------------------------------------------------------------- Get Subslice
 * @brief Returns a subslice of the current slice, starting at the specified offset and with
 * the specified length. This will fulfill the current slice if it has not been fulfilled yet.
 * @param offset The starting offset of the subslice.
 * @param length The length of the subslice.
 * @return A new Slice representing the subslice.
 */
Slice PotentialSlice::slice(size_t offset, size_t length) const {
    Slice* slice = details_->slice.load(std::memory_order_acquire);
    while (slice == nullptr || slice == Details::SWAP_SENTINEL) {
        Slice* expected = nullptr;
        if (details_->slice.compare_exchange_strong(expected, Details::SWAP_SENTINEL)) {
            Slice fulfilled = details_->fullfillment_method(details_->context);
            details_->slice.store(new Slice(std::move(fulfilled)), std::memory_order_release);
            if (details_->context_deleter != nullptr && details_->context != nullptr) {
                details_->context_deleter(details_->context);
                details_->context = nullptr;
                details_->context_deleter = nullptr;
            }
            slice = details_->slice.load(std::memory_order_acquire);
        }
        std::this_thread::yield();
        slice = details_->slice.load(std::memory_order_acquire);
    }
    return (*slice).slice(offset, length);
}
/** ------------------------------------------------------------------------------------------- Get Size in Bytes
 * @brief Returns the size of the slice in bytes.
 * @return The size of the slice in bytes.
 */
size_t PotentialSlice::size_bytes() const {
    Slice* slice = details_->slice.load(std::memory_order_acquire);
    if (slice == nullptr || slice == Details::SWAP_SENTINEL) {
        return 0;
    }
    return slice->size_bytes();
}
/** ------------------------------------------------------------------------------------------- Free Slice
 * @brief Frees the underlying memory of the slice if it has been allocated.
 */
void PotentialSlice::free() {
    Slice* slice = details_->slice.load(std::memory_order_acquire);
    if (slice != nullptr && slice != Details::SWAP_SENTINEL) {
        slice->free();
        if (details_->context_deleter != nullptr && details_->context != nullptr) {
            details_->context_deleter(details_->context);
            details_->context = nullptr;
            details_->context_deleter = nullptr;
        }
    }
}
/** ------------------------------------------------------------------------------------------- Null Slice
 * @brief Checks if the slice is null.
 * @return True if the slice is null, false otherwise.
 */
bool PotentialSlice::is_null() const {
    Slice* slice = details_->slice.load(std::memory_order_acquire);
    return slice == nullptr || slice == Details::SWAP_SENTINEL;
}
/** ------------------------------------------------------------------------------------------- Valid Slice
 * @brief Checks if the slice is valid.
 * @return True if the slice is valid, false otherwise.
 */
bool PotentialSlice::valid() const {
    Slice* slice = details_->slice.load(std::memory_order_acquire);
    return slice != nullptr && slice != Details::SWAP_SENTINEL;
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
    Slice* sl = details_->slice.exchange(nullptr, std::memory_order_acquire);
    if (sl != nullptr && sl != Details::SWAP_SENTINEL) {
        delete sl;
        sl = nullptr;
    }
    while (sl == nullptr || sl == Details::SWAP_SENTINEL) {
        Slice* expected = nullptr;
        if (details_->slice.compare_exchange_strong(expected, Details::SWAP_SENTINEL)) {
            details_->slice.store(new Slice(std::move(slice)), std::memory_order_release);
            if (details_->context_deleter != nullptr && details_->context != nullptr) {
                details_->context_deleter(details_->context);
            }
            details_->context = nullptr;
            details_->context_deleter = nullptr;
            return;
        }
        if (expected != nullptr && expected->raw() != slice.raw()) {
            sl = details_->slice.exchange(nullptr, std::memory_order_acquire);
            if (sl != nullptr && sl != Details::SWAP_SENTINEL) {
                delete sl;
                sl = nullptr;
            }
        }
    }
}
} // namespace buffetalligator
