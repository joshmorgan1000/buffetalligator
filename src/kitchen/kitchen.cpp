/** --------------------------------------------------------------------------------------------------------- Kitchen
 * @file kitchen.cpp
 * @brief The worker pool, the waiter pool, and the thread changer, moved out of the Alligator.
 */
#include <alligator/kitchen.hpp>
#include <logging.hpp>
#include <loggingutils.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <new>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- TaskCountdown Constructor
 * @brief Arms the countdown for a number of tasks.
 * @param count The number of arrivals that release the waiter.
 */
TaskCountdown::TaskCountdown(uint32_t count) : pending(count), notified(count == 0) {}
/** --------------------------------------------------------------------------------------------------------- TaskCountdown Arrive
 * @brief The `done` hook: counts one task down and wakes the waiter on the last one.
 * @param countdown The TaskCountdown passed as `done_context`.
 */
void TaskCountdown::arrive(void* countdown) {
    TaskCountdown* self = static_cast<TaskCountdown*>(countdown);
    if (self->pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        self->pending.notify_all();
        self->notified.store(true, std::memory_order_release);
    }
}
/** --------------------------------------------------------------------------------------------------------- TaskCountdown Wait
 * @brief Parks until every armed task has arrived; each task's writes are visible afterwards.
 */
void TaskCountdown::wait() {
    uint32_t remaining = pending.load(std::memory_order_acquire);
    while (remaining != 0) {
        pending.wait(remaining, std::memory_order_acquire);
        remaining = pending.load(std::memory_order_acquire);
    }
    while (!notified.load(std::memory_order_acquire)) std::this_thread::yield();
}
/** --------------------------------------------------------------------------------------------------------- TaskCountdown Rearm
 * @brief Resets a drained countdown for another round.
 * @param count The number of arrivals that release the next wait.
 */
void TaskCountdown::rearm(uint32_t count) {
    notified.store(count == 0, std::memory_order_relaxed);
    pending.store(count, std::memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Submit
 * @brief Queues one task on the worker pool.
 * @param task The task record, copied into the queue.
 */
void Kitchen::submit(const Task& task) {
    moodycamel::ProducerToken& token = worker_token();
    outstanding_tasks_.fetch_add(1, std::memory_order_relaxed);
    if (!task_queue_.enqueue(token, task)) {
        outstanding_tasks_.fetch_sub(1, std::memory_order_release);
        throw std::bad_alloc();
    }
    maybe_wakeup();
}
/** --------------------------------------------------------------------------------------------------------- Submit (fields)
 * @brief Queues `run(context)` on the worker pool, then `done(done_context)` when set.
 * @param run The work.
 * @param context Handed to run.
 * @param done Completion callback, or nullptr.
 * @param done_context Handed to done.
 */
void Kitchen::submit(
    void (*run)(void* context),
    void* context,
    void (*done)(void* done_context),
    void* done_context
) {
    submit(Task{run, context, done, done_context});
}
/** --------------------------------------------------------------------------------------------------------- Submit Bulk
 * @brief Queues a batch on the worker pool with one enqueue and one wakeup signal.
 * @param tasks The task records, copied into the queue.
 * @param count How many tasks to queue.
 */
void Kitchen::submit_bulk(const Task* tasks, size_t count) {
    moodycamel::ProducerToken& token = worker_token();
    outstanding_tasks_.fetch_add(count, std::memory_order_relaxed);
    if (!task_queue_.enqueue_bulk(token, tasks, count)) {
        outstanding_tasks_.fetch_sub(count, std::memory_order_release);
        throw std::bad_alloc();
    }
    maybe_wakeup();
}
/** --------------------------------------------------------------------------------------------------------- Submit Waiting
 * @brief Queues one task on the waiter pool.
 * @param task The task record, copied into the queue.
 */
void Kitchen::submit_waiting(const Task& task) {
    moodycamel::ProducerToken& token = waiter_token();
    outstanding_tasks_.fetch_add(1, std::memory_order_relaxed);
    if (!waiting_task_queue_.enqueue(token, task)) {
        outstanding_tasks_.fetch_sub(1, std::memory_order_release);
        throw std::bad_alloc();
    }
}
/** --------------------------------------------------------------------------------------------------------- Submit Waiting (fields)
 * @brief Queues `run(context)` on the waiter pool, then `done(done_context)` when set.
 * @param run The work.
 * @param context Handed to run.
 * @param done Completion callback, or nullptr.
 * @param done_context Handed to done.
 */
void Kitchen::submit_waiting(
    void (*run)(void* context),
    void* context,
    void (*done)(void* done_context),
    void* done_context
) {
    submit_waiting(Task{run, context, done, done_context});
}
/** --------------------------------------------------------------------------------------------------------- Maybe Wakeup
 * @brief Wakes up the Kitchen if necessary.
 */
void Kitchen::maybe_wakeup() {
    const size_t active = active_workers_.load(std::memory_order_acquire);
    if (active >= max_thread_count_.load(std::memory_order_relaxed)
        || outstanding_tasks_.load(std::memory_order_acquire) <= active) return;
    bool expected = false;
    if (!growth_requested_.compare_exchange_strong(expected, true,
        std::memory_order_acq_rel, std::memory_order_relaxed)) return;
    try {
        if (thread_change_queue_.enqueue(ThreadChangeReq{
            std::make_unique<size_t>(active), nullptr
        })) return;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Kitchen worker growth failed after accepting work: " << error.what();
        std::terminate();
    }
    LOG_ERROR_STREAM << "Kitchen could not queue worker growth for accepted tasks";
    std::terminate();
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
            }
            if ((backlog_dequeues++ & 63u) == 0) {
                // Poll backlog on wakeup and every 64th dequeue because size_approx visits producers.
                const size_t current_tasks = pool.task_queue_.size_approx();
                const size_t active_threads = pool.active_workers_.load(std::memory_order_acquire);
                if (current_tasks >= active_threads
                    && active_threads < pool.max_thread_count_.load(std::memory_order_acquire)
                ) {
                    pool.maybe_wakeup();
                }
            }
            if (task.run == nullptr || pool.stop_signal_.load(std::memory_order_acquire)
                || stop.load(std::memory_order_acquire)) {
                break;
            }
            try {
                task.run(task.context);
            } catch (const std::exception& error) {
                LOG_ERROR_STREAM << "Kitchen task failed: " << error.what();
            } catch (...) {
                LOG_ERROR_STREAM << "Kitchen task failed with a nonstandard exception";
            }
            if (task.done != nullptr) {
                try {
                    task.done(task.done_context);
                } catch (const std::exception& error) {
                    LOG_ERROR_STREAM << "Kitchen completion failed: " << error.what();
                } catch (...) {
                    LOG_ERROR_STREAM << "Kitchen completion failed with a nonstandard exception";
                }
            }
            pool.outstanding_tasks_.fetch_sub(1, std::memory_order_release);
        }
        pool.active_workers_.fetch_sub(1, std::memory_order_release);
        if (!pool.thread_change_queue_.enqueue({
            nullptr,
            std::make_unique<int>(id)
        })) {
            LOG_ERROR_STREAM << "Kitchen could not queue a completed worker's release";
            std::terminate();
        }
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
                pool.growth_requested_.store(false, std::memory_order_release);
                if (*request.current_active !=
                        pool.active_workers_.load(std::memory_order_acquire)
                ) {
                    pool.maybe_wakeup();
                    continue;
                }
                if (pool.active_workers_.load(std::memory_order_relaxed)
                    >= pool.max_thread_count_.load(std::memory_order_acquire)) continue;
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
                pool.maybe_wakeup();
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
        if (!kitchen->thread_change_queue_.enqueue(ThreadChangeReq{})) {
            LOG_ERROR_STREAM << "Kitchen could not wake the thread manager for shutdown";
            std::terminate();
        }
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
            try {
                task.run(task.context);
            } catch (const std::exception& error) {
                LOG_ERROR_STREAM << "Kitchen waiting task failed: " << error.what();
            } catch (...) {
                LOG_ERROR_STREAM << "Kitchen waiting task failed with a nonstandard exception";
            }
            if (task.done != nullptr) {
                try {
                    task.done(task.done_context);
                } catch (const std::exception& error) {
                    LOG_ERROR_STREAM << "Kitchen waiting completion failed: " << error.what();
                } catch (...) {
                    LOG_ERROR_STREAM << "Kitchen waiting completion failed with a nonstandard exception";
                }
            }
            pool.outstanding_tasks_.fetch_sub(1, std::memory_order_release);
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
 * @brief Starts the waiter pool, initial worker, and thread changer at the probed thread limit.
 */
Kitchen::Kitchen() {
    const size_t hardware_threads = std::max(1u, std::thread::hardware_concurrency());
    max_thread_count_.store(hardware_threads, std::memory_order_relaxed);
    for (size_t i = 0; i < hardware_threads; ++i) {
        waiting_threads_.emplace_back(std::make_unique<WaitingThread>(this));
        waiting_threads_.back()->start();
    }
    worker_threads_.emplace(0, std::make_unique<WorkerThread>(this, 0));
    active_workers_.store(1, std::memory_order_relaxed);
    worker_threads_.at(0)->start();
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
/** --------------------------------------------------------------------------------------------------------- Drain
 * @brief Waits until accepted tasks and their callbacks have returned.
 */
void Kitchen::drain() {
    auto report_at = std::chrono::steady_clock::now();
    while (outstanding_tasks_.load(std::memory_order_acquire) != 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= report_at) {
            LOG_INFO_STREAM << "Kitchen: completing "
                << outstanding_tasks_.load(std::memory_order_relaxed) << " queued tasks";
            report_at = now + std::chrono::seconds(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Completes accepted work before stopping pool growth and joining every worker.
 */
Kitchen::~Kitchen() {
    drain();
    stop_signal_.store(true, std::memory_order_release);
    thread_changer_.reset();
    for (size_t index = 0; index < worker_threads_.size(); ++index) {
        if (!task_queue_.enqueue(Task{})) {
            LOG_ERROR_STREAM << "Kitchen could not wake a worker for shutdown";
            std::terminate();
        }
    }
    for (size_t index = 0; index < waiting_threads_.size(); ++index) {
        if (!waiting_task_queue_.enqueue(Task{})) {
            LOG_ERROR_STREAM << "Kitchen could not wake a waiting worker for shutdown";
            std::terminate();
        }
    }
    worker_threads_.clear();
    waiting_threads_.clear();
}
} // namespace buffetalligator
