/** --------------------------------------------------------------------------------------------------------- Buffet Alligator
 * @file alligator.cpp
 * @brief Implementation of the Alligator class.
 */
#include <buffetalligator.hpp>
#include <memory/alligator.hpp>
#include <memory/buffet.hpp>


namespace buffetalligator {
namespace {
/** --------------------------------------------------------------------------------------------------------- Allocate Buffer For
 * @brief Allocates a buffer for the given SizeAndContext.
 * @param context The context containing the SizeAndContext.
 * @return Always returns nullptr.
 */
inline static void* allocate_buffer_for(void* context) {
    SizeAndContext* sac = static_cast<SizeAndContext*>(context);
    sac->handle = sac->allocator(sac->size, sac->context);
    return nullptr;
}
/** --------------------------------------------------------------------------------------------------------- SAC Deleter
 * @brief Custom deleter for SizeAndContext wrapped in a BuffetOrder.
 * @param order The BuffetOrder containing the SizeAndContext to be deleted.
 */
inline static void sac_deleter(BuffetOrder* order) {
    if (order) {
        if (order->context) {
            SizeAndContext* sac = static_cast<SizeAndContext*>(order->context);
            delete sac;
        }
        delete order;
    }
}

} // anonymous namespace
/** --------------------------------------------------------------------------------------------------------- Constructor & Destructor
 * @brief Implements the Alligator's constructor and destructor.
 */
Alligator::Alligator() {
    BuffetMenu::register_change_listener(
        this,
        [](void* myself) {
            if (myself) {
                static_cast<Alligator*>(myself)->ensure_chains();
            }
        }
    );
    for (size_t i = 0; i < pool_current_.size(); ++i) {
        pool_previous_[i]->store(nullptr, std::memory_order_release);
        const Placemat* placement = BuffetMenu::get(static_cast<uint16_t>(i));
        SizeAndContext* sac = new SizeAndContext();
        sac->allocator = placement->alligator_;
        sac->context = placement->get_context_();
        sac->size = placement->default_slab_size_;
        sac->placement_type = static_cast<uint16_t>(i);
        sac->instance = this;
        BuffetOrder* order = new BuffetOrder(
            sac,
            &allocate_buffer_for,
            &sac_deleter
        );
        enqueue_order(order);
    }
    worker_thread_ = std::thread(&Alligator::worker_loop, this);
    worker_thread_.detach();
}
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Implements the Alligator's destructor.
 */
Alligator::~Alligator() {
    LOG_DEBUG_STREAM << "Alligator shutting down.";
    AtomicContainer* stop_signal = AtomicRegistry::get_global("stop_signal");
    if (stop_signal) {
        stop_signal->store(true, std::memory_order_release);
    } else {
        LOG_ERROR_STREAM << "Failed to retrieve stop_signal from AtomicRegistry.";
    }
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
    if (instance().pool_current_.size() < BuffetMenu::count()) {
        instance().pool_current_.resize(BuffetMenu::count());
        for (size_t i = 0; i < instance().pool_current_.size(); ++i) {
            if (!instance().pool_current_[i]) {
                instance().pool_current_[i] = std::make_unique<std::atomic<Buffet*>>(nullptr);
                const Placemat* placement = BuffetMenu::get(static_cast<uint16_t>(i));
                Buffet* buf = new Buffet(
                    placement,
                    placement->get_context_(),
                    placement->default_slab_size_,
                    false
                );
                instance().pool_current_[i]->store(buf, std::memory_order_release);
            }
        }
        instance().pool_previous_.resize(BuffetMenu::count());
        for (size_t i = 0; i < instance().pool_previous_.size(); ++i) {
            if (!instance().pool_previous_[i]) {
                instance().pool_previous_[i] = std::make_unique<std::atomic<Buffet*>>(nullptr);
            }
        }
    }
    for (size_t i = 0; i < instance().pool_current_.size(); ++i) {
        Buffet* buf = instance().pool_current_[i]->load(std::memory_order_acquire);
        size_t length = 4;
        while (0 < --length) {
            buf = buf->next();
        }
    }
}
/** ------------------------------------------------------------------------------------------- Worker Loop
 * @brief Replenishes active chains without a dynamically allocating task queue.
 */
void Alligator::worker_loop() {
    AtomicContainer* stop_signal = AtomicRegistry::get_or_create_global("stop_signal", false);
    while (!stop_signal->load<bool>(std::memory_order_acquire)) {
        BuffetOrder* order = dequeue_order();
        if (order == nullptr) [[unlikely]] {
            if (stop_signal->load<bool>(std::memory_order_acquire)) [[unlikely]] {
                break;
            } else {
                std::this_thread::yield();
            }
        }
        if (order->promise) {
            order->promise->set_value(order->task(order->context));
            delete order;
        } else {
            order->task(order->context);
            delete order;
        }
    }
}
/** ------------------------------------------------------------------------------------------- Get Buffet
 * @brief Retrieves the buffet at the specified index.
 * @param index The index of the buffet.
 * @return The buffet at the given index.
 */
Buffet* Alligator::get(size_t index) {
    return instance().bufs[index & 0x1FFFF].load(std::memory_order_acquire);
}
} // namespace buffetalligator