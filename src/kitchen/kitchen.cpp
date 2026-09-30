/** --------------------------------------------------------------------------------------------------------- Kitchen
 * @file kitchen.cpp
 * @brief The worker pool, the waiter pool, and the thread changer, moved out of the Alligator.
 */
#include <alligator/kitchen.hpp>
#include <logging.hpp>
#include <loggingutils.hpp>
#include <cstdlib>
#include <new>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Maybe Wakeup
 * @brief Wakes up the Kitchen if necessary.
 */
void Kitchen::maybe_wakeup() {
    if (active_workers_.load(std::memory_order_acquire) == 0) {
        thread_change_queue_.enqueue(ThreadChangeReq{
            std::make_unique<size_t>(0),
            nullptr
        });
    }
}
/** ------------------------------------------------------------------------------------------- Worker Thread
 * @struct WorkerThread
 * @brief Runs queued tasks and parks until more work or a stop wakeup arrives.
 */
struct Kitchen::WorkerThread {
    /// @brief The owning Kitchen, handed in so the thread never touches inst().
    Kitchen* kitchen;
    /// @brief The unique identifier for this worker thread.
    int id;
    /// @brief Local stop flag for this worker thread.
    std::atomic<bool> stop{false};
    /// @brief The thread object, started by start() once the worker is published in its slot.
    std::thread thread_;
    /** ----------------------------------------------------------------------------- Work Loop
     * @brief Blocks for tasks, requests workers for the backlog, and exits when stopped.
     */
    void work() {
        Kitchen& pool = *kitchen;
        moodycamel::ConsumerToken consumer(pool.task_queue_);
        uint32_t backlog_dequeues = 0;
        while (!pool.stop_signal_.load(std::memory_order_acquire)
               && !stop.load(std::memory_order_acquire)) {
            Task task;
            if (!pool.task_queue_.try_dequeue(consumer, task)) {
                pool.task_queue_.wait_dequeue(consumer, task);
                backlog_dequeues = 0;
            } else if ((backlog_dequeues++ & 63u) == 0) {
                // Non-parking dequeue means backlog; size_approx is O(producers), so check only every 64th.
                const size_t current_tasks = pool.task_queue_.size_approx();
                const size_t active_threads = pool.active_workers_.load(std::memory_order_acquire);
                if (current_tasks >= active_threads
                    && active_threads < pool.max_thread_count_.load(std::memory_order_acquire)
                ) {
                    pool.thread_change_queue_.enqueue({
                        std::make_unique<size_t>(active_threads),
                        nullptr
                    });
                }
            }
            if (task.run == nullptr || pool.stop_signal_.load(std::memory_order_acquire)
                || stop.load(std::memory_order_acquire)) {
                break;
            }
            task.run(task.context);
            if (task.done != nullptr) {
                task.done(task.done_context);
            }
        }
        pool.active_workers_.fetch_sub(1, std::memory_order_release);
        pool.thread_change_queue_.enqueue({
            nullptr,
            std::make_unique<int>(id)
        });
    }
    /** ----------------------------------------------------------------------------- Start
     * @brief Starts the thread; called only after the worker is published in its slot.
     */
    void start() {
        thread_ = std::thread(&WorkerThread::work, this);
    }
    /** ----------------------------------------------------------------------------- Constructor
     * @brief Constructs an unstarted worker.
     * @param kitchen_ The owning Kitchen.
     * @param id_ This worker's key in the worker map.
     */
    WorkerThread(Kitchen* kitchen_, int id_) : kitchen(kitchen_), id(id_) {}
    /** ----------------------------------------------------------------------------- Destructor
     * @brief Destroys the WorkerThread object and stops the thread.
     */
    ~WorkerThread() {
        stop.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;
};
/** --------------------------------------------------------------------------------------------------------- Thread Changer
 * @struct ThreadChanger
 * @brief Manages the thread that handles dynamic changes to the number of worker and waiter threads.
 */
struct Kitchen::ThreadChanger {
    /// @brief The owning Kitchen, handed in so the thread never touches inst().
    Kitchen* kitchen;
    /// @brief The thread object for the thread changer.
    std::thread thread_;
    /// @brief The stop signal for the thread changer.
    std::atomic<bool> stop{false};
    /** ------------------------------------------------------------------------------------------- Work Loop
     * @brief The main work loop for the thread changer.
     */
    void work() {
        Kitchen& pool = *kitchen;
        while (!pool.stop_signal_.load(std::memory_order_acquire)
                && !stop.load(std::memory_order_acquire)
        ) {
            ThreadChangeReq request;
            pool.thread_change_queue_.wait_dequeue(request);
            if (pool.stop_signal_.load(std::memory_order_acquire)
                || stop.load(std::memory_order_acquire)) {
                break;
            }
            if (request.shutdown_id) {
                int id_to_shutdown = *request.shutdown_id;
                auto it = pool.worker_threads_.find(id_to_shutdown);
                if (it != pool.worker_threads_.end()) {
                    pool.worker_threads_.erase(it);
                }
                continue;
            }
            if (request.current_active) {
                if (*request.current_active !=
                        pool.active_workers_.load(std::memory_order_acquire)
                ) {
                    continue;
                }
                pool.active_workers_.fetch_add(1, std::memory_order_acq_rel);
                int new_id = rand();
                while (pool.worker_threads_.find(new_id)
                        != pool.worker_threads_.end()
                ) {
                    new_id = rand();
                }
                auto new_worker = std::make_unique<WorkerThread>(&pool, new_id);
                WorkerThread* worker_ptr = new_worker.get();
                pool.worker_threads_[new_id] = std::move(new_worker);
                worker_ptr->start();
                continue;
            }
        }
    }
    /** ------------------------------------------------------------------------------------------- Start
     * @brief Starts the thread changer by launching its work loop in a separate thread.
     */
    void start() {
        thread_ = std::thread(&ThreadChanger::work, this);
    }
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs an unstarted thread changer.
     * @param kitchen_ The owning Kitchen.
     */
    ThreadChanger(Kitchen* kitchen_) : kitchen(kitchen_) {}
    ~ThreadChanger() {
        stop.store(true, std::memory_order_release);
        kitchen->thread_change_queue_.enqueue(ThreadChangeReq{});
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    ThreadChanger(const ThreadChanger&) = delete;
    ThreadChanger& operator=(const ThreadChanger&) = delete;
};
/** ------------------------------------------------------------------------------------------- Waiting Thread
 * @struct WaitingThread
 * @brief Runs waiting tasks and parks until more work or a stop wakeup arrives.
 */
struct Kitchen::WaitingThread {
    /// @brief The owning Kitchen, handed in so the thread never touches inst().
    Kitchen* kitchen;
    /// @brief The thread object, started by start() once the worker is published in its slot.
    std::thread thread_;
    /// @brief The stop signal for this waiting thread.
    std::atomic<bool> stop{false};
    /** ----------------------------------------------------------------------------- Work Loop
     * @brief Blocks for waiting tasks and forwards their continuations to the worker pool.
     */
    void work() {
        Kitchen& pool = *kitchen;
        moodycamel::ConsumerToken consumer(pool.waiting_task_queue_);
        while (!pool.stop_signal_.load(std::memory_order_acquire)
            && !stop.load(std::memory_order_acquire)
        ) {
            Task task;
            pool.waiting_task_queue_.wait_dequeue(consumer, task);
            if (task.run == nullptr || pool.stop_signal_.load(std::memory_order_acquire)
                || stop.load(std::memory_order_acquire)) {
                break;
            }
            task.run(task.context);
            if (task.done != nullptr) {
                task.done(task.done_context);
            }
        }
    }
    /** ----------------------------------------------------------------------------- Start
     * @brief Starts the thread; called only after the waiting thread is published
     * in its slot.
     */
    void start() {
        thread_ = std::thread(&WaitingThread::work, this);
    }
    /** ----------------------------------------------------------------------------- Constructor
     * @brief Constructs an unstarted waiting thread.
     * @param kitchen_ The owning Kitchen.
     */
    WaitingThread(Kitchen* kitchen_) : kitchen(kitchen_) {}
    /** ----------------------------------------------------------------------------- Destructor
     * @brief Destroys the WaitingThread object and stops the thread.
     */
    ~WaitingThread() {
        stop.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    WaitingThread(const WaitingThread&) = delete;
    WaitingThread& operator=(const WaitingThread&) = delete;
};
/** --------------------------------------------------------------------------------------------------------- Worker Token
 * @brief This thread's explicit producer on the worker queue, created on its first submit.
 * @return The calling thread's token.
 */
moodycamel::ProducerToken& Kitchen::worker_token() {
    thread_local moodycamel::ProducerToken token(inst().task_queue_);
    return token;
}
/** --------------------------------------------------------------------------------------------------------- Waiter Token
 * @brief This thread's explicit producer on the waiter queue, created on its first submit.
 * @return The calling thread's token.
 */
moodycamel::ProducerToken& Kitchen::waiter_token() {
    thread_local moodycamel::ProducerToken token(inst().waiting_task_queue_);
    return token;
}
/** --------------------------------------------------------------------------------------------------------- Kitchen Constructor
 * @brief Starts the waiter pool and the thread changer; workers spawn on the first submission.
 */
Kitchen::Kitchen() {
    for (size_t i = 0; i < std::thread::hardware_concurrency(); ++i) {
        waiting_threads_.emplace_back(std::make_unique<WaitingThread>(this));
        waiting_threads_.back()->start();
    }
    thread_changer_ = std::make_unique<ThreadChanger>(this);
    thread_changer_->start();
}
/** --------------------------------------------------------------------------------------------------------- Kitchen Storage
 * @brief Raw storage for the singleton; zero-initialized, so it exists before any dynamic init runs.
 */
alignas(Kitchen) static unsigned char kitchen_storage[sizeof(Kitchen)];
/// @brief Count of live KitchenInitializer objects, one per including translation unit.
static size_t kitchen_initializer_count = 0;
/** --------------------------------------------------------------------------------------------------------- Kitchen Initializer
 * @brief The first including translation unit constructs the logger, then the Kitchen.
 */
KitchenInitializer::KitchenInitializer() {
    if (kitchen_initializer_count++ == 0) {
        static_cast<void>(threadsafe_logger::logging::GlobalLoggingContext::instance());
        new (kitchen_storage) Kitchen();
    }
}
/** --------------------------------------------------------------------------------------------------------- Kitchen Finalizer
 * @brief The last including translation unit to tear down destroys the Kitchen.
 */
KitchenInitializer::~KitchenInitializer() {
    if (--kitchen_initializer_count == 0) {
        Kitchen::inst().~Kitchen();
    }
}
/** --------------------------------------------------------------------------------------------------------- Instance
 * @brief Retrieves the singleton instance of the Kitchen.
 * @return A reference to the Kitchen instance.
 */
Kitchen& Kitchen::inst() {
    return *std::launder(reinterpret_cast<Kitchen*>(kitchen_storage));
}
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Stops pool growth, wakes every parked thread, and joins without draining queued tasks.
 */
Kitchen::~Kitchen() {
    stop_signal_.store(true, std::memory_order_release);
    thread_changer_.reset();
    for (size_t index = 0; index < worker_threads_.size(); ++index) {
        task_queue_.enqueue(Task{});
    }
    for (size_t index = 0; index < waiting_threads_.size(); ++index) {
        waiting_task_queue_.enqueue(Task{});
    }
    worker_threads_.clear();
    waiting_threads_.clear();
}
} // namespace buffetalligator
