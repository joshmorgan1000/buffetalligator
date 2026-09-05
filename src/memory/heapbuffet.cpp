/** --------------------------------------------------------------------------------------------------------- Heap Buffet
 * @file heapbuffet.cpp
 * @brief Built-in heap and aligned-heap placements plus their one-time registration.
 */
#include <buffetalligator.hpp>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

namespace buffetalligator {
namespace {
/** ------------------------------------------------------------------------------------------- Heap Allocate
 * @brief Allocates one zeroed slab from the C heap.
 * @param size The slab size in bytes.
 * @return The handle wrapping the zeroed block.
 */
Placemat::Handle* heap_allocate(size_t size, void*) {
    void* memory = std::calloc(1, size);
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    return new Placemat::Handle{memory, nullptr};
}
/** ------------------------------------------------------------------------------------------- Aligned Heap Allocate
 * @brief Allocates one zeroed, 64-byte-aligned slab from the C heap.
 * @param size The slab size in bytes.
 * @return The handle wrapping the zeroed block.
 */
Placemat::Handle* aligned_heap_allocate(size_t size, void*) {
    void* memory = nullptr;
    if (posix_memalign(&memory, 64, size) != 0) {
        throw std::bad_alloc();
    }
    std::memset(memory, 0, size);
    return new Placemat::Handle{memory, nullptr};
}
/** ------------------------------------------------------------------------------------------- Heap Deallocate
 * @brief Frees a heap slab's block; the handle itself is deleted by the framework.
 * @param handle The handle wrapping the block.
 */
void heap_deallocate(Placemat::Handle* handle, void*) {
    if (handle != nullptr && handle->substrate_handle != nullptr) {
        std::free(handle->substrate_handle);
    }
}
/** ------------------------------------------------------------------------------------------- Heap Host Pointer
 * @brief Returns the host-visible base pointer of a heap slab.
 * @param handle The handle wrapping the block.
 * @return The block's base pointer.
 */
void* heap_host_ptr(Placemat::Handle* handle) {
    return handle->substrate_handle;
}
/** ------------------------------------------------------------------------------------------- Heap Context
 * @brief The heap placements carry no context.
 * @return Always returns nullptr.
 */
void* heap_context() {
    return nullptr;
}
}
/** --------------------------------------------------------------------------------------------------------- Ensure Builtins Slow
 * @brief Registers the heap (0) and aligned-heap (1) placements exactly once; a caller that
 * loses the claim waits until the identifiers are stable.
 */
void BuffetMenu::ensure_builtins_slow() {
    BuffetMenu& menu = instance();
    bool unclaimed = false;
    if (!menu.builtins_claimed_.compare_exchange_strong(unclaimed, true, std::memory_order_acq_rel)) {
        while (!menu.builtins_ready_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return;
    }
    register_type_unchecked(
        "heap",
        64ull * 1024 * 1024,
        16,
        &heap_allocate,
        &heap_deallocate,
        &heap_host_ptr,
        &heap_context,
        false
    );
    register_type_unchecked(
        "aligned_heap",
        64ull * 1024 * 1024,
        64,
        &aligned_heap_allocate,
        &heap_deallocate,
        &heap_host_ptr,
        &heap_context,
        true
    );
    menu.builtins_ready_.store(true, std::memory_order_release);
}
} // namespace buffetalligator
