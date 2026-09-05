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
    Alligator::instance().get_next_free_slot(this);
    if (is_novel) {
        bump_offset_.store(size << 6, std::memory_order_release);
    } else {
        Slice* new_slice = new Slice();
        new_slice->meta_ = size_;
        new_slice->cached_ = cold_->placement->get_host_ptr_(cold_->handle);
        cold_->root.store(new_slice, std::memory_order_release);
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
        if (deleter->cold_->root.load(std::memory_order_acquire) != nullptr) {
            delete deleter->cold_->root.load(std::memory_order_acquire);
            deleter->cold_->root.store(nullptr, std::memory_order_release);
        }
        if (deleter->cold_->handle != nullptr) {
            if (deleter->cold_->placement != nullptr) {
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
void buffet_order_deleter(BuffetOrder* order) {
    if (order != nullptr) {
        delete order;
    }
}
}
void Buffet::free() {
    if (cold_ && ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        DeleteCold* deleter = new DeleteCold();
        deleter->cold_ = cold_;
        deleter->buffet_ = this;
        BuffetOrder* order = new BuffetOrder(static_cast<void*>(deleter), &delete_cold, &buffet_order_deleter);
        Alligator::instance().enqueue_order(order);
        Alligator::instance().bufs[deleter->buffet_->size_ & 0x1FFFF].store(nullptr, std::memory_order_release);
        cold_ = nullptr;
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
    Slice slice;
    slice.cached_ = buffet->cold_->placement->get_host_ptr_(buffet->cold_->handle);
    slice.meta_ = (size << 17) | (buffet->size_ & 0x1FFFF);
    return slice;
}
/** ------------------------------------------------------------------------------------------- Make Active
 * @brief Marks this slab as its pool's current claim target, demoting the occupant to
 * previous status and dropping the self-reference of the slab two generations back.
 */
void Buffet::make_active() {
    Buffet* previous = Alligator::instance().pool_current_[size_ & 0x1FFFF]->exchange(this, std::memory_order_acq_rel);
    if (previous != nullptr) {
        previous = Alligator::instance().pool_previous_[size_ & 0x1FFFF]->exchange(previous, std::memory_order_acq_rel);
        if (previous != nullptr) {
            DeleteCold* deleter = new DeleteCold();
            deleter->buffet_ = previous;
            deleter->cold_ = previous->cold_;
            BuffetOrder* order = new BuffetOrder(static_cast<void*>(deleter), &delete_cold, &buffet_order_deleter);
            Alligator::instance().enqueue_order(order);
        }
    }
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
            cold_->next.store(fresh, std::memory_order_release);
            fresh->make_active();
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
    if (cold_ == nullptr) [[unlikely]] {
        return Slice();
    }
    if (size_requested >= size()) {
        return novel_slice(size_requested, cold_->placement, cold_->context);
    }
    size_t start_offset = bump_offset_.fetch_add(size_requested, std::memory_order_relaxed);
    if (start_offset + size_requested > size()) {
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
