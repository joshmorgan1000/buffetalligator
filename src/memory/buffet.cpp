/** --------------------------------------------------------------------------------------------------------- Buffet
 * @file buffet.cpp
 * @brief Implements the counted slab handle, its bump-pointer claim, and chain advancement.
 */
#include <memory/alligator.hpp>
#include <memory/slicefriend.hpp>
#include <memory/tracker.hpp>
#include <bit>
#include <chrono>
#include <cstring>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Constructor
 * @brief Creates the counted handle around one completed placement allocation.
 */
Buffet::Buffet(
    const Placemat* placement,
    void* context,
    size_t size,
    bool is_novel
) : cold_(
    new Entree{
        .placement = placement,
        .context = context,
        .handle = placement->alligator_(size, context),
    }
), host_(placement->get_host_ptr_(cold_->handle)) {
    size_ = size << 17;
    if (is_novel) {
        bump_offset_.store(size << 6, std::memory_order_release);
        cold_->next.store(NOVEL_NEXT_SENTINEL, std::memory_order_release);
    }
}
struct DeleteCold {
    Buffet::Entree* cold_ = nullptr;
    Buffet* buffet_ = nullptr;
};
namespace {
inline static void* delete_cold(void* ptr) {
    DeleteCold* deleter = static_cast<DeleteCold*>(ptr);
    if (deleter->cold_ != nullptr) {
        if (deleter->cold_->handle != nullptr) {
            if (deleter->cold_->placement != nullptr) {
                Memory::record_deallocation(*deleter->cold_->placement, deleter->buffet_->size());
                void* de_all = const_cast<Placemat*>(deleter->cold_->placement)->deallocate();
                void (*deallocate)(Placemat::Handle* handle, void* context) = 
                    reinterpret_cast<void (*)(Placemat::Handle* handle, void* context)>(de_all);
                deallocate(deleter->cold_->handle, deleter->cold_->context);
            }
            delete deleter->cold_->handle;
            deleter->cold_->handle = nullptr;
        }
        delete deleter->cold_;
        deleter->cold_ = nullptr;
    }
    if (deleter->buffet_ != nullptr) {
        delete deleter->buffet_;
        deleter->buffet_ = nullptr;
    }
    delete deleter;
    return nullptr;
}
}
void Buffet::free() {
    if (cold_ && ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        DeleteCold* deleter = new DeleteCold();
        deleter->cold_ = cold_;
        deleter->buffet_ = this;
        Alligator::instance().bufs[size_ & 0x1FFFF].store(nullptr, std::memory_order_release);
        cold_ = nullptr;
        SliceFriend::execute_async<void*>(&delete_cold, static_cast<void*>(deleter));
    }
}
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Releases the cold block; placement cleanup happens in free() at zero references.
 */
Buffet::~Buffet() {
    free();
}
/** ------------------------------------------------------------------------------------------- Novel Slice
 * @brief Creates a new slice from a novel buffer with the specified size, placement, and context.
 * @param size The size of the slice in bytes.
 * @param placement The factory that owns the allocation.
 * @param context The context associated with the allocation.
 * @return A Slice object representing the requested portion of the novel buffer.
 */
Slice Buffet::novel_slice(size_t size, const Placemat* placement, void* context) {
    Buffet* buffet = new Buffet(
        placement,
        context,
        size,
        true
    );
    Alligator::instance().get_next_free_slot(buffet);
    Slice slice;
    slice.cached_ = buffet->cold_->placement->get_host_ptr_(buffet->cold_->handle);
    slice.meta_ = (size << 17) | (buffet->size_ & 0x1FFFF);
    return slice;
}
/** ------------------------------------------------------------------------------------------- Deallocate
 * @brief Drops the Arena's chain pin on a demoted slab.
 */
void Buffet::deallocate(Buffet* buffer) {
    if (buffer != nullptr) {
        if (buffer->cold_ != nullptr) {
            if (buffer->cold_->handle != nullptr) {
                if (buffer->cold_->placement != nullptr) {
                    buffer->cold_->placement->deallocate_(
                        buffer->cold_->handle,
                        buffer->cold_->context
                    );
                }
                delete buffer->cold_->handle;
                buffer->cold_->handle = nullptr;
            }
            delete buffer->cold_;
            buffer->cold_ = nullptr;
        }
        delete buffer;
    }
}
/** ------------------------------------------------------------------------------------------- Next
 * @brief Waits for and returns the successor preallocated by the Arena worker.
 */
Buffet* Buffet::next() {
    Buffet* current = cold_->next.load(std::memory_order_acquire);
    if (current == NOVEL_NEXT_SENTINEL) {
        ALLIGATOR_THROW("Buffer::next: Attempted to get next buffer from a novel buffer");
    }
    while (current == nullptr || current == SWAP_SENTINEL) {
        Buffet* expected = nullptr;
        if (cold_->next.compare_exchange_strong(expected, SWAP_SENTINEL, std::memory_order_acq_rel)) {
            Buffet* fresh = new Buffet(cold_->placement, cold_->context, size(), false);
            Alligator::instance().get_next_free_slot(fresh);
            cold_->next.store(fresh, std::memory_order_release);
            return fresh;
        }
        std::this_thread::yield();
        current = cold_->next.load(std::memory_order_acquire);
    }
    return current;
}
/** ------------------------------------------------------------------------------------------- Size
 * @brief Returns the granularity-rounded slab size in bytes.
 */
size_t Buffet::size() const {
    return size_ >> 17;
}
/** ------------------------------------------------------------------------------------------- Placemat
 * @brief Returns the registered factory that owns this slab.
 */
Placemat* Buffet::placement() const {
    return cold_ ? const_cast<Placemat*>(cold_->placement) : nullptr;
}
/** ------------------------------------------------------------------------------------------- Full
 * @brief Reports whether the slab's bump cursor has reached its capacity.
 */
bool Buffet::full() const {
    return bump_offset_.load(std::memory_order_acquire) >= (size_ >> 17);
}
/** ------------------------------------------------------------------------------------------- Claim
 * @brief Bump-allocates a Slice from this chain.
 */
Slice Buffet::claim(size_t size_requested) {
    size_t size_they_ll_get = ((size_requested + 63ull) & ~static_cast<size_t>(63ull));
    if (cold_ == nullptr || cold_->root.load(std::memory_order_acquire) == nullptr) [[unlikely]] {
        return Slice();
    }
    if (size_they_ll_get >= size()) {
        return novel_slice(size_they_ll_get, cold_->placement, cold_->context);
    }
    size_t start_offset = (bump_offset_.fetch_add(size_they_ll_get >> 6, std::memory_order_relaxed)) << 6;
    if (start_offset + size_they_ll_get >= size()) {
        if (start_offset >= size()) {
            return next()->claim(size_requested);
        }
        int32_t refs = ref_count_.fetch_add(1, std::memory_order_relaxed);
        if (refs == 0) {
            ref_count_.fetch_sub(1, std::memory_order_relaxed);
            return next()->claim(size_requested);
        }
        // We are the thread that crossed the boundary. Atomics mean it is impossible that
        // any other thread could claim any more from this buffer.
        std::atomic<Buffet*>& current_pool = *Alligator::instance().pool_current_[cold_->placement->type()];
        std::atomic<Buffet*>& previous_pool = *Alligator::instance().pool_previous_[cold_->placement->type()];
        if (current_pool.load(std::memory_order_acquire) == this) {
            Buffet* nxt = next();
            current_pool.store(nxt, std::memory_order_release);
            for (size_t i = 0; i < 4; ++i) {
                nxt = nxt->next();
            }
            Buffet* previous = previous_pool.exchange(this, std::memory_order_acq_rel);
            if (previous != nullptr) {
                Slice* prev_root = previous->cold_->root.exchange(nullptr, std::memory_order_acquire);
                delete prev_root;
            }
        }
        if (start_offset + size_they_ll_get == size()) {
            return cold_->root.load(std::memory_order_acquire)->slice(start_offset, size_requested);
        }
        return next()->claim(size_requested);
    }
    return cold_->root.load(std::memory_order_acquire)->slice(start_offset, size_requested);
}
/** ------------------------------------------------------------------------------------------- Claim with Novel Buffer
 * @brief Bump-allocates a Slice from this chain, optionally creating a novel buffer.
 */
Slice Buffet::claim(size_t size, bool novel_buffer) {
    if (novel_buffer) {
        return novel_slice(size, cold_->placement, cold_->context);
    }
    return claim(size);
}
/** ------------------------------------------------------------------------------------------- Claim with Copy
 * @brief Bump-allocates a Slice from this chain and copies data into it.
 */
Slice Buffet::claim(const void* copy_from, size_t size, bool novel_buffer) {
    Slice slice = claim(size, novel_buffer);
    std::memcpy(slice.data<uint8_t>(), copy_from, size);
    return slice;
}
/** ------------------------------------------------------------------------------------------- Handle
 * @brief Returns the substrate handle associated with this allocation.
 * @return The substrate handle.
 */
void* Buffet::handle() {
    return cold_ ? cold_->handle : nullptr;
}
/** ------------------------------------------------------------------------------------------- Handle (const)
 * @brief Returns the substrate handle associated with this allocation (const version).
 * @return The substrate handle.
 */
const void* Buffet::handle() const {
    return cold_ ? cold_->handle : nullptr;
}
} // namespace buffetalligator
