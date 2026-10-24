/** --------------------------------------------------------------------------------------------------------- ChainBuffet
 * @file chainbuffet.cpp
 * @brief The chain buffets the Alligator cuts slots from: one buffer, one shared handle every slot on it points
 * at, and the next link kept one ahead.
 */
#include <alligator.hpp>
#include <alligator/kitchen.hpp>
#include <memory/tracker.hpp>
#include <atomic>
#include <cstdint>
#include <utility>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- ChainBuffet Constructor
 * @brief Makes the buffer, takes or makes the shared handle that counts this link's slots and starts at one
 * for the link itself, and keeps one buffer ahead unless told not to.
 * @param parent The handle this link belongs to, or null to make its own.
 * @param desc Pointer to the BuffetDescriptor.
 * @param size Size of the buffer to allocate.
 * @param allocate_next Whether the next ChainBuffet is made now; a novel buffer never has one.
 */
Alligator::ChainBuffet::ChainBuffet(
    SharedBuffet* parent,
    const BuffetDescriptor* descriptor,
    size_t size,
    bool allocate_next
) : buffer_(descriptor->active_factory(size)), desc(descriptor), parent_(parent) {
    if (!parent_) {
        parent_ = new SharedBuffet();
        parent_->data_ = new SharedBuffet::Data{this, MultiAtomic()};
    }
    if (allocate_next) (void)next(false);
}
/** --------------------------------------------------------------------------------------------------------- Next ChainBuffet
 * @brief Retrieves the next ChainBuffet in the chain.
 * @param allocate_next If true, ensures that the next ChainBuffet is allocated.
 * @return Pointer to the next ChainBuffet.
 */
Alligator::ChainBuffet* Alligator::ChainBuffet::next(bool allocate_next) {
    ChainBuffet* n = next_.load(std::memory_order_acquire);
    while (n == nullptr || n == reinterpret_cast<ChainBuffet*>(-1)) {
        if (!buffer_) [[unlikely]] {
            std::string error_msg = "Alligator::claim: all buffer references have been lost, the chain "
                "has been broken. This should never happen.";
            ALLIGATOR_THROW(error_msg);
        }
        ChainBuffet* expected = nullptr;
        if (next_.compare_exchange_strong(expected, reinterpret_cast<ChainBuffet*>(-1))) {
            n = new ChainBuffet(nullptr, desc, desc->size_of(buffer_), allocate_next);
            next_.store(n, std::memory_order_release);
            return n;
        }
        std::this_thread::yield();
        n = next_.load(std::memory_order_acquire);
    }
    if (allocate_next) (void)n->next(false);
    return n;
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffet Free
 * @brief Returns the buffer to its placement; the handle that owns this link has already let go of it.
 */
void Alligator::ChainBuffet::free() {
    /// Two last counts can race here, so only the one that takes the buffer returns it.
    void* buffer = std::atomic_ref<void*>(buffer_).exchange(nullptr, std::memory_order_acq_rel);
    if (buffer) desc->active_deleter(buffer);
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffet Claim Into
 * @brief Cuts whole granules from this link into a slot: the slot takes the link's handle and one more count
 * on it, the record takes the address, length and offset. A full link advances the chain, retires the link
 * two behind, and claims from the next.
 * @param slot The slot the claim lands in.
 * @param record The slot's GPUBuf record.
 * @param size The size in bytes, already whole granules.
 * @return The slot.
 */
ChompEntry* Alligator::ChainBuffet::claim_into(ChompEntry* slot, GPUBuf* record, size_t size) {
    SharedBuffet* handle = std::atomic_ref<SharedBuffet*>(parent_).load(std::memory_order_seq_cst);
    if (!handle) [[unlikely]] return next(true)->claim_into(slot, record, size);
    /// The count comes first and becomes the slot's if the claim lands, so a retire in flight sees this claimer
    /// and leaves the buffer; a link closed in the meantime is let go of and the walk moves on.
    handle->data_->refs.increment_refcount();
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (std::atomic_ref<SharedBuffet*>(parent_).load(std::memory_order_seq_cst) == nullptr) [[unlikely]] {
        if (handle->data_->refs.decrement_refcount() == 0) Alligator::retire_handle(handle);
        return next(true)->claim_into(slot, record, size);
    }
    const uint32_t offset = bump_ptr_.fetch_add(static_cast<uint32_t>(size), std::memory_order_acq_rel);
    if (offset + size > desc->size_of(buffer_)) {
        /// The successor learns who came before it, whichever thread advances the chain; a lost advance means
        /// another thread already moved current past this link, and the walk still goes link by link.
        ChainBuffet* successor = next(true);
        ChainBuffet* no_prev = nullptr;
        successor->prev_.compare_exchange_strong(no_prev, this, std::memory_order_acq_rel);
        void* expected = this;
        Alligator::current_for(desc, &expected, successor);
        ChainBuffet* prev = prev_.exchange(nullptr, std::memory_order_acq_rel);
        if (prev) {
            /// The link two back lets go of itself, and the last count out frees it.
            SharedBuffet* retired = std::atomic_ref<SharedBuffet*>(prev->parent_).exchange(nullptr,
                std::memory_order_seq_cst);
            if (retired && retired->data_->refs.decrement_refcount() == 0) Alligator::retire_handle(retired);
        }
        if (handle->data_->refs.decrement_refcount() == 0) Alligator::retire_handle(handle);
        return successor->claim_into(slot, record, size);
    }
    std::atomic_ref<SharedBuffet*>(slot->buffet).store(handle, std::memory_order_release);
    record->set(desc->device_address(buffer_), static_cast<uint32_t>(size >> 6), offset >> 6);
    return slot;
}
} // namespace buffetalligator
