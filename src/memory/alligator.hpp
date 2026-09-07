#pragma once
/** --------------------------------------------------------------------------------------------------------- Alligator
 * @file alligator.hpp
 * @brief Internal multi-placement slab arena with background chain replenishment.
 */
#include <logging.hpp>
#include <alligator.hpp>
#include <memory/buffet.hpp>
#include <memory/slicefriend.hpp>
#include <array>
#include <atomic>
#include <exception>
#include <thread>

namespace buffetalligator {
/** ------------------------------------------------------------------------------------------- Heap Allocate
 * @brief Allocates one zeroed slab from the C heap.
 * @param size The slab size in bytes.
 * @return The handle wrapping the zeroed block.
 */
inline static Placemat::Handle* heap_allocate(size_t size, void*) {
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
inline static Placemat::Handle* aligned_heap_allocate(size_t size, void*) {
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
inline static void heap_deallocate(Placemat::Handle* handle, void*) {
    if (handle != nullptr && handle->substrate_handle != nullptr) {
        std::free(handle->substrate_handle);
    }
}
/** ------------------------------------------------------------------------------------------- Heap Host Pointer
 * @brief Returns the host-visible base pointer of a heap slab.
 * @param handle The handle wrapping the block.
 * @return The block's base pointer.
 */
inline static void* heap_host_ptr(Placemat::Handle* handle) {
    return handle->substrate_handle;
}
/** ------------------------------------------------------------------------------------------- Heap Context
 * @brief The heap placements carry no context.
 * @return Always returns nullptr.
 */
inline static void* heap_context() {
    return nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Arena
 * @class Alligator
 * @brief Maintains one preallocated Buffer chain per registered Placemat.
 */
class Alligator {
private:
    inline static constexpr size_t SLOT_CAPACITY = 0x20000;
    inline static constexpr size_t ORDER_RING_CAPACITY = 0x10000;
    std::array<std::atomic<Buffet*>, SLOT_CAPACITY> bufs{};
    std::atomic<uint64_t> next_buffer_{0};
    std::vector<std::unique_ptr<std::atomic<Buffet*>>> pool_current_;  /// This is a vector on purpose. It holds the current active Buffer pointer for each Placemat* it is NOT the same as bufs.
    std::vector<std::unique_ptr<std::atomic<Buffet*>>> pool_previous_;   /// One version behind the above.
    std::atomic<bool> stop_{false};
    std::thread worker_thread_;
    std::atomic<uint16_t> ring_head_{0};
    std::atomic<uint16_t> ring_tail_{0};
    std::atomic_flag enqueue_lock_ = ATOMIC_FLAG_INIT;
    std::array<void*, ORDER_RING_CAPACITY> orders_{nullptr};
    /** ------------------------------------------------------------------------------------------- Enqueue Order
     * @brief Adds a new order to the ring buffer.
     * @param order The order to enqueue.
     */
    void enqueue_order(void* order);
    /** ------------------------------------------------------------------------------------------- Spin Yield Wait
     * @brief Spins and yields until the ring tail is not equal to the ring head.
     * @return The updated tail index.
     */
    uint16_t spin_yield_wait() {
        uint16_t tail = ring_tail_.load(std::memory_order_acquire);
        int spins = 0;
        while (tail == ring_head_.load(std::memory_order_acquire)) {
            if (++spins > 1000) {
                ring_head_.wait(tail, std::memory_order_acquire);
            } else {
                std::this_thread::yield();
            }
            tail = ring_tail_.load(std::memory_order_acquire);
        }
        return tail;
    }
    /** ------------------------------------------------------------------------------------------- Dequeue Order
     * @brief Retrieves the next order from the ring buffer.
     * @return The next order, or nullptr if the ring is empty.
     */
    void* dequeue_order();
    /** ------------------------------------------------------------------------------------------- Worker Loop
     * @brief Executes queued orders and replenishes runway.
     */
    void worker_loop();
    /** ------------------------------------------------------------------------------------------- Next Free Slot
     * @brief Atomically claims an Arena registry slot.
     * @param buffer The slab to publish.
     */
    void get_next_free_slot(Buffet* buffer);
    /** ------------------------------------------------------------------------------------------- Ensure Chains
     * @brief Ensures that all buffer chains are properly initialized.
     */
    void ensure_chains();
    /** ------------------------------------------------------------------------------------------- Build Initial Chain
     * @brief Worker task that builds a placement's first slab plus its successor and publishes
     * the pool head.
     * @param type The placement identifier.
     * @return The published pool head.
     */
    Buffet* build_initial_chain(uint16_t type);
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Initializes and cleans up the Alligator instance.
     */
    Alligator();
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Cleans up the Alligator instance.
     */
    ~Alligator();
    static Alligator& instance() {
        static Alligator inst;
        return inst;
    }
    friend class Buffet;
    friend class Placemat;
    friend class Slice;
    friend class Memory;
    friend class SliceFriend;
public:
    Alligator(const Alligator&) = delete;
    Alligator& operator=(const Alligator&) = delete;
    Alligator(Alligator&&) = delete;
    Alligator& operator=(Alligator&&) = delete;
    /** ------------------------------------------------------------------------------------------- Get
     * @brief Resolves a registry slot to its slab.
     * @param slot The registry slot held in a Slice's meta word.
     * @return The slab, or nullptr when the slot is unoccupied.
     */
    Buffet* get(uint32_t slot) {
        Buffet* b = bufs[slot & 0x1FFFF].load(std::memory_order_acquire);
        while (b == nullptr) {
            b = bufs[slot & 0x1FFFF].load(std::memory_order_acquire);
        }
        return b;
    }
    /** ------------------------------------------------------------------------------------------- Current for Placement
     * @brief Returns the current buffer for a given placement type.
     * @param placement_type The placement identifier.
     * @return The current buffer for the specified placement type.
     */
    Buffet* current_for_placement(uint16_t placement_type) {
        Buffet* head = pool_current_[placement_type]->load(std::memory_order_acquire);
        while (head == nullptr) {
            std::this_thread::yield();
            head = pool_current_[placement_type]->load(std::memory_order_acquire);
        }
        return head;
    }
};
} // namespace buffetalligator
