#pragma once
/** --------------------------------------------------------------------------------------------------------- Alligator
 * @file alligator.hpp
 * @brief Internal multi-placement slab arena with background chain replenishment.
 */
#include <logging.hpp>
#include <buffetalligator.hpp>
#include <memory/buffet.hpp>
#include <memory/slicefriend.hpp>
#include <array>
#include <atomic>
#include <exception>
#include <thread>

namespace buffetalligator {
struct SizeAndContext {
    Placemat::Handle* (*allocator)(size_t size, void* context) = nullptr;
    void* context = nullptr;
    size_t size = 0;
    uint16_t placement_type = 0;
    Alligator* instance = nullptr;
    Placemat::Handle* handle = nullptr;
};
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
    std::vector<std::unique_ptr<std::atomic<Buffet*>>> pool_current_;
    std::vector<std::unique_ptr<std::atomic<Buffet*>>> pool_previous_;
    std::atomic<bool> stop_{false};
    std::thread worker_thread_;
    std::atomic<uint16_t> ring_head_{0};
    std::atomic<uint16_t> ring_tail_{0};
    std::atomic_flag enqueue_lock_ = ATOMIC_FLAG_INIT;
    std::array<BuffetOrder*, ORDER_RING_CAPACITY> orders_{nullptr};
    /** ------------------------------------------------------------------------------------------- Enqueue Order
     * @brief Adds a new order to the ring buffer.
     * @param order The order to enqueue.
     * @return True if the order was successfully enqueued, false if the ring is full.
     */
    void enqueue_order(BuffetOrder* order) {
        while (enqueue_lock_.test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        uint16_t head = ring_head_.load(std::memory_order_relaxed);
        uint16_t desired = static_cast<uint16_t>(head + 1);
        while (desired == ring_tail_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        orders_[head] = order;
        ring_head_.store(desired, std::memory_order_release);
        enqueue_lock_.clear(std::memory_order_release);
        ring_head_.notify_one();
    }
    /** ------------------------------------------------------------------------------------------- Spin Yield Wait
     * @brief Spins and yields until the ring tail is not equal to the ring head.
     * @return The updated tail index.
     */
    uint16_t spin_yield_wait() {
        uint16_t tail = ring_tail_.load(std::memory_order_acquire);
        int spins = 0;
        while (tail == ring_head_.load(std::memory_order_acquire)) {
            if (++spins > 1000) {
                ring_head_.wait(ring_head_.load(std::memory_order_acquire), std::memory_order_acquire);
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
    BuffetOrder* dequeue_order() {
        uint16_t tail = ring_tail_.load(std::memory_order_acquire);
        while (tail == ring_head_.load(std::memory_order_acquire)) {
            tail = spin_yield_wait();
        }
        BuffetOrder* order = orders_[tail];
        ring_tail_.store(static_cast<uint16_t>(tail + 1), std::memory_order_release);
        ring_tail_.notify_one();
        return order;
    }
    /** ------------------------------------------------------------------------------------------- Ensure Chains
     * @brief Ensures that all buffer chains are properly initialized.
     */
    static void ensure_chains();
    /** ------------------------------------------------------------------------------------------- Worker Loop
     * @brief Replenishes active chains without a dynamically allocating task queue.
     */
    void worker_loop();
    /** ------------------------------------------------------------------------------------------- Next Free Slot
     * @brief Atomically claims an Arena registry slot.
     * @param buffer The slot to publish.
     * @return The registry index.
     */
    void get_next_free_slot(Buffet* buffer) {
        size_t rounds = 0;
        while (true) {
            const uint32_t index = next_buffer_.fetch_add(1, std::memory_order_relaxed) & 0x1FFFF;
            if (bufs[index].load(std::memory_order_acquire) == nullptr) {
                Buffet* expected = nullptr;
                if (bufs[index].compare_exchange_weak(
                    expected, buffer, std::memory_order_release, std::memory_order_relaxed
                )) {
                    buffer->size_ = (buffer->size_ & ~0x1FFFF) | static_cast<uint32_t>(index);
                    return;
                }
            }
            if (++rounds > SLOT_CAPACITY * 2) {
                std::string error_message = "Alligator::get_next_free_slot: exceeded "
                    "maximum rounds while searching for a free slot";
                ALLIGATOR_THROW(error_message);
            }
        }
        ALLIGATOR_THROW("Arena::get_next_free_slot: every slab slot is occupied");
    }
    Alligator();
    ~Alligator();
    static Alligator& instance() {
        static Alligator alligator;
        return alligator;
    }
    friend class Buffet;
    friend class Placemat;
    friend class Slice;
    friend struct SizeAndContext;
    friend class Memory;
public:
    Alligator(const Alligator&) = delete;
    Alligator& operator=(const Alligator&) = delete;
    Alligator(Alligator&&) = delete;
    Alligator& operator=(Alligator&&) = delete;
    /** ------------------------------------------------------------------------------------------- Get
     * @brief Retrieves a Buffet instance by its registry index.
     * @param index The registry index.
     * @return The Buffet instance at the specified index.
     */
    static Buffet* get(size_t index);
};
} // namespace buffetalligator
