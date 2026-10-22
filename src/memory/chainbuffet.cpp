/** --------------------------------------------------------------------------------------------------------- ChainBuffet
 * @file chainbuffet.cpp
 * @brief The placement descriptors, the chain buffets the Alligator cuts slots from, and the slot regions.
 */
#include <alligator.hpp>
#include <alligator/easyvulkan.hpp>
#include <alligator/kitchen.hpp>
#include <memory/tracker.hpp>
#include <atomic>
#include <cstdint>
#include <array>

namespace buffetalligator {
namespace {
inline static AlignedHeapBuffer* dummy_aligned_heap_buffer = nullptr;
inline static VulkanBuffer* dummy_vulkan_buffer = nullptr;
} // anonymous namespace
/** --------------------------------------------------------------------------------------------------------- Retire Buffet
 * @brief The Kitchen order that deletes a ChainBuffet whose reference count reached zero, its buffer already
 * deallocated by the release that scheduled it.
 * @param buffet The ChainBuffet.
 */
void Alligator::retire_buffet(void* buffet) {
    delete static_cast<ChainBuffet*>(buffet);
}
/** --------------------------------------------------------------------------------------------------------- Hold Current
 * @brief The placement's current ChainBuffet with one count taken for the caller, under the pending sentinel
 * on `current_for`, so no advance can retire it between the read and the count.
 * @param placement The placement.
 * @return The held ChainBuffet.
 */
ChainBuffet* Alligator::hold_current(const BuffetDescriptor* placement) {
    std::atomic<ChainBuffet*>& current = ChainBuffet::current_for(placement->type_idx);
    ChainBuffet* const pending = reinterpret_cast<ChainBuffet*>(-1);
    for (;;) {
        ChainBuffet* buffet = current.load(std::memory_order_acquire);
        if (buffet == pending) {
            current.wait(pending, std::memory_order_acquire);
            continue;
        }
        if (!current.compare_exchange_weak(buffet, pending, std::memory_order_acq_rel, std::memory_order_acquire)) {
            continue;
        }
        ChainBuffet::fetch_add_ref_count(buffet);
        current.store(buffet, std::memory_order_release);
        current.notify_all();
        return buffet;
    }
}
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
/** --------------------------------------------------------------------------------------------------------- ChainBuffet Constructor
 * @brief Makes the buffer, takes the ChainBuffet's own slot in the atomics table with the ChainBuffet
 * itself as that slot's host pointer, starts the reference count at one, and keeps one buffer ahead
 * unless told not to.
 * @param desc Pointer to the BuffetDescriptor.
 * @param size Size of the buffer to allocate.
 * @param allocate_next Whether the next ChainBuffet is made now; a novel buffer never has one.
 */
ChainBuffet::ChainBuffet(const BuffetDescriptor* desc, size_t size, bool allocate_next)
: buffer_(desc->factory(size)), descriptor(desc) {
    Alligator& arena = Alligator::inst();
    atomic_id_ = arena.next_id(desc);
    Region& region = *arena.regions[(atomic_id_ >> 3) & 0x3f].load(std::memory_order_acquire);
    const uint32_t slot = atomic_id_ >> 9;
    region.host_ptrs[slot] = this;
    region.parents[slot] = UINT32_MAX;
    std::atomic_ref<uint32_t>(region.slots[slot].state).store(
        static_cast<uint32_t>(SliceTypes::CHAIN_BUFFET) << SliceEntry::TYPE_SHIFT, std::memory_order_release);
    fetch_add_ref_count(this);
    if (allocate_next) (void)next(false);
}
/** --------------------------------------------------------------------------------------------------------- Fetch Add Ref Count
 * @brief Adds one to the ChainBuffet's reference count in the atomics table.
 * @param buffet The ChainBuffet.
 * @return The count before the add.
 */
int32_t ChainBuffet::fetch_add_ref_count(const ChainBuffet* buffet) {
    Region& region = *Alligator::inst().regions[(buffet->atomic_id_ >> 3) & 0x3f].load(std::memory_order_acquire);
    return static_cast<int32_t>(std::atomic_ref<uint32_t>(region.slots[buffet->atomic_id_ >> 9].state)
        .fetch_add(1u, std::memory_order_acq_rel) & SliceEntry::OWNERS_MASK);
}
/** --------------------------------------------------------------------------------------------------------- Fetch Sub Ref Count
 * @brief Takes one from the ChainBuffet's reference count in the atomics table.
 * @param buffet The ChainBuffet.
 * @return The count before the subtract.
 */
int32_t ChainBuffet::fetch_sub_ref_count(const ChainBuffet* buffet) {
    Region& region = *Alligator::inst().regions[(buffet->atomic_id_ >> 3) & 0x3f].load(std::memory_order_acquire);
    return static_cast<int32_t>(std::atomic_ref<uint32_t>(region.slots[buffet->atomic_id_ >> 9].state)
        .fetch_sub(1u, std::memory_order_acq_rel) & SliceEntry::OWNERS_MASK);
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
            n = new ChainBuffet(
                descriptor,
                descriptor->size_of(buffer_),
                false
            );
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
            ChainBuffet* n = new ChainBuffet(selected, selected->default_size, true);
            instances[idx].store(n, std::memory_order_release);
            break;
        }
        std::this_thread::yield();
    }
    return instances[idx];
}
/** ------------------------------------------------------------------------------------------- Free Buffer
 * @brief Frees the allocated buffer, if still held, and the ChainBuffet's own slot.
 */
void ChainBuffet::free() {
    if (buffer_ != nullptr) {
        descriptor->deleter(buffer_);
        buffer_ = nullptr;
    }
    if (descriptor != nullptr) {
        Region& region = *Alligator::inst().regions[(atomic_id_ >> 3) & 0x3f].load(std::memory_order_acquire);
        region.release(&region.slots[atomic_id_ >> 9]);
        descriptor = nullptr;
    }
}
/** ------------------------------------------------------------------------------------------- Claim Buffer
 * @brief Claims a span of the buffer of the specified size into a fresh slot counting on this ChainBuffet,
 * which the caller holds a count on. Past the end the buffer is full: the next ChainBuffet becomes the
 * current one for the descriptor under the pending sentinel, this one drops the reference that kept it
 * current, and the claim goes to whichever ChainBuffet is current then.
 * @param size Size of the buffer to claim.
 * @return The slot's entry in the atomics table.
 */
SliceEntry* ChainBuffet::claim(size_t size) {
    const size_t rounded = (size + 63) & ~size_t{63};
    const size_t start = bump_ptr_.fetch_add(rounded, std::memory_order_acq_rel);
    if (start + rounded > descriptor->size_of(buffer_)) {
        ChainBuffet* successor = next(true);
        std::atomic<ChainBuffet*>& current = current_for(descriptor->type_idx);
        ChainBuffet* const pending = reinterpret_cast<ChainBuffet*>(-1);
        for (;;) {
            ChainBuffet* expected = this;
            if (current.compare_exchange_strong(expected, pending, std::memory_order_acq_rel, std::memory_order_acquire)) {
                current.store(successor, std::memory_order_release);
                current.notify_all();
                Alligator::release_buffet(this);
                break;
            }
            if (expected != pending) break;
            current.wait(pending, std::memory_order_acquire);
        }
        ChainBuffet* held = Alligator::hold_current(descriptor);
        SliceEntry* entry = held->claim(size);
        Alligator::release_buffet(held);
        return entry;
    }
    Alligator& arena = Alligator::inst();
    const uint32_t id = arena.next_id(descriptor);
    Region& region = *arena.regions[(id >> 3) & 0x3f].load(std::memory_order_acquire);
    const uint32_t slot = id >> 9;
    const uint64_t address = descriptor->device_address != nullptr
        ? descriptor->device_address(buffer_)
        : reinterpret_cast<uint64_t>(descriptor->host_ptr(buffer_, 0));
    region.gpu_slots[slot].set(address, static_cast<uint32_t>(rounded >> 6), static_cast<uint32_t>(start >> 6));
    region.host_ptrs[slot] = descriptor->host_ptr(buffer_, start);
    region.parents[slot] = atomic_id_;
    fetch_add_ref_count(this);
    std::atomic_ref<uint32_t>(region.slots[slot].state).store(1u, std::memory_order_release);
    return &region.slots[slot];
}
/** ------------------------------------------------------------------------------------------- Slots Available
 * @brief Returns the number of available slots in the entry region.
 */
size_t Region::slots_available() const {
    return (POOL_SIZE >> 6) - (claimed.load(std::memory_order_acquire) - freed.load(std::memory_order_acquire));
}
/** ------------------------------------------------------------------------------------------- Claim Slice Entry
 * @brief Claims a slice entry from the entry region.
 * @param skip_at_most Collisions tolerated before the region reports itself full.
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
 * @brief Releases a previously claimed slice entry, clearing every table at its index.
 * @param entry Pointer to the slice entry to release.
 * @return True if the release was successful, false otherwise.
 */
bool Region::release(SliceEntry* entry) {
    if (entry == nullptr || entry < &slots[0] || entry >= &slots[0] + slots.size()) {
        return false;
    }
    const size_t idx = entry - &slots[0];
    gpu_slots[idx].clear();
    host_ptrs[idx] = nullptr;
    parents[idx] = UINT32_MAX;
    std::atomic_ref<uint32_t>(slots[idx].state).store(0u, std::memory_order_relaxed);
    const uint64_t bit = 1ULL << (idx & 63);
    // fetch_and cannot lose to a neighbouring bit changing the word, which a single CAS could.
    if (occupancy[idx >> 6].fetch_and(~bit, std::memory_order_acq_rel) & bit) {
        freed.fetch_add(1, std::memory_order_release);
        return true;
    }
    return false;
}
/** --------------------------------------------------------------------------------------------------------- Release Buffet
 * @brief Takes one from a ChainBuffet's reference count; the last one deallocates the buffer now and has the
 * Kitchen delete the ChainBuffet. Once teardown has begun the buffer stays with the process, since the
 * tracker behind the deleter may already be gone.
 * @param buffet The ChainBuffet.
 */
void Alligator::release_buffet(ChainBuffet* buffet) {
    if (ChainBuffet::fetch_sub_ref_count(buffet) != 1) return;
    if (finalizing_.load(std::memory_order_acquire)) [[unlikely]] return;
    buffet->descriptor->deleter(buffet->buffer_);
    buffet->buffer_ = nullptr;
    Kitchen::submit(&retire_buffet, buffet);
}
} // namespace buffetalligator
