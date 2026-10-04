/** --------------------------------------------------------------------------------------------------------- Kitchen
 * @file kitchen.cpp
 * @brief Prestarted compute and waiting pools with bounded allocation-free queue submissions.
 */
#include <alligator/kitchen.hpp>
#include <logging.hpp>
#include <loggingutils.hpp>
#include <moodycamel/blockingconcurrentqueue.h>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <iterator>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Kitchen State
 * @struct Impl
 * @brief Keeps queue storage and thread management outside the public interface.
 */
struct Kitchen::Impl {
    using Queue = moodycamel::BlockingConcurrentQueue<Order>;
    /// @brief Reserves reusable queue records for each registered producer.
    static constexpr size_t producer_capacity = 4096;
    /** ------------------------------------------------------------------------------------------- Producer
     * @struct Producer
     * @brief Registers one submitting thread's queue lane during setup.
     */
    struct Producer {
        /// @brief The token retaining this producer's reusable queue blocks.
        moodycamel::ProducerToken token;
        /** ----------------------------------------------------------------------------- Preparation
         * @struct Preparation
         * @brief Retains one setup latch until every owned setup record is retired.
         */
        struct Preparation {
            /// @brief Releases registration after every setup record has been consumed.
            std::latch completed{producer_capacity + Queue::BLOCK_SIZE};
            /// @brief Keeps setup ownership inline until each record releases its own slot.
            std::array<std::shared_ptr<Preparation>, producer_capacity + Queue::BLOCK_SIZE> owners;
        };
        /** ----------------------------------------------------------------------------- Complete Preparation
         * @brief Publishes consumption of one setup record through its retained latch.
         * @param context The retained setup completion owner.
         */
        static void complete_preparation(void* context) {
            (*static_cast<std::shared_ptr<Preparation>*>(context))->completed.count_down();
        }
        /** ----------------------------------------------------------------------------- Release Preparation
         * @brief Releases setup completion ownership after execution or discarded registration.
         * @param context The retained setup completion owner.
         */
        static void release_preparation(void* context) noexcept {
            auto retained = std::move(*static_cast<std::shared_ptr<Preparation>*>(context));
        }
        /** ----------------------------------------------------------------------------- Provision
         * @brief Acquires reusable blocks for one producer before allocation-free
         * submissions.
         * @param kitchen The implementation accounting for setup records.
         * @param queue The queue receiving the producer's setup records.
         * @param producer The producer whose blocks must remain reusable.
         * @return The completion retained until this producer lane is usable.
         */
        static std::shared_ptr<Preparation> provision(
            Impl& kitchen,
            Queue& queue,
            moodycamel::ProducerToken& producer
        ) {
            std::vector<Order> records(producer_capacity + Queue::BLOCK_SIZE);
            auto prepared = std::make_shared<Preparation>();
            for (size_t index = 0; index < records.size(); ++index) {
                prepared->owners[index] = prepared;
                records[index] = Order(
                    &complete_preparation,
                    &prepared->owners[index],
                    nullptr,
                    nullptr
                );
                records[index].destroy_ = &release_preparation;
            }
            kitchen.outstanding.fetch_add(records.size(), std::memory_order_relaxed);
            if (!queue.enqueue_bulk(
                producer,
                std::make_move_iterator(records.begin()),
                records.size()
            )) {
                kitchen.outstanding.fetch_sub(
                    records.size(),
                    std::memory_order_release
                );
                std::string error_msg = "Kitchen could not provision producer"
                    " queue storage";
                ALLIGATOR_KITCHEN_THROW(error_msg);
            }
            return prepared;
        }
        /** ----------------------------------------------------------------------------- Producer Constructor
         * @brief Registers and provisions one producer lane before its first submission.
         * @param kitchen The kitchen instance to obtain the queues from.
         * @param queue The destination queue whose blocks belong to this producer.
         * @param worker_startup Whether pool startup must defer waiting for this lane.
         */
        explicit Producer(Impl& kitchen, Queue& queue, bool worker_startup)
        : token(queue) {
            if (!token.valid()) {
                ALLIGATOR_KITCHEN_THROW("Kitchen could not register producer tokens");
            }
            auto prepared = provision(kitchen, queue, token);
            if (!worker_startup) prepared->completed.wait();
        }
    };
    /** ------------------------------------------------------------------------------------------- Pool
     * @struct Pool
     * @brief Owns a stable queue whose producers survive idle worker resizing.
     */
    struct Pool {
        /// @brief The queue for storing orders to be processed by the pool's workers.
        Queue queue;
        /// @brief Producer token for the shutdown queue.
        moodycamel::ProducerToken shutdown;
        /// @brief The worker threads that process orders from the pool's queue.
        std::vector<std::thread> workers;
        /// @brief The number of workers that have completed their producer setup.
        std::atomic<size_t> prepared{0};
        /// @brief Mutex protecting startup error state.
        std::mutex startup_mutex;
        /// @brief Pointer to the exception thrown during startup, if any.
        std::exception_ptr startup_error;
        /** ----------------------------------------------------------------------------- Pool Constructor
         * @brief Constructs the pool with the specified queue capacity.
         * @param capacity The maximum number of orders the queue can hold.
         */
        explicit Pool(size_t capacity)
        : queue(capacity)
        , shutdown(queue) {
            if (!shutdown.valid()) {
                ALLIGATOR_KITCHEN_THROW("Failed to create shutdown producer token");
            }
        }
        /** ----------------------------------------------------------------------------- Work
         * @brief Registers submission tokens before processing Orders until its
         * shutdown sentinel.
         * @param pool The pool instance managing the worker threads and queue.
         * @param kitchen The kitchen instance providing the order execution context.
         */
        static void work(Pool* pool, Impl* kitchen) {
            try {
                static_cast<void>(producer<false>(*kitchen, true));
                static_cast<void>(producer<true>(*kitchen, true));
            } catch (...) {
                std::lock_guard<std::mutex> lock(pool->startup_mutex);
                pool->startup_error = std::current_exception();
                pool->prepared.fetch_add(1, std::memory_order_release);
                pool->prepared.notify_all();
                return;
            }
            moodycamel::ConsumerToken consumer(pool->queue);
            pool->prepared.fetch_add(1, std::memory_order_release);
            pool->prepared.notify_all();
            for (;;) {
                Order order;
                pool->queue.wait_dequeue(consumer, order);
                if (!order) return;
                order.execute();
                kitchen->outstanding.fetch_sub(1, std::memory_order_release);
            }
        }
        /** ----------------------------------------------------------------------------- Prepared
         * @brief Waits for every started worker to finish its producer setup.
         */
        void wait_prepared() {
            size_t count = prepared.load(std::memory_order_acquire);
            while (count != workers.size()) {
                prepared.wait(count, std::memory_order_acquire);
                count = prepared.load(std::memory_order_acquire);
            }
        }
        /** ----------------------------------------------------------------------------- Start
         * @brief Starts the requested capacity before allowing submission to resume.
         * @param kitchen The kitchen instance providing the order execution context.
         * @param count The number of worker threads to start.
         */
        void start(Impl& kitchen, size_t count) {
            prepared.store(0, std::memory_order_relaxed);
            startup_error = {};
            workers.reserve(count);
            try {
                for (size_t index = 0; index < count; ++index)
                    workers.emplace_back(&Pool::work, this, &kitchen);
                wait_prepared();
                if (startup_error) std::rethrow_exception(startup_error);
            } catch (...) {
                wait_prepared();
                stop();
                throw;
            }
        }
        /** ----------------------------------------------------------------------------- Stop
         * @brief Joins an idle team before queue destruction or worker reconfiguration.
         */
        void stop() noexcept {
            for (size_t index = 0; index < workers.size(); ++index) {
                if (!queue.enqueue(shutdown, Order{})) {
                    LOG_ERROR_STREAM << "Kitchen could not wake a worker during shutdown";
                    std::terminate();
                }
            }
            for (std::thread& worker : workers) worker.join();
            workers.clear();
            Order sentinel;
            while (queue.try_dequeue(sentinel)) {}
        }
        /** ----------------------------------------------------------------------------- Destructor
         * @brief Joins the remaining workers before releasing queue storage.
         */
        ~Pool() { stop(); }
    };
    /** ------------------------------------------------------------------------------------------- Hardware Threads
     * @brief Uses the reported hardware capacity and rejects a missing runtime probe.
     * @return The number of hardware threads available on the system.
     */
    static size_t hardware_threads() {
        const size_t count = std::thread::hardware_concurrency();
        if (count == 0) {
            ALLIGATOR_KITCHEN_THROW("Kitchen could not query the hardware thread count");
        }
        return count;
    }
    /** ------------------------------------------------------------------------------------------- Queue Capacity
     * @brief Reserves 4096 queue records per reported hardware thread during setup.
     * @param threads The number of hardware threads to reserve queue capacity for.
     * @return The total queue capacity based on the number of threads.
     */
    static size_t capacity(size_t threads) {
        if (threads > std::numeric_limits<size_t>::max() / producer_capacity) {
            ALLIGATOR_KITCHEN_THROW("Kitchen queue capacity exceeds the addressable size");
        }
        return threads * producer_capacity;
    }
    /// @brief The number of hardware threads available on the system.
    const size_t hardware = hardware_threads();
    /// @brief The number of outstanding orders currently being processed.
    std::atomic<size_t> outstanding{0};
    /// @brief The current number of configured compute workers.
    std::atomic<size_t> maximum{hardware};
    /// @brief The pool managing compute worker threads.
    Pool compute{capacity(hardware)};
    /// @brief The pool managing waiting worker threads.
    Pool waiting{capacity(hardware)};
    /** ------------------------------------------------------------------------------------------- Producer State
     * @brief Reuses one destination's thread-local producer without coupling queue readiness.
     * @tparam waiting_queue Whether the producer belongs to the waiting pool.
     * @param kitchen The Kitchen implementation instance to register the producer with.
     * @param worker_startup Whether registration belongs to a pool worker starting up.
     * @return The prepared producer token for the selected destination.
     */
    template<bool waiting_queue>
    static moodycamel::ProducerToken& producer(Impl& kitchen, bool worker_startup = false) {
        Queue& queue = waiting_queue ? kitchen.waiting.queue : kitchen.compute.queue;
        thread_local Producer registered(kitchen, queue, worker_startup);
        return registered.token;
    }
    /** ------------------------------------------------------------------------------------------- Implementation Constructor
     * @brief Initializes the Kitchen implementation, starting compute and waiting worker threads.
     */
    Impl() {
        LOG_INFO_STREAM << "Kitchen: starting " << hardware << " compute and " << hardware
            << " waiting workers with preallocated queues";
        compute.start(*this, hardware);
        waiting.start(*this, hardware);
    }
};
/** --------------------------------------------------------------------------------------------------------- Constructor
 * @brief Preallocates queues and starts all hardware-reported workers.
 */
Kitchen::Kitchen() : impl_(std::make_unique<Impl>()) {
    drain();
}
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Drains accepted work before the implementation joins its parked workers.
 */
Kitchen::~Kitchen() { drain(); }
/** --------------------------------------------------------------------------------------------------------- Prepare Producer
 * @brief Registers this thread once before its submission hot path begins.
 */
void Kitchen::prepare_producer() {
    static_cast<void>(Impl::producer<false>(*impl_));
    static_cast<void>(Impl::producer<true>(*impl_));
}
/** --------------------------------------------------------------------------------------------------------- Accept Order
 * @brief Includes a published fanout invocation in Kitchen drain accounting.
 */
void Kitchen::accept_order() noexcept {
    impl_->outstanding.fetch_add(1, std::memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Finish Order
 * @brief Retires a fanout invocation after its callback and argument cleanup.
 */
void Kitchen::finish_order() noexcept {
    impl_->outstanding.fetch_sub(1, std::memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Try Submit
 * @brief Moves one Order only when existing queue blocks can accept it.
 * @param order The Order to be submitted if the queue has capacity.
 * @return true if the Order was successfully enqueued, false otherwise.
 */
bool Kitchen::try_submit(Order&& order) {
    if (order.destination_) {
        return order.destination_->try_submit(std::move(order));
    }
    moodycamel::ProducerToken& producer = Impl::producer<false>(*impl_);
    accept_order();
    if (impl_->compute.queue.try_enqueue(producer, std::move(order))) {
        return true;
    }
    finish_order();
    return false;
}
/** --------------------------------------------------------------------------------------------------------- Submit
 * @brief Reports exhausted queue storage without allocating additional queue blocks.
 * @param order The Order to be submitted.
 */
void Kitchen::submit(Order&& order) {
    if (!try_submit(std::move(order))) {
        ALLIGATOR_KITCHEN_THROW("Kitchen Order queue is full; use try_submit for backpressure");
    }
}
/** --------------------------------------------------------------------------------------------------------- Submit Borrowed
 * @brief Queues caller-owned contexts through the allocation-free borrowed Order constructor.
 * @param run The function to execute for the Order.
 * @param context The context to pass to the run function.
 * @param done The function to call when the Order is complete.
 * @param done_context The context to pass to the done function.
 */
void Kitchen::submit(
    void (*run)(void*),
    void* context,
    void (*done)(void*),
    void* done_context
) {
    submit(Order(run, context, done, done_context));
}
/** --------------------------------------------------------------------------------------------------------- Try Submit Bulk
 * @brief Moves an entire batch only when its preallocated producer can accept every record.
 * @param orders The array of Orders to be submitted.
 * @param count The number of Orders in the array.
 * @return true if all Orders were successfully enqueued, false otherwise.
 */
bool Kitchen::try_submit_bulk(Order* orders, size_t count) {
    moodycamel::ProducerToken& producer = Impl::producer<false>(*impl_);
    impl_->outstanding.fetch_add(count, std::memory_order_relaxed);
    if (impl_->compute.queue.try_enqueue_bulk(
        producer,
        std::make_move_iterator(orders),
        count
    )) return true;
    impl_->outstanding.fetch_sub(count, std::memory_order_release);
    return false;
}
/** --------------------------------------------------------------------------------------------------------- Submit Bulk
 * @brief Reports batch capacity exhaustion while retaining rejected Orders in caller storage.
 * @param orders The array of Orders to be submitted.
 * @param count The number of Orders in the array.
 */
void Kitchen::submit_bulk(Order* orders, size_t count) {
    if (!try_submit_bulk(orders, count)) {
        ALLIGATOR_KITCHEN_THROW(
            "Kitchen compute queue is full; use try_submit_bulk for backpressure"
        );
    }
}
/** --------------------------------------------------------------------------------------------------------- Try Submit Waiting
 * @brief Moves one blocking Order only when preallocated waiting capacity is available.
 * @param order The Order to be submitted.
 * @return true if the Order was successfully enqueued, false otherwise.
 */
bool Kitchen::try_submit_waiting(Order&& order) {
    if (order.destination_) return order.destination_->try_submit(std::move(order));
    moodycamel::ProducerToken& producer = Impl::producer<true>(*impl_);
    accept_order();
    if (impl_->waiting.queue.try_enqueue(producer, std::move(order))) return true;
    finish_order();
    return false;
}
/** --------------------------------------------------------------------------------------------------------- Submit Waiting
 * @brief Reports waiting capacity exhaustion without growing the queue.
 * @param order The Order to be submitted.
 */
void Kitchen::submit_waiting(Order&& order) {
    if (!try_submit_waiting(std::move(order))) {
        ALLIGATOR_KITCHEN_THROW(
            "Kitchen waiting queue is full; use try_submit_waiting for backpressure"
        );
    }
}
/** --------------------------------------------------------------------------------------------------------- Submit Waiting Borrowed
 * @brief Queues borrowed storage or blocking contexts whose owners observe completion.
 * @param run The function to execute for the borrowed storage or blocking context.
 * @param context The context to pass to the run function.
 * @param done The function to call upon completion.
 * @param done_context The context to pass to the done function.
 */
void Kitchen::submit_waiting(
    void (*run)(void*),
    void* context,
    void (*done)(void*),
    void* done_context
) {
    submit_waiting(Order(run, context, done, done_context));
}
/** --------------------------------------------------------------------------------------------------------- Set Max Threads
 * @brief Resizes compute workers after accepted work and external producers become quiescent.
 * @param threads The new maximum number of compute threads.
 */
void Kitchen::set_max_threads(size_t threads) {
    if (threads == 0) threads = 1;
    if (threads == max_threads()) return;
    LOG_INFO_STREAM << "Kitchen: preparing " << threads << " compute workers";
    drain();
    impl_->compute.workers.reserve(threads);
    impl_->compute.stop();
    impl_->maximum.store(0, std::memory_order_release);
    try {
        impl_->compute.start(*impl_, threads);
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Kitchen could not start the requested compute team: " << error.what();
        std::terminate();
    } catch (...) {
        LOG_ERROR_STREAM << "Kitchen could not start the requested compute team";
        std::terminate();
    }
    impl_->maximum.store(threads, std::memory_order_release);
    drain();
}
/** --------------------------------------------------------------------------------------------------------- Max Threads
 * @brief Reports the started compute team's capacity.
 * @return The configured compute worker count.
 * @return The maximum number of compute threads.
 */
size_t Kitchen::max_threads() const noexcept {
    return impl_->maximum.load(std::memory_order_acquire);
}
/** --------------------------------------------------------------------------------------------------------- Drain
 * @brief Waits for accepted Orders and descendants while external producers are quiescent.
 */
void Kitchen::drain() {
    auto report_at = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (impl_->outstanding.load(std::memory_order_acquire) != 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= report_at) {
            LOG_INFO_STREAM << "Kitchen: completing "
                << impl_->outstanding.load(std::memory_order_relaxed) << " outstanding Orders";
            report_at = now + std::chrono::seconds(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
/** --------------------------------------------------------------------------------------------------------- Kitchen Storage
 * @brief Reserves static singleton storage before including translation units initialize.
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
 * @brief Resolves the Kitchen whose lifetime is retained by public initializers.
 * @return The shared Kitchen instance.
 */
Kitchen& Kitchen::inst() { return *std::launder(reinterpret_cast<Kitchen*>(kitchen_storage)); }
} // namespace buffetalligator
