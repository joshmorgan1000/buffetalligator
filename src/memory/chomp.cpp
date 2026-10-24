/** --------------------------------------------------------------------------------------------------------- BuffetAlligator Memory
 * @file chomp.cpp
 * @brief Chomp construction, sharing, views, and release, every one of them through the Alligator.
 */
#include <alligator.hpp>
#include <algorithm>
#include <cstring>
#include <utility>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Default Placement
 * @brief Resolves the payload placement before the first Chomp claim.
 * @return The default placement descriptor for the new Chomp.
 */
const BuffetDescriptor* Chomp::default_placement() {
    /// The Alligator's construction registers every placement and lets the GPU probe pick the default.
    Alligator::inst();
    return BuffetDescriptors::default_placement();
}
/** --------------------------------------------------------------------------------------------------------- Fresh Claim
 * @brief Claims a range rounded up to 64-byte granules from the selected placement; zero claims nothing.
 * @param size The number of bytes to claim.
 * @param placement The placement descriptor for the new Chomp.
 */
Chomp::Chomp(size_t size, const BuffetDescriptor* placement) {
    if (size == 0) [[unlikely]] return;
    *this = Alligator::claim(size, false, placement);
}
/** --------------------------------------------------------------------------------------------------------- Dedicated Claim
 * @brief Claims 64-byte granules with optional dedicated backing ownership; zero claims nothing.
 * @param size The number of bytes to claim.
 * @param novel_buffer Whether to allocate a dedicated buffer.
 * @param placement The placement descriptor for the new Chomp.
 */
Chomp::Chomp(size_t size, bool novel_buffer, const BuffetDescriptor* placement) {
    if (size == 0) [[unlikely]] return;
    *this = Alligator::claim(size, novel_buffer, placement);
}
/** --------------------------------------------------------------------------------------------------------- External Copy
 * @brief Copies an external byte range into a new owned Chomp.
 * @param copy_from The source memory to copy from.
 * @param size The number of bytes to copy.
 * @param novel_buffer Whether to allocate a dedicated buffer.
 * @param placement The placement descriptor for the new Chomp.
 */
Chomp::Chomp(
    const void* copy_from,
    size_t size,
    bool novel_buffer,
    const BuffetDescriptor* placement
) {
    if (size == 0) return;
    if (copy_from == nullptr) [[unlikely]] {
        ALLIGATOR_THROW("Copying a nonempty Chomp requires source memory");
    }
    *this = Alligator::claim(size, novel_buffer, placement);
    std::memcpy(raw(), copy_from, size);
}
/** --------------------------------------------------------------------------------------------------------- Shared Copy
 * @brief A copy keeps the same id and slot and takes one more reference on the chain buffer behind it.
 * @param other The Chomp object to copy from.
 */
Chomp::Chomp(const Chomp& other) : id_(other.id_) {
    if (id_ != UINT32_MAX) Alligator::retain(*this);
}
/** --------------------------------------------------------------------------------------------------------- Shared Assignment
 * @brief Releases whatever this held, then shares the other Chomp's slot.
 * @param other The Chomp object to share from.
 * @return A reference to the current Chomp object after the shared assignment.
 */
Chomp& Chomp::operator=(const Chomp& other) {
    if (id_ == other.id_) return *this;
    free();
    id_ = other.id_;
    if (id_ != UINT32_MAX) Alligator::retain(*this);
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Move
 * @brief Moving transfers the id and leaves the source null; no reference count changes.
 * @param other The Chomp object to move from.
 */
Chomp::Chomp(Chomp&& other) noexcept : id_(std::exchange(other.id_, UINT32_MAX)) {}
/** --------------------------------------------------------------------------------------------------------- Move Assignment
 * @brief Releases whatever this held, then takes ownership of the other Chomp's id.
 * @param other The Chomp object to move from.
 * @return A reference to the current Chomp object after the move assignment.
 */
Chomp& Chomp::operator=(Chomp&& other) noexcept {
    if (this == &other) return *this;
    free();
    id_ = std::exchange(other.id_, UINT32_MAX);
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Placement
 * @brief The descriptor of the chain buffer behind the slot, null for a null Chomp.
 * @return The placement descriptor of the chain buffer behind the slot, or nullptr for a null Chomp.
 */
const BuffetDescriptor* Chomp::placement() const {
    return id_ == UINT32_MAX ? nullptr : Alligator::buffer_of(*this)->descriptor();
}
/** --------------------------------------------------------------------------------------------------------- Raw
 * @brief The chain buffer's host mapping plus this slot's byte offset.
 * @return The raw pointer to the start of the slot's memory, or nullptr for a null Chomp.
 */
void* Chomp::raw() {
    if (id_ == UINT32_MAX) return nullptr;
    return static_cast<uint8_t*>(Alligator::buffer_of(*this)->raw()) + (size_t(ChompID(id_).gpubuf()->offset) << 6);
}
/** --------------------------------------------------------------------------------------------------------- Raw (const)
 * @brief The chain buffer's host mapping plus this slot's byte offset.
 * @return The raw pointer to the start of the slot's memory, or nullptr for a null Chomp.
 */
const void* Chomp::raw() const {
    if (id_ == UINT32_MAX) return nullptr;
    return static_cast<const uint8_t*>(Alligator::buffer_of(*this)->raw())
        + (size_t(ChompID(id_).gpubuf()->offset) << 6);
}
/** --------------------------------------------------------------------------------------------------------- View
 * @brief The whole range is a shared copy; a narrower range takes its own slot on the same chain buffer.
 * @param offset The starting byte offset within the current Chomp.
 * @param length The number of bytes to include in the new Chomp.
 * @return A new Chomp object representing the specified subrange of the current Chomp.
 */
Chomp Chomp::chomp(size_t offset, size_t length) const {
    const size_t bytes = size_bytes();
    if (offset == 0 && length == SIZE_MAX) return Chomp(*this);
    if (offset > bytes || (length != SIZE_MAX && offset + length > bytes)) [[unlikely]] {
        std::string error_msg = "Chomp::chomp: requested range exceeds available bytes";
        ALLIGATOR_THROW(error_msg);
    }
    length = std::min(length, bytes - offset);
    if (length == 0) return Chomp{};
    // The sub-chomp is its own id: the next free slot in this id's region class, whose GPUBuf is the
    // parent's address over the granule-rounded range, sharing the parent's link handle with one more count.
    const bool novel = is_novel_chomp_id(id_);
    Alligator& alligator = Alligator::inst();
    SharedBuffet* const taken = reinterpret_cast<SharedBuffet*>(-1);
    ChompEntry* slot = nullptr;
    ChompID chomp_id;
    while (slot == nullptr) {
        const uint32_t counter = novel ? alligator.novel_idx.fetch_add(1, std::memory_order_relaxed)
            : alligator.transient_idx.fetch_add(1, std::memory_order_relaxed);
        chomp_id = novel
            ? ChompID((counter >> CLAIM_BITS) & NOVEL_CLAIM_MASK, counter & CLAIM_MASK, 0, true)
            : ChompID((counter >> CLAIM_BITS) % NUMBER_OF_TRANSIENT_REGIONS, counter & CLAIM_MASK, 0, false);
        SharedBuffet* expected = nullptr;
        if (std::atomic_ref<SharedBuffet*>(chomp_id.entry()->buffet).compare_exchange_strong(
                expected, taken, std::memory_order_acq_rel)) slot = chomp_id.entry();
    }
    const GPUBuf* parent = ChompID(id_).gpubuf();
    const uint32_t first = static_cast<uint32_t>(offset) >> 6;
    const uint32_t last = static_cast<uint32_t>(offset + length + 63u) >> 6;
    chomp_id.gpubuf()->set(parent->address, last - first, parent->offset + first);
    SharedBuffet* handle = Alligator::buffer_of(*this);
    handle->data_->refs.increment_refcount();
    std::atomic_ref<uint32_t>(*chomp_id.owners()).store(1u, std::memory_order_release);
    std::atomic_ref<SharedBuffet*>(slot->buffet).store(handle, std::memory_order_release);
    Chomp view;
    view.id_ = chomp_id.id();
    return view;
}
/** --------------------------------------------------------------------------------------------------------- Size
 * @brief The slot's granule length from its GPUBuf record, in bytes.
 * @note Returns 0 for a null Chomp.
 */
size_t Chomp::size_bytes() const {
    return id_ == UINT32_MAX ? 0 : size_t(ChompID(id_).gpubuf()->size) << 6;
}
/** --------------------------------------------------------------------------------------------------------- Resize
 * @brief Claims the new range, carries the overlap across when asked, and releases the old slot.
 * @param new_size The desired size of the Chomp after resizing.
 * @param preserve_data Whether to copy existing data to the new slot.
 * @param novel_buffer Whether to allocate a novel buffer for the new slot.
 * @param placement The descriptor of the buffer to place the new slot in.
 */
void Chomp::resize(
    size_t new_size,
    bool preserve_data,
    bool novel_buffer,
    const BuffetDescriptor* placement
) {
    /// A preserving shrink stays in place: the slot's record just covers fewer granules.
    if (preserve_data && id_ != UINT32_MAX && new_size != 0 && new_size <= size_bytes()) {
        ChompID(id_).gpubuf()->size = static_cast<uint32_t>((new_size + 63u) >> 6);
        return;
    }
    Chomp fresh = Alligator::claim(new_size, novel_buffer, placement);
    if (preserve_data && id_ != UINT32_MAX && new_size != 0) {
        std::memcpy(fresh.raw(), raw(), std::min(size_bytes(), new_size));
    }
    *this = std::move(fresh);
}
/** --------------------------------------------------------------------------------------------------------- Is Null
 * @brief Checks if the Chomp object is null.
 * @return True if the Chomp object holds a null id, false otherwise.
 */
bool Chomp::is_null() const { return id_ == UINT32_MAX; }
/** --------------------------------------------------------------------------------------------------------- Valid
 * @brief Checks if the Chomp object is valid.
 * @return True if the Chomp object holds a valid id, false otherwise.
 */
bool Chomp::valid() const { return id_ != UINT32_MAX; }
/** --------------------------------------------------------------------------------------------------------- Bool Conversion
 * @brief Checks if the Chomp object is valid.
 * @return True if the Chomp object holds a valid id, false otherwise.
 */
Chomp::operator bool() const { return id_ != UINT32_MAX; }
/** --------------------------------------------------------------------------------------------------------- Free
 * @brief Hands the slot back to the Alligator and goes null.
 */
void Chomp::free() {
    if (id_ == UINT32_MAX) return;
    Alligator::inst().destroy(*this);
    id_ = UINT32_MAX;
}
/** --------------------------------------------------------------------------------------------------------- Novel Backing
 * @brief Whether the id lives in a novel claim region.
 * @return True if the Chomp id resides in a novel claim region, false otherwise.
 */
bool Chomp::is_novel() const noexcept {
    return id_ != UINT32_MAX && is_novel_chomp_id(id_);
}
/** --------------------------------------------------------------------------------------------------------- Adopt
 * @brief Takes over another Chomp's slot, releasing whatever this held.
 * @param other The Chomp object to adopt.
 * @note The other Chomp will be left in a null state after adoption.
 */
void Chomp::adopt(Chomp other) {
    *this = std::move(other);
}
/** --------------------------------------------------------------------------------------------------------- Discharge and Adopt by Id
 * @brief The id crosses a queue or a shader as a bare word and comes back through from_id.
 * @return The raw Chomp id that was held by this object.
 */
uint32_t Chomp::discharge_unsafely() {
    return std::exchange(id_, UINT32_MAX);
}
/** --------------------------------------------------------------------------------------------------------- From Id
 * @brief Constructs a Chomp from a raw id.
 * @param id The raw Chomp id to wrap.
 * @return A Chomp object representing the given id.
 */
Chomp Chomp::from_id(uint32_t id) {
    Chomp chomp;
    chomp.id_ = id;
    return chomp;
}
} // namespace buffetalligator
