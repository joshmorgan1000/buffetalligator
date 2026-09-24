/** --------------------------------------------------------------------------------------------------------- BuffetAlligator Memory
 * @file slice.cpp
 * @brief Implements registered placement chains, background replenishment, and Slice lifetime.
 */
#include <alligator.hpp>
#include <containers/bitplane.hpp>
#include <memory/tracker.hpp>
#include <memory/plate.hpp>
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
/** --------------------------------------------------------------------------------------------------------- SliceId Slice
 * @brief Cuts a view from this identifier into a fresh pool slot, so a plate's base slot is never
 * owned by the Slice handed out.
 * @param offset The byte offset of the view.
 * @param size The view length in bytes, or SIZE_MAX for the remainder.
 * @return The view, holding one reference on the backing plate.
 */
Slice SliceId::slice(size_t offset, size_t size) const {
    Alligator& alligator = Alligator::inst();
    const SliceId view_id = alligator.next_id();
    const GPUBuf base = *alligator.gpubuf(*this);
    const uint32_t length = size == SIZE_MAX ? base.size - static_cast<uint32_t>(offset) : static_cast<uint32_t>(size);
    *alligator.gpubuf(view_id) = GPUBuf{
        base.address + offset, length, base.offset + static_cast<uint32_t>(offset)};
    *alligator.host_ptr(view_id) = HostPtr{static_cast<uint8_t*>(alligator.host_ptr(*this)->ptr) + offset};
    alligator.plate(view_id) = alligator.plate(*this);
    return Slice(view_id);
}
/** --------------------------------------------------------------------------------------------------------- Constructor - SliceId
 * @brief Constructs a slice object from an existing SliceId.
 * @param slice_id The identifier of the slice.
 */
Slice::Slice(SliceId slice_id) : id_(slice_id) {
    if (slice_id == static_cast<SliceId>(0xFFFFFFFFu)) return;
    if (!Alligator::inst().occupancy_->is_set(slice_id.id_)) {
        id_ = 0xFFFFFFFFu;
        return;
    }
    Alligator::inst().plate(slice_id)->ref_count.fetch_add(1, std::memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Constructor - Fresh Claim
 * @brief Claims a slice of pre-allocated memory in the buffet alligator's slab arena. The slice is
 * guaranteed to be zero-initialized.
 * @param size The size of the slice in bytes.
 */
Slice::Slice(size_t size, const Placemat* placement) {
    *this = placement->current_plate()->claim(size, false);
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
    *this = placement->current_plate()->claim(size, novel_buffer);
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
    *this = placement->current_plate()->claim(size, novel_buffer);
    std::memcpy(Alligator::inst().host_ptr(id_)->ptr, copy_from, size);
}
/** --------------------------------------------------------------------------------------------------------- Copy/move semantics
 * @brief Copying a `Slice` does not actually copy the underlying memory, `Slice` objects act
 * much like `std::shared_ptr` in that they share the same reference counter and underlying
 * memory. Move semantics transfer ownership without reference-counting traffic.
 */
Slice::Slice(const Slice& other) {
    if (other.id_ == 0xFFFFFFFFu || Alligator::inst().plate(other.id_) == nullptr) {
        id_ = 0xFFFFFFFFu;
        return;
    }
    id_ = Alligator::inst().next_id();
    (*Alligator::inst().gpubuf(id_)) = (*Alligator::inst().gpubuf(other.id_));
    (*Alligator::inst().host_ptr(id_)) = (*Alligator::inst().host_ptr(other.id_));
    Alligator::inst().plate(id_) = Alligator::inst().plate(other.id_);
    Alligator::inst().plate(id_)->ref_count.fetch_add(1, std::memory_order_relaxed);
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
        Slice other_copy(other);
        *this = std::move(other_copy);
    }
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Move constructor
 * @brief Moves the contents of one `Slice` to another, transferring ownership of the
 * underlying memory.
 * @param other The `Slice` to move from.
 */
Slice::Slice(Slice&& other) noexcept {
    id_ = other.id_;
    other.id_ = 0xFFFFFFFFu;
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
        id_ = other.id_;
        other.id_ = 0xFFFFFFFFu;
    }
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Placement
 * @brief Returns the memory placement type of the slice.
 * @return The `Placement` enum value representing the slice's memory placement.
 */
const Placemat* Slice::placement() const {
    if (id_ == 0xFFFFFFFFu || Alligator::inst().plate(id_) == nullptr) {
        return nullptr;
    }
    return Alligator::inst().plate(id_)->placemat;
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
        return Slice(*this);
    }
    if (is_null()) {
        return Slice();
    }
    const size_t current_size = size_bytes();
    if (offset >= current_size) {
        ALLIGATOR_THROW("Slice::slice: offset exceeds slice size");
    }
    const size_t view_size = current_size - offset;
    if (length == SIZE_MAX) {
        length = view_size;
    } else if (length > view_size) {
        ALLIGATOR_THROW("Slice::slice: length exceeds slice size");
    }
    Slice view(*this);
    HostPtr* host = Alligator::inst().host_ptr(view.id_);
    host->ptr = static_cast<uint8_t*>(host->ptr) + offset;
    GPUBuf* gpu = Alligator::inst().gpubuf(view.id_);
    gpu->address += offset;
    gpu->size = length;
    gpu->offset += offset;
    return view;
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
    if (preserve_data) {
        // Whole-block, sole-handle slices on resizable placements grow in place through the
        // substrate (realloc expands without copying whenever the allocator can).
        Plate* plate = Alligator::inst().plate(id_);
        const GPUBuf* whole = Alligator::inst().gpubuf(id_);
        if (is_novel() && plate->placemat->resizer_ != nullptr
            && whole->offset == 0 && whole->size == plate->size
        ) [[likely]] {
            const size_t old_size = size_bytes();
            auto [new_host, new_substrate] =
                plate->placemat->resizer_(Alligator::inst().host_ptr(id_)->ptr, plate->substrate_handle, new_size);
            if (new_host != nullptr) {
                // realloc leaves the growth uninitialized; the zero-init contract covers it.
                std::memset(static_cast<uint8_t*>(new_host) + old_size, 0, new_size - old_size);
                plate->substrate_handle = new_substrate;
                Memory::record_allocation(*plate->placemat, new_size - old_size);
                plate->size = new_size;
                Alligator::inst().host_ptr(id_)->ptr = new_host;
                GPUBuf* gpu = Alligator::inst().gpubuf(id_);
                gpu->address = reinterpret_cast<uint64_t>(new_host);
                gpu->size = new_size;
                const SliceId base_id(plate->slice_id.load(std::memory_order_acquire));
                *Alligator::inst().host_ptr(base_id) = HostPtr{new_host};
                *Alligator::inst().gpubuf(base_id) = *gpu;
                return;
            }
        }
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
    if (id_ == 0xFFFFFFFFu) {
        return false;
    }
    const Plate* plate = Alligator::inst().plate(id_);
    return plate != nullptr && plate->ref_count.load(std::memory_order_acquire) == 1;
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
    id_ = other.id_;
    other.id_ = 0xFFFFFFFFu;
}
/** --------------------------------------------------------------------------------------------------------- Free
 * @brief Frees the underlying memory of the slice. This is called automatically when the
 * slice is destroyed, but can be called manually to free the memory early. After calling this
 * method, the slice will be null.
 */
void Slice::free() {
    if (id_ == 0xFFFFFFFFu) {
        return;
    }
    Alligator::inst().destroy(id_);
    id_ = 0xFFFFFFFFu;
}
/** --------------------------------------------------------------------------------------------------------- Is Null
 * @brief Checks if the slice is null (i.e., has no underlying memory).
 * @return True if the slice is null, false otherwise.
 */
bool Slice::is_null() const {
    return id_ == 0xFFFFFFFFu || Alligator::inst().plate(id_) == nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Is Valid
 * @brief Checks if the slice is valid (i.e., has underlying memory).
 * @return True if the slice is valid, false otherwise.
 */
bool Slice::valid() const {
    return !is_null();
}
/** --------------------------------------------------------------------------------------------------------- Conversion to bool
 * @brief Allows the slice to be used in boolean contexts.
 * @return True if the slice is valid, false if it is null.
 */
Slice::operator bool() const {
    return !is_null();
}
/** --------------------------------------------------------------------------------------------------------- Raw
 * @brief Returns a raw pointer to the underlying memory of the slice.
 * @return A pointer to the raw memory, or nullptr if the slice is invalid.
 */
void* Slice::raw() {
    if (id_ == 0xFFFFFFFFu || Alligator::inst().plate(id_) == nullptr) {
        return nullptr;
    }
    return Alligator::inst().host_ptr(id_)->ptr;
}
/** --------------------------------------------------------------------------------------------------------- Raw (const)
 * @brief Returns a raw pointer to the underlying memory of the slice.
 * @return A pointer to the raw memory, or nullptr if the slice is invalid.
 */
const void* Slice::raw() const {
    if (id_ == 0xFFFFFFFFu || Alligator::inst().plate(id_) == nullptr) {
        return nullptr;
    }
    return Alligator::inst().host_ptr(id_)->ptr;
}
/** --------------------------------------------------------------------------------------------------------- Size in bytes
 * @brief Returns the size of the slice in bytes.
 * @return The size of the slice in bytes.
 */
size_t Slice::size_bytes() const {
    if (id_ == 0xFFFFFFFFu || Alligator::inst().plate(id_) == nullptr) {
        return 0;
    }
    return Alligator::inst().gpubuf(id_)->size;
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
/** --------------------------------------------------------------------------------------------------------- Record Slab Allocation
 * @brief Reports one completed slab allocation to the memory tracker.
 * @param plate The plate that now owns the slab.
 */
void Placemat::record_slab_allocation(const Plate* plate) {
    const size_t bytes = plate->size != 0 ? plate->size : plate->bump.load(std::memory_order_acquire);
    Memory::record_allocation(*plate->placemat, bytes);
    Memory::record_code_location(*plate->placemat, bytes, plate);
}
/** --------------------------------------------------------------------------------------------------------- Record Slab Release
 * @brief Reports one completed slab release to the memory tracker and frees a novel plate's base slot.
 * @param plate The plate whose slab was just returned.
 */
void Placemat::record_slab_release(const Plate* plate) {
    const size_t bytes = plate->size != 0 ? plate->size : plate->bump.load(std::memory_order_acquire);
    Memory::record_deallocation(*plate->placemat, bytes);
    Memory::forget_code_location(plate);
    const SliceId slice_id(plate->slice_id.load(std::memory_order_acquire));
    if (slice_id == static_cast<SliceId>(0xFFFFFFFFu)) return;  // Exhaustion already destroyed the base slot.
    Alligator& alligator = Alligator::inst();
    alligator.plate(slice_id) = nullptr;
    *alligator.gpubuf(slice_id) = GPUBuf{};
    *alligator.host_ptr(slice_id) = HostPtr{};
    alligator.occupancy_->test_and_clear(slice_id);  // Cleared last so the slot is not reissued early.
}
/** --------------------------------------------------------------------------------------------------------- Call Free Later
 * @brief Hands a plate's reference drop to the allocator thread.
 * @param plate The plate to free.
 */
void Placemat::call_free_later(Plate* plate) {
    Alligator::inst().submit([](Plate* later) { later->free(); }, plate);
}
} // namespace buffetalligator
