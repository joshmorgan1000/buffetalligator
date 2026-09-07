/** --------------------------------------------------------------------------------------------------------- BuffetAlligator Memory
 * @file slice.cpp
 * @brief Implements registered placement chains, background replenishment, and Slice lifetime.
 */
#include <alligator.hpp>
#include <memory/alligator.hpp>
#include <memory/buffet.hpp>
#include <memory/slicefriend.hpp>
#include <algorithm>
#include <cstring>

namespace buffetalligator {
/** ------------------------------------------------------------------------------------------- Default Placement
 * @brief Returns the default placement for slices, which is determined by the system's
 * capabilities.
 * @return The default `Placement` enum value for slices.
 */
const Placemat* Slice::default_placement() {
    return BuffetMenu::default_placement();
}
/** ------------------------------------------------------------------------------------------- Constructor - Fresh Claim
 * @brief Claims a slice of pre-allocated memory in the buffet alligator's slab arena. The slice is
 * guaranteed to be zero-initialized.
 * @param size The size of the slice in bytes.
 */
Slice::Slice(size_t size, const Placemat* placement) {
    *this = Alligator::instance().current_for_placement(placement->type())->claim(size);
}
/** ------------------------------------------------------------------------------------------- Constructor - Fresh Claim
 * @brief Claims a slice of pre-allocated memory in the buffet alligator's slab arena, with the option to
 * specify whether the slice should be part of a larger slab or a novel buffer. The slice is
 * guaranteed to be zero-initialized.
 * @param size The size of the slice in bytes.
 * @param novel_buffer If true, then the slice is allocated as a novel buffer instead of being
 * a claim of a pre-allocated slab. This is ideal for slices that are long-lived to help
 * reduce fragmentation in the arena.
 */
Slice::Slice(size_t size, bool novel_buffer, const Placemat* placement) {
    *this = Alligator::instance().current_for_placement(placement->type())->claim(size, novel_buffer);
}
/** ------------------------------------------------------------------------------------------- Constructor - Copy from External Memory
 * @brief Copies data from an external memory location into a new slice of memory in the buffet alligator.
 * This can be used to deep-copy a slice, or load data from an external source into the buffet alligator's
 * memory management system.
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
    *this = Alligator::instance().current_for_placement(placement->type())->claim(copy_from, size, novel_buffer);
}
/** ------------------------------------------------------------------------------------------- Copy/move semantics
 * @brief Copying a `Slice` does not actually copy the underlying memory, `Slice` objects act
 * much like `std::shared_ptr` in that they share the same reference counter and underlying
 * memory. Move semantics transfer ownership without reference-counting traffic.
 */
Slice::Slice(const Slice& other)
: meta_(other.meta_)
, cached_(other.cached_) {
    if (cached_) {
        Alligator::instance().get(meta_ & 0x1FFFFu)->ref_count_.fetch_add(1, std::memory_order_acq_rel);
    }
}
/** ------------------------------------------------------------------------------------------- Copy assignment operator
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
/** ------------------------------------------------------------------------------------------- Move constructor
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
/** ------------------------------------------------------------------------------------------- Move assignment operator
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
/** ------------------------------------------------------------------------------------------- Placement
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
/** ------------------------------------------------------------------------------------------- Create new view
 * @brief Creates a new view of the slice, which is a sub-slice of the original slice. The new
 * view shares the same underlying memory and reference counter as the original slice. Using
 * the default parameters will create a new view that is essentially identical to the original
 * slice - a shared view that increments the reference counter and will keep the underlying
 * memory alive until all views are destroyed.
 * @param offset The offset in bytes from the start of the original slice to the start of the
 * new view.
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
/** ------------------------------------------------------------------------------------------- Resize
 * @brief Resizes the slice to a new size. If `preserve_data` is true, the existing data in
 * the slice will be preserved up to the minimum of the old and new sizes. If `preserve_data`
 * is false, the existing data will be discarded and the slice will be reallocated. This can
 * be called on a freed or null slice, in which case it will behave like a normal constructor
 * and allocate a new slice of the specified size.
 * @param new_size The new size of the slice in bytes.
 * @param preserve_data Whether to preserve existing data in the slice. Default is true.
 * @param novel_buffer Whether to allocate a novel buffer even if the slice is not null.
 * Default is false.
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
/** ------------------------------------------------------------------------------------------- Free
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
/** ------------------------------------------------------------------------------------------- Do Something Fun
 * @brief A placeholder function for demonstration purposes.
 * @param ptr A void pointer parameter.
 */
void SliceFriend::do_somthing_fun(void* ptr) {
    Alligator::instance().enqueue_order(ptr);
}
} // namespace buffetalligator
