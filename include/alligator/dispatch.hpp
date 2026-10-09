#pragma once
/** --------------------------------------------------------------------------------------------------------- Dispatch
 * @file dispatch.hpp
 * @brief Platform abstraction around either Apple's GCD or OpenMP.
 */
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include <moodycamel/blockingconcurrentqueue.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <dispatch/dispatch.h>
#endif
#if defined(_OPENMP)
#include <omp.h>
#elif !defined(__APPLE__)
static_assert(false, "Unsupported platform for dispatch or OpenMP not found");
#endif

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- dispatch_for
 * @brief Parallel for loop abstraction over GCD or OpenMP.
 * @param start The starting index (inclusive).
 * @param end The ending index (exclusive).
 * @param func The function to execute for each index.
 */
inline static void dispatch_for(size_t start, size_t end, auto&& func) {
#if defined(__APPLE__)
    dispatch_apply(end - start, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^(size_t i) {
        func(start + i);
    });
#elif defined(_OPENMP)
    #pragma omp parallel for
    for (size_t i = start; i < end; ++i) {
        func(i);
    }
#endif
}
/** --------------------------------------------------------------------------------------------------------- dispatch_foreach
 * @brief Parallel for-each loop abstraction over GCD or OpenMP.
 * @param container The container to iterate over.
 * @param func The function to execute for each element.
 */
inline static void dispatch_foreach(auto&& container, auto&& func) {
#if defined(__APPLE__)
    dispatch_apply(container.size(), dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^(size_t i) {
        func(container[i]);
    });
#elif defined(_OPENMP)
    #pragma omp parallel for
    for (size_t i = 0; i < container.size(); ++i) {
        func(container[i]);
    }
#endif
}
#if !defined(__APPLE__)
/** --------------------------------------------------------------------------------------------------------- dispatch_task
 * @brief Runs one function as an OpenMP task inside an enclosing parallel region; OpenMP is
 * required off Apple, as the header's static_assert enforces.
 * @param func The function to run.
 */
inline static void dispatch_task(auto&& func) {
    auto* callable = std::addressof(func);
    #pragma omp task firstprivate(callable)
    { (*callable)(); }
}
#endif
/** --------------------------------------------------------------------------------------------------------- PackagedFunction
 * @struct PackagedFunction
 * @brief Encapsulates a function and its arguments for deferred execution with an optional callback.
 */
template<typename Func, typename... Args>
struct PackagedFunction {
private:
    /// @brief The result type of the packaged function when invoked with its arguments.
    using Result = std::invoke_result_t<Func&, Args&...>;
    /** ------------------------------------------------------------------------------------------- Callback Type
     * @brief Selects a valid callback signature for value-returning and void functions.
     * @return The type of the callback function.
     */
    static auto callback_type() {
        if constexpr (std::is_void_v<Result>) {
            return static_cast<void (*)(std::tuple<Args...>)>(nullptr);
        } else {
            return static_cast<void (*)(Result, std::tuple<Args...>)>(nullptr);
        }
    }
public:
    /// @brief The type of the callback function associated with this packaged function.
    using Callback = decltype(callback_type());
    /// @brief The function to be executed by this packaged function.
    Func func;
    /// @brief The arguments to be passed to the function when executed.
    std::tuple<Args...> args;
    /// @brief The callback function to be invoked after the function execution.
    Callback callback;
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs a packaged function with the given function, arguments, and callback.
     * @param f The function to be executed.
     * @param a The arguments to be passed to the function.
     * @param cb The optional callback function to be invoked after execution.
     */
    PackagedFunction(
        Func f,
        std::tuple<Args...> a,
        Callback cb = nullptr
    ) : func(std::move(f))
    , args(std::move(a))
    , callback(cb) {}
    /** ------------------------------------------------------------------------------------------- No Copy/move ok */
    PackagedFunction(const PackagedFunction&) = delete;
    PackagedFunction& operator=(const PackagedFunction&) = delete;
    PackagedFunction(PackagedFunction&&) = default;
    PackagedFunction& operator=(PackagedFunction&&) = default;
    /** ------------------------------------------------------------------------------------------- Destructor */
    ~PackagedFunction() = default;
    /** ------------------------------------------------------------------------------------------- Call Operator
     * @brief Invokes the packaged function with its stored arguments and executes the callback if
     * provided.
     */
    void operator()() {
        if constexpr (std::is_void_v<Result>) {
            std::apply(func, args);
            if (callback) {
                callback(args);
            }
        } else {
            Result result = std::apply(func, args);
            if (callback) {
                callback(std::forward<Result>(result), args);
            }
        }
    }
};
namespace dispatch_detail {
/** --------------------------------------------------------------------------------------------------------- Is Packaged Function
 * @brief Recognizes only specializations of the packaged function template.
 */
template<typename Type>
struct IsPackagedFunction : std::false_type {};
template<typename Function, typename... Arguments>
struct IsPackagedFunction<PackagedFunction<Function, Arguments...>> : std::true_type {};
} // namespace dispatch_detail
/** --------------------------------------------------------------------------------------------------------- Packaged Function Type
 * @brief Constrains each variadic argument to a packaged function specialization.
 */
template<typename Type>
concept PackagedFunctionType =
    dispatch_detail::IsPackagedFunction<std::remove_cvref_t<Type>>::value;
/** --------------------------------------------------------------------------------------------------------- dispatch_parallel
 * @brief Parallel execution of multiple functions over GCD or OpenMP.
 * @tparam Funcs The types of the functions to execute.
 * @param funcs The functions to execute in parallel.
 */
template<PackagedFunctionType... Funcs>
inline static void dispatch_parallel(Funcs&&... funcs) {
#if defined(__APPLE__)
    dispatch_group_t group = dispatch_group_create();
    dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0);
    /// A pack cannot expand inside a block literal, so each function gets its own block.
    auto submit = [&](auto&& func) {
        auto* callable = std::addressof(func);
        dispatch_group_async(group, queue, ^{ (*callable)(); });
    };
    (submit(funcs), ...);
    dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
    dispatch_release(group);
#elif defined(_OPENMP)
    /// One task per function; the region's closing barrier waits for every task.
    #pragma omp parallel
    #pragma omp single
    {
        (dispatch_task(funcs), ...);
    }
#endif
}
/** --------------------------------------------------------------------------------------------------------- TaskForce
 * @class TaskForce
 * @brief A class that manages a pool of threads to execute tasks with warmup and post-task synchronization.
 */
class TaskForce {
private:
    /// @brief The queue that holds tasks to be executed by the thread pool.
    moodycamel::BlockingConcurrentQueue<std::pair<std::promise<void*>, void*>> queue_;
    /// @brief The currently executing task item.
    std::atomic<void*> current_item_{nullptr};
    /// @brief The promise associated with the currently executing task, used to signal its completion.
    std::atomic<std::promise<void*>*> current_promise_{nullptr};
    /// @brief The warmup function to call before executing the task.
    void (*warmup_)(void*, void*);
    /// @brief The barrier that synchronizes threads after the warmup function has been called.
    std::barrier<> post_warmup_;
    /// @brief The task function to execute after the warmup.
    void (*task_)(void*, void*);
    /// @brief The context to pass to the warmup and task functions.
    void* context_;
    /// @brief The barrier that synchronizes threads after the task function has been called.
    std::barrier<> post_task_;
    /// @brief The collection of threads that make up the thread pool.
    std::vector<std::unique_ptr<std::thread>> threads_;
    /// @brief Flag indicating whether the thread pool should stop execution.
    std::atomic<bool> stop_{false};
public:
    /** ------------------------------------------------------------------------------------------- TaskForce
     * @brief Constructs a TaskForce with the specified number of threads, warmup and task
     * functions, and context.
     * @param num_threads The number of threads to create.
     * @param warmup The warmup function to call before executing the task.
     * @param task The task function to execute.
     * @param context The context to pass to the warmup and task functions.
     */
    TaskForce(
        size_t num_threads,
        void (*warmup)(void*, void*),
        void (*task)(void*, void*),
        void* context
    ) : warmup_(warmup)
    , post_warmup_(num_threads)
    , task_(task)
    , context_(context)
    , post_task_(num_threads) {
        threads_.reserve(num_threads);
        for (size_t i = 1; i < num_threads; ++i) {
            threads_.emplace_back(std::make_unique<std::thread>([this] {
                for (;;) {  // Always meet the leader at post_warmup_, which is where it releases us on stop.
                    post_warmup_.arrive_and_wait();
                    if (stop_.load(std::memory_order_acquire)) break;
                    void* item = current_item_.load(std::memory_order_acquire);
                    task_(item, context_);
                    post_task_.arrive_and_wait();
                }
            }));
        }
        threads_.emplace_back(std::make_unique<std::thread>([this] {
            while (!stop_.load(std::memory_order_acquire)) {
                std::pair<std::promise<void*>, void*> item;
                if (queue_.wait_dequeue_timed(item, std::chrono::milliseconds(100))) {
                    current_item_.store(item.second, std::memory_order_release);
                    warmup_(item.second, context_);
                    post_warmup_.arrive_and_wait();
                    task_(item.second, context_);
                    post_task_.arrive_and_wait();
                    item.first.set_value(item.second);
                }
            }
            post_warmup_.arrive_and_wait();
        }));
    }
    /** ------------------------------------------------------------------------------------------- No copy/move */
    TaskForce(const TaskForce&) = delete;
    TaskForce& operator=(const TaskForce&) = delete;
    TaskForce(TaskForce&&) = delete;
    TaskForce& operator=(TaskForce&&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor */
    ~TaskForce() {
        stop_.store(true, std::memory_order_release);
        for (auto& thread : threads_) {
            if (thread->joinable()) {
                thread->join();
            }
        }
    }
    /** ------------------------------------------------------------------------------------------- Enqueue
     * @brief Enqueue an item to be processed by the task force.
     * @param item The item to enqueue.
     */
    std::future<void*> enqueue(void* item) {
        std::promise<void*> promise;
        auto future = promise.get_future();
        queue_.enqueue(std::make_pair(std::move(promise), item));
        return future;
    }
    /** ------------------------------------------------------------------------------------------- Stop
     * @brief Stops the task force by setting the stop flag.
     */
    void stop() {
        stop_.store(true, std::memory_order_release);
        for (auto& thread : threads_) {
            if (thread->joinable()) {
                thread->join();
            }
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- TaskForceT
 * @class TaskForceT
 * @brief A templated task force that manages concurrent execution of tasks with warmup and task phases.
 * @tparam Warmup The type of the warmup callable.
 * @tparam Task The type of the task callable.
 * @tparam Args The types of the arguments passed to the warmup and task callables.
 */
template<typename Warmup, typename Task, typename... Args>
    requires std::is_invocable_v<Warmup, Args...> && std::is_invocable_v<Task, Args...>
class TaskForceT {
private:
    /// @brief The return type of the warmup callable.
    using WarmupReturnType = std::invoke_result_t<Warmup, Args...>;
    /// @brief The return type of the task callable.
    using ReturnType = std::invoke_result_t<Task, Args...>;
    /// @brief The tuple of return types for the task callable.
    using ReturnTypes = std::conditional_t<std::is_void_v<ReturnType>,
        std::tuple<>, std::tuple<ReturnType>>;
    /// @brief The promise type associated with the task callable.
    using PromiseType = std::promise<ReturnTypes>;
    /// @brief The warmup callable.
    Warmup warmup_;
    /// @brief The task callable.
    Task task_;
    /// @brief The tuple of arguments passed to the warmup and task callables.
    std::tuple<Args...> args_;
    /// @brief The type of the concurrent queue used to store tasks and their associated promises.
    using QueueType = moodycamel::ConcurrentQueue<std::pair<PromiseType, std::tuple<Args...>>>;
    /// @brief The concurrent queue used to store tasks and their associated promises.
    QueueType queue_;
    /// @brief The currently executing warmup item.
    std::atomic<WarmupReturnType*> current_item_;
    /// @brief The currently executing task item.
    std::atomic<ReturnType*> current_task_;
    /// @brief The currently associated promise.
    std::atomic<PromiseType*> current_promise_;
    /// @brief Barrier to synchronize after warmup phase.
    std::barrier<> post_warmup_;
    /// @brief Barrier to synchronize after task phase.
    std::barrier<> post_task_;
    /// @brief The vector of worker threads.
    std::vector<std::unique_ptr<std::thread>> threads_;
    /// @brief Flag to indicate whether the task force should stop.
    std::atomic<bool> stop_{false};
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs a TaskForceT object with the given warmup and task callables and their
     * arguments.
     * @param warmup The warmup callable.
     * @param task The task callable.
     * @param args The arguments to be passed to the warmup and task callables.
     */
    TaskForceT(Warmup warmup, Task task, Args... args)
    : warmup_(std::move(warmup))
    , task_(std::move(task))
    , args_(std::make_tuple(std::move(args)...))
    , post_warmup_(1)
    , post_task_(1) {}
    /** ------------------------------------------------------------------------------------------- No Copy/Move */
    TaskForceT(const TaskForceT&) = delete;
    TaskForceT& operator=(const TaskForceT&) = delete;
    TaskForceT(TaskForceT&&) = delete;
    TaskForceT& operator=(TaskForceT&&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor */
    ~TaskForceT() {
        stop_.store(true, std::memory_order_release);
        for (auto& thread : threads_) {
            if (thread->joinable()) {
                thread->join();
            }
        }
    }
    /** ------------------------------------------------------------------------------------------- Stop the Task Force
     * @brief Stops the task force by setting the stop flag and joining all worker threads.
     */
    void stop() {
        stop_.store(true, std::memory_order_release);
        for (auto& thread : threads_) {
            if (thread->joinable()) {
                thread->join();
            }
        }
    }
    /** ------------------------------------------------------------------------------------------- Enqueue a Task
     * @brief Enqueues a task with the given arguments and returns a future for the result.
     * @param args The arguments to be passed to the task callable.
     * @return A future representing the result of the enqueued task.
     */
    auto enqueue(Args... args) {
        std::promise<ReturnTypes> promise;
        auto future = promise.get_future();
        queue_.enqueue(std::make_pair(std::move(promise), std::make_tuple(std::move(args)...)));
        return future;
    }
};
} // namespace nebula
