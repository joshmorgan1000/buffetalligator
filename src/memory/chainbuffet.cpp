/** --------------------------------------------------------------------------------------------------------- ChainBuffet
 * @file chainbuffet.cpp
 * @brief The chain buffets the Alligator cuts records from: one slab, one count every record on it holds, the
 * next link kept one ahead, and one parked spare per placement for the next link to take.
 */
#include <alligator.hpp>
#include <memory/chainbuffet.hpp>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <utility>

namespace buffetalligator {
std::array<std::atomic<void*>, 128> ChainBuffet::spares_{};
/** --------------------------------------------------------------------------------------------------------- ChainBuffet Constructor
 * @brief Wraps the given slab, or allocates one, and caches its host and device addresses; a chain link starts
 * with the chain's own hold, a dedicated link with nothing until its record takes it.
 * @param desc The placement.
 * @param size The slab bytes.
 * @param novel Whether the link is dedicated to one record.
 * @param slab A parked slab to wrap, or null to allocate one.
 */
ChainBuffet::ChainBuffet(const BuffetDescriptor* desc, size_t size, bool novel, void* slab)
: buffer_(slab ? slab : desc->active_factory(size)), desc_(desc), host_(desc->host_ptr(buffer_)),
  device_(desc->device_address(buffer_)), size_(desc->size_of(buffer_)), novel_(novel),
  count_(novel ? 0u : 1u) {}
/** --------------------------------------------------------------------------------------------------------- Open
 * @brief A fresh shell around the placement's parked spare slab, or around a fresh slab; shells are never
 * reused, so a stale pointer to a retired link stays harmless.
 * @param desc The placement.
 * @param size The slab bytes.
 * @param novel Whether the link is dedicated to one record.
 * @param ahead Whether to make the link ahead as well.
 * @return The link.
 */
ChainBuffet* ChainBuffet::open(const BuffetDescriptor* desc, size_t size, bool novel, bool ahead) {
    std::atomic<void*>& spare = spares_[*desc->type_idx_getter()];
    void* slab = novel ? nullptr : spare.load(std::memory_order_acquire);
    /// The pending sentinel is teardown's: the slot parks nothing any more and lends nothing either.
    while (slab != nullptr && slab != PENDING_LINK
        && !spare.compare_exchange_weak(slab, nullptr, std::memory_order_acq_rel, std::memory_order_acquire)) {}
    if (slab == PENDING_LINK) slab = nullptr;
    ChainBuffet* link = new ChainBuffet(desc, size, novel, slab);
    if (ahead) (void)link->next(false);
    return link;
}
/** --------------------------------------------------------------------------------------------------------- Next ChainBuffet
 * @brief The link ahead, made by whichever thread finds the slot empty while the others wait on the sentinel.
 * @param allocate_next Whether the link ahead gets its own link ahead too.
 * @return The link ahead.
 */
ChainBuffet* ChainBuffet::next(bool allocate_next) {
    ChainBuffet* link = next_.load(std::memory_order_acquire);
    while (link == nullptr || link == PENDING_LINK) {
        if (!buffer_) [[unlikely]] {
            /// A retired link makes no more links, so its slot is final: a thread that saw the slot pending
            /// before the link retired reads the link made meanwhile, and an unset slot is a broken chain.
            link = next_.load(std::memory_order_acquire);
            if (link == nullptr || link == PENDING_LINK) {
                ALLIGATOR_THROW("Alligator::claim: the chain has been broken, this should never happen");
            }
            break;
        }
        ChainBuffet* expected = nullptr;
        if (next_.compare_exchange_strong(expected, PENDING_LINK, std::memory_order_acq_rel)) {
            link = open(desc_, size_, false, allocate_next);
            next_.store(link, std::memory_order_release);
            return link;
        }
        std::this_thread::yield();
        link = next_.load(std::memory_order_acquire);
    }
    if (allocate_next) (void)link->next(false);
    return link;
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffet Claim
 * @brief The count comes first and becomes the record's if the claim lands; a closed link is let go of for the
 * lane's current one, and a full link hands the lane to its successor, closes the link two back, and claims from
 * the successor.
 * @param total The bytes to cut, the record granule included.
 * @return The record.
 */
SliceRecord* ChainBuffet::claim(size_t total) {
    if (count_.fetch_add(1, std::memory_order_acq_rel) & CLOSED) [[unlikely]] {
        release();
        return Alligator::current_for(desc_)->claim(total);
    }
    const size_t offset = bump_.fetch_add(total, std::memory_order_acq_rel);
    if (offset + total > size_) {
        /// The successor learns who came before it, whichever thread advances the lane; a lost advance means
        /// another thread already moved current past this link, and the walk still goes link by link.
        ChainBuffet* successor = next(true);
        ChainBuffet* no_prev = nullptr;
        successor->prev_.compare_exchange_strong(no_prev, this, std::memory_order_acq_rel);
        ChainBuffet* expected = this;
        Alligator::current_for(desc_, &expected, successor);
        ChainBuffet* prev = prev_.exchange(nullptr, std::memory_order_acq_rel);
        if (prev) prev->close();
        release();
        return successor->claim(total);
    }
    SliceRecord* record = reinterpret_cast<SliceRecord*>(static_cast<uint8_t*>(host_) + offset);
    record->gpu.set(device_, static_cast<uint32_t>((total - sizeof(SliceRecord)) >> 6),
        static_cast<uint32_t>((offset >> 6) + 1));
    record->host = reinterpret_cast<uint8_t*>(record) + sizeof(SliceRecord);
    record->link = this;
    return record;
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffet Release
 * @brief One count leaves the link; the last one out of a closed link retires its slab.
 */
void ChainBuffet::release() {
    if (count_.fetch_sub(1, std::memory_order_acq_rel) == CLOSED + 1u) retire();
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffet Close
 * @brief The chain lets go of its own hold and closes the link to new claims; with no records left the slab
 * retires at once, otherwise the last record out retires it, and a link already closed is left alone.
 */
void ChainBuffet::close() {
    uint32_t old = count_.load(std::memory_order_acquire);
    do {
        if (old & CLOSED) return;
    } while (!count_.compare_exchange_weak(old, old - 1u + CLOSED, std::memory_order_acq_rel,
        std::memory_order_acquire));
    if (old == 1u) retire();
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffet Retire
 * @brief The slab's last count is gone: a dedicated link frees itself whole; a chain link zeroes what it handed
 * out and parks its slab as the placement's spare, or frees it when a spare is already parked, and its shell
 * stays retired for whoever still holds a pointer to it.
 */
void ChainBuffet::retire() {
    /// Two last counts can race here, so only the one that marks the link retired takes the slab.
    uint32_t closed = CLOSED;
    if (!count_.compare_exchange_strong(closed, RETIRED, std::memory_order_acq_rel)) return;
    if (novel_) {
        desc_->active_deleter(buffer_);
        delete this;
        return;
    }
    std::memset(host_, 0, std::min(bump_.load(std::memory_order_relaxed), size_));
    void* parked = nullptr;
    void* slab = std::exchange(buffer_, nullptr);
    if (!spares_[*desc_->type_idx_getter()].compare_exchange_strong(parked, slab, std::memory_order_acq_rel)) {
        desc_->active_deleter(slab);
    }
}
} // namespace buffetalligator
