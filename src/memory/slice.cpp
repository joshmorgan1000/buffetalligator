/** --------------------------------------------------------------------------------------------------------- BuffetAlligator Memory
 * @file slice.cpp
 * @brief Implements registered placement chains, background replenishment, and Slice lifetime.
 */
#include <alligator.hpp>
#include <containers/bitplane.hpp>
#include <memory/tracker.hpp>
#include <simd.hpp>
#include <algorithm>
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
    if (!buffer_) throw std::bad_alloc();
    std::memset(buffer_, 0, size_);
}
/** --------------------------------------------------------------------------------------------------------- Shared Buffet
 * @brief Allocates a representable shared heap buffer with one counted owner.
 */
SharedBuffet::SharedBuffet(size_t size) {
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
/** --------------------------------------------------------------------------------------------------------- Default Placement
 * @brief Resolves the payload placement before the first Slice claim.
 */
const BuffetDescriptor* Slice::default_placement() {
    return BuffetDescriptors::default_placement();
}
/** --------------------------------------------------------------------------------------------------------- Fresh Claim
 * @brief Claims a range rounded up to 64-byte granules from the selected placement.
 */
Slice::Slice(size_t size, const BuffetDescriptor* placement) {
    *this = ChainBuffet::chain(placement, size);
}
/** --------------------------------------------------------------------------------------------------------- Dedicated Claim
 * @brief Claims 64-byte granules with optional dedicated backing ownership.
 */
Slice::Slice(size_t size, bool novel_buffer, const BuffetDescriptor* placement) {
    *this = ChainBuffet::chain(placement, size, novel_buffer);
}
/** --------------------------------------------------------------------------------------------------------- External Copy
 * @brief Copies an external byte range into a new owned Slice.
 */
Slice::Slice(
    const void* copy_from,
    size_t size,
    bool novel_buffer,
    const BuffetDescriptor* placement
) {
    if (size == 0) return;
    if (copy_from == nullptr) ALLIGATOR_THROW("Copying a nonempty Slice requires source memory");
    *this = ChainBuffet::chain(placement, size, novel_buffer);
    std::memcpy(raw(), copy_from, size);
}
/** --------------------------------------------------------------------------------------------------------- Copy Constructor
 * @brief Publishes a fresh identifier retaining the source byte view and backing allocation.
 */
Slice::Slice(const Slice& other) {
    if (other.is_null()) return;
    Alligator& arena = Alligator::inst();
    id_ = arena.next_id(other.placement());
    arena.entry(*this).set(arena.entry(other).token(), static_cast<uint8_t>((id_ >> 3) & 63));
    *arena.gpubuf(*this) = *arena.gpubuf(other);
}
/** --------------------------------------------------------------------------------------------------------- Copy Assignment
 * @brief Replaces this view only after the source's retained view has been published.
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
 * @brief Returns the descriptor encoded in a live Slice identifier.
 */
const BuffetDescriptor* Slice::placement() const {
    return is_null() ? nullptr : BuffetDescriptors::get(id_ & 7);
}
/** --------------------------------------------------------------------------------------------------------- Byte View
 * @brief Retains the 64-byte granules covering a requested range without moving its payload.
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
    Slice view(*this);
    Alligator& arena = Alligator::inst();
    GPUBuf& record = *arena.gpubuf(view);
    record.offset += static_cast<uint32_t>(first);
    record.size = static_cast<uint32_t>(end - first);
    return view;
}
/** --------------------------------------------------------------------------------------------------------- Resize
 * @brief Resizes to whole 64-byte granules and preserves existing data when requested.
 */
void Slice::resize(
    size_t new_size,
    bool preserve_data,
    bool novel_buffer,
    const BuffetDescriptor* placement
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
    Slice resized(new_size, novel_buffer, placement);
    if (preserve_data && previous != 0) {
        std::memcpy(resized.raw(), raw(), std::min(previous, rounded_size));
    }
    *this = std::move(resized);
}
/** --------------------------------------------------------------------------------------------------------- Novel Backing
 * @brief Reports whether this view retains a dedicated backing allocation.
 */
bool Slice::is_novel() const noexcept {
    return !is_null() && Alligator::inst().entry(*this).token()->is_novel();
}
/** --------------------------------------------------------------------------------------------------------- Adopt
 * @brief Transfers the passed view into this handle.
 */
void Slice::adopt(Slice other) { *this = std::move(other); }
/** --------------------------------------------------------------------------------------------------------- Free
 * @brief Releases one owned identifier and leaves this handle null.
 */
void Slice::free() {
    if (!is_null()) Alligator::inst().destroy(*this);
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
 * @brief Resolves a live view's exact first host byte.
 */
void* Slice::raw() { return is_null() ? nullptr : Alligator::inst().entry(*this).host_ptr(); }
/** --------------------------------------------------------------------------------------------------------- Raw Constant
 * @brief Resolves a live view's exact first host byte for constant access.
 */
const void* Slice::raw() const {
    return is_null() ? nullptr : std::as_const(Alligator::inst()).entry(*this).host_ptr();
}
/** --------------------------------------------------------------------------------------------------------- Byte Length
 * @brief Returns the represented granule length in bytes.
 */
size_t Slice::size_bytes() const {
    return is_null() ? 0 : Alligator::inst().entry(*this).size();
}
/** --------------------------------------------------------------------------------------------------------- Entry Token
 * @brief Resolves the aligned, constructed token in an occupied entry.
 */
ChainBuffet::ChainBuffetToken* SliceEntry::token() {
    return std::launder(reinterpret_cast<ChainBuffet::ChainBuffetToken*>(data));
}
/** --------------------------------------------------------------------------------------------------------- Constant Entry Token
 * @brief Resolves the aligned, constructed token for constant access.
 */
const ChainBuffet::ChainBuffetToken* SliceEntry::token() const {
    return std::launder(reinterpret_cast<const ChainBuffet::ChainBuffetToken*>(data));
}
/** --------------------------------------------------------------------------------------------------------- Set Entry Token
 * @brief Constructs exactly one counted backing reference in a reserved entry.
 */
void SliceEntry::set_token(const ChainBuffet::ChainBuffetToken* retained) {
    std::construct_at(reinterpret_cast<ChainBuffet::ChainBuffetToken*>(data), *retained);
}
/** --------------------------------------------------------------------------------------------------------- Entry Region
 * @brief Returns the region containing this entry.
 */
uint8_t SliceEntry::region() const { return region_id_; }
/** --------------------------------------------------------------------------------------------------------- Set Entry Region
 * @brief Records the entry's destination region.
 */
void SliceEntry::set_region(uint8_t index) { region_id_ = index; }
/** --------------------------------------------------------------------------------------------------------- Publish Entry
 * @brief Constructs a backing reference and initializes one identifier owner.
 */
void SliceEntry::set(const ChainBuffet::ChainBuffetToken* retained, uint8_t index) {
    set_token(retained);
    set_region(index);
    owners.store(1, std::memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Clear Entry
 * @brief Ends the token's lifetime after the identifier's last owner retires.
 */
void SliceEntry::clear() { std::destroy_at(token()); }
/** --------------------------------------------------------------------------------------------------------- Entry Size
 * @brief Widens the stored granule length before converting it to bytes.
 */
size_t SliceEntry::size() const {
    const Region& source = *Alligator::inst().regions[region()].load(std::memory_order_acquire);
    const size_t index = this - source.slots.data();
    return uint64_t(source.gpu_slots[index].size) << 6;
}
/** --------------------------------------------------------------------------------------------------------- Entry Offset
 * @brief Widens the stored granule offset before converting it to bytes.
 */
size_t SliceEntry::offset() const {
    const Region& source = *Alligator::inst().regions[region()].load(std::memory_order_acquire);
    const size_t index = this - source.slots.data();
    return uint64_t(source.gpu_slots[index].offset) << 6;
}
/** --------------------------------------------------------------------------------------------------------- Entry Host Pointer
 * @brief Resolves this entry's exact byte offset through its retained backing.
 */
void* SliceEntry::host_ptr() { return token()->raw(offset()); }
/** --------------------------------------------------------------------------------------------------------- Constant Entry Host Pointer
 * @brief Resolves this entry's exact byte offset for constant access.
 */
const void* SliceEntry::host_ptr() const { return token()->raw(offset()); }
/** --------------------------------------------------------------------------------------------------------- Entry GPU Record
 * @brief Returns the granule record sharing this entry's region and slot.
 */
GPUBuf* SliceEntry::gpu_buf() {
    Region& source = *Alligator::inst().regions[region()].load(std::memory_order_acquire);
    return &source.gpu_slots[this - source.slots.data()];
}
/** --------------------------------------------------------------------------------------------------------- Constant Entry GPU Record
 * @brief Returns the granule record for constant access.
 */
const GPUBuf* SliceEntry::gpu_buf() const {
    const Region& source = *Alligator::inst().regions[region()].load(std::memory_order_acquire);
    return &source.gpu_slots[this - source.slots.data()];
}
/** --------------------------------------------------------------------------------------------------------- Entry From Slice
 * @brief Resolves an entry belonging to a live Slice.
 */
SliceEntry* SliceEntry::from_slice(const Slice& slice) {
    return &Alligator::inst().entry(slice);
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
