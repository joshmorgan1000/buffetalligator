/** --------------------------------------------------------------------------------------------------------- Heap Buffet
 * @file heapbuffet.cpp
 * @brief Built-in heap and aligned-heap placements plus their one-time registration.
 */
#include <alligator.hpp>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

extern "C" void ba_net_shutdown(void);

namespace buffetalligator {
namespace {
/** ------------------------------------------------------------------------------------------- Heap Allocate
 * @brief Allocates one zeroed slab from the C heap.
 * @param size The slab size in bytes.
 * @return The host pointer and the freeable allocation (the same block here).
 */
std::pair<void*, void*> heap_allocate(size_t size, void*) {
    void* memory = std::calloc(1, size);
    if (memory == nullptr) [[unlikely]] {
        throw std::bad_alloc();
    }
    return {memory, memory};
}
/** ------------------------------------------------------------------------------------------- Aligned Heap Allocate
 * @brief Allocates one zeroed, 64-byte-aligned slab from the C heap.
 * @param size The slab size in bytes.
 * @return The aligned host pointer and the freeable allocation (the same block here).
 */
std::pair<void*, void*> aligned_heap_allocate(size_t size, void*) {
    void* memory = nullptr;
    if (posix_memalign(&memory, 64, size) != 0) [[unlikely]] {
        throw std::bad_alloc();
    }
    std::memset(memory, 0, size);
    return {memory, memory};
}
/** ------------------------------------------------------------------------------------------- Heap Deallocate
 * @brief Frees a heap slab's block.
 * @param host_ptr The host pointer of the block.
 * @param substrate_handle The freeable allocation.
 * @return The cleared host pointer and substrate handle pair.
 */
std::pair<void*, void*> heap_deallocate(void* host_ptr, void* substrate_handle) {
    static_cast<void>(host_ptr);
    std::free(substrate_handle);
    return {nullptr, nullptr};
}
/** ------------------------------------------------------------------------------------------- Heap Context
 * @brief The heap placements carry no context.
 * @return Always returns nullptr.
 */
void* heap_context() {
    return nullptr;
}
/** ------------------------------------------------------------------------------------------- Heap Try Resize
 * @brief Resizes one whole-block heap allocation with realloc, which expands in place whenever
 * the allocator can. Failure returns nulls and leaves the original block untouched.
 * @param host_ptr The block's host pointer.
 * @param substrate_handle The freeable allocation.
 * @param new_size The requested size in bytes.
 * @return The resized pair, or nulls when realloc failed.
 */
std::pair<void*, void*> heap_try_resize(void* host_ptr, void* substrate_handle, size_t new_size) {
    static_cast<void>(host_ptr);
    void* resized = std::realloc(substrate_handle, new_size);
    if (resized == nullptr) [[unlikely]] {
        return {nullptr, nullptr};
    }
    return {resized, resized};
}
}
/** --------------------------------------------------------------------------------------------------------- Host Placements
 * @brief The heap built-ins resolved once at load; PAGE_ALIGNED stays null until MmapAllocator
 * registers the page-backed placement.
 */
const Placemat* const Placemat::HEAP = BuffetMenu::get("heap");
const Placemat* const Placemat::HEAP_ALIGNED = BuffetMenu::get("aligned_heap");
const Placemat* Placemat::PAGE_ALIGNED = nullptr;
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
        &heap_context,
        false,
        &heap_try_resize
    );
    register_type_unchecked(
        "aligned_heap",
        64ull * 1024 * 1024,
        64,
        &aligned_heap_allocate,
        &heap_deallocate,
        &heap_context,
        true
    );
    menu.builtins_ready_.store(true, std::memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Shutdown
 * @brief Stops channel reactors at application quiescence before the process exits.
 */
void BuffetMenu::shutdown() { ba_net_shutdown(); }
} // namespace buffetalligator
