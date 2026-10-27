/** --------------------------------------------------------------------------------------------------------- ChainBuffet
 * @file chainbuffet.cpp
 * @brief The chain buffets the Alligator cuts records from: one slab, one count every record on it holds, the
 * next link kept one ahead, and one parked spare per placement for the next link to take.
 */
#include <alligator.hpp>
#include <memory/chainbuffet.hpp>
#include <memory/slicetype.hpp>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <utility>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- ChainBuffet Constructor
 * @brief Constructs a new ChainBuffet, and alloc_ahead buffets ahead if requested.
 * @param desc The placement.
 * @param size The slab bytes.
 * @param semantics The slice semantics for this buffer.
 * @param type The type word for the slices in this buffer.
 * @param alloc_ahead The number of additional buffers to pre-allocate ahead.
 */
ChainBuffet::ChainBuffet(
    const BuffetDescriptor* desc,
    size_t size,
    void* semantics,
    uint32_t type,
    std::atomic<ChainBuffet*>* ref_current,
    int alloc_ahead,
    bool novel
) : handle_(desc->active_factory(size))
, host_ptr_(desc->host_ptr(handle_))
, desc_(desc)
, semantics_(semantics)
, type_(type)
, device_(desc->device_address(handle_))
, size_(desc->size_of(handle_))
, novel_(novel)
, ref_current_(ref_current) {
    if (semantics_ == nullptr) [[unlikely]] {
        ALLIGATOR_THROW("ChainBuffet: semantics cannot be null.");
    }
    if (size < 64 * 1024 * 1024 && novel == false) [[unlikely]] {
        ALLIGATOR_THROW("ChainBuffet: only novel buffers can be smaller than 64 MiB.");
    }
    if (novel_ && alloc_ahead > 0) [[unlikely]] {
        ALLIGATOR_THROW("ChainBuffet: novel buffers cannot pre-allocate ahead.");
    }
    if (alloc_ahead > 0) {
        (void)next(alloc_ahead - 1);
    }
}
/** --------------------------------------------------------------------------------------------------------- Next ChainBuffet
 * @brief The link ahead, made by whichever thread finds the slot empty while the others wait on the sentinel.
 * @param allocate_next Whether the link ahead gets its own link ahead too.
 * @return The link ahead.
 */
ChainBuffet* ChainBuffet::next(int allocate_next) {
    if (novel_ || size_ < 64 * 1024 * 1024) [[unlikely]] {
        std::string error_msg = "ChainBuffet: only slabs can be chained. This is by design. Buffers that "
            "are too small to meet the definition of slab are far too detrimental to performance.";
        ALLIGATOR_THROW(error_msg);
    }
    ChainBuffet* link = next_.load(std::memory_order_acquire);
    while (link == nullptr || link == reinterpret_cast<ChainBuffet*>(-1)) {
        ChainBuffet* expected = nullptr;
        if (next_.compare_exchange_strong(expected, reinterpret_cast<ChainBuffet*>(-1))) {
            link = new ChainBuffet(desc_, size_, semantics_, type_, ref_current_, allocate_next - 1, false);
            next_.store(link, std::memory_order_release);
            buffers_ahead_.fetch_add(1, std::memory_order_relaxed);
            if (allocate_next) (void)link->next(allocate_next - 1);
            return link;
        }
        std::this_thread::yield();
        link = next_.load(std::memory_order_acquire);
    }
    if (allocate_next > 0) (void)link->next(allocate_next - 1);
    return link;
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffet Claim
 * @brief Claims a slice of the given size from this buffer.
 * @param size The number of bytes to claim from the buffer.
 * @return The Slice.
 */
Slice ChainBuffet::claim(uint32_t size, bool novel_buffer) {
    const types::SliceSemantics* table = static_cast<const types::SliceSemantics*>(semantics_);
    if (size == 0) return Slice{};
    const size_t alignment = SliceStatics::type_emulator(type_, 1)->alignment();
    const size_t adjusted_size = (size + alignment - 1) & ~(alignment - 1);
    if (adjusted_size >= size_ || novel_buffer) {
        ChainBuffet* novel_buf = new ChainBuffet(desc_, size, semantics_, type_, ref_current_, 0, true);
        novel_buf->bump_ptr_.store(adjusted_size, std::memory_order_release);
        novel_buf->size_ = 0;
        Slice result = table->sub_slice_fn(table->type, novel_buf, 0, size);
        ChainBuffet::decrement_refcount(novel_buf);
        return result;
    }
    const size_t offset = bump_ptr_.fetch_add(adjusted_size, std::memory_order_acq_rel);
    if (offset + adjusted_size >= size_) {
        buffers_ahead_.fetch_sub(1, std::memory_order_release);
        if (novel_ || size_ < 64 * 1024 * 1024) [[unlikely]] {
            std::string error_msg = "ChainBuffet: only slabs can be chained. This is by design. Buffers"
                " that are too small to meet the definition of slab are too detrimental to performance.";
            ALLIGATOR_THROW(error_msg);
        }
        ChainBuffet* next_one = next(1);
        next_one->prev_.store(this, std::memory_order_release);
        ChainBuffet* expected = this;
        if (ref_current_ && ref_current_->compare_exchange_strong(expected, next_one, std::memory_order_acq_rel)) {
            next_one = prev_.exchange(nullptr, std::memory_order_acq_rel);
            if (next_one && (std::atomic_ref<uint32_t>(next_one->count_).fetch_sub(1, std::memory_order_acq_rel) == 1)) {
                if (!shutting_down().load(std::memory_order_acquire)
                    && buffers_ahead_.fetch_add(1, std::memory_order_acquire) < 2
                    && desc_->default_size_method() <= next_one->size_
                ) {
                    std::memset(next_one->host_ptr_, 0, next_one->size_);
                    next_one->bump_ptr_.store(0, std::memory_order_release);
                    next_one->next_.store(nullptr, std::memory_order_release);
                    next_one->prev_.store(nullptr, std::memory_order_release);
                    std::atomic_ref<uint32_t>(next_one->count_).store(1, std::memory_order_release);
                    take_to_front(next_one, 0);
                } else {
                    buffers_ahead_.fetch_sub(1, std::memory_order_release);
                    delete next_one;
                }
            }
        }
        if (offset + adjusted_size == size_) {
            return table->sub_slice_fn(table->type, this, offset, adjusted_size);
        }
        return next()->claim(size);
    }
    return table->sub_slice_fn(table->type, this, offset, adjusted_size);
}
/** --------------------------------------------------------------------------------------------------------- Take to Front
 * @brief Moves the given buffer to the front of the chain.
 * @param buf The buffer to move to the front.
 * @param distance_from_current The distance of the buffer from the current position in the chain.
 */
void ChainBuffet::take_to_front(ChainBuffet* buf, int distance_from_current) {
    if (distance_from_current > 4) {
        delete buf;
        buffers_ahead_.fetch_sub(1, std::memory_order_release);
        return;
    }
    ChainBuffet* nextone = next_.load(std::memory_order_acquire);
    while (nextone == reinterpret_cast<ChainBuffet*>(-1)) {
        std::this_thread::yield();
        nextone = next_.load(std::memory_order_acquire);
    }
    if (nextone) {
        nextone->take_to_front(buf, distance_from_current + 1);
    } else {
        ChainBuffet* expected = nullptr;
        if (next_.compare_exchange_strong(expected, buf, std::memory_order_acq_rel)) {
            buffers_ahead_.fetch_add(1, std::memory_order_release);
        } else {
            nextone->take_to_front(buf, distance_from_current + 1);
        }
    }
}
/** --------------------------------------------------------------------------------------------------------- Novel Backing
 * @brief Whether the bytes live on a dedicated link or in a buffer of their own.
 * @return True if the Slice's root owns its backing alone, false otherwise.
 */
bool Slice::is_novel() const noexcept {
    return parent_ != nullptr && (!is_pooled() || static_cast<const ChainBuffet*>(parent_)->is_novel()
        || static_cast<const ChainBuffet*>(parent_)->size_ < 64 * 1024 * 1024);
}
/** --------------------------------------------------------------------------------------------------------- Alligator::claim
 * @brief Claims a Slice: a record and its payload cut from this thread's lane of the placement, or a
 * dedicated link for a novel or slab-sized request.
 * @param size The size of the Slice to claim.
 * @param novel Whether the Slice should be allocated in a novel buffet.
 * @param placement The placement descriptor for the Slice.
 * @param type The type index for the Slice.
 * @return The claimed Slice.
 */
Slice Alligator::claim(
    uint32_t size,
    bool novel,
    const BuffetDescriptor* placement
) {
    if (size == 0) [[unlikely]] return Slice{};
    return current_for(placement)->claim(static_cast<uint32_t>(size), novel);
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffet::shutting_down
 * @brief Returns a reference to the atomic flag indicating if the ChainBuffet system is shutting down.
 * @return Reference to the atomic shutting down flag.
 */
std::atomic<bool>& ChainBuffet::shutting_down() {
    static std::atomic<bool> shutting_down_flag{false};
    return shutting_down_flag;
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffet::decrement_refcount
 * @brief Decrements the reference count of the given buffer and deletes it if the count reaches zero.
 * @param buf The buffer whose reference count is to be decremented.
 * @return True if the buffer could still have references, false if there are no more references and the
 * buffer is either null or has otherwise been destroyed.
 */
bool ChainBuffet::decrement_refcount(ChainBuffet* buf) {
    if (!buf) return false;
    uint32_t old_count = std::atomic_ref<uint32_t>(buf->count_).fetch_sub(1, std::memory_order_acq_rel);
    if (old_count == 1) {
        if (buf->handle_ && buf->desc_ && buf->desc_->active_deleter) {
            buf->desc_->active_deleter(buf->handle_);
        }
        delete buf;
        return true;
    }
    return false;
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffet::is_novel
 * @brief Checks if this buffer is novel or too small to be considered a slab.
 * @return True if the buffer is novel or too small, false otherwise.
 */
bool ChainBuffet::is_novel() const {
    return novel_ || size_ < 64 * 1024 * 1024;
}
/** --------------------------------------------------------------------------------------------------------- ChainBuffet::is_novel
 * @brief Checks if the given buffer is novel or too small to be considered a slab.
 * @param buf The buffer to check.
 * @return True if the buffer is novel or too small, false otherwise.
 */
bool ChainBuffet::is_novel(const ChainBuffet* buf) {
    return buf && (buf->is_novel() || buf->size_ < 64 * 1024 * 1024);
}
} // namespace buffetalligator
