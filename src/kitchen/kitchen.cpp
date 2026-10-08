/** --------------------------------------------------------------------------------------------------------- Kitchen
 * @file kitchen.cpp
 * @brief Runs the shared Order queue on the available hardware threads.
 */
#include <alligator/kitchen.hpp>
#include <logging.hpp>
#include <loggingutils.hpp>
#include <chrono>
#include <iterator>
#include <new>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Constructor
 * @brief Starts workers using the hardware's reported concurrency.
 */
Kitchen::Kitchen() {
    const size_t count = std::thread::hardware_concurrency();
    if (count == 0) ALLIGATOR_KITCHEN_THROW("Kitchen could not query the hardware thread count");
    LOG_INFO_STREAM << "Kitchen: starting " << count << " workers";
    threads_.reserve(count);
    try {
        for (size_t index = 0; index < count; ++index) {
            threads_.emplace_back(std::make_unique<std::thread>(&Kitchen::work, this));
        }
    } catch (...) {
        stop_.store(true, std::memory_order_release);
        for (auto& thread : threads_) thread->join();
        throw;
    }
}
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Completes accepted orders before joining every worker.
 */
Kitchen::~Kitchen() {
    LOG_INFO_STREAM << "Kitchen: completing queued orders and stopping workers";
    stop_.store(true, std::memory_order_release);
    for (auto& thread : threads_) thread->join();
}
/** --------------------------------------------------------------------------------------------------------- Work
 * @brief Executes queued orders until shutdown observes an empty queue.
 * @param kitchen The shared queue owner.
 */
void Kitchen::work(Kitchen* kitchen) {
    for (;;) {
        Order order;
        if (kitchen->order_queue_.wait_dequeue_timed(order, std::chrono::milliseconds(100))) {
            order.execute();
        } else if (kitchen->stop_.load(std::memory_order_acquire)) {
            return;
        }
    }
}
/** --------------------------------------------------------------------------------------------------------- Submit Internal
 * @brief Transfers one Order into the queue and reports allocation failure.
 * @param order The Order to submit.
 */
void Kitchen::submit_(Order&& order) {
    if (!order_queue_.enqueue(std::move(order))) {
        ALLIGATOR_KITCHEN_THROW("Kitchen could not allocate queue storage for the Order");
    }
}
/** --------------------------------------------------------------------------------------------------------- Submit Bulk Internal
 * @brief Transfers an array of Orders into the queue and reports allocation failure.
 * @param orders The array of Orders to submit.
 * @param count The number of Orders in the array.
 */
void Kitchen::submit_bulk_(Order* orders, size_t count) {
    if (!order_queue_.enqueue_bulk(std::make_move_iterator(orders), count)) {
        ALLIGATOR_KITCHEN_THROW("Kitchen could not allocate queue storage for the Orders");
    }
}
/** --------------------------------------------------------------------------------------------------------- Kitchen Storage
 * @brief Reserves singleton storage before public translation units initialize.
 */
alignas(Kitchen) static unsigned char kitchen_storage[sizeof(Kitchen)];
static size_t kitchen_initializer_count = 0;
/** --------------------------------------------------------------------------------------------------------- Initializer
 * @brief Constructs the logger before starting the first Kitchen owner.
 */
KitchenInitializer::KitchenInitializer() {
    if (kitchen_initializer_count++ == 0) {
        static_cast<void>(threadsafe_logger::logging::GlobalLoggingContext::instance());
        new (kitchen_storage) Kitchen();
    }
}
/** --------------------------------------------------------------------------------------------------------- Finalizer
 * @brief Releases Kitchen after its final public owner has been destroyed.
 */
KitchenInitializer::~KitchenInitializer() {
    if (--kitchen_initializer_count == 0) Kitchen::inst().~Kitchen();
}
/** --------------------------------------------------------------------------------------------------------- Instance
 * @brief Resolves the Kitchen retained by public initializers.
 * @return The shared Kitchen instance.
 */
Kitchen& Kitchen::inst() {
    return *std::launder(reinterpret_cast<Kitchen*>(kitchen_storage));
}
} // namespace buffetalligator
