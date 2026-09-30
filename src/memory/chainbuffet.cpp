/** --------------------------------------------------------------------------------------------------------- ChainBuffet
 * @file chainbuffet.hpp
 * @brief Defines the ChainBuffet class for managing chained memory buffers.
 */
#include <alligator.hpp>
#include <alligator/easyvulkan.hpp>
#include <memory/tracker.hpp>
#include <atomic>
#include <tuple>
#include <cstdint>
#include <array>

namespace buffetalligator {
namespace {
inline static AlignedHeapBuffer* dummy_aligned_heap_buffer = nullptr;
inline static VulkanBuffer* dummy_vulkan_buffer = nullptr;
} // anonymous namespace
/** --------------------------------------------------------------------------------------------------------- BuffetDescriptors static members
 * @brief Gets the next available index for registering a new buffet descriptor.
 * @return Reference to the atomic variable holding the next available index.
 */
std::atomic<size_t>& BuffetDescriptors::next_index() {
    static std::atomic<size_t> index{2}; return index;
}
/** --------------------------------------------------------------------------------------------------------- BuffetDescriptors list
 * @brief Gets the list of registered buffet descriptors.
 * @return Reference to the array of buffet descriptors.
 */
std::array<const BuffetDescriptor*, 8>& BuffetDescriptors::list() {
    static std::array<const BuffetDescriptor*, 8> descriptors = std::array<const BuffetDescriptor*, 8>{
        BuffetDescriptors::descriptor_for(dummy_aligned_heap_buffer),
        BuffetDescriptors::descriptor_for(dummy_vulkan_buffer),
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
    };
    return descriptors;
}
/** --------------------------------------------------------------------------------------------------------- BuffetDescriptors get
 * @brief Retrieves the buffet descriptor for the specified type.
 * @param type The type index of the buffet descriptor.
 * @return Pointer to the corresponding buffet descriptor.
 */
const BuffetDescriptor* BuffetDescriptors::get(size_t type) {
    return BuffetDescriptors::list()[type & 7];
}
/** --------------------------------------------------------------------------------------------------------- BuffetDescriptors register_descriptor
 * @brief Registers a descriptor at the fixed type index it reports, the index Slice ids carry.
 * @param descriptor Pointer to the buffet descriptor to register.
 * @return The descriptor's type index.
 */
size_t BuffetDescriptors::register_descriptor(const BuffetDescriptor* descriptor) {
    const size_t index = descriptor->type_idx;
    if (index >= BuffetDescriptors::list().size()) {
        ALLIGATOR_THROW(std::string("Buffet type ") + descriptor->type_name + " reports type index "
            + std::to_string(index) + "; Slice ids carry 3 type bits, so indices stop at 7");
    }
    if (BuffetDescriptors::list()[index] != nullptr && BuffetDescriptors::list()[index] != descriptor) {
        ALLIGATOR_THROW(std::string("Buffet type index ") + std::to_string(index) + " already belongs to "
            + BuffetDescriptors::list()[index]->type_name + "; " + descriptor->type_name + " cannot share it");
    }
    BuffetDescriptors::list()[index] = descriptor;
    size_t count = BuffetDescriptors::next_index().load(std::memory_order_acquire);
    while (count <= index && !BuffetDescriptors::next_index().compare_exchange_weak(
        count, index + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {}
    return index;
}
/** --------------------------------------------------------------------------------------------------------- BuffetDescriptors default_placement
 * @brief The placement plain Slices claim from: VulkanBuffer when a unified-memory device is
 * present, so ordinary Slices are GPU-visible for free, AlignedHeapBuffer otherwise.
 * @return The default descriptor slot.
 */
const BuffetDescriptor*& BuffetDescriptors::default_placement() {
    static const BuffetDescriptor* placement =
        VulkanContext::device_present() && VulkanContext::device_unified()
            ? BuffetDescriptors::list()[VulkanBuffer::type_idx()]
            : BuffetDescriptors::list()[AlignedHeapBuffer::type_idx()];
    return placement;
}
/** --------------------------------------------------------------------------------------------------------- BuffetDescriptors count
 * @brief Returns the number of registered buffet descriptors.
 * @return The count of registered buffet descriptors.
 */
size_t BuffetDescriptors::count() { return BuffetDescriptors::next_index().load(std::memory_order_acquire); }
/** --------------------------------------------------------------------------------------------------------- BuffetDescriptors record_allocation
 * @brief Forwards one completed buffet allocation to the memory tracker.
 * @param type The buffet type index.
 * @param bytes The allocation size in bytes.
 * @param buffet The new buffet handle.
 */
void BuffetDescriptors::record_allocation(size_t type, size_t bytes, const void* buffet) {
    const BuffetDescriptor& placement = *BuffetDescriptors::list()[type];
    Memory::record_allocation(placement, bytes);
    Memory::record_code_location(placement, bytes, buffet);
}
/** --------------------------------------------------------------------------------------------------------- BuffetDescriptors record_deallocation
 * @brief Forwards one buffet release to the memory tracker.
 * @param type The buffet type index.
 * @param bytes The allocation size in bytes.
 * @param buffet The buffet handle being released.
 */
void BuffetDescriptors::record_deallocation(size_t type, size_t bytes, const void* buffet) {
    Memory::record_deallocation(*BuffetDescriptors::list()[type], bytes);
    Memory::forget_code_location(buffet);
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffetToken addref
 * @brief Increments the reference count of the ChainBuffetToken.
 */
void ChainBuffet::ChainBuffetToken::add_ref() {
    if (token_) std::get<1>(*token_).fetch_add(1, std::memory_order_acq_rel);
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffetToken free
 * @brief Decrements the reference count of the ChainBuffetToken and deletes it if it reaches zero.
 */
void ChainBuffet::ChainBuffetToken::free() {
    if (token_ ) {
        if (std::get<1>(*token_).fetch_sub(1, std::memory_order_acq_rel) == 1) {
            ChainBuffet* buffer = const_cast<ChainBuffet*>(std::get<0>(*token_));
            if (buffer && buffer->descriptor) {
                buffer->descriptor->deleter(buffer->buffer_);
                buffer->buffer_ = nullptr;
                buffer->descriptor = nullptr;
                delete token_;
            }
        }
        token_ = nullptr;
    }
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffetToken Constructor
 * @brief Constructs a ChainBuffetToken for the specified ChainBuffet.
 * @param buffer Pointer to the ChainBuffet.
 */
ChainBuffet::ChainBuffetToken::ChainBuffetToken(const ChainBuffet* buffer)
: token_(buffer ? new std::tuple<const ChainBuffet*, std::atomic<int32_t>,
    const BuffetDescriptor*>(buffer, 1, buffer->descriptor) : nullptr) {}
/** --------------------------------------------------------------------------------------------------------- ChainBuffetToken Copy Constructor
 * @brief Constructs a ChainBuffetToken by copying another token.
 * @param other The other ChainBuffetToken to copy.
 */
ChainBuffet::ChainBuffetToken::ChainBuffetToken(const ChainBuffetToken& other)
: token_(other.token_) { add_ref(); }
/** --------------------------------------------------------------------------------------------------------- ChainBuffetToken Copy Assignment Operator
 * @brief Assigns another ChainBuffetToken to this token.
 * @param other The other ChainBuffetToken to assign.
 */
ChainBuffet::ChainBuffetToken& ChainBuffet::ChainBuffetToken::operator=(const ChainBuffetToken& other) {
    if (this != &other) { free(); token_ = other.token_; add_ref(); } return *this;
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffetToken Move Constructor
 * @brief Constructs a ChainBuffetToken by moving another token.
 * @param other The other ChainBuffetToken to move.
 */
ChainBuffet::ChainBuffetToken::ChainBuffetToken(ChainBuffetToken&& other) noexcept
: token_(other.token_) { other.token_ = nullptr; }
/** --------------------------------------------------------------------------------------------------------- ChainBuffetToken Move Assignment Operator
 * @brief Assigns another ChainBuffetToken to this token by moving it.
 * @param other The other ChainBuffetToken to move.
 */
ChainBuffet::ChainBuffetToken& ChainBuffet::ChainBuffetToken::operator=(ChainBuffetToken&& other) noexcept {
    if (this != &other) { free(); token_ = other.token_; other.token_ = nullptr; }
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffetToken Destructor
 * @brief Destroys the ChainBuffetToken and releases its reference.
 */
ChainBuffet::ChainBuffetToken::~ChainBuffetToken() { free(); }
/** --------------------------------------------------------------------------------------------------------- ChainBuffetToken Buffet
 * @brief The buffet handle this token keeps alive, as the descriptor's hooks expect it.
 * @return The buffet handle.
 */
void* ChainBuffet::ChainBuffetToken::buffet() const {
    return std::get<0>(*token_)->buffer_;
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffetToken Raw Pointer
 * @brief Retrieves the raw pointer to the buffer at the specified offset.
 * @param offset Offset within the buffer.
 * @return Pointer to the buffer at the specified offset.
 */
void* ChainBuffet::ChainBuffetToken::raw(size_t offset) {
    if (!token_) return nullptr;
    const ChainBuffet* c = std::get<0>(*token_);
    const BuffetDescriptor* b = c->descriptor;
    return b->host_ptr(c->buffer_, offset);
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffetToken Raw Pointer (Const)
 * @brief Retrieves the raw pointer to the buffer at the specified offset (const version).
 * @param offset Offset within the buffer.
 * @return Pointer to the buffer at the specified offset.
 */
const void* ChainBuffet::ChainBuffetToken::raw(size_t offset) const {
    if (!token_) return nullptr;
    const ChainBuffet* c = std::get<0>(*token_);
    const BuffetDescriptor* b = c->descriptor;
    return b->host_ptr(c->buffer_, offset);
}
/** ------------------------------------------------------------------------------------------- Next ChainBuffet
 * @brief Retrieves the next ChainBuffet in the chain.
 * @param allocate_next If true, ensures that the next ChainBuffet is allocated.
 * @return Pointer to the next ChainBuffet.
 */
ChainBuffet* ChainBuffet::next(bool allocate_next) {
    ChainBuffet* n = next_.load(std::memory_order_acquire);
    while (n == nullptr || n == reinterpret_cast<ChainBuffet*>(-1)) {
        ChainBuffet* expected = nullptr;
        if (next_.compare_exchange_strong(expected, reinterpret_cast<ChainBuffet*>(-1))) {
            n = new ChainBuffet(descriptor, descriptor->factory(descriptor->size_of(buffer_)));
            next_.store(n, std::memory_order_release);
            if (allocate_next) { (void)n->next(false); allocate_next = false; }
            return n;
        }
        std::this_thread::yield();
        n = next_.load(std::memory_order_acquire);
    }
    if (allocate_next) (void)n->next(false);
    return n;
}
/** ------------------------------------------------------------------------------------------- Current ChainBuffet for Index
 * @brief Retrieves the current ChainBuffet instance for the specified index.
 * @param idx Index of the BuffetDescriptor.
 * @return Reference to the atomic pointer holding the current ChainBuffet.
 */
std::atomic<ChainBuffet*>& ChainBuffet::current_for(size_t idx) {
    static std::array<std::atomic<ChainBuffet*>, 8> instances = {
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
    };
    if (idx >= BuffetDescriptors::count()) idx = 0;
    while (instances[idx] == nullptr || instances[idx] == reinterpret_cast<ChainBuffet*>(-1)) {
        ChainBuffet* expected = nullptr;
        if (instances[idx].compare_exchange_strong(expected, reinterpret_cast<ChainBuffet*>(-1))) {
            const BuffetDescriptor* selected = BuffetDescriptors::get(idx);
            ChainBuffet* n = new ChainBuffet(selected, selected->factory(selected->default_size));
            instances[idx].store(n, std::memory_order_release);
            break;
        }
        std::this_thread::yield();
    }
    return instances[idx];
}
/** ------------------------------------------------------------------------------------------- ChainBuffet Chain Function
 * @brief Retrieves the current ChainBuffet instance for the specified BuffetDescriptor.
 * @param desc Pointer to the BuffetDescriptor.
 * @return Pointer to the current ChainBuffet instance.
 */
ChainBuffet* ChainBuffet::chain(const BuffetDescriptor* desc) {
    size_t idx = desc->type_idx;
    return ChainBuffet::current_for(idx).load(std::memory_order_acquire);
}
/** ------------------------------------------------------------------------------------------- ChainBuffet Constructor
 * @brief Constructs a ChainBuffet with the specified descriptor and buffer (internal use).
 * @param desc Pointer to the BuffetDescriptor.
 * @param buffer Pointer to the buffer.
 */
ChainBuffet::ChainBuffet(const BuffetDescriptor* desc, void* buffer)
: buffer_(buffer), descriptor(desc), token_(new ChainBuffetToken(this)) {}
/** ------------------------------------------------------------------------------------------- Free Buffer
 * @brief Frees the allocated buffer and resets the descriptor.
 */
void ChainBuffet::free() {
    if (token_) {
        ChainBuffetToken* other = token_;
        token_ = nullptr;
        delete other;
    }
}
/** ------------------------------------------------------------------------------------------- Claim Buffer
 * @brief Claims a span of the buffer of the specified size.
 * @param size Size of the buffer to claim.
 * @return Span representing the claimed buffer.
 */
SliceEntry* ChainBuffet::claim(size_t size) {
    ChainBuffetToken* t = token_.load(std::memory_order_acquire);
    if (!t) return next()->claim(size);
    if (descriptor->size_of(buffer_) <= ((size + 63) & ~63)) {
        ChainBuffet* n = new ChainBuffet(descriptor, descriptor->factory((size + 63) & ~63));
        t = n->token_.exchange(nullptr, std::memory_order_acq_rel);
        uint32_t slice_id = Alligator::inst().next_id(descriptor);
        uint8_t region = (slice_id >> 3) & 0x3F;
        size_t idx = slice_id >> 9;
        Region* r = Alligator::inst().regions[region].load(std::memory_order_acquire);
        r->slice_table()[idx].set(t, region);
        if (descriptor->device_address != nullptr) {
            r->gpu_slots[idx].set(
                descriptor->device_address(buffer_),
                (size + 63) >> 6,
                0
            );
        } else {
            r->gpu_slots[idx].set(
                reinterpret_cast<uint64_t>(t->raw(0)),
                (size + 63) >> 6,
                0
            );
        }
        delete t;  // The entry took its own reference; drop the one exchanged out of the dedicated buffet.
    }
    size_t start_pos = bump_ptr_.fetch_add((size + 63) & ~63, std::memory_order_acquire);
    if (start_pos + ((size + 63) & ~63) > descriptor->size_of(buffer_)) {
        ChainBuffet* n = this;
        if (current_for(n->descriptor->type_idx).compare_exchange_strong(n, next())) {
            t = token_.exchange(nullptr, std::memory_order_acq_rel);
            delete t;
        };
        return next()->claim(size);
    }
    uint32_t slice_id = Alligator::inst().next_id(descriptor);
    uint8_t region = (slice_id >> 3) & 0x3F;
    size_t idx = slice_id >> 9;
    Region* r = Alligator::inst().regions[region].load(std::memory_order_acquire);
    t = token_.load(std::memory_order_acquire);
    r->slots[idx].set(t, region);
    if (descriptor->device_address != nullptr) {
        r->gpu_slots[idx].set(
            descriptor->device_address(buffer_),
            (size + 63) >> 6,
            start_pos >> 6
        );
    } else {
        r->gpu_slots[idx].set(
            reinterpret_cast<uint64_t>(t->raw(0)),
            (size + 63) >> 6,
            start_pos >> 6
        );
    }
    return &r->slots[idx];
}
/** ------------------------------------------------------------------------------------------- Slice Entry Token
 * @brief Retrieves the token associated with the slice entry.
 * @return The `ChainBuffetToken` associated with the slice entry.
 */
ChainBuffet::ChainBuffetToken* SliceEntry::token() {
    return reinterpret_cast<ChainBuffet::ChainBuffetToken*>(&data[0]);
}
/** ------------------------------------------------------------------------------------------- Slice Entry Token (const)
 * @brief Retrieves the token associated with the slice entry.
 * @return The `ChainBuffetToken` associated with the slice entry.
 */
const ChainBuffet::ChainBuffetToken* SliceEntry::token() const {
    return reinterpret_cast<const ChainBuffet::ChainBuffetToken*>(&data[0]);
}
/** ------------------------------------------------------------------------------------------- Slice Entry Set Token
 * @brief Sets the token associated with the slice entry.
 * @param token The `ChainBuffetToken` to associate with the slice entry.
 */
void SliceEntry::set_token(const ChainBuffet::ChainBuffetToken* token) {
    std::memcpy(data, token, sizeof(ChainBuffet::ChainBuffetToken));
    // The entry holds its own reference, which clear() releases.
    this->token()->add_ref();
}
/** ------------------------------------------------------------------------------------------- Slice Entry Region
 * @brief Retrieves the region associated with the slice entry.
 * @return The region of the slice entry.
 */
uint8_t SliceEntry::region() const {
    return *reinterpret_cast<const uint8_t*>(&data[8]);
}
/** ------------------------------------------------------------------------------------------- Slice Entry Set Region
 * @brief Sets the region associated with the slice entry.
 * @param region The region to associate with the slice entry.
 */
void SliceEntry::set_region(uint8_t region) {
    *reinterpret_cast<uint8_t*>(&data[8]) = region;
}
/** ------------------------------------------------------------------------------------------- Slice Entry Set
 * @brief Sets the token and region associated with the slice entry.
 * @param token The `ChainBuffetToken` to associate with the slice entry.
 * @param region The region to associate with the slice entry.
 */
void SliceEntry::set(const ChainBuffet::ChainBuffetToken* token, uint8_t region) {
    set_token(token);
    set_region(region);
}
/** ------------------------------------------------------------------------------------------- Slice Entry Clear
 * @brief Clears the slice entry, freeing its token and resetting its data.
 */
void SliceEntry::clear() {
    ChainBuffet::ChainBuffetToken* t = token();
    t->free();
    std::memset(data, 0, sizeof(data));
}
/** ------------------------------------------------------------------------------------------- Slice Entry Size
 * @brief Retrieves the size of the slice entry.
 * @return The size of the slice entry.
 */
size_t SliceEntry::size() const {
    const size_t idx = this - &Alligator::inst().regions[region()].load(std::memory_order_acquire)->slice_table()[0];
    return Alligator::inst().regions[region()].load(std::memory_order_acquire)->gpu_slots[idx].size;
}
/** ------------------------------------------------------------------------------------------- Slice Entry Offset
 * @brief Retrieves the offset of the slice entry.
 * @return The offset of the slice entry.
 */
size_t SliceEntry::offset() const {
    const size_t idx = this - &Alligator::inst().regions[region()].load(std::memory_order_acquire)->slice_table()[0];
    return Alligator::inst().regions[region()].load(std::memory_order_acquire)->gpu_slots[idx].offset;
}
/** ------------------------------------------------------------------------------------------- Slice Entry Host Pointer
 * @brief Retrieves the host pointer associated with the slice entry.
 * @return The host pointer of the slice entry.
 */
void* SliceEntry::host_ptr() {
    return token()->raw(offset());
}
/** ------------------------------------------------------------------------------------------- Slice Entry Host Pointer (Const)
 * @brief Retrieves the host pointer associated with the slice entry (const version).
 * @return The host pointer of the slice entry.
 */
const void* SliceEntry::host_ptr() const {
    return token()->raw(offset());
}
/** ------------------------------------------------------------------------------------------- Slice Entry GPU Buffer
 * @brief Retrieves the GPU buffer associated with the slice entry.
 * @return The GPU buffer of the slice entry.
 */
GPUBuf* SliceEntry::gpu_buf() {
    Region* r = Alligator::inst().regions[region()].load(std::memory_order_acquire);
    const size_t idx = this - r->slice_table();
    return r->gpu_table() + idx;
}
/** ------------------------------------------------------------------------------------------- Slice Entry GPU Buffer (Const)
 * @brief Retrieves the GPU buffer associated with the slice entry (const version).
 * @return The GPU buffer of the slice entry.
 */
const GPUBuf* SliceEntry::gpu_buf() const {
    Region* r = Alligator::inst().regions[region()].load(std::memory_order_acquire);
    const size_t idx = this - r->slice_table();
    return r->gpu_table() + idx;
}
/** ------------------------------------------------------------------------------------------- Slice Entry From Slice
 * @brief Retrieves the slice entry corresponding to the given slice.
 * @param slice The slice to retrieve the entry for.
 * @return A pointer to the corresponding slice entry.
 */
SliceEntry* SliceEntry::from_slice(const Slice& slice) {
    const uint8_t region_id = (slice.id_ >> 3) & 0x3F;
    Region* r = Alligator::inst().regions[region_id].load(std::memory_order_acquire);
    const size_t idx = (slice.id_ >> 9) & ((POOL_SIZE >> 6) - 1);
    return &r->slice_table()[idx];
}
/** ------------------------------------------------------------------------------------------- Slots Available
 * @brief Returns the number of available slots in the entry region.
 */
size_t Region::slots_available() const {
    return (POOL_SIZE >> 6) - (claimed.load(std::memory_order_acquire) - freed.load(std::memory_order_acquire));
}
/** ------------------------------------------------------------------------------------------- Claim Slice Entry
 * @brief Claims a slice entry from the entry region.
 * @param token The chain buffet token associated with the claim.
 * @param size The size of the slice entry to claim.
 * @param offset The offset of the slice entry to claim.
 * @return A pointer to the claimed slice entry, or nullptr if no slots are available.
 */
SliceEntry* Region::claim(size_t skip_at_most) {
    const size_t available = slots_available();
    if (available == 0) return nullptr;
    uint64_t idx = last_idx.fetch_add(1, std::memory_order_relaxed) & (REGION_SIZE - 1);
    size_t loops = 0;
    while (true) {
        const uint64_t bit = 1ULL << (idx & 63);
        uint64_t expected = occupancy[idx >> 6].load(std::memory_order_acquire);
        // Retry only while the slot is free; a failed CAS from a neighbouring bit is not a collision.
        while ((expected & bit) == 0) {
            if (occupancy[idx >> 6].compare_exchange_weak(expected, expected | bit,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                claimed.fetch_add(1, std::memory_order_acq_rel);
                return &slots[idx];
            }
        }
        // Give up after skip_at_most collisions once the region is half full, or after one full lap.
        if (++loops > skip_at_most && (available < (REGION_SIZE >> 1) || loops >= REGION_SIZE)) {
            return nullptr;
        }
        idx = last_idx.fetch_add(1, std::memory_order_relaxed) & (REGION_SIZE - 1);
    }
}
/** ------------------------------------------------------------------------------------------- Release Slice Entry
 * @brief Releases a previously claimed slice entry.
 * @param entry Pointer to the slice entry to release.
 * @return True if the release was successful, false otherwise.
 */
bool Region::release(SliceEntry* entry) {
    if (entry == nullptr || entry < &slots[0] || entry >= &slots[0] + slots.size()) {
        return false;
    }
    const size_t idx = entry - &slots[0];
    const uint64_t bit = 1ULL << (idx & 63);
    // fetch_and cannot lose to a neighbouring bit changing the word, which a single CAS could.
    if (occupancy[idx >> 6].fetch_and(~bit, std::memory_order_acq_rel) & bit) {
        freed.fetch_add(1, std::memory_order_release);
        return true;
    }
    return false;
}
} // namespace buffetalligator