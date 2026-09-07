/** --------------------------------------------------------------------------------------------------------- Buffet Alligator
 * @file alligator.cpp
 * @brief Implementation of the Alligator class.
 */
#include <alligator.hpp>
#include <memory/alligator.hpp>
#include <memory/buffet.hpp>
#include <memory/slicefriend.hpp>
#include <memory/tracker.hpp>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Constructor & Destructor
 * @brief Implements the Alligator's constructor and destructor.
 */
Alligator::Alligator() {
    worker_thread_ = std::thread(&Alligator::worker_loop, this);
    ensure_chains();
    BuffetMenu::register_change_listener(
        this,
        [](void* myself) {
            if (myself) {
                static_cast<Alligator*>(myself)->ensure_chains();
            }
        }
    );
}
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Implements the Alligator's destructor.
 */
Alligator::~Alligator() {
    AtomicContainer* stop_signal = AtomicRegistry::get_global("stop_signal");
    if (!stop_signal->load<bool>(std::memory_order_acquire)) {
        stop_signal->store(true, std::memory_order_release);
    }
    enqueue_order(nullptr);
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}
/** ------------------------------------------------------------------------------------------- Chain Length
 * @brief Returns the length of the buffer chain for a given placement type.
 * @param placement_type The placement identifier.
 * @return The length of the buffer chain.
 */
void Alligator::ensure_chains() {
    const size_t placement_count = BuffetMenu::count();
    const size_t first_new_type = pool_current_.size();
    while (pool_current_.size() < placement_count) {
        pool_current_.emplace_back(std::make_unique<std::atomic<Buffet*>>(nullptr));
        pool_previous_.emplace_back(std::make_unique<std::atomic<Buffet*>>(nullptr));
    }
    std::vector<std::future<Buffet*>> pending;
    for (size_t type = first_new_type; type < placement_count; ++type) {
        auto [order, future] = SliceFriend::BuffetOrder::bind<Buffet*>(
            &Alligator::build_initial_chain, this, static_cast<uint16_t>(type)
        );
        enqueue_order(order.release());
        pending.emplace_back(std::move(future));
    }
    for (std::future<Buffet*>& future : pending) {
        future.get();
    }
}
/** ------------------------------------------------------------------------------------------- Build Initial Chain
 * @brief Worker task that builds a placement's first slab plus its successor and sets the
 * first as the active Buffet for that Placemat.
 */
Buffet* Alligator::build_initial_chain(uint16_t type) {
    const Placemat* placement = BuffetMenu::get(type);
    const size_t slab_bytes = static_cast<size_t>(placement->default_slab_size_) << 12;
    Buffet* first = new Buffet(placement, placement->get_context_(), slab_bytes, false);
    get_next_free_slot(first);
    Buffet* second = new Buffet(placement, placement->get_context_(), slab_bytes, false);
    get_next_free_slot(second);
    first->cold_->next.store(second, std::memory_order_release);
    pool_current_[type]->store(first, std::memory_order_release);
    return first;
}
/** ------------------------------------------------------------------------------------------- Worker Loop
 * @brief Replenishes active chains without a dynamically allocating task queue.
 */
void Alligator::worker_loop() {
    AtomicContainer* stop_signal = AtomicRegistry::get_or_create_global("stop_signal", false);
    while (!stop_signal->load<bool>(std::memory_order_acquire)) {
        void* order = dequeue_order();
        if (!order && stop_signal->load<bool>(std::memory_order_acquire)) {
            break;
        } else if (!order) {
            std::this_thread::yield();
            continue;
        }
        SliceFriend::BuffetOrder* order_ptr = static_cast<SliceFriend::BuffetOrder*>(order);
        if (order_ptr->main) {
            order_ptr->main(order_ptr);
        }
        delete order_ptr;
    }
}
/** ------------------------------------------------------------------------------------------- Dequeue Order
 * @brief Retrieves the next order from the ring buffer.
 * @return The next order, or nullptr if the ring is empty.
 */
void* Alligator::dequeue_order() {
    uint16_t tail = ring_tail_.load(std::memory_order_acquire);
    while (tail == ring_head_.load(std::memory_order_acquire)) {
        tail = spin_yield_wait();
    }
    void* order = orders_[tail];
    ring_tail_.store(static_cast<uint16_t>(tail + 1), std::memory_order_release);
    ring_tail_.notify_one();
    return order;
}
/** ------------------------------------------------------------------------------------------- Next Free Slot
 * @brief Atomically claims an Arena registry slot.
 * @param buffer The slab to publish.
 */
void Alligator::get_next_free_slot(Buffet* buffer) {
    size_t rounds = 0;
    while (true) {
        const uint32_t index = next_buffer_.fetch_add(1, std::memory_order_relaxed) & 0x1FFFF;
        if (bufs[index].load(std::memory_order_acquire) == nullptr) {
            buffer->size_ = (buffer->size_ & ~static_cast<uint64_t>(0x1FFFF)) | index;
            Buffet* expected = nullptr;
            if (bufs[index].compare_exchange_weak(
                expected, buffer, std::memory_order_release, std::memory_order_relaxed
            )) {
                if (buffer->cold_->next.load(std::memory_order_acquire) != Buffet::NOVEL_NEXT_SENTINEL) {
                    buffer->cold_->root.store(new Slice(buffer->slice()), std::memory_order_release);
                }
                Memory::record_allocation(*buffer->cold_->placement, buffer->size());
                return;
            }
        }
        if (++rounds > SLOT_CAPACITY * 2) {
            std::string error_message = "Alligator::get_next_free_slot: exceeded "
                "maximum rounds while searching for a free slot";
            ALLIGATOR_THROW(error_message);
        }
    }
}
/** ------------------------------------------------------------------------------------------- Enqueue Order
 * @brief Adds a new order to the ring buffer.
 * @param order The order to enqueue.
 */
void Alligator::enqueue_order(void* order) {
    SliceFriend::BuffetOrder* order_ptr = static_cast<SliceFriend::BuffetOrder*>(order);
    while (enqueue_lock_.test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    uint16_t head = ring_head_.load(std::memory_order_relaxed);
    uint16_t desired = static_cast<uint16_t>(head + 1);
    while (desired == ring_tail_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    orders_[head] = order_ptr;
    ring_head_.store(desired, std::memory_order_release);
    enqueue_lock_.clear(std::memory_order_release);
    ring_head_.notify_one();
}
} // namespace buffetalligator
