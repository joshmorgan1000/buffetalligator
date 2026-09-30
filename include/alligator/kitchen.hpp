#pragma once
/** --------------------------------------------------------------------------------------------------------- Kitchen Module
 * @file kitchen.hpp
 * @brief Declarations for the kitchen module of the Alligator library, the Alligator's thread pool.
 */
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <moodycamel/concurrentqueue.h>
#include <moodycamel/blockingconcurrentqueue.h>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Task
 * @struct Task
 * @brief One unit of Kitchen work as a trivially copyable record: the worker calls `run(context)`,
 * then `done(done_context)` when `done` is set. Arguments and results live in the caller-owned
 * contexts, so a submission allocates nothing. `run` and `done` must not throw.
 */
struct Task {
    void (*run)(void* context) = nullptr;         ///< The work; nullptr marks a shutdown wakeup.
    void* context = nullptr;                       ///< Handed to run.
    void (*done)(void* done_context) = nullptr;   ///< Completion callback, or nullptr for none.
    void* done_context = nullptr;                  ///< Handed to done.
};
static_assert(std::is_trivially_copyable_v<Task> && sizeof(Task) == 32, "Task must stay a 32-byte POD");
/** --------------------------------------------------------------------------------------------------------- Task Countdown
 * @struct TaskCountdown
 * @brief Synchronous completion for one task or a batch: pass `&TaskCountdown::arrive` as `done`
 * and the countdown as `done_context`, then `wait()` before releasing its storage.
 */
struct TaskCountdown {
    std::atomic<uint32_t> pending;  ///< Tasks still outstanding.
    std::atomic<bool> notified;  ///< The last arrival has finished touching the pending atomic.
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Arms the countdown for a number of tasks.
     * @param count The number of arrivals that release the waiter.
     */
    explicit TaskCountdown(uint32_t count = 1);
    TaskCountdown(const TaskCountdown&) = delete;
    TaskCountdown& operator=(const TaskCountdown&) = delete;
    /** ------------------------------------------------------------------------------------------- Arrive
     * @brief The `done` hook: counts one task down and wakes the waiter on the last one.
     * @param countdown The TaskCountdown passed as `done_context`.
     */
    static void arrive(void* countdown);
    /** ------------------------------------------------------------------------------------------- Wait
     * @brief Parks until every armed task has arrived; each task's writes are visible afterwards.
     */
    void wait();
    /** ------------------------------------------------------------------------------------------- Rearm
     * @brief Resets a drained countdown for another round.
     * @param count The number of arrivals that release the next wait.
     */
    void rearm(uint32_t count);
};
/** --------------------------------------------------------------------------------------------------------- Kitchen
 * @class Kitchen
 * @brief Manages worker and waiter threads, handling thread change requests.
 */
class Kitchen {
public:
    /** ------------------------------------------------------------------------------------------- Thread Change Request
     * @struct ThreadChangeReq
     * @brief Represents a request to change the number of worker or waiter threads.
     */
    struct ThreadChangeReq {
        /// @brief The number of active threads at the time of the thread change request.
        std::unique_ptr<size_t> current_active;
        /// @brief The ID of the thread to be shut down.
        std::unique_ptr<int> shutdown_id;
    };
    /** ------------------------------------------------------------------------------------------- Instance
     * @brief Returns the singleton instance of the Kitchen.
     * @return The Kitchen instance.
     */
    static Kitchen& inst();
    Kitchen(const Kitchen&) = delete;
    Kitchen& operator=(const Kitchen&) = delete;
    Kitchen(Kitchen&&) = delete;
    Kitchen& operator=(Kitchen&&) = delete;
    /** ------------------------------------------------------------------------------------------- Set Max Threads
     * @brief Sets the maximum number of worker threads allowed.
     * @param max_threads The maximum number of worker threads.
     */
    void set_max_threads(size_t max_threads) {
        max_thread_count_.store(max_threads, std::memory_order_release);
    }
    /** ------------------------------------------------------------------------------------------- Max Threads
     * @brief Gets the maximum number of worker threads allowed.
     * @return The maximum number of worker threads.
     */
    size_t max_threads() const {
        return max_thread_count_.load(std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Submit
     * @brief Queues one task on the worker pool.
     * @param task The task record, copied into the queue.
     */
    void submit(const Task& task);
    /** ------------------------------------------------------------------------------------------- Submit (fields)
     * @brief Queues `run(context)` on the worker pool, then `done(done_context)` when set.
     * @param run The work.
     * @param context Handed to run.
     * @param done Completion callback, or nullptr.
     * @param done_context Handed to done.
     */
    void submit(
        void (*run)(void* context),
        void* context,
        void (*done)(void* done_context) = nullptr,
        void* done_context = nullptr
    );
    /** ------------------------------------------------------------------------------------------- Submit Bulk
     * @brief Queues a batch on the worker pool with one enqueue and one wakeup signal.
     * @param tasks The task records, copied into the queue.
     * @param count How many tasks to queue.
     */
    void submit_bulk(const Task* tasks, size_t count);
    /** ------------------------------------------------------------------------------------------- Submit Waiting
     * @brief Queues one task on the waiter pool, for work that mostly parks on a fence,
     * semaphore or atomic.
     * @param task The task record, copied into the queue.
     */
    void submit_waiting(const Task& task);
    /** ------------------------------------------------------------------------------------------- Submit Waiting (fields)
     * @brief Queues `run(context)` on the waiter pool, then `done(done_context)` when set.
     * @param run The work.
     * @param context Handed to run.
     * @param done Completion callback, or nullptr.
     * @param done_context Handed to done.
     */
    void submit_waiting(
        void (*run)(void* context),
        void* context,
        void (*done)(void* done_context) = nullptr,
        void* done_context = nullptr
    );
    /** ------------------------------------------------------------------------------------------- Drain
     * @brief Waits for accepted tasks and their completion callbacks while producers are quiescent.
     */
    void drain();
private:
    std::atomic<bool> stop_signal_{false};
    std::atomic<size_t> max_thread_count_{0};
    std::atomic<size_t> outstanding_tasks_{0};
    std::atomic<bool> growth_requested_{false};
    struct WorkerThread;
    std::atomic<size_t> active_workers_{0};
    moodycamel::BlockingConcurrentQueue<Task> task_queue_;
    std::unordered_map<int, std::unique_ptr<WorkerThread>> worker_threads_;
    struct WaitingThread;
    moodycamel::BlockingConcurrentQueue<Task> waiting_task_queue_;
    std::vector<std::unique_ptr<WaitingThread>> waiting_threads_;
    struct ThreadChanger;
    moodycamel::BlockingConcurrentQueue<ThreadChangeReq> thread_change_queue_;
    std::unique_ptr<ThreadChanger> thread_changer_;
    void maybe_wakeup();
    /** ------------------------------------------------------------------------------------------- Worker Token
     * @brief This thread's explicit producer on the worker queue, skipping the implicit-producer lookup.
     * @return The calling thread's token.
     */
    static moodycamel::ProducerToken& worker_token();
    /** ------------------------------------------------------------------------------------------- Waiter Token
     * @brief This thread's explicit producer on the waiter queue, skipping the implicit-producer lookup.
     * @return The calling thread's token.
     */
    static moodycamel::ProducerToken& waiter_token();
    Kitchen();
    ~Kitchen();
    friend struct KitchenInitializer;
};
/** --------------------------------------------------------------------------------------------------------- Kitchen Initializer
 * @struct KitchenInitializer
 * @brief Schwarz counter; every including translation unit holds one, so the Kitchen is built
 * before their statics and destroyed after them, and inst() needs no guard.
 */
struct KitchenInitializer {
    KitchenInitializer();
    ~KitchenInitializer();
};
static KitchenInitializer kitchen_initializer;
} // namespace buffetalligator
