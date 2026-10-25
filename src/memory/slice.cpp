/** --------------------------------------------------------------------------------------------------------- BuffetAlligator Memory
 * @file slice.cpp
 * @brief Slice construction, sharing, views, and release: the Slice is the record, and its claim's lifecycle
 * granule is touched only when a Slice is copied or let go of.
 */
#include <alligator.hpp>
#include <memory/chainbuffet.hpp>
#include <algorithm>
#include <atomic>
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
 * @brief Claims a range rounded up to 64-byte granules from the selected placement; zero claims nothing.
 * @param size The number of bytes to claim.
 * @param placement The placement descriptor for the new Slice.
 */
Slice::Slice(size_t size, const BuffetDescriptor* placement) {
    if (size == 0) [[unlikely]] return;
    *this = Alligator::claim(size, false, placement);
}
/** --------------------------------------------------------------------------------------------------------- Dedicated Claim
 * @brief Claims 64-byte granules with optional dedicated backing ownership; zero claims nothing.
 * @param size The number of bytes to claim.
 * @param novel_buffer Whether to allocate a dedicated buffer.
 * @param placement The placement descriptor for the new Slice.
 */
Slice::Slice(size_t size, bool novel_buffer, const BuffetDescriptor* placement) {
    if (size == 0) [[unlikely]] return;
    *this = Alligator::claim(size, novel_buffer, placement);
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
    *this = Alligator::claim(size, novel_buffer, placement);
    std::memcpy(raw(), copy_from, size);
}
/** --------------------------------------------------------------------------------------------------------- Shared Copy
 * @brief A copy is the same record with one more owner on its claim.
 * @param other The Slice object to copy from.
 */
Slice::Slice(const Slice& other)
: gpu_(other.gpu_), host_(other.host_), record_(other.record_), info_(other.info_) {
    if (record_) Alligator::retain(*this);
}
/** --------------------------------------------------------------------------------------------------------- Shared Assignment
 * @brief Takes one more owner on the other claim before letting go of this one, then is the other record.
 * @param other The Slice object to share from.
 * @return A reference to the current Slice object after the shared assignment.
 */
Slice& Slice::operator=(const Slice& other) {
    if (this == &other) return *this;
    if (other.record_) Alligator::retain(other);
    free();
    gpu_ = other.gpu_;
    host_ = other.host_;
    record_ = other.record_;
    info_ = other.info_;
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Move
 * @brief Moving takes the record and leaves the source null; no count changes.
 * @param other The Slice object to move from.
 */
Slice::Slice(Slice&& other) noexcept
: gpu_(other.gpu_), host_(other.host_), record_(other.record_), info_(other.info_) {
    other.gpu_ = GPUBuf{};
    other.host_ = nullptr;
    other.record_ = nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Move Assignment
 * @brief Releases whatever this held, then takes the other Slice's record and leaves the source null.
 * @param other The Slice object to move from.
 * @return A reference to the current Slice object after the move assignment.
 */
Slice& Slice::operator=(Slice&& other) noexcept {
    if (this == &other) return *this;
    free();
    gpu_ = other.gpu_;
    host_ = other.host_;
    record_ = other.record_;
    info_ = other.info_;
    other.gpu_ = GPUBuf{};
    other.host_ = nullptr;
    other.record_ = nullptr;
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Placement
 * @brief The placement of the link the claim is on, null for a null Slice.
 * @return The placement descriptor, or nullptr for a null Slice.
 */
const BuffetDescriptor* Slice::placement() const {
    const uintptr_t bits = reinterpret_cast<uintptr_t>(record_);
    return record_ ? reinterpret_cast<SliceRecord*>(bits & ~SLICE_VIEW_FLAG)->link->desc_ : nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Raw
 * @brief The payload's host address.
 * @return The raw pointer to the start of the payload, or nullptr for a null Slice.
 */
void* Slice::raw() {
    return host_;
}
/** --------------------------------------------------------------------------------------------------------- Raw (const)
 * @brief The payload's host address.
 * @return The raw pointer to the start of the payload, or nullptr for a null Slice.
 */
const void* Slice::raw() const {
    return host_;
}
/** --------------------------------------------------------------------------------------------------------- GPU Record
 * @brief The record a shader reads: the slab's device address, the length and the offset in granules.
 * @return The GPU record, all zero for a null Slice.
 */
const GPUBuf& Slice::gpu() const {
    return gpu_;
}
/** --------------------------------------------------------------------------------------------------------- View
 * @brief The whole range is a shared copy; a narrower range is a view that keeps the bytes, counted on the link
 * so the claim's owners alone decide when the element goes.
 * @param offset The starting byte offset within the current Slice.
 * @param length The number of bytes to include in the new Slice.
 * @return A new Slice object representing the specified subrange of the current Slice.
 */
Slice Slice::slice(size_t offset, size_t length) const {
    const size_t bytes = size_bytes();
    if (offset == 0 && length == SIZE_MAX) return Slice(*this);
    if (offset > bytes || (length != SIZE_MAX && offset + length > bytes)) [[unlikely]] {
        std::string error_msg = "Slice::slice: requested range exceeds available bytes";
        ALLIGATOR_THROW(error_msg);
    }
    length = std::min(length, bytes - offset);
    if (length == 0) return Slice{};
    const uint32_t first = static_cast<uint32_t>(offset) >> 6;
    const uint32_t last = static_cast<uint32_t>(offset + length + 63u) >> 6;
    const uintptr_t bits = reinterpret_cast<uintptr_t>(record_);
    SliceRecord* record = reinterpret_cast<SliceRecord*>(bits & ~SLICE_VIEW_FLAG);
    record->link->count_.fetch_add(1, std::memory_order_acq_rel);
    Slice view;
    view.gpu_.set(gpu_.address, last - first, gpu_.offset + first);
    view.host_ = static_cast<uint8_t*>(host_) + (size_t(first) << 6);
    view.record_ = reinterpret_cast<SliceRecord*>(bits | SLICE_VIEW_FLAG);
    view.info_ = info_;
    return view;
}
/** --------------------------------------------------------------------------------------------------------- Size
 * @brief The Slice's granule length, in bytes.
 * @note Returns 0 for a null Slice.
 */
size_t Slice::size_bytes() const {
    return size_t(gpu_.size) << 6;
}
/** --------------------------------------------------------------------------------------------------------- Resize
 * @brief Claims the new range, carries the overlap across when asked, and releases the old record.
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
    /// A preserving shrink stays in place: this Slice covers fewer granules.
    if (preserve_data && record_ && new_size != 0 && new_size <= size_bytes()) {
        gpu_.size = static_cast<uint32_t>((new_size + 63u) >> 6);
        return;
    }
    Slice fresh = Alligator::claim(new_size, novel_buffer, placement);
    if (preserve_data && record_ && new_size != 0) {
        std::memcpy(fresh.raw(), raw(), std::min(size_bytes(), new_size));
    }
    *this = std::move(fresh);
}
/** --------------------------------------------------------------------------------------------------------- Is Null
 * @brief Checks if the Slice object is null.
 * @return True if the Slice object holds no claim, false otherwise.
 */
bool Slice::is_null() const { return record_ == nullptr; }
/** --------------------------------------------------------------------------------------------------------- Valid
 * @brief Checks if the Slice object is valid.
 * @return True if the Slice object holds a claim, false otherwise.
 */
bool Slice::valid() const { return record_ != nullptr; }
/** --------------------------------------------------------------------------------------------------------- Bool Conversion
 * @brief Checks if the Slice object is valid.
 * @return True if the Slice object holds a claim, false otherwise.
 */
Slice::operator bool() const { return record_ != nullptr; }
/** --------------------------------------------------------------------------------------------------------- Free
 * @brief Hands this owner back to the Alligator and goes null.
 */
void Slice::free() {
    if (!record_) return;
    Alligator::inst().destroy(*this);
    gpu_ = GPUBuf{};
    host_ = nullptr;
    record_ = nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Novel Backing
 * @brief Whether the bytes live on a dedicated link.
 * @return True if the Slice's bytes are on a dedicated link, false otherwise.
 */
bool Slice::is_novel() const noexcept {
    const uintptr_t bits = reinterpret_cast<uintptr_t>(record_);
    return record_ && reinterpret_cast<SliceRecord*>(bits & ~SLICE_VIEW_FLAG)->link->novel_;
}
/** --------------------------------------------------------------------------------------------------------- Adopt
 * @brief Takes over another Slice's record, releasing whatever this held.
 * @param other The Slice object to adopt.
 * @note The other Slice will be left in a null state after adoption.
 */
void Slice::adopt(Slice other) {
    *this = std::move(other);
}
} // namespace buffetalligator
