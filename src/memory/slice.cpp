/** --------------------------------------------------------------------------------------------------------- BuffetAlligator Memory
 * @file slice.cpp
 * @brief Slice construction, sharing, views, and release: every operation dispatches through the semantics table
 * the Slice's type word names.
 */
#include <alligator.hpp>
#include <memory/chainbuffet.hpp>
#include <memory/slicetype.hpp>
#include <algorithm>
#include <bit>
#include <cstring>
#include <utility>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Default Placement
 * @brief Resolves the payload placement before the first Slice claim.
 * @return The default placement descriptor for the new Slice.
 */
const BuffetDescriptor* Slice::default_placement() {
    /// The Alligator's construction registers every placement and lets the GPU probe pick the default.
    Alligator::inst();
    return BuffetDescriptors::default_placement();
}
/** --------------------------------------------------------------------------------------------------------- Fresh Claim
 * @brief Claims a range rounded up to 64 bytes from the selected placement; zero claims nothing.
 * @param size The number of bytes to claim.
 * @param placement The placement descriptor for the new Slice.
 */
Slice::Slice(size_t size, const BuffetDescriptor* placement) {
    if (size == 0) [[unlikely]] return;
    if (size > UINT32_MAX) [[unlikely]] ALLIGATOR_THROW("Slice: requested size exceeds the 32-bit limit");
    *this = Alligator::claim(static_cast<uint32_t>(size), false, placement);
    num_elements_ = static_cast<uint32_t>((size << 3) / bitwidth());
}
/** --------------------------------------------------------------------------------------------------------- Dedicated Claim
 * @brief Claims a range rounded up to 64 bytes, dedicated when asked; zero claims nothing.
 * @param size The number of bytes to claim.
 * @param novel_buffer Whether to allocate a dedicated buffer.
 * @param placement The placement descriptor for the new Slice.
 */
Slice::Slice(size_t size, bool novel_buffer, const BuffetDescriptor* placement) {
    if (size == 0) [[unlikely]] return;
    if (size > UINT32_MAX) [[unlikely]] ALLIGATOR_THROW("Slice: requested size exceeds the 32-bit limit");
    *this = Alligator::claim(static_cast<uint32_t>(size), novel_buffer, placement);
    num_elements_ = static_cast<uint32_t>((size << 3) / bitwidth());
}
/** --------------------------------------------------------------------------------------------------------- External Copy
 * @brief Copies an external byte range into a new owned Slice.
 * @param copy_from The source memory to copy from.
 * @param size The number of bytes to copy.
 * @param novel_buffer Whether to allocate a dedicated buffer.
 * @param placement The placement descriptor for the new Slice.
 */
Slice::Slice(
    const void* copy_from,
    size_t size,
    bool novel_buffer,
    const BuffetDescriptor* placement
) {
    if (size == 0) return;
    if (copy_from == nullptr) [[unlikely]] {
        ALLIGATOR_THROW("Copying a nonempty Slice requires source memory");
    }
    if (size > UINT32_MAX) [[unlikely]] ALLIGATOR_THROW("Slice: requested size exceeds the 32-bit limit");
    *this = Alligator::claim(static_cast<uint32_t>(size), novel_buffer, placement);
    num_elements_ = static_cast<uint32_t>((size << 3) / bitwidth());
    std::memcpy(raw(), copy_from, size);
}
/** --------------------------------------------------------------------------------------------------------- Shared Copy
 * @brief What a copy is depends on the other Slice's table: one more count on its root for a handle.
 * @param other The Slice object to copy from.
 */
Slice::Slice(const Slice& other) {
    SliceStatics::semantics(other.type_).copy_fn(other, *this);
}
/** --------------------------------------------------------------------------------------------------------- Shared Assignment
 * @brief Lets go of what this held, then copies the other Slice as its table says.
 * @param other The Slice object to share from.
 * @return A reference to the current Slice object after the shared assignment.
 */
Slice& Slice::operator=(const Slice& other) {
    if (this == &other) return *this;
    SliceStatics::semantics(type_).destructor_fn(*this);
    SliceStatics::semantics(other.type_).copy_fn(other, *this);
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Move
 * @brief Moving takes the other Slice's fields and leaves the source null; no count changes.
 * @param other The Slice object to move from.
 */
Slice::Slice(Slice&& other) noexcept {
    SliceStatics::semantics(other.type_).move_fn(std::move(other), *this);
}
/** --------------------------------------------------------------------------------------------------------- Move Assignment
 * @brief Lets go of what this held, then takes the other Slice's fields and leaves the source null.
 * @param other The Slice object to move from.
 * @return A reference to the current Slice object after the move assignment.
 */
Slice& Slice::operator=(Slice&& other) noexcept {
    if (this == &other) return *this;
    SliceStatics::semantics(type_).destructor_fn(*this);
    SliceStatics::semantics(other.type_).move_fn(std::move(other), *this);
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Placement
 * @brief The placement behind the root: its link's for a pooled root, its buffer's for an owned one.
 * @return The placement descriptor, or nullptr for a null Slice or a weak view.
 */
const BuffetDescriptor* Slice::placement() const {
    if (parent_ == nullptr) return nullptr;
    if (is_pooled()) return static_cast<const ChainBuffet*>(parent_)->desc_->base;
    const Slice* root = static_cast<const Slice*>(parent_);
    return root->is_heap() ? BuffetDescriptors::descriptor<AlignedHeapBuffer>()
                           : BuffetDescriptors::descriptor<VulkanBuffer>();
}
/** --------------------------------------------------------------------------------------------------------- Raw
 * @brief The payload's host address, as this Slice's table resolves it.
 * @return The raw pointer to the start of the payload, or nullptr for a null Slice.
 */
void* Slice::raw() {
    return SliceStatics::semantics(type_).host_pointer_fn(*this);
}
/** --------------------------------------------------------------------------------------------------------- Raw (const)
 * @brief The payload's host address, as this Slice's table resolves it.
 * @return The raw pointer to the start of the payload, or nullptr for a null Slice.
 */
const void* Slice::raw() const {
    return SliceStatics::semantics(type_).host_pointer_fn(const_cast<Slice&>(*this));
}
/** --------------------------------------------------------------------------------------------------------- View
 * @brief A narrower Slice over this one's bytes, sharing as this Slice's table says and keeping its unit: the
 * view starts at the unit holding the offset and ends at the unit holding the last byte.
 * @param offset The starting byte offset within the current Slice.
 * @param length The number of bytes to include in the new Slice.
 * @return A new Slice object representing the specified subrange of the current Slice.
 */
Slice Slice::slice(size_t offset, size_t length) const {
    /// A view may reach into the padding the cut holds, so the bound is the capacity.
    const size_t bytes = capacity();
    if (offset > bytes || (length != SIZE_MAX && offset + length > bytes)) [[unlikely]] {
        std::string error_msg = "Slice::slice: requested range exceeds available bytes";
        ALLIGATOR_THROW(error_msg);
    }
    length = std::min(length, bytes - offset);
    if (length == 0) return Slice{};
    Slice view;
    SliceStatics::semantics(type_).copy_fn(*this, view);
    const size_t unit = alignment();
    const size_t start = offset & ~(unit - 1);
    const size_t end = (offset + length + unit - 1) & ~(unit - 1);
    view.address_ += start;
    view.size_ = static_cast<uint32_t>((end - start) >> std::countr_zero(unit));
    view.num_elements_ = static_cast<uint32_t>(((end - start) << 3) / bitwidth());
    return view;
}
/** --------------------------------------------------------------------------------------------------------- Resize
 * @brief Claims the new range, carries the overlap across when asked, and releases the old one.
 * @param new_size The desired size of the Slice after resizing.
 * @param preserve_data Whether to copy existing data to the new range.
 * @param novel_buffer Whether to allocate a novel buffer for the new range.
 * @param placement The descriptor of the buffer to place the new range in.
 */
void Slice::resize(
    size_t new_size,
    bool preserve_data,
    bool novel_buffer,
    const BuffetDescriptor* placement
) {
    /// A preserving shrink stays in place: fewer elements, and fewer padded units holding them.
    if (preserve_data && parent_ && new_size != 0 && new_size <= size_bytes()) {
        const size_t unit = alignment();
        size_ = static_cast<uint32_t>((new_size + unit - 1) >> std::countr_zero(unit));
        num_elements_ = static_cast<uint32_t>((new_size << 3) / bitwidth());
        return;
    }
    if (new_size > UINT32_MAX) [[unlikely]] ALLIGATOR_THROW("Slice: requested size exceeds the 32-bit limit");
    Slice fresh = Alligator::claim(static_cast<uint32_t>(new_size), novel_buffer, placement);
    fresh.num_elements_ = static_cast<uint32_t>((new_size << 3) / fresh.bitwidth());
    if (preserve_data && parent_ && new_size != 0) {
        std::memcpy(fresh.raw(), raw(), std::min(size_bytes(), new_size));
    }
    *this = std::move(fresh);
}
/** --------------------------------------------------------------------------------------------------------- Valid
 * @brief Checks if the Slice object is valid.
 * @return True if the Slice object holds a claim or a view, false otherwise.
 */
bool Slice::valid() const { return !is_null(); }
/** --------------------------------------------------------------------------------------------------------- Bool Conversion
 * @brief Checks if the Slice object is valid.
 * @return True if the Slice object holds a claim or a view, false otherwise.
 */
Slice::operator bool() const { return !is_null(); }
/** --------------------------------------------------------------------------------------------------------- Free
 * @brief Lets go of what this Slice holds as its table says, and goes null.
 */
void Slice::free() {
    SliceStatics::semantics(type_).destructor_fn(*this);
}
/** --------------------------------------------------------------------------------------------------------- Adopt
 * @brief Takes over another Slice, releasing whatever this held.
 * @param other The Slice object to adopt.
 * @note The other Slice will be left in a null state after adoption.
 */
void Slice::adopt(Slice other) {
    *this = std::move(other);
}
} // namespace buffetalligator
