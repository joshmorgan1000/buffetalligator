#pragma once
/** --------------------------------------------------------------------------------------------------------- Kitchen Dispatch Executors
 * @file executors.hpp
 * @brief Compares borrowed records on experimental executors and the production Kitchen.
 */
#include <alligator/kitchen.hpp>
#include "../dispatch_record.hpp"
#include <barrier>
#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include <moodycamel/blockingconcurrentqueue.h>
#include <moodycamel/concurrentqueue.h>

namespace kitchen_dispatch {
using Order = experiments::DispatchRecord;
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
     * @brief Submits borrowed experiment records through the production Kitchen.
     */
    class Producer {
    private:
        ProductionAdapter& executor_;
        std::vector<buffetalligator::Order> orders_;
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
            orders_.clear();
            orders_.reserve(count);
            for (size_t index = 0; index < count; ++index) {
                orders_.emplace_back(&Order::run, tasks[index]);
            }
            executor_.kitchen_.submit_bulk(orders_.data(), count);
        }
        /** ----------------------------------------------------------------------------- Storage
         * @brief Sends one blocking task through the production queue.
         */
        void storage(Order task) { executor_.storage(std::move(task)); }
    };
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Checks the requested worker count matches the production hardware-sized team.
     */
    explicit ProductionAdapter(
        size_t threads = std::thread::hardware_concurrency(), size_t capacity = 4096
    ) : kitchen_(buffetalligator::Kitchen::inst()) {
        static_cast<void>(capacity);
        if (threads != std::thread::hardware_concurrency()) {
            throw std::runtime_error("The production Kitchen uses the reported hardware thread count");
        }
    }
    /** ------------------------------------------------------------------------------------------- Compute
     * @brief Enqueues one task on the production queue.
     */
    void compute(Order task) {
        kitchen_.submit(&Order::run, task);
    }
    /** ------------------------------------------------------------------------------------------- Storage
     * @brief Enqueues one blocking task on the production queue.
     */
    void storage(Order task) {
        kitchen_.submit(&Order::run, task);
    }
};
} // namespace kitchen_dispatch
