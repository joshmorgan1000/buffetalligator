#pragma once
/** --------------------------------------------------------------------------------------------------------- Kitchen Dispatch Executors
 * @file executors.hpp
 * @brief Compares persistent queue workers with the actual production Kitchen using its Order record.
 */
#include <alligator/kitchen.hpp>
#include <barrier>
#include <cstddef>
#include <iterator>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include <moodycamel/blockingconcurrentqueue.h>
#include <moodycamel/concurrentqueue.h>

namespace kitchen_dispatch {
using buffetalligator::Order;
/** --------------------------------------------------------------------------------------------------------- Strategy
 * @brief Selects default semaphore spinning, immediate semaphore sleeping, or continuous queue polling.
 */
enum class Strategy { Blocking, Sleeping, Spinning };
/** --------------------------------------------------------------------------------------------------------- Sleeping Traits
 * @brief Disables the blocking queue's semaphore spin phase without changing its queue algorithm.
 */
struct SleepingTraits : moodycamel::ConcurrentQueueDefaultTraits {
    static constexpr int MAX_SEMA_SPINS = 0;
};
/** --------------------------------------------------------------------------------------------------------- Executor
 * @brief Owns separate compute and storage teams whose accepted work must finish before destruction.
 */
template<Strategy strategy>
class Executor {
private:
    using Queue = std::conditional_t<strategy == Strategy::Spinning,
        moodycamel::ConcurrentQueue<Order>,
        moodycamel::BlockingConcurrentQueue<Order,
            std::conditional_t<strategy == Strategy::Sleeping, SleepingTraits,
                moodycamel::ConcurrentQueueDefaultTraits>>>;
    Queue compute_;
    Queue storage_;
    std::barrier<> ready_;
    std::vector<std::thread> compute_threads_;
    std::vector<std::thread> storage_threads_;
    /** ------------------------------------------------------------------------------------------- Run
     * @brief Executes Order callbacks until one empty sentinel releases this worker.
     */
    static void run(Queue* queue, std::barrier<>* ready) {
        moodycamel::ConsumerToken consumer(*queue);
        ready->arrive_and_wait();
        for (;;) {
            Order task;
            if constexpr (strategy == Strategy::Spinning) {
                while (!queue->try_dequeue(consumer, task)) {}
            } else {
                queue->wait_dequeue(consumer, task);
            }
            if (!task) return;
            task.execute();
        }
    }
public:
    /** ------------------------------------------------------------------------------------------- Producer
     * @brief Owns explicit tokens for one submitting thread and must be destroyed before its executor.
     */
    class Producer {
    private:
        Executor& executor_;
        moodycamel::ProducerToken compute_;
        moodycamel::ProducerToken storage_;
    public:
        explicit Producer(Executor& executor)
            : executor_(executor), compute_(executor.compute_), storage_(executor.storage_) {}
        Producer(const Producer&) = delete;
        Producer& operator=(const Producer&) = delete;
        /** ----------------------------------------------------------------------------- Compute
         * @brief Enqueues one compute callback through this producer's private token.
         */
        void compute(Order task) {
            static_cast<void>(executor_.compute_.enqueue(compute_, std::move(task)));
        }
        /** ----------------------------------------------------------------------------- Compute Bulk
         * @brief Enqueues a task batch through this producer's private compute token.
         */
        void compute_bulk(Order* tasks, size_t count) {
            static_cast<void>(executor_.compute_.enqueue_bulk(compute_,
                std::make_move_iterator(tasks), count));
        }
        /** ----------------------------------------------------------------------------- Storage
         * @brief Enqueues one blocking storage callback through this producer's private token.
         */
        void storage(Order task) {
            static_cast<void>(executor_.storage_.enqueue(storage_, std::move(task)));
        }
    };
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Starts both probed-size teams and waits until every consumer token is ready.
     */
    explicit Executor(
        size_t threads = std::thread::hardware_concurrency(), size_t capacity = 4096
    ) : compute_(capacity), storage_(capacity), ready_(2 * threads + 1) {
        compute_threads_.reserve(threads);
        storage_threads_.reserve(threads);
        for (size_t index = 0; index < threads; ++index) {
            compute_threads_.emplace_back(&Executor::run, &compute_, &ready_);
            storage_threads_.emplace_back(&Executor::run, &storage_, &ready_);
        }
        ready_.arrive_and_wait();
    }
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Joins every worker after external completion has quiesced all submissions and accepted work.
     */
    ~Executor() {
        moodycamel::ProducerToken compute(compute_);
        moodycamel::ProducerToken storage(storage_);
        for (size_t index = 0; index < compute_threads_.size(); ++index) {
            static_cast<void>(compute_.enqueue(compute, Order{}));
            static_cast<void>(storage_.enqueue(storage, Order{}));
        }
        for (auto& worker : compute_threads_) worker.join();
        for (auto& worker : storage_threads_) worker.join();
    }
    /** ------------------------------------------------------------------------------------------- Compute
     * @brief Uses a thread-local implicit queue producer for continuations that migrate between workers.
     */
    void compute(Order task) { static_cast<void>(compute_.enqueue(std::move(task))); }
    /** ------------------------------------------------------------------------------------------- Storage
     * @brief Uses a thread-local implicit queue producer for storage submissions from arbitrary workers.
     */
    void storage(Order task) { static_cast<void>(storage_.enqueue(std::move(task))); }
};
using BlockingExecutor = Executor<Strategy::Blocking>;
using SleepingExecutor = Executor<Strategy::Sleeping>;
using SpinningExecutor = Executor<Strategy::Spinning>;
/** --------------------------------------------------------------------------------------------------------- Production Adapter
 * @brief Invokes the actual production Kitchen without substituting a queue implementation.
 */
class ProductionAdapter {
private:
    buffetalligator::Kitchen& kitchen_;
public:
    /** ------------------------------------------------------------------------------------------- Producer
     * @brief Uses the production Kitchen's own per-thread submission tokens.
     */
    class Producer {
    private:
        ProductionAdapter& executor_;
    public:
        explicit Producer(ProductionAdapter& executor) : executor_(executor) {}
        Producer(const Producer&) = delete;
        Producer& operator=(const Producer&) = delete;
        /** ----------------------------------------------------------------------------- Compute
         * @brief Sends one ordinary task through the installed production implementation.
         */
        void compute(Order task) { executor_.compute(std::move(task)); }
        /** ----------------------------------------------------------------------------- Compute Bulk
         * @brief Sends a task batch through the production Kitchen bulk endpoint.
         */
        void compute_bulk(Order* tasks, size_t count) {
            while (!executor_.kitchen_.try_submit_bulk(tasks, count)) std::this_thread::yield();
        }
        /** ----------------------------------------------------------------------------- Storage
         * @brief Sends one blocking task through the production waiter endpoint.
         */
        void storage(Order task) { executor_.storage(std::move(task)); }
    };
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Selects the same requested compute limit while retaining the production storage team.
     */
    explicit ProductionAdapter(
        size_t threads = std::thread::hardware_concurrency(), size_t capacity = 4096
    ) : kitchen_(buffetalligator::Kitchen::inst()) {
        static_cast<void>(capacity);
        kitchen_.set_max_threads(threads);
    }
    /** ------------------------------------------------------------------------------------------- Compute
     * @brief Enqueues one task on the production compute pool.
     */
    void compute(Order task) {
        while (!kitchen_.try_submit(std::move(task))) std::this_thread::yield();
    }
    /** ------------------------------------------------------------------------------------------- Storage
     * @brief Enqueues one task on the production blocking waiter pool.
     */
    void storage(Order task) {
        while (!kitchen_.try_submit_waiting(std::move(task))) std::this_thread::yield();
    }
};
} // namespace kitchen_dispatch
