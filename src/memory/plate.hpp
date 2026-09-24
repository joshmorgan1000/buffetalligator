#pragma once
/** --------------------------------------------------------------------------------------------------------- Plate
 * @file plate.hpp
 * @brief Defines the Plate structure used within a Placemat.
 */
#include <alligator.hpp>

namespace buffetalligator {
/** ------------------------------------------------------------------------------------------- Plate
 * @struct Plate
 * @brief A `Plate` is a specific allocation of memory within a given `Placemat`. One might
 * call these "slabs" in some contexts, they are large contiguous blocks of memory that the
 * framework hands out sub-slices of.
 */
struct Plate {
    /// @brief The placemat that owns this plate.
    Placemat* placemat;
    /// @brief The size of the allocation in bytes.
    size_t size;
    /// @brief The special handle associated with this allocation.
    /// Example: vk::Device for Vulkan, or CUcontext for CUDA.
    void* substrate_handle;
    /** ----------------------------------------------------------------------------- next atomic
     * @brief We try to pre-allocate a slab or two ahead of the current bump pointer
     * so that contention around boundaries is minimized when allocating new slices.
     */
    std::atomic<Plate*> next_buf;
    /** ----------------------------------------------------------------------------- Slice ID
     * @brief The identifier for the slice within the plate.
     */
    std::atomic<uint32_t> slice_id{0xFFFFFFFFu};
    /// @brief When this reaches zero, that means nothing is referencing this handle
    /// anymore, so it gets freed or recycled.
    std::atomic<int> ref_count;
    /// @brief The bump pointer for this handle, used to slice the slab into smaller
    /// allocations.
    std::atomic<size_t> bump;
    /** ----------------------------------------------------------------------------- Next
     * @brief Retrieves or allocates the next handle in the pre-allocated slab
     * chain, or allocates a new handle if the next handle is not available yet.
     * @param ahead_alloc The number of plates to make sure are pre-allocated ahead
     * of the current plate.
     */
    Plate* next(size_t ahead_alloc) {
        Plate* current_next = next_buf.load(std::memory_order_acquire);
        while (current_next == nullptr
            || current_next == reinterpret_cast<Plate*>(-1ll)
        ) {
            Plate* expected = nullptr;
            if (next_buf.compare_exchange_strong(
                expected,
                reinterpret_cast<Plate*>(-1ll),
                std::memory_order_acq_rel)
            ) {
                auto [host_ptr, substrate_handle] =
                    placemat->allocate()(size, placemat->get_context()());
                current_next = new Plate(
                    placemat,
                    size,
                    substrate_handle,
                    false
                );
                Placemat::record_slab_allocation(current_next);
                next_buf.store(current_next, std::memory_order_release);
                break;
            }
            std::this_thread::yield();
            current_next = next_buf.load(std::memory_order_acquire);
        }
        if (ahead_alloc > 0) {
            (void)current_next->next(ahead_alloc - 1);
        }
        return current_next;
    }
    /** ----------------------------------------------------------------------------- Free Plate
     * @brief Decrements the reference count and frees the plate if it reaches zero.
     */
    /// @brief Parked reference count of a released plate, so a late claim can never revive it.
    inline static constexpr int RELEASED = INT32_MIN / 2;
    void free() {
        if (ref_count && ref_count.fetch_sub(1, std::memory_order_acquire) == 1) {
            HostPtr host_ptr = placemat->get_host_ptr()(substrate_handle);
            auto [a, b] = placemat->deallocate()(host_ptr.ptr, substrate_handle);
            host_ptr = HostPtr(a);
            substrate_handle = b;
            Placemat::record_slab_release(this);
        }
    }
    /** ----------------------------------------------------------------------------- Claim
     * @brief Claims a sub-allocation from this plate if the plate has it available.
     * If it does not, we try the next plate in the chain.
     * @param size The size of the sub-allocation to claim.
     * @return A tuple containing the plate pointer, the current bump pointer, and
     * the size of the allocation - this area in memory is now owned by the caller.
     */
    Slice claim(size_t size, bool novel_buffer = false) {
        if (size > this->size || novel_buffer) {
            // The requested size is larger than we normally allocate the entire
            // plate for, so we're just going to give you your own dedicated
            // allocation.
            auto [a, b] = placemat->allocate()(size, placemat->get_context()());
            SliceId slice_id = Alligator::inst().next_id();
            Plate* novel = new Plate(placemat, size, b, true);
            Placemat::record_slab_allocation(novel);
            Alligator::inst().plate(slice_id) = novel;
            (*Alligator::inst().gpubuf(slice_id)) = placemat->get_gpu_buf()(novel->substrate_handle);
            (*Alligator::inst().host_ptr(slice_id)) = HostPtr{a};
            return Slice(slice_id);
        }
        // Claims advance by the placement's alignment so every slice start stays aligned.
        const size_t alignment = placemat->bump_alignment_;
        const size_t aligned_size = (size + alignment - 1) & ~(alignment - 1);
        size_t prev_bump = bump.fetch_add(aligned_size, std::memory_order_acquire);
        if (prev_bump + size > this->size) {
            // We don't subtract the bump because that can create a race condition.
            // We'll just call this one full. Here we do our trick where we
            // delete the slice that we've been holding as a token.
            // If it weren't for the ref_count add a few lines up, this could
            // potentially trigger a call to `free()` prematurely.
            uint32_t slice_id_swap = slice_id.exchange(0xFFFFFFFFu, std::memory_order_acquire);
            uint32_t* slice_id_ptr = new uint32_t(slice_id_swap);
            Slice* swap = reinterpret_cast<Slice*>(slice_id_ptr);
            if (swap != nullptr) delete swap;  // Calls the Slice destructor for the slice ID
            // Publish the ready successor only if this plate is still the head.
            Plate* successor = next(0);
            if (placemat->set_current_plate(this, successor)) {
                // The winner keeps two slabs ahead of the new head.
                (void)successor->next(1);
            }
            // Claim from the successor before releasing the pin on this plate.
            Slice result = successor->claim(size);
            // NOW we can safely free this plate without affecting the claim we
            // just made.
            if (slice_id_swap != 0xFFFFFFFFu) Alligator::inst().submit([](Plate* plate) { plate->free(); }, this);
            return result;
        }
        // If we reach here, it means we successfully claimed from this plate.
        SliceId slice_id = Alligator::inst().next_id();
        Alligator::inst().plate(slice_id) = this;
        (*Alligator::inst().gpubuf(slice_id)) = placemat->get_gpu_buf()(substrate_handle);
        Alligator::inst().gpubuf(slice_id)->offset = prev_bump;
        Alligator::inst().gpubuf(slice_id)->size = size;
        (*Alligator::inst().host_ptr(slice_id)) = placemat->get_host_ptr()(substrate_handle);
        Alligator::inst().host_ptr(slice_id)->ptr =
            static_cast<uint8_t*>(placemat->get_host_ptr()(substrate_handle).ptr) + prev_bump;
        return Slice(slice_id);
    }
    /** ----------------------------------------------------------------------------- Constructor
     * @brief Constructs a new Plate object.
     * @param placemat_ Pointer to the placemat this plate belongs to, so it knows
     * where it lives.
     * @param size_ Size of the plate.
     * @param host_ptr_ Host pointer associated with the plate.
     * @param substrate_handle_ Substrate handle associated with the plate.
     * @param final_course Indicates if this is a novel allocation or if it is part
     * of the chain of buffer allocations.
     */
    Plate(
        Placemat* placemat_,
        size_t size_,
        void* substrate_handle_,
        bool final_course
    ) : placemat(placemat_)
    , size(size_)
    , substrate_handle(substrate_handle_)
    , next_buf(nullptr)
    , slice_id(0xFFFFFFFFu)
    , ref_count(0)
    , bump(0) {
        if (final_course) {
            bump.store(size_, std::memory_order_release);
        } else {
            ref_count.store(1, std::memory_order_release);
        }
        slice_id.store(
            Placemat::create_base_slice_id(placemat, size, substrate_handle),
            std::memory_order_release
        );
    }
    /** ----------------------------------------------------------------------------------------------------- No Copy/Move */
    Plate(const Plate& other) = delete;
    Plate& operator=(const Plate& other) = delete;
    Plate(Plate&& other) = delete;
    Plate& operator=(Plate&& other) = delete;
    /** ----------------------------------------------------------------------------- Destructor
     * @brief Destroys the Plate object and frees its resources.
     */
    ~Plate() { free(); }
};
} // namespace buffetalligator