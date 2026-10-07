/** --------------------------------------------------------------------------------------------------------- Chain Buffet
 * @file chainbuffet.cpp
 * @brief Owns counted allocation controls and safely publishes claims from chained slabs.
 */
#include <alligator.hpp>
#include <alligator/easyvulkan.hpp>
#include <gpu/runtime.hpp>
#include <memory/tracker.hpp>
#include <atomic>
#include <cstdint>
#include <array>
#include <limits>
#include <new>

namespace buffetalligator {
namespace {
constinit std::array<std::atomic<ChainBuffet*>, 8> current_chains{};
} // namespace
/** --------------------------------------------------------------------------------------------------------- Next Descriptor Index
 * @brief Tracks the highest registered descriptor index.
 */
std::atomic<size_t>& BuffetDescriptors::next_index() {
    static std::atomic<size_t> index{2};
    return index;
}
/** --------------------------------------------------------------------------------------------------------- Descriptor List
 * @brief Stores the registered heap and Vulkan descriptors before custom placements are added.
 */
std::array<const BuffetDescriptor*, 8>& BuffetDescriptors::list() {
    static std::array<const BuffetDescriptor*, 8> descriptors{
        descriptor_for(static_cast<AlignedHeapBuffer*>(nullptr)),
        descriptor_for(static_cast<VulkanBuffer*>(nullptr)),
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
    };
    return descriptors;
}
/** --------------------------------------------------------------------------------------------------------- Get Descriptor
 * @brief Resolves an exact descriptor index without remapping unregistered placements.
 */
const BuffetDescriptor* BuffetDescriptors::get(size_t type) {
    return type < list().size() ? list()[type] : nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Register Descriptor
 * @brief Registers a descriptor at the fixed type index encoded in its Slice identifiers.
 */
size_t BuffetDescriptors::register_descriptor(const BuffetDescriptor* descriptor) {
    const size_t index = descriptor->type_idx;
    if (index >= list().size()) {
        ALLIGATOR_THROW(std::string("Buffet type ") + descriptor->type_name + " reports type index "
            + std::to_string(index) + "; Slice ids carry 3 type bits, so indices stop at 7");
    }
    if (list()[index] != nullptr && list()[index] != descriptor) {
        ALLIGATOR_THROW(std::string("Buffet type index ") + std::to_string(index) + " already belongs to "
            + list()[index]->type_name + "; " + descriptor->type_name + " cannot share it");
    }
    list()[index] = descriptor;
    size_t count = next_index().load(std::memory_order_acquire);
    while (count <= index && !next_index().compare_exchange_weak(
        count, index + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {}
    return index;
}
/** --------------------------------------------------------------------------------------------------------- Default Placement
 * @brief Selects the frozen placement of the active compute device at startup: its host-visible
 * buffers whenever a GPU exists, unified or discrete, and registered host memory without one.
 */
const BuffetDescriptor*& BuffetDescriptors::default_placement() {
    static const BuffetDescriptor* placement = gpu_device().placement;
    return placement;
}
/** --------------------------------------------------------------------------------------------------------- Descriptor Count
 * @brief Returns the exclusive upper bound of registered descriptor indexes.
 */
size_t BuffetDescriptors::count() { return next_index().load(std::memory_order_acquire); }
/** --------------------------------------------------------------------------------------------------------- Record Allocation
 * @brief Reports one completed allocation through its registered placement.
 */
void BuffetDescriptors::record_allocation(size_t type, size_t bytes, const void* buffet) {
    const BuffetDescriptor& placement = *list()[type];
    Memory::record_code_location(placement, bytes, buffet);
    Memory::record_allocation(placement, bytes);
}
/** --------------------------------------------------------------------------------------------------------- Record Deallocation
 * @brief Reports one released allocation through its registered placement.
 */
void BuffetDescriptors::record_deallocation(size_t type, size_t bytes, const void* buffet) {
    Memory::record_deallocation(*list()[type], bytes);
    Memory::forget_code_location(buffet);
}
/** --------------------------------------------------------------------------------------------------------- Allocation Control
 * @brief Keeps allocation ownership independent of a caller-owned ChainBuffet object's lifetime.
 */
struct ChainBuffet::ChainBuffetToken::Control {
    void* buffer;
    const BuffetDescriptor* descriptor;
    std::atomic<size_t> references{1};
    ChainBuffet* owner;
    bool novel;
};
/** --------------------------------------------------------------------------------------------------------- Add Reference
 * @brief Retains a control whose lifetime is already owned by the caller.
 */
void ChainBuffet::ChainBuffetToken::add_ref() {
    if (token_) token_->references.fetch_add(1, std::memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Release Reference
 * @brief Releases the backing allocation and its internally owned node after the last reference.
 */
void ChainBuffet::ChainBuffetToken::free() {
    Control* released = std::exchange(token_, nullptr);
    if (released && released->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        ChainBuffet* owner = released->owner;
        released->descriptor->deleter(released->buffer);
        delete released;
        delete owner;
    }
}
/** --------------------------------------------------------------------------------------------------------- Token Constructor
 * @brief Retains an allocation while its node is protected by existing ownership.
 */
ChainBuffet::ChainBuffetToken::ChainBuffetToken(const ChainBuffet* buffer)
: token_(buffer ? buffer->token_ : nullptr) { add_ref(); }
/** --------------------------------------------------------------------------------------------------------- Token Copy Constructor
 * @brief Retains another token's allocation.
 */
ChainBuffet::ChainBuffetToken::ChainBuffetToken(const ChainBuffetToken& other)
: token_(other.token_) { add_ref(); }
/** --------------------------------------------------------------------------------------------------------- Token Copy Assignment
 * @brief Replaces this token's ownership with another live allocation.
 */
ChainBuffet::ChainBuffetToken& ChainBuffet::ChainBuffetToken::operator=(const ChainBuffetToken& other) {
    if (this != &other) { free(); token_ = other.token_; add_ref(); }
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Token Move Constructor
 * @brief Transfers a token's counted ownership.
 */
ChainBuffet::ChainBuffetToken::ChainBuffetToken(ChainBuffetToken&& other) noexcept
: token_(std::exchange(other.token_, nullptr)) {}
/** --------------------------------------------------------------------------------------------------------- Token Move Assignment
 * @brief Releases existing ownership before taking the source token.
 */
ChainBuffet::ChainBuffetToken& ChainBuffet::ChainBuffetToken::operator=(ChainBuffetToken&& other) noexcept {
    if (this != &other) { free(); token_ = std::exchange(other.token_, nullptr); }
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Token Destructor
 * @brief Releases this token's allocation reference.
 */
ChainBuffet::ChainBuffetToken::~ChainBuffetToken() { free(); }
/** --------------------------------------------------------------------------------------------------------- Token Descriptor
 * @brief Returns the descriptor retained with the allocation.
 */
const BuffetDescriptor* ChainBuffet::ChainBuffetToken::descriptor() const {
    return token_->descriptor;
}
/** --------------------------------------------------------------------------------------------------------- Token Novel Allocation
 * @brief Reports whether this allocation was dedicated to a single original claim.
 */
bool ChainBuffet::ChainBuffetToken::is_novel() const { return token_->novel; }
/** --------------------------------------------------------------------------------------------------------- Token Buffet
 * @brief Returns the live backing handle expected by descriptor operations.
 */
void* ChainBuffet::ChainBuffetToken::buffet() const { return token_->buffer; }
/** --------------------------------------------------------------------------------------------------------- Token Raw Pointer
 * @brief Resolves a byte offset within this token's live allocation.
 */
void* ChainBuffet::ChainBuffetToken::raw(size_t offset) {
    return token_ ? token_->descriptor->host_ptr(token_->buffer, offset) : nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Token Const Raw Pointer
 * @brief Resolves a byte offset within this token's live allocation.
 */
const void* ChainBuffet::ChainBuffetToken::raw(size_t offset) const {
    return token_ ? token_->descriptor->host_ptr(token_->buffer, offset) : nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Next Chain Buffet
 * @brief Publishes one owned successor and resets failed allocation claims for retry.
 */
ChainBuffet* ChainBuffet::next(bool allocate_next) {
    ChainBuffet* const pending = reinterpret_cast<ChainBuffet*>(-1);
    ChainBuffet* successor = next_.load(std::memory_order_acquire);
    for (;;) {
        if (successor == pending) {
            next_.wait(pending, std::memory_order_acquire);
            successor = next_.load(std::memory_order_acquire);
            continue;
        }
        if (successor) break;
        if (!next_.compare_exchange_weak(successor, pending, std::memory_order_acq_rel,
            std::memory_order_acquire)) continue;
        try {
            successor = new ChainBuffet(descriptor, descriptor->size_of(buffer_), true, false);
        } catch (...) {
            next_.store(nullptr, std::memory_order_release);
            next_.notify_all();
            throw;
        }
        next_.store(successor, std::memory_order_release);
        next_.notify_all();
        break;
    }
    if (allocate_next) (void)successor->next(false);
    return successor;
}
/** --------------------------------------------------------------------------------------------------------- Current Chain Buffet
 * @brief Publishes registered placement roots with retryable initialization failures.
 */
std::atomic<ChainBuffet*>& ChainBuffet::current_for(size_t index) {
    const BuffetDescriptor* selected = BuffetDescriptors::get(index);
    if (!selected) ALLIGATOR_THROW("Cannot claim an unregistered buffet placement");
    auto& current = current_chains[index];
    ChainBuffet* const pending = reinterpret_cast<ChainBuffet*>(-1);
    ChainBuffet* node = current.load(std::memory_order_acquire);
    for (;;) {
        if (node == pending) {
            current.wait(pending, std::memory_order_acquire);
            node = current.load(std::memory_order_acquire);
            continue;
        }
        if (node) return current;
        if (!current.compare_exchange_weak(node, pending, std::memory_order_acq_rel,
            std::memory_order_acquire)) continue;
        try {
            node = new ChainBuffet(selected, selected->default_size, true, false);
        } catch (...) {
            current.store(nullptr, std::memory_order_release);
            current.notify_all();
            throw;
        }
        current.store(node, std::memory_order_release);
        current.notify_all();
        return current;
    }
}
/** --------------------------------------------------------------------------------------------------------- Release Chains
 * @brief Releases current and prepared roots after the arena drains all asynchronous users.
 */
void ChainBuffet::release_chains() {
    for (auto& current : current_chains) {
        ChainBuffet* node = current.exchange(nullptr, std::memory_order_acq_rel);
        if (node) node->free();
    }
}
/** --------------------------------------------------------------------------------------------------------- Claim From Chain
 * @brief Acquires counted current-node ownership before allowing another claimant to retire it.
 */
Slice ChainBuffet::chain(const BuffetDescriptor* placement, size_t size, bool novel_buffer) {
    if (!size) return Slice();
    (void)Alligator::inst();
    auto& current = current_for(placement->type_idx);
    ChainBuffet* const pending = reinterpret_cast<ChainBuffet*>(-1);
    ChainBuffet* node = current.load(std::memory_order_acquire);
    for (;;) {
        if (node == pending) {
            current.wait(pending, std::memory_order_acquire);
            node = current.load(std::memory_order_acquire);
            continue;
        }
        if (!current.compare_exchange_weak(node, pending, std::memory_order_acq_rel,
            std::memory_order_acquire)) continue;
        ChainBuffetToken retained(node);
        current.store(node, std::memory_order_release);
        current.notify_all();
        return node->claim(size, novel_buffer);
    }
}
/** --------------------------------------------------------------------------------------------------------- Chain Buffet Constructor
 * @brief Allocates one independent control and marks whether final release also owns this node.
 */
ChainBuffet::ChainBuffet(const BuffetDescriptor* selected, size_t size, bool owned, bool novel)
: descriptor(selected) {
    if (size > (uint64_t{UINT32_MAX} << 6))
        ALLIGATOR_THROW("Buffet size exceeds the 64-byte granule representation");
    buffer_ = descriptor->factory((size + 63) & ~size_t(63));
    try {
        token_ = new ChainBuffetToken::Control{buffer_, descriptor, 1, owned ? this : nullptr, novel};
    } catch (...) {
        descriptor->deleter(buffer_);
        buffer_ = nullptr;
        throw;
    }
    owns_token_.store(true, std::memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Release Chain Ownership
 * @brief Releases prepared successors and this node's root without counting borrowed pointers.
 */
void ChainBuffet::free() {
    ChainBuffet* successor = next_.exchange(nullptr, std::memory_order_acq_rel);
    if (successor) successor->free();
    if (owns_token_.exchange(false, std::memory_order_acq_rel)) {
        ChainBuffetToken released;
        released.token_ = token_;
    }
}
/** --------------------------------------------------------------------------------------------------------- Claim Buffer
 * @brief Publishes a granule view while counted ownership protects allocation and node lifetime.
 */
Slice ChainBuffet::claim(size_t size, bool novel_buffer) {
    if (!size) return Slice();
    if (size > (uint64_t{UINT32_MAX} << 6))
        ALLIGATOR_THROW("Slice size exceeds the 64-byte granule representation");
    const size_t rounded = (size + 63) & ~size_t(63);
    ChainBuffetToken retained(this);
    size_t offset = 0;
    if (novel_buffer || rounded >= descriptor->size_of(buffer_)) {
        ChainBuffet* dedicated = new ChainBuffet(descriptor, rounded, true, true);
        retained.free();
        retained.token_ = dedicated->token_;
        dedicated->owns_token_.store(false, std::memory_order_relaxed);
    } else {
        offset = bump_ptr_.fetch_add(rounded, std::memory_order_relaxed);
        if (offset > descriptor->size_of(buffer_) - rounded) {
            auto& current = current_for(descriptor->type_idx);
            ChainBuffet* const pending = reinterpret_cast<ChainBuffet*>(-1);
            ChainBuffet* node = current.load(std::memory_order_acquire);
            for (;;) {
                if (node == pending) {
                    current.wait(pending, std::memory_order_acquire);
                    node = current.load(std::memory_order_acquire);
                    continue;
                }
                if (!current.compare_exchange_weak(node, pending, std::memory_order_acq_rel,
                    std::memory_order_acquire)) continue;
                if (node == this) {
                    ChainBuffet* successor;
                    try {
                        successor = next();
                    } catch (...) {
                        current.store(node, std::memory_order_release);
                        current.notify_all();
                        throw;
                    }
                    next_.store(nullptr, std::memory_order_release);
                    current.store(successor, std::memory_order_release);
                    current.notify_all();
                    free();
                } else {
                    current.store(node, std::memory_order_release);
                    current.notify_all();
                }
                return chain(descriptor, size, false);
            }
        }
    }
    const BuffetDescriptor* selected = retained.descriptor();
    const uint64_t address = selected->device_address(retained.buffet());
    Alligator& arena = Alligator::inst();
    const uint32_t slice_id = arena.next_id(selected);
    const uint8_t region_id = (slice_id >> 3) & 0x3f;
    const size_t index = slice_id >> 9;
    Region* region = arena.regions[region_id].load(std::memory_order_acquire);
    region->slots[index].set(&retained, region_id);
    region->gpu_slots[index].set(address, static_cast<uint32_t>(rounded >> 6),
        static_cast<uint32_t>(offset >> 6));
    return Slice(Slice::AdoptId{}, slice_id);
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
