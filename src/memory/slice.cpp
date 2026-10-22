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
/** --------------------------------------------------------------------------------------------------------- Default Placement
 * @brief Resolves the payload placement before the first Slice claim.
 */
const BuffetDescriptor* Slice::default_placement() {
    return BuffetDescriptors::default_placement();
}
/** --------------------------------------------------------------------------------------------------------- Fresh Claim
 * @brief Claims a range rounded up to 64-byte granules from the selected placement; zero claims nothing.
 */
Slice::Slice(size_t size, const BuffetDescriptor* placement) {
    if (size == 0) [[unlikely]] return;
    *this = Alligator::claim(size, false, placement);
}
/** --------------------------------------------------------------------------------------------------------- Dedicated Claim
 * @brief Claims 64-byte granules with optional dedicated backing ownership; zero claims nothing.
 */
Slice::Slice(size_t size, bool novel_buffer, const BuffetDescriptor* placement) {
    if (size == 0) [[unlikely]] return;
    *this = Alligator::claim(size, novel_buffer, placement);
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
    if (copy_from == nullptr) [[unlikely]] {
        ALLIGATOR_THROW("Copying a nonempty Slice requires source memory");
    }
    *this = Alligator::claim(size, novel_buffer, placement);
    std::memcpy(raw(), copy_from, size);
}
/** --------------------------------------------------------------------------------------------------------- Copy Constructor
 * @brief Shares the source's slot, adding one handle to it.
 */
Slice::Slice(const Slice& other) : id_(other.id_) {
    if (is_null()) [[unlikely]] return;
    std::atomic_ref<uint32_t>(Alligator::inst().entry(*this).state).fetch_add(1u, std::memory_order_relaxed);
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
    /// The whole span is a copy of this handle; any other span gets its own slot counting on this one.
    if (first == 0 && end == (bytes >> 6)) return Slice(*this);
    Alligator& arena = Alligator::inst();
    std::atomic_ref<uint32_t>(arena.entry(*this).state).fetch_add(1u, std::memory_order_relaxed);
    const Region& source = *arena.regions[(id_ >> 3) & 0x3f].load(std::memory_order_acquire);
    const GPUBuf record = source.gpu_slots[id_ >> 9];
    const uint32_t id = arena.next_id(placement());
    Region& region = *arena.regions[(id >> 3) & 0x3f].load(std::memory_order_acquire);
    const uint32_t slot = id >> 9;
    region.gpu_slots[slot].set(record.address, static_cast<uint32_t>(end - first),
        record.offset + static_cast<uint32_t>(first));
    region.host_ptrs[slot] = static_cast<uint8_t*>(source.host_ptrs[id_ >> 9]) + (first << 6);
    region.parents[slot] = id_;
    std::atomic_ref<uint32_t>(region.slots[slot].state).store(1u, std::memory_order_release);
    return from_id(id);
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
    return !is_null() && Alligator::inst().entry(*this).is_novel();
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
 * @brief Resolves a live view's exact first host byte, one table read.
 */
void* Slice::raw() {
    return is_null() ? nullptr
        : Alligator::inst().regions[(id_ >> 3) & 0x3f].load(std::memory_order_acquire)->host_ptrs[id_ >> 9];
}
/** --------------------------------------------------------------------------------------------------------- Raw Constant
 * @brief Resolves a live view's exact first host byte for constant access.
 */
const void* Slice::raw() const {
    return is_null() ? nullptr
        : Alligator::inst().regions[(id_ >> 3) & 0x3f].load(std::memory_order_acquire)->host_ptrs[id_ >> 9];
}
/** --------------------------------------------------------------------------------------------------------- Byte Length
 * @brief Returns the represented granule length in bytes.
 */
size_t Slice::size_bytes() const {
    return is_null() ? 0 : uint64_t{Alligator::inst().gpubuf(*this)->size} << 6;
}
/** --------------------------------------------------------------------------------------------------------- Discharge Unsafely
 * @brief Hands the id out without releasing it, for a `from_id` to take back later.
 */
uint32_t Slice::discharge_unsafely() { return std::exchange(id_, UINT32_MAX); }
/** --------------------------------------------------------------------------------------------------------- From ID
 * @brief Adopts an id that was claimed or discharged; the handle releases it like any other.
 */
Slice Slice::from_id(uint32_t id) {
    Slice slice;
    slice.id_ = id;
    return slice;
}
/** --------------------------------------------------------------------------------------------------------- Region Holding
 * @brief The region whose atomics table holds an entry, by address; the entry accessors are not the hot path.
 * @param entry The entry.
 * @return The region.
 */
Region& SliceEntry::region_holding(const SliceEntry* entry) {
    Alligator& arena = Alligator::inst();
    for (size_t index = 0; index < 64; ++index) {
        Region* region = arena.regions[index].load(std::memory_order_acquire);
        if (region == nullptr || region == reinterpret_cast<Region*>(-1)) continue;
        if (entry >= region->slots.data() && entry < region->slots.data() + region->slots.size()) return *region;
    }
    ALLIGATOR_THROW("SliceEntry lies outside every region");
}
/** --------------------------------------------------------------------------------------------------------- Chain Buffet Holding
 * @brief The ChainBuffet an entry's bytes live in, up through its parents to the ChainBuffet's own slot.
 * @param entry The entry.
 * @return The ChainBuffet.
 */
ChainBuffet* SliceEntry::chain_buffet_holding(const SliceEntry* entry) {
    Alligator& arena = Alligator::inst();
    Region* region = &region_holding(entry);
    uint32_t slot = static_cast<uint32_t>(entry - region->slots.data());
    while (std::atomic_ref<uint32_t>(region->slots[slot].state).load(std::memory_order_acquire)
            >> SliceEntry::TYPE_SHIFT != SliceTypes::CHAIN_BUFFET) {
        const uint32_t parent = region->parents[slot];
        region = arena.regions[(parent >> 3) & 0x3f].load(std::memory_order_acquire);
        slot = parent >> 9;
    }
    return static_cast<ChainBuffet*>(region->host_ptrs[slot]);
}
/** --------------------------------------------------------------------------------------------------------- Entry Buffet
 * @brief The buffet handle the entry's bytes live in.
 */
void* SliceEntry::buffet() const {
    return chain_buffet_holding(this)->buffer_;
}
/** --------------------------------------------------------------------------------------------------------- Entry Novel
 * @brief Whether the entry's bytes live in a novel buffer, the one kind of ChainBuffet with no successor.
 */
bool SliceEntry::is_novel() const {
    return chain_buffet_holding(this)->next_.load(std::memory_order_acquire) == nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Entry Size
 * @brief Widens the stored granule length before converting it to bytes.
 */
size_t SliceEntry::size() const {
    return uint64_t{gpu_buf()->size} << 6;
}
/** --------------------------------------------------------------------------------------------------------- Entry Offset
 * @brief Widens the stored granule offset before converting it to bytes.
 */
size_t SliceEntry::offset() const {
    return uint64_t{gpu_buf()->offset} << 6;
}
/** --------------------------------------------------------------------------------------------------------- Entry Host Pointer
 * @brief The entry's first host byte from the region's host pointer table.
 */
void* SliceEntry::host_ptr() {
    const Region& source = region_holding(this);
    return source.host_ptrs[this - source.slots.data()];
}
/** --------------------------------------------------------------------------------------------------------- Constant Entry Host Pointer
 * @brief The entry's first host byte for constant access.
 */
const void* SliceEntry::host_ptr() const {
    const Region& source = region_holding(this);
    return source.host_ptrs[this - source.slots.data()];
}
/** --------------------------------------------------------------------------------------------------------- Entry GPU Record
 * @brief Returns the granule record sharing this entry's region and slot.
 */
GPUBuf* SliceEntry::gpu_buf() {
    Region& source = region_holding(this);
    return &source.gpu_slots[this - source.slots.data()];
}
/** --------------------------------------------------------------------------------------------------------- Constant Entry GPU Record
 * @brief Returns the granule record for constant access.
 */
const GPUBuf* SliceEntry::gpu_buf() const {
    const Region& source = region_holding(this);
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