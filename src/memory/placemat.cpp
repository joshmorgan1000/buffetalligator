/** --------------------------------------------------------------------------------------------------------- Plate implementation
 * @file plate.cpp
 * @brief Implementation of the Plate structure and its associated functions.
 */
#include <alligator.hpp>
#include <memory/plate.hpp>

namespace buffetalligator {
/** ------------------------------------------------------------------------------------------- Current Handle
 * @brief Retrieves the current handle (plate) associated with this placemat.
 * @return Pointer to the current plate.
 */
Plate* Placemat::current_plate() const {
    Plate* current = current_handle_.load(std::memory_order_acquire);
    while (current == nullptr || current == reinterpret_cast<Plate*>(-1ll)) [[unlikely]] {
        Plate* expected = nullptr;
        if (current == nullptr && current_handle_.compare_exchange_strong(
            expected, reinterpret_cast<Plate*>(-1ll),
            std::memory_order_acq_rel, std::memory_order_acquire)
        ) {
            std::pair<void*, void*> allocation;
            try {
                allocation = allocator_(default_slab_size_, get_context_());
            } catch (...) {
                current_handle_.store(nullptr, std::memory_order_release);
                throw;
            }
            current = new Plate(const_cast<Placemat*>(this), default_slab_size_, allocation.second, false);
            record_slab_allocation(current);
            current_handle_.store(current, std::memory_order_release);
            (void)current->next(1);
            return current;
        }
        std::this_thread::yield();
        current = current_handle_.load(std::memory_order_acquire);
    }
    return current;
}
/** ------------------------------------------------------------------------------------------- Create Base Slice ID
 * @brief Occupies one pool slot spanning a whole slab so views can be cut from it; only the
 * plate's release in record_slab_release ever frees it.
 * @param placemat Pointer to the placemat.
 * @param size Size of the allocation.
 * @param substrate_handle Substrate handle associated with the allocation.
 * @return The base slice identifier.
 */
uint32_t Placemat::create_base_slice_id(Placemat* placemat, size_t size, void* substrate_handle) {
    Alligator& alligator = Alligator::inst();
    const SliceId slice_id = alligator.next_id();
    GPUBuf gpu = placemat->get_gpu_buf_(substrate_handle);
    gpu.size = static_cast<uint32_t>(size);
    *alligator.gpubuf(slice_id) = gpu;
    *alligator.host_ptr(slice_id) = placemat->get_host_ptr_(substrate_handle);
    return slice_id;
}
} // namespace buffetalligator