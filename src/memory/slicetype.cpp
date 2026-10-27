/** --------------------------------------------------------------------------------------------------------- Slice Implementation
 * @file slicetype.cpp
 * @brief Implementation of the Slice class.
 */
#include <logging.hpp>
#include <alligator.hpp>
#include <memory/chainbuffet.hpp>
#include <memory/slicetype.hpp>
#include <atomic>
#include <bit>
#include <utility>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Null Status
 * @brief Checks if the memory slice is null or not.
 * @return True if the memory slice is null, false otherwise.
 */
bool Slice::is_null() const noexcept { return types::from<types::Status>(type_) == types::Status::Null; }
/** --------------------------------------------------------------------------------------------------------- Encryption Status
 * @brief Checks if the memory slice is encrypted.
 * @return True if the memory slice is encrypted, false otherwise.
 */
bool Slice::is_encrypted() const noexcept {
    return types::from<types::State>(type_) == types::State::Encrypted
        || types::from<types::State>(type_) == types::State::EncryptedAndCompressed;
}
/** --------------------------------------------------------------------------------------------------------- Compression Status
 * @brief Checks if the memory slice is compressed.
 * @return True if the memory slice is compressed, false otherwise.
 */
bool Slice::is_compressed() const noexcept {
    return types::from<types::State>(type_) == types::State::Compressed
        || types::from<types::State>(type_) == types::State::EncryptedAndCompressed;
}
/** --------------------------------------------------------------------------------------------------------- Text Status
 * @brief Checks if the memory slice contains text data.
 * @return True if the memory slice contains text, false otherwise.
 */
bool Slice::is_text() const noexcept {
    return types::from<types::PrimitiveType>(type_) == types::PrimitiveType::Text;
}
/** --------------------------------------------------------------------------------------------------------- Numeric Status
 * @brief Checks if the memory slice contains numeric data.
 * @return True if the memory slice contains numeric data, false otherwise.
 */
bool Slice::is_numeric() const noexcept { return !is_text(); }
/** --------------------------------------------------------------------------------------------------------- Unsigned Status
 * @brief Checks if the memory slice contains unsigned numeric data.
 * @return True if the memory slice contains unsigned numeric data, false otherwise.
 */
bool Slice::is_unsigned() const noexcept {
    return types::from<types::PrimitiveType>(type_) == types::PrimitiveType::Unsigned;
}
/** --------------------------------------------------------------------------------------------------------- Floating Point Status
 * @brief Checks if the memory slice contains floating-point numeric data.
 * @return True if the memory slice contains floating-point numeric data, false otherwise.
 */
bool Slice::is_floating_point() const noexcept {
    return types::from<types::PrimitiveType>(type_) == types::PrimitiveType::FloatingPoint;
}
/** --------------------------------------------------------------------------------------------------------- Signed Status
 * @brief Checks if the memory slice contains signed numeric data.
 * @return True if the memory slice contains signed numeric data, false otherwise.
 */
bool Slice::is_signed() const noexcept {
    return !is_text() && types::from<types::PrimitiveType>(type_) != types::PrimitiveType::Unsigned;
}
/** --------------------------------------------------------------------------------------------------------- Integral Status
 * @brief Checks if the memory slice contains integral numeric data.
 * @return True if the memory slice contains integral numeric data, false otherwise.
 */
bool Slice::is_integral() const noexcept {
    return !is_text() && types::from<types::PrimitiveType>(type_) != types::PrimitiveType::FloatingPoint;
}
/** --------------------------------------------------------------------------------------------------------- Single Element Status
 * @brief Checks if the memory slice represents a single element.
 * @return True if the memory slice represents a single element, false otherwise.
 */
bool Slice::is_single_element() const noexcept {
    return types::from<types::Structure>(type_) == types::Structure::SingleElement;
}
/** --------------------------------------------------------------------------------------------------------- Singular Status
 * @brief Checks if the memory slice represents a singular element.
 * @return True if the memory slice represents a singular element, false otherwise.
 */
bool Slice::is_singular() const noexcept { return is_single_element(); }
/** --------------------------------------------------------------------------------------------------------- List Status
 * @brief Checks if the memory slice represents a list of elements.
 * @return True if the memory slice represents a list, false otherwise.
 */
bool Slice::is_list() const noexcept {
    return types::from<types::Structure>(type_) == types::Structure::List;
}
/** --------------------------------------------------------------------------------------------------------- Chunked Status
 * @brief Checks if the memory slice represents a chunked sequence.
 * @return True if the memory slice represents a chunked sequence, false otherwise.
 */
bool Slice::is_chunked() const noexcept {
    return types::from<types::Structure>(type_) == types::Structure::Chunked;
}
/** --------------------------------------------------------------------------------------------------------- JSON Status
 * @brief Checks if the memory slice represents JSON data.
 * @return True if the memory slice represents JSON data, false otherwise.
 */
bool Slice::is_json() const noexcept {
    return types::from<types::Structure>(type_) == types::Structure::JSON;
}
/** --------------------------------------------------------------------------------------------------------- Alignment
 * @brief Returns the alignment in bytes based on the Alignment enum.
 * @return The alignment in bytes.
 */
size_t Slice::alignment() const noexcept {
    return types::constants::ALIGNMENTS[static_cast<size_t>(
        types::from<types::Alignment>(type_)
    ) >> types::constants::ALIGNMENT_BITS];
}
/** --------------------------------------------------------------------------------------------------------- Aligned Status
 * @brief Checks if the memory slice is aligned.
 * @return True if the memory slice is aligned, false otherwise.
 */
bool Slice::is_aligned() const noexcept {
    return types::from<types::Alignment>(type_) != types::Alignment::Unaligned;
}
/** --------------------------------------------------------------------------------------------------------- Unaligned Status
 * @brief Checks if the memory slice is unaligned.
 * @return True if the memory slice is unaligned, false otherwise.
 */
bool Slice::is_unaligned() const noexcept {
    return types::from<types::Alignment>(type_) == types::Alignment::Unaligned;
}
/** --------------------------------------------------------------------------------------------------------- Page Aligned Status
 * @brief Checks if the memory slice is page aligned.
 * @return True if the memory slice is page aligned, false otherwise.
 */
bool Slice::is_page_aligned() const noexcept {
    return types::from<types::Alignment>(type_) == types::Alignment::PageAligned;
}
/** --------------------------------------------------------------------------------------------------------- Borrowed Status
 * @brief Checks if the memory slice is borrowed (weak reference).
 * @return True if the memory slice is borrowed, false otherwise.
 */
bool Slice::is_borrowed() const noexcept {
    return types::from<types::Ownership>(type_) == types::Ownership::Weak;
}
/** --------------------------------------------------------------------------------------------------------- Owned Status
 * @brief Checks if the memory slice is owned.
 * @return True if the memory slice is owned, false otherwise.
 */
bool Slice::is_owned() const noexcept {
    return types::from<types::Ownership>(type_) == types::Ownership::Owned;
}
/** --------------------------------------------------------------------------------------------------------- Shared Status
 * @brief Checks if the memory slice is shared.
 * @return True if the memory slice is shared, false otherwise.
 */
bool Slice::is_shared() const noexcept {
    return types::from<types::Ownership>(type_) == types::Ownership::Shared;
}
/** --------------------------------------------------------------------------------------------------------- Pooled Status
 * @brief Checks if the memory slice is part of a pooled allocation.
 * @return True if the memory slice is pooled, false otherwise.
 */
bool Slice::is_pooled() const noexcept {
    return types::from<types::Ownership>(type_) == types::Ownership::Pooled;
}
/** --------------------------------------------------------------------------------------------------------- File Mapped Status
 * @brief Checks if the memory slice is file mapped.
 * @return True if the memory slice is file mapped, false otherwise.
 */
bool Slice::is_file_mapped() const noexcept {
    return types::from<types::Location>(type_) == types::Location::FileMapped;
}
/** --------------------------------------------------------------------------------------------------------- Host Coherent Status
 * @brief Checks if the memory slice is host coherent.
 * @return True if the memory slice is host coherent, false otherwise.
 */
bool Slice::is_host_coherent() const noexcept {
    return types::from<types::Location>(type_) == types::Location::HostCoherent;
}
/** --------------------------------------------------------------------------------------------------------- Device Local Status
 * @brief Checks if the memory slice is device local.
 * @return True if the memory slice is device local, false otherwise.
 */
bool Slice::is_device_local() const noexcept {
    return types::from<types::Location>(type_) == types::Location::DeviceLocal;
}
/** --------------------------------------------------------------------------------------------------------- Heap Status
 * @brief Checks if the memory slice is heap allocated.
 * @return True if the memory slice is heap allocated, false otherwise.
 */
bool Slice::is_heap() const noexcept {
    return types::from<types::Location>(type_) == types::Location::Heap;
}
/** --------------------------------------------------------------------------------------------------------- Slice of Slices Status
 * @brief Checks if the memory slice is a slice of slices.
 * @return True if the memory slice is a slice of slices, false otherwise.
 */
bool Slice::is_slice_of_slices() const noexcept {
    return type_ & (1 << types::constants::SLICE_OF_SLICES_BIT);
}
/** --------------------------------------------------------------------------------------------------------- Slice Map Status
 * @brief Checks if the memory slice is a slice map.
 * @return True if the memory slice is a slice map, false otherwise.
 */
bool Slice::is_slicemap() const noexcept { return type_ & (1 << types::constants::SLICEMAP_BIT); }
/** --------------------------------------------------------------------------------------------------------- Atomic Status
 * @brief Checks if the memory slice is atomic.
 * @return True if the memory slice is atomic, false otherwise.
 */
bool Slice::is_atomic() const noexcept { return types::from<types::Sync>(type_) == types::Sync::Atomic; }
/** --------------------------------------------------------------------------------------------------------- Bitwidth Accessor (Instance)
 * @brief Retrieves the Bitwidth in number of bits per element. If the bitwidth
 * is unspecified or invalid it will default to 8 bits per element.
 * @return The Bitwidth in number of bits per element.
 */
size_t Slice::bitwidth() const noexcept {
    return types::constants::BITWIDTHS[(type_ >> types::constants::BITWIDTH_BITS) & 0xF];
}
/** --------------------------------------------------------------------------------------------------------- Size (in bits, instance)
 * @brief Calculates the total size in bits using the header-defined element count, if applicable.
 * @return The total size in bits for the specified header-defined element count.
 */
size_t Slice::size_bits() const noexcept {
    return types::type_size_bits(type_, num_elements_);
}
/** --------------------------------------------------------------------------------------------------------- Element Count Accessor (Instance)
 * @brief Retrieves the element count for the current element data.
 * @param data Optional pointer to header-defined element count.
 * @return The number of elements corresponding to the current element data.
 */
size_t Slice::size() const noexcept {
    return types::size(type_, num_elements_);
}
/** --------------------------------------------------------------------------------------------------------- Size (in bytes, instance)
 * @brief Calculates the total size in bytes using the header-defined element count, if applicable.
 * @param header_defined_element_count Optional header-defined element count.
 * @return The total size in bytes for the specified header-defined element count.
 */
size_t Slice::size_bytes() const noexcept {
    return types::size_bytes(type_, num_elements_);
}
/** --------------------------------------------------------------------------------------------------------- Capacity
 * @brief The bytes the claim holds for this Slice: its units times the unit its type word names.
 * @return The capacity in bytes.
 */
size_t Slice::capacity() const noexcept {
    return static_cast<size_t>(size_) * alignment();
}
/** --------------------------------------------------------------------------------------------------------- Parent Type
 * @brief The type this Slice's parent is, as its table was registered; `std::nullptr_t` for a null Slice.
 * @return The parent type.
 */
const std::type_info& Slice::parent_type() const noexcept {
    return SliceStatics::parent_type_of(type_);
}
/** --------------------------------------------------------------------------------------------------------- Semantics For
 * @brief The table for a registered type, made on its first registration: a type with a constructor backs its
 * own roots through its hooks, and a type without holds its payload in a chain claim whose last release runs
 * the destructor ahead of the link's.
 * @param type_info The registered type.
 * @param construct The type's constructor hook, returning its object, or null for a payload type.
 * @param size The byte size hook for the object.
 * @param host_pointer The host pointer hook for the object.
 * @param destroy The destroy hook for the object, or the payload's destructor.
 * @param share Whether Slices over one object share it by reference counting.
 * @return The table.
 */
void* Slice::semantics_for(
    const void* type_info,
    void* construct,
    void* size,
    void* host_pointer,
    void* destroy,
    bool share
) {
    const std::type_info* type = static_cast<const std::type_info*>(type_info);
    {
        std::shared_lock lock(SliceStatics::type_index_mutex());
        const auto found = SliceStatics::type_index_map().find(type->hash_code());
        if (found != SliceStatics::type_index_map().end()) {
            return &SliceStatics::semantics(uint32_t{found->second} << types::constants::SEMANTIC_BITS);
        }
    }
    types::SliceSemantics table;
    if (construct != nullptr) {
        table = SliceStatics::REGISTERED_SEMANTICS;
    } else {
        /// A payload type's Slices come from chains of its own, and the last handle on one runs its destructor.
        table = SliceStatics::CHAIN_BUFFER_SEMANTICS;
        table.destructor_fn = SliceStatics::payload_destroy;
    }
    table.share = share;
    table.parent_construct = reinterpret_cast<types::ParentConstructFn>(construct);
    table.parent_size = reinterpret_cast<types::ParentSizeFn>(size);
    table.parent_host_pointer = reinterpret_cast<types::ParentHostPointerFn>(host_pointer);
    table.parent_destroy = reinterpret_cast<types::ParentDestroyFn>(destroy);
    const uint16_t index = SliceStatics::add_semantics(table, type);
    types::SliceSemantics& registered = SliceStatics::semantics(uint32_t{index} << types::constants::SEMANTIC_BITS);
    if (construct == nullptr) {
        registered.placement = BuffetDescriptors::inst().add_typed(*Slice::default_placement(), &registered,
            registered.type);
    }
    return &registered;
}
/** --------------------------------------------------------------------------------------------------------- Seeded Type Index Map
 * @brief The built-in tables' type hashes mapped to their indices, read from the tables themselves.
 * @return The seeded map.
 */
absl::flat_hash_map<size_t, uint16_t> SliceStatics::seeded_type_index_map() {
    absl::flat_hash_map<size_t, uint16_t> map;
    const auto* tables = static_cast<const std::pair<types::SliceSemantics, const std::type_info*>*>(semantics());
    for (uint16_t index = 0; index < BUILTIN_TABLES; ++index) map[tables[index].second->hash_code()] = index;
    return map;
}
/** --------------------------------------------------------------------------------------------------------- Null: Sub-slice
 * @brief A narrower view of the same host address: nothing to count.
 * @param type The view's type word.
 * @param parent The view being cut.
 * @param offset The byte offset into it.
 * @param length The byte length.
 * @return The view.
 */
Slice SliceStatics::null_sub_slice(uint32_t type, void* parent, size_t offset, size_t length) {
    const Slice& view = *static_cast<const Slice*>(parent);
    Slice slice;
    slice.address_ = view.address_ + offset;
    slice.size_ = static_cast<uint32_t>(length);
    slice.num_elements_ = static_cast<uint32_t>((length << 3) / Slice::type_emulator(type, 1)->bitwidth());
    slice.type_ = type & ~types::mask_of<types::Alignment>();
    return slice;
}
/** --------------------------------------------------------------------------------------------------------- Null: Copy
 * @brief A copy of a view is the same view, and a copy of null is null.
 * @param from The Slice to copy.
 * @param to The Slice that becomes the copy.
 */
void SliceStatics::null_copy(const Slice& from, Slice& to) {
    to.address_ = from.address_;
    to.size_ = from.size_;
    to.num_elements_ = from.num_elements_;
    to.parent_ = from.parent_;
    to.type_ = from.type_;
}
/** --------------------------------------------------------------------------------------------------------- Null: Move
 * @brief Moving takes the fields and leaves the source null.
 * @param from The Slice to move.
 * @param to The Slice that takes its place.
 */
void SliceStatics::null_move(Slice&& from, Slice& to) {
    to.address_ = std::exchange(from.address_, uint64_t{0});
    to.size_ = std::exchange(from.size_, 0u);
    to.num_elements_ = std::exchange(from.num_elements_, 0u);
    to.parent_ = std::exchange(from.parent_, nullptr);
    to.type_ = std::exchange(from.type_, 0u);
    to.count_ = std::exchange(from.count_, 1);
}
/** --------------------------------------------------------------------------------------------------------- Null: Destroy
 * @brief A view owns nothing, so letting go of it only makes it null.
 * @param slice The Slice.
 */
void SliceStatics::null_destroy(Slice& slice) {
    slice.address_ = 0;
    slice.size_ = 0;
    slice.num_elements_ = 0;
    slice.parent_ = nullptr;
    slice.type_ = 0;
    slice.count_ = 1;
}
/** --------------------------------------------------------------------------------------------------------- Null: Host Pointer
 * @brief The address is the host pointer, and a null Slice's is null.
 * @param slice The Slice.
 * @return The host pointer.
 */
void* SliceStatics::null_host_pointer(Slice& slice) {
    return reinterpret_cast<void*>(slice.address_);
}
/** --------------------------------------------------------------------------------------------------------- AlignedHeapBuffer: Construct
 * @brief A buffer of the given bytes through its placement, and a handle on its root.
 * @param type The root's type word.
 * @param args The byte size.
 * @return The handle.
 */
Slice SliceStatics::aligned_buffer_construct(uint32_t type, ArgumentsHolder&& args) {
    const size_t bytes = *static_cast<size_t*>(args.args[0]);
    return aligned_buffer_sub_slice(type, BuffetDescriptors::descriptor<AlignedHeapBuffer>()->active_factory(bytes),
        0, bytes);
}
/** --------------------------------------------------------------------------------------------------------- AlignedHeapBuffer: Sub-slice
 * @brief A root over a range of the buffer, on the heap so its handles have a stable count to share.
 * @param type The root's type word.
 * @param parent The AlignedHeapBuffer.
 * @param offset The byte offset into it.
 * @param length The byte length.
 * @return A handle on the root.
 */
Slice SliceStatics::aligned_buffer_sub_slice(uint32_t type, void* parent, size_t offset, size_t length) {
    const size_t unit = Slice::type_emulator(type, 1)->alignment();
    Slice* root = new Slice;
    root->address_ = AlignedHeapBuffer::device_address(parent) + offset;
    root->size_ = static_cast<uint32_t>((length + unit - 1) >> std::countr_zero(unit));
    root->num_elements_ = static_cast<uint32_t>((length << 3) / Slice::type_emulator(type, 1)->bitwidth());
    root->parent_ = parent;
    root->type_ = type;
    root->count_ = 0;
    Slice slice;
    aligned_buffer_copy(*root, slice);
    return slice;
}
/** --------------------------------------------------------------------------------------------------------- AlignedHeapBuffer: Copy
 * @brief Copying a root makes a handle on it: one more count, the root's range, dispatching as a subslice.
 * @param from The root.
 * @param to The Slice that becomes the handle.
 */
void SliceStatics::aligned_buffer_copy(const Slice& from, Slice& to) {
    Slice* root = const_cast<Slice*>(&from);
    std::atomic_ref<int32_t>(root->count_).fetch_add(1, std::memory_order_acq_rel);
    to.address_ = root->address_;
    to.size_ = root->size_;
    to.num_elements_ = root->num_elements_;
    to.parent_ = root;
    to.type_ = (root->type_ & ((1u << types::constants::SEMANTIC_BITS) - 1u) & ~types::mask_of<types::Ownership>())
        | SUBSLICE_SEMANTICS.type;
}
/** --------------------------------------------------------------------------------------------------------- AlignedHeapBuffer: Move
 * @brief Moving takes the fields and leaves the source null; no count changes.
 * @param from The Slice to move.
 * @param to The Slice that takes its place.
 */
void SliceStatics::aligned_buffer_move(Slice&& from, Slice& to) {
    to.address_ = std::exchange(from.address_, uint64_t{0});
    to.size_ = std::exchange(from.size_, 0u);
    to.num_elements_ = std::exchange(from.num_elements_, 0u);
    to.parent_ = std::exchange(from.parent_, nullptr);
    to.type_ = std::exchange(from.type_, 0u);
    to.count_ = std::exchange(from.count_, 1);
}
/** --------------------------------------------------------------------------------------------------------- AlignedHeapBuffer: Destroy
 * @brief The last handle is gone: the buffer goes back through its placement, then the root itself goes.
 * @param slice The root.
 */
void SliceStatics::aligned_buffer_destroy(Slice& slice) {
    BuffetDescriptors::descriptor<AlignedHeapBuffer>()->active_deleter(slice.parent_);
    slice.type_ = 0;
    delete &slice;
}
/** --------------------------------------------------------------------------------------------------------- AlignedHeapBuffer: Host Pointer
 * @brief The heap's device address is its host address.
 * @param slice The root.
 * @return The host pointer.
 */
void* SliceStatics::aligned_buffer_host_pointer(Slice& slice) {
    return reinterpret_cast<void*>(slice.address_);
}
/** --------------------------------------------------------------------------------------------------------- VulkanBuffer: Construct
 * @brief A mapped buffer of the given bytes through its placement, and a handle on its root.
 * @param type The root's type word.
 * @param args The byte size.
 * @return The handle.
 */
Slice SliceStatics::vulkan_buffer_construct(uint32_t type, ArgumentsHolder&& args) {
    const size_t bytes = *static_cast<size_t*>(args.args[0]);
    return vulkan_buffer_sub_slice(type, BuffetDescriptors::descriptor<VulkanBuffer>()->active_factory(bytes),
        0, bytes);
}
/** --------------------------------------------------------------------------------------------------------- VulkanBuffer: Sub-slice
 * @brief A root over a range of the buffer, on the heap so its handles have a stable count to share.
 * @param type The root's type word.
 * @param parent The VulkanBuffer.
 * @param offset The byte offset into it.
 * @param length The byte length.
 * @return A handle on the root.
 */
Slice SliceStatics::vulkan_buffer_sub_slice(uint32_t type, void* parent, size_t offset, size_t length) {
    const size_t unit = Slice::type_emulator(type, 1)->alignment();
    Slice* root = new Slice;
    root->address_ = VulkanBuffer::device_address(parent) + offset;
    root->size_ = static_cast<uint32_t>((length + unit - 1) >> std::countr_zero(unit));
    root->num_elements_ = static_cast<uint32_t>((length << 3) / Slice::type_emulator(type, 1)->bitwidth());
    root->parent_ = parent;
    root->type_ = type;
    root->count_ = 0;
    Slice slice;
    vulkan_buffer_copy(*root, slice);
    return slice;
}
/** --------------------------------------------------------------------------------------------------------- VulkanBuffer: Copy
 * @brief Copying a root makes a handle on it: one more count, the root's range, dispatching as a subslice.
 * @param from The root.
 * @param to The Slice that becomes the handle.
 */
void SliceStatics::vulkan_buffer_copy(const Slice& from, Slice& to) {
    Slice* root = const_cast<Slice*>(&from);
    std::atomic_ref<int32_t>(root->count_).fetch_add(1, std::memory_order_acq_rel);
    to.address_ = root->address_;
    to.size_ = root->size_;
    to.num_elements_ = root->num_elements_;
    to.parent_ = root;
    to.type_ = (root->type_ & ((1u << types::constants::SEMANTIC_BITS) - 1u) & ~types::mask_of<types::Ownership>())
        | SUBSLICE_SEMANTICS.type;
}
/** --------------------------------------------------------------------------------------------------------- VulkanBuffer: Move
 * @brief Moving takes the fields and leaves the source null; no count changes.
 * @param from The Slice to move.
 * @param to The Slice that takes its place.
 */
void SliceStatics::vulkan_buffer_move(Slice&& from, Slice& to) {
    to.address_ = std::exchange(from.address_, uint64_t{0});
    to.size_ = std::exchange(from.size_, 0u);
    to.num_elements_ = std::exchange(from.num_elements_, 0u);
    to.parent_ = std::exchange(from.parent_, nullptr);
    to.type_ = std::exchange(from.type_, 0u);
    to.count_ = std::exchange(from.count_, 1);
}
/** --------------------------------------------------------------------------------------------------------- VulkanBuffer: Destroy
 * @brief The last handle is gone: the buffer goes back through its placement, then the root itself goes.
 * @param slice The root.
 */
void SliceStatics::vulkan_buffer_destroy(Slice& slice) {
    BuffetDescriptors::descriptor<VulkanBuffer>()->active_deleter(slice.parent_);
    slice.type_ = 0;
    delete &slice;
}
/** --------------------------------------------------------------------------------------------------------- VulkanBuffer: Host Pointer
 * @brief The mapping, at the root's offset into the buffer.
 * @param slice The root.
 * @return The host pointer.
 */
void* SliceStatics::vulkan_buffer_host_pointer(Slice& slice) {
    return static_cast<uint8_t*>(VulkanBuffer::host_ptr(slice.parent_))
        + (slice.address_ - VulkanBuffer::device_address(slice.parent_));
}
/** --------------------------------------------------------------------------------------------------------- Subslice: Construct
 * @brief A handle is only ever cut from a root, so there is nothing to construct.
 * @param type Unused.
 * @param args Unused.
 * @return A null Slice.
 */
Slice SliceStatics::subslice_construct([[maybe_unused]] uint32_t type, [[maybe_unused]] ArgumentsHolder&& args) {
    return Slice{};
}
/** --------------------------------------------------------------------------------------------------------- Subslice: Sub-slice
 * @brief A narrower handle on the same root: one more count, and the unit stays while the offset keeps it.
 * @param type The handle's type word.
 * @param parent The handle being cut.
 * @param offset The byte offset into it.
 * @param length The byte length.
 * @return The handle.
 */
Slice SliceStatics::subslice_sub_slice(uint32_t type, void* parent, size_t offset, size_t length) {
    const Slice& handle = *static_cast<const Slice*>(parent);
    Slice* root = static_cast<Slice*>(handle.parent_);
    std::atomic_ref<int32_t>(root->count_).fetch_add(1, std::memory_order_acq_rel);
    const size_t unit = handle.alignment();
    const bool keeps_unit = (offset & (unit - 1)) == 0;
    Slice slice;
    slice.address_ = handle.address_ + offset;
    slice.size_ = static_cast<uint32_t>(keeps_unit ? (length + unit - 1) >> std::countr_zero(unit) : length);
    slice.num_elements_ = static_cast<uint32_t>((length << 3) / Slice::type_emulator(type, 1)->bitwidth());
    slice.parent_ = root;
    slice.type_ = keeps_unit ? type : type & ~types::mask_of<types::Alignment>();
    return slice;
}
/** --------------------------------------------------------------------------------------------------------- Subslice: Copy
 * @brief A copy is the same handle with one more count on the root.
 * @param from The handle to copy.
 * @param to The Slice that becomes the copy.
 */
void SliceStatics::subslice_copy(const Slice& from, Slice& to) {
    std::atomic_ref<int32_t>(static_cast<Slice*>(from.parent_)->count_).fetch_add(1, std::memory_order_acq_rel);
    to.address_ = from.address_;
    to.size_ = from.size_;
    to.num_elements_ = from.num_elements_;
    to.parent_ = from.parent_;
    to.type_ = from.type_;
}
/** --------------------------------------------------------------------------------------------------------- Subslice: Move
 * @brief Moving takes the fields and leaves the source null; no count changes.
 * @param from The handle to move.
 * @param to The Slice that takes its place.
 */
void SliceStatics::subslice_move(Slice&& from, Slice& to) {
    to.address_ = std::exchange(from.address_, uint64_t{0});
    to.size_ = std::exchange(from.size_, 0u);
    to.num_elements_ = std::exchange(from.num_elements_, 0u);
    to.parent_ = std::exchange(from.parent_, nullptr);
    to.type_ = std::exchange(from.type_, 0u);
    to.count_ = std::exchange(from.count_, 1);
}
/** --------------------------------------------------------------------------------------------------------- Subslice: Destroy
 * @brief One count leaves the root, and the last one out runs the root's own table; the handle goes null.
 * @param slice The handle.
 */
void SliceStatics::subslice_destroy(Slice& slice) {
    Slice* root = static_cast<Slice*>(slice.parent_);
    slice.address_ = 0;
    slice.size_ = 0;
    slice.num_elements_ = 0;
    slice.parent_ = nullptr;
    slice.type_ = 0;
    slice.count_ = 1;
    if (std::atomic_ref<int32_t>(root->count_).fetch_sub(1, std::memory_order_acq_rel) == 1) {
        semantics(root->type_).destructor_fn(*root);
    }
}
/** --------------------------------------------------------------------------------------------------------- Subslice: Host Pointer
 * @brief The root's host pointer, at the handle's offset from the root.
 * @param slice The handle.
 * @return The host pointer.
 */
void* SliceStatics::subslice_host_pointer(Slice& slice) {
    Slice* root = static_cast<Slice*>(slice.parent_);
    return static_cast<uint8_t*>(semantics(root->type_).host_pointer_fn(*root)) + (slice.address_ - root->address_);
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffet: Construct
 * @brief A claim of the given bytes from the placement, novel or from its lanes, carrying the type word.
 * @param type The word the claim carries.
 * @param args The byte size, whether the claim is novel, and the placement.
 * @return The handle.
 */
Slice SliceStatics::chain_buffer_construct([[maybe_unused]] uint32_t type, ArgumentsHolder&& args) {
    return Alligator::claim(*static_cast<size_t*>(args.args[0]), *static_cast<bool*>(args.args[1]),
        *static_cast<const BuffetDescriptor**>(args.args[2]));
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffet: Sub-slice
 * @brief The Slice over the bytes the ChainBuffet cut, holding one count on it; the unit and the element width
 * come from the type word through the proxy.
 * @param type The type word for the Slice.
 * @param parent The ChainBuffet.
 * @param offset The byte offset of the cut into the buffer.
 * @param length The bytes cut.
 * @return The Slice.
 */
Slice SliceStatics::chain_buffer_sub_slice(uint32_t type, void* parent, size_t offset, size_t length) {
    ChainBuffet* link = static_cast<ChainBuffet*>(parent);
    const Slice* proxy = Slice::type_emulator(type, 1);
    const size_t unit = proxy->alignment();
    std::atomic_ref<uint32_t>(link->count_).fetch_add(1, std::memory_order_acq_rel);
    Slice slice;
    slice.address_ = link->device_ + offset;
    /// The cut pads to whole units; the elements are the bytes asked for.
    slice.size_ = static_cast<uint32_t>((length + unit - 1) >> std::countr_zero(unit));
    slice.num_elements_ = static_cast<uint32_t>((length << 3) / proxy->bitwidth());
    slice.parent_ = link;
    slice.type_ = type;
    return slice;
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffet: Copy
 * @brief A copy is the same Slice with one more count on the ChainBuffet.
 * @param from The Slice to copy.
 * @param to The Slice that becomes the copy.
 */
void SliceStatics::chain_buffer_copy(const Slice& from, Slice& to) {
    std::atomic_ref<uint32_t>(static_cast<ChainBuffet*>(from.parent_)->count_)
        .fetch_add(1, std::memory_order_acq_rel);
    to.address_ = from.address_;
    to.size_ = from.size_;
    to.num_elements_ = from.num_elements_;
    to.parent_ = from.parent_;
    to.type_ = from.type_;
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffet: Move
 * @brief Moving takes the fields and leaves the source null; no count changes.
 * @param from The Slice to move.
 * @param to The Slice that takes its place.
 */
void SliceStatics::chain_buffer_move(Slice&& from, Slice& to) {
    to.address_ = std::exchange(from.address_, uint64_t{0});
    to.size_ = std::exchange(from.size_, 0u);
    to.num_elements_ = std::exchange(from.num_elements_, 0u);
    to.parent_ = std::exchange(from.parent_, nullptr);
    to.type_ = std::exchange(from.type_, 0u);
    to.count_ = std::exchange(from.count_, 1);
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffet: Destroy
 * @brief The Slice goes null and its count leaves the ChainBuffet, which frees the buffer on the last one.
 * @param slice The Slice.
 */
void SliceStatics::chain_buffer_destroy(Slice& slice) {
    ChainBuffet* link = static_cast<ChainBuffet*>(slice.parent_);
    slice.address_ = 0;
    slice.size_ = 0;
    slice.num_elements_ = 0;
    slice.parent_ = nullptr;
    slice.type_ = 0;
    slice.count_ = 1;
    ChainBuffet::decrement_refcount(link);
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffet: Host Pointer
 * @brief The buffer's host address, at the Slice's offset from the buffer's device address.
 * @param slice The Slice.
 * @return The host pointer.
 */
void* SliceStatics::chain_buffer_host_pointer(Slice& slice) {
    ChainBuffet* link = static_cast<ChainBuffet*>(slice.parent_);
    return static_cast<uint8_t*>(link->host_ptr_) + (slice.address_ - link->device_);
}
/** --------------------------------------------------------------------------------------------------------- Registered: Construct
 * @brief The type's object from its constructor hook, and a handle on a root over all of it.
 * @param type The root's type word, the table's.
 * @param args The object's constructor arguments.
 * @return The handle.
 */
Slice SliceStatics::registered_construct(uint32_t type, ArgumentsHolder&& args) {
    const types::SliceSemantics& table = semantics(type);
    void* object = table.parent_construct(std::move(args));
    return registered_sub_slice(type, object, 0, table.parent_size(object));
}
/** --------------------------------------------------------------------------------------------------------- Registered: Sub-slice
 * @brief A root over a range of the object, at the host address its hook gives, on the heap so its handles
 * have a stable count to share.
 * @param type The root's type word, the table's.
 * @param parent The object.
 * @param offset The byte offset into it.
 * @param length The byte length.
 * @return A handle on the root.
 */
Slice SliceStatics::registered_sub_slice(uint32_t type, void* parent, size_t offset, size_t length) {
    const size_t unit = Slice::type_emulator(type, 1)->alignment();
    Slice* root = new Slice;
    root->address_ = reinterpret_cast<uint64_t>(semantics(type).parent_host_pointer(parent)) + offset;
    root->size_ = static_cast<uint32_t>((length + unit - 1) >> std::countr_zero(unit));
    root->num_elements_ = static_cast<uint32_t>((length << 3) / Slice::type_emulator(type, 1)->bitwidth());
    root->parent_ = parent;
    root->type_ = type;
    root->count_ = 0;
    Slice slice;
    registered_copy(*root, slice);
    return slice;
}
/** --------------------------------------------------------------------------------------------------------- Registered: Copy
 * @brief Copying a root makes a handle on it: one more count, the root's range, dispatching as a subslice.
 * @param from The root.
 * @param to The Slice that becomes the handle.
 */
void SliceStatics::registered_copy(const Slice& from, Slice& to) {
    Slice* root = const_cast<Slice*>(&from);
    std::atomic_ref<int32_t>(root->count_).fetch_add(1, std::memory_order_acq_rel);
    to.address_ = root->address_;
    to.size_ = root->size_;
    to.num_elements_ = root->num_elements_;
    to.parent_ = root;
    to.type_ = (root->type_ & ((1u << types::constants::SEMANTIC_BITS) - 1u) & ~types::mask_of<types::Ownership>())
        | SUBSLICE_SEMANTICS.type;
}
/** --------------------------------------------------------------------------------------------------------- Registered: Move
 * @brief Moving takes the fields and leaves the source null; no count changes.
 * @param from The Slice to move.
 * @param to The Slice that takes its place.
 */
void SliceStatics::registered_move(Slice&& from, Slice& to) {
    to.address_ = std::exchange(from.address_, uint64_t{0});
    to.size_ = std::exchange(from.size_, 0u);
    to.num_elements_ = std::exchange(from.num_elements_, 0u);
    to.parent_ = std::exchange(from.parent_, nullptr);
    to.type_ = std::exchange(from.type_, 0u);
    to.count_ = std::exchange(from.count_, 1);
}
/** --------------------------------------------------------------------------------------------------------- Registered: Destroy
 * @brief The last handle is gone: the object goes through the type's destroy hook, then the root itself goes.
 * @param slice The root.
 */
void SliceStatics::registered_destroy(Slice& slice) {
    semantics(slice.type_).parent_destroy(slice.parent_);
    slice.type_ = 0;
    delete &slice;
}
/** --------------------------------------------------------------------------------------------------------- Registered: Host Pointer
 * @brief The address the type's host pointer hook gave the root.
 * @param slice The root.
 * @return The host pointer.
 */
void* SliceStatics::registered_host_pointer(Slice& slice) {
    return reinterpret_cast<void*>(slice.address_);
}
/** --------------------------------------------------------------------------------------------------------- Payload: Destroy
 * @brief A typed claim has its buffer to itself, so the ChainBuffet's last count is the element's last handle:
 * the element's destructor runs before the buffer goes.
 * @param slice The Slice.
 */
void SliceStatics::payload_destroy(Slice& slice) {
    ChainBuffet* link = static_cast<ChainBuffet*>(slice.parent_);
    const uint32_t type = slice.type_;
    void* payload = chain_buffer_host_pointer(slice);
    slice.address_ = 0;
    slice.size_ = 0;
    slice.num_elements_ = 0;
    slice.parent_ = nullptr;
    slice.type_ = 0;
    slice.count_ = 1;
    if (std::atomic_ref<uint32_t>(link->count_).fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    semantics(type).parent_destroy(payload);
    link->desc_->active_deleter(link->handle_);
    delete link;
}
/** --------------------------------------------------------------------------------------------------------- Type Emulator
 * @brief Provides a Slice that emulates the given type, so that the type-related accessors can function
 * correctly and friend classes can look up information for a specific type.
 * @param type The type index to emulate.
 * @return A pointer to a Slice representing the type.
 */
const Slice* Slice::type_emulator(uint32_t type, uint32_t num_elements) {
    static thread_local Slice slice;
    /// The accessors never read the semantic bits, and without them the emulator has no table to run at exit.
    slice.type_ = type & ((1u << types::constants::SEMANTIC_BITS) - 1u);
    slice.num_elements_ = num_elements;
    return &slice;
}
} // namespace buffetalligator