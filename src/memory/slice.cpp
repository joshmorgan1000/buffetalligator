/** --------------------------------------------------------------------------------------------------------- BuffetAlligator Memory
 * @file slice.cpp
 * @brief Implements registered placement chains, background replenishment, and Slice lifetime.
 */
#include <alligator.hpp>
#include <containers/bitplane.hpp>
#include <memory/arena.hpp>
#include <memory/tracker.hpp>
#include <simd.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Heap Buffer
 * @brief Allocates one zeroed buffer with checked 64-byte alignment.
 */
AlignedHeapBuffer::AlignedHeapBuffer(size_t size) {
    if (size > SIZE_MAX - 63) ALLIGATOR_THROW("Heap buffer size overflows 64-byte alignment");
    size_ = (size + 63) & ~size_t{63};
    if (size_ == 0) return;
    buffer_ = std::aligned_alloc(64, size_);
    if (!buffer_) ALLIGATOR_THROW("Cannot allocate a 64-byte-aligned heap buffer");
    std::memset(buffer_, 0, size_);
}
/** --------------------------------------------------------------------------------------------------------- Shared Buffet
 * @brief Allocates a representable shared heap buffer with one counted owner.
 */
SharedBuffet::SharedBuffet(size_t size ALLIGATOR_SITE_DEFINITION) {
    ALLIGATOR_SITE_GUARD;
    if (size > (uint64_t{UINT32_MAX} << 6)) {
        ALLIGATOR_THROW("SharedBuffet exceeds the 64-byte-granule size limit");
    }
    if (size == 0) return;
    const BuffetDescriptor* placement =
        BuffetDescriptors::descriptor_for(static_cast<AlignedHeapBuffer*>(nullptr));
    const size_t bytes = (size + 63) & ~size_t{63};
    void* buffer = placement->factory(bytes);
    try {
        token_ = new std::tuple<void*, const BuffetDescriptor*, std::atomic<uint32_t>, uint32_t>(
            buffer, placement, 1, static_cast<uint32_t>(bytes >> 6));
    } catch (...) {
        placement->deleter(buffer);
        throw;
    }
}
/** --------------------------------------------------------------------------------------------------------- Shared Buffet Release
 * @brief Releases this reference and frees the backing buffer after its last owner.
 */
void SharedBuffet::free() {
    auto* released = std::exchange(token_, nullptr);
    if (released && std::get<2>(*released).fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::get<1>(*released)->deleter(std::get<0>(*released));
        delete released;
    }
}
/** --------------------------------------------------------------------------------------------------------- Slice Type Records
 * @brief Registered element types; index zero stays null for raw bytes.
 */
const SliceTypeRecord* SliceTypes::records_[256] = {};
std::atomic<uint32_t> SliceTypes::count_{0};
/** --------------------------------------------------------------------------------------------------------- Register Type
 * @brief Assigns the next of the 255 type indices a slot's type byte can carry.
 */
uint8_t SliceTypes::register_type(const SliceTypeRecord* record) {
    const uint32_t index = count_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (index > 255) ALLIGATOR_THROW("SliceT supports at most 255 element types");
    records_[index] = record;
    return static_cast<uint8_t>(index);
}
/** --------------------------------------------------------------------------------------------------------- Default Placement
 * @brief Resolves the payload placement before the first Slice claim.
 */
const BuffetDescriptor* Slice::default_placement() {
    return BuffetDescriptors::default_placement();
}
/** --------------------------------------------------------------------------------------------------------- Fresh Claim
 * @brief Claims a range rounded up to 64-byte granules from the selected placement.
 */
Slice::Slice(size_t size, const BuffetDescriptor* placement ALLIGATOR_SITE_DEFINITION) {
    *this = ChainBuffet::chain(placement, size, false, 0 ALLIGATOR_SITE_ARGUMENT);
}
/** --------------------------------------------------------------------------------------------------------- Dedicated Claim
 * @brief Claims 64-byte granules with optional dedicated backing ownership.
 */
Slice::Slice(
    size_t size,
    bool novel_buffer,
    const BuffetDescriptor* placement
    ALLIGATOR_SITE_DEFINITION
) {
    *this = ChainBuffet::chain(placement, size, novel_buffer, 0 ALLIGATOR_SITE_ARGUMENT);
}
/** --------------------------------------------------------------------------------------------------------- External Copy
 * @brief Copies an external byte range into a new owned Slice.
 */
Slice::Slice(
    const void* copy_from,
    size_t size,
    bool novel_buffer,
    const BuffetDescriptor* placement
    ALLIGATOR_SITE_DEFINITION
) {
    if (size == 0) return;
    if (copy_from == nullptr) ALLIGATOR_THROW("Copying a nonempty Slice requires source memory");
    *this = ChainBuffet::chain(placement, size, novel_buffer, 0 ALLIGATOR_SITE_ARGUMENT);
    std::memcpy(raw(), copy_from, size);
}
/** --------------------------------------------------------------------------------------------------------- Copy Constructor
 * @brief Shares the slot and counts one more handle; a count past 65,535 throws.
 */
Slice::Slice(const Slice& other) : id_(other.id_) {
    if (is_null()) return;
    SlotState& state = Arena::states[id_ >> SEGMENT_BITS][id_ & SEGMENT_MASK];
    if (std::atomic_ref<uint16_t>(state.references).fetch_add(1, std::memory_order_relaxed)
        == UINT16_MAX) {
        std::atomic_ref<uint16_t>(state.references).fetch_sub(1, std::memory_order_relaxed);
        ALLIGATOR_THROW("A Slice slot passed 65,535 handles; share it through a novel buffer");
    }
}
/** --------------------------------------------------------------------------------------------------------- Copy Assignment
 * @brief Counts the source handle before releasing this one, so self-assignment stays live.
 */
Slice& Slice::operator=(const Slice& other) {
    if (this != &other) {
        Slice retained(other);
        *this = std::move(retained);
    }
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Move Constructor
 * @brief Transfers the identifier and leaves the source null.
 */
Slice::Slice(Slice&& other) noexcept : id_(std::exchange(other.id_, UINT32_MAX)) {}
/** --------------------------------------------------------------------------------------------------------- Move Assignment
 * @brief Releases the destination and transfers the source identifier.
 */
Slice& Slice::operator=(Slice&& other) noexcept {
    if (this != &other) {
        free();
        id_ = std::exchange(other.id_, UINT32_MAX);
    }
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Placement
 * @brief Returns the placement recorded for the slot's buffer.
 */
const BuffetDescriptor* Slice::placement() const {
    if (is_null()) return nullptr;
    const uint64_t word = Arena::locations[id_ >> SEGMENT_BITS][id_ & SEGMENT_MASK];
    return BuffetDescriptors::get(Arena::buffers[Location::buffer(word)].placement);
}
/** --------------------------------------------------------------------------------------------------------- Byte View
 * @brief Shares this slot for the whole span, or publishes an untyped slot that holds the buffer.
 */
Slice Slice::slice(size_t offset, size_t length) const {
    if (is_null() || length == 0) return Slice();
    const size_t bytes = size_bytes();
    if (offset > bytes) ALLIGATOR_THROW("Slice view offset exceeds its parent");
    const size_t available = bytes - offset;
    if (length == SIZE_MAX) length = available;
    if (length > available) ALLIGATOR_THROW("Slice view length exceeds its parent");
    if (length == 0) return Slice();
    const size_t first = offset >> 6;
    const size_t end = (offset + length + 63) >> 6;
    if (first == 0 && end << 6 == bytes) return *this;
    const uint64_t word = Arena::locations[id_ >> SEGMENT_BITS][id_ & SEGMENT_MASK];
    const uint32_t buffer = Location::buffer(word);
    const uint64_t start = Location::offset(word) + first;
    const Extent extent = Extent::of(end - first);
    if (start > OFFSET_LIMIT || extent.units == 0 || first + extent.granules > bytes >> 6)
        ALLIGATOR_THROW("Slice view does not fit the 26-bit offset or rounds past its parent");
    std::atomic_ref<uint64_t>(Arena::buffers[buffer].cursor)
        .fetch_add(LIVE_ONE, std::memory_order_relaxed);
    try {
        return Slice(AdoptId{},
            Arena::publish(Location::pack(buffer, extent.scale, start, extent.units), 0));
    } catch (...) {
        Arena::retire(buffer);
        throw;
    }
}
/** --------------------------------------------------------------------------------------------------------- Resize
 * @brief Resizes to whole 64-byte granules and preserves existing data when requested.
 */
void Slice::resize(
    size_t new_size,
    bool preserve_data,
    bool novel_buffer,
    const BuffetDescriptor* placement
    ALLIGATOR_SITE_DEFINITION
) {
    if (new_size == 0) {
        free();
        return;
    }
    if (new_size > (uint64_t{UINT32_MAX} << 6)) {
        ALLIGATOR_THROW("Slice resize exceeds the 64-byte-granule size limit");
    }
    const size_t rounded_size = (new_size + 63) & ~size_t{63};
    const size_t previous = size_bytes();
    if (preserve_data && !novel_buffer && placement == this->placement()) {
        if (rounded_size == previous) return;
        if (rounded_size < previous) {
            *this = slice(0, rounded_size);
            return;
        }
    }
    Slice resized(new_size, novel_buffer, placement ALLIGATOR_SITE_ARGUMENT);
    if (preserve_data && previous != 0) {
        std::memcpy(resized.raw(), raw(), std::min(previous, rounded_size));
    }
    *this = std::move(resized);
}
/** --------------------------------------------------------------------------------------------------------- Novel Backing
 * @brief Reports whether the slot's buffer is dedicated.
 */
bool Slice::is_novel() const noexcept {
    if (is_null()) return false;
    const uint64_t word = Arena::locations[id_ >> SEGMENT_BITS][id_ & SEGMENT_MASK];
    return Arena::buffers[Location::buffer(word)].novel != 0;
}
/** --------------------------------------------------------------------------------------------------------- Adopt
 * @brief Transfers the passed view into this handle.
 */
void Slice::adopt(Slice other) { *this = std::move(other); }
/** --------------------------------------------------------------------------------------------------------- Free
 * @brief Drops a handle; the last destroys typed elements, frees the slot, and retires the buffer.
 */
void Slice::free() {
    if (is_null()) return;
    SlotState& state = Arena::states[id_ >> SEGMENT_BITS][id_ & SEGMENT_MASK];
    if (std::atomic_ref<uint16_t>(state.references).fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (state.type) {
            const SliceTypeRecord& record = *SliceTypes::records_[state.type];
            if (record.destroy) record.destroy(raw(), size_bytes() / record.size);
        }
        const uint64_t word = Arena::locations[id_ >> SEGMENT_BITS][id_ & SEGMENT_MASK];
        const uint32_t buffer = Location::buffer(word);
        std::atomic_ref<uint64_t>(Arena::occupancy[id_ >> SEGMENT_BITS][(id_ & SEGMENT_MASK) >> 6])
            .fetch_and(~(uint64_t{1} << (id_ % 64)), std::memory_order_release);
        Arena::retire(buffer);
    }
    id_ = UINT32_MAX;
}
/** --------------------------------------------------------------------------------------------------------- Is Null
 * @brief Tests the null identifier without accessing arena metadata.
 */
bool Slice::is_null() const { return id_ == UINT32_MAX; }
/** --------------------------------------------------------------------------------------------------------- Valid
 * @brief Reports whether this handle owns an identifier.
 */
bool Slice::valid() const { return !is_null(); }
/** --------------------------------------------------------------------------------------------------------- Boolean Conversion
 * @brief Reports whether this handle owns an identifier.
 */
Slice::operator bool() const { return !is_null(); }
/** --------------------------------------------------------------------------------------------------------- Raw
 * @brief Adds the slot's granule offset to its buffer's host base.
 */
void* Slice::raw() {
    if (is_null()) return nullptr;
    const uint64_t word = Arena::locations[id_ >> SEGMENT_BITS][id_ & SEGMENT_MASK];
    return Arena::buffers[Location::buffer(word)].host_base + (Location::offset(word) << 6);
}
/** --------------------------------------------------------------------------------------------------------- Raw Constant
 * @brief Adds the slot's granule offset to its buffer's host base for constant access.
 */
const void* Slice::raw() const {
    if (is_null()) return nullptr;
    const uint64_t word = Arena::locations[id_ >> SEGMENT_BITS][id_ & SEGMENT_MASK];
    return Arena::buffers[Location::buffer(word)].host_base + (Location::offset(word) << 6);
}
/** --------------------------------------------------------------------------------------------------------- Byte Length
 * @brief Widens the scaled length field, or reads the buffer size for a whole-buffer slot.
 */
size_t Slice::size_bytes() const {
    if (is_null()) return 0;
    const uint64_t word = Arena::locations[id_ >> SEGMENT_BITS][id_ & SEGMENT_MASK];
    const uint64_t units = Location::units(word);
    return units ? units << (6 + 2 * Location::scale(word))
        : uint64_t{Arena::buffers[Location::buffer(word)].granules} << 6;
}
/** ------------------------------------------------------------------------------------------- Discharge Unsafely
 * @brief Releases the underlying ID of the slice without marking the slots as unallocated,
 * which must be used carefully because it could lead to a memory leak if it is not re-adopted
 * by another slice object later (e.g. by using `Slice::from_id`).
 * @return The pool index of the discharged slice.
 */
uint32_t Slice::discharge_unsafely() {
    uint32_t to_discharge = id_;
    id_ = UINT32_MAX;
    return to_discharge;
}
/** ------------------------------------------------------------------------------------------- From ID
 * @brief Creates a slice from the given previously discharged ID, that will safely release
 * the claim to memory when the `Slice` object is destroyed.
 * @param id The ID to create the slice from.
 * @return A slice corresponding to the given ID.
 */
Slice Slice::from_id(uint32_t id) {
    Slice slice;
    slice.id_ = id;
    return slice;
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
) : details_(std::make_shared<Details>()) {
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
