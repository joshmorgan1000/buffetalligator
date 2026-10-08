#pragma once
/** --------------------------------------------------------------------------------------------------------- Kitchen
 * @file kitchen.hpp
 * @brief Owned function calls with optional futures or callbacks on a shared worker queue.
 */
#include <alligator.hpp>
#include <atomic>
#include <cstddef>
#include <exception>
#include <future>
#include <memory>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Kitchen Exception
 * @class KitchenException
 * @brief Exception type thrown by the Kitchen.
 */
class KitchenException : public threadsafe_logger::Exception {
public:
    using threadsafe_logger::Exception::Exception;
    using threadsafe_logger::Exception::what;
};
#define ALLIGATOR_KITCHEN_THROW(msg) throw KitchenException(msg)
/** --------------------------------------------------------------------------------------------------------- Order
 * @class Order
 * @brief Owns a concrete function and its arguments until execution finishes.
 */
class Order {
private:
    /** ------------------------------------------------------------------------------------------- Result Callback
     * @brief Defines callbacks receiving a result followed by the owned arguments.
     */
    template<typename Result, typename... Arguments>
    struct ResultCallback {
        using Type = void (*)(Result, Arguments...);
    };
    /** ------------------------------------------------------------------------------------------- Void Callback
     * @brief Defines callbacks receiving the owned arguments after a void function.
     */
    template<typename... Arguments>
    struct ResultCallback<void, Arguments...> {
        using Type = void (*)(Arguments...);
    };
    /** ------------------------------------------------------------------------------------------- Invocation
     * @struct Invocation
     * @brief Keeps a concrete function and its owned arguments together.
     */
    template<typename Function, typename... Arguments>
    struct Invocation {
        using ReturnType = std::invoke_result_t<Function, Arguments...>;
        using Callback = typename ResultCallback<ReturnType, Arguments...>::Type;
        Function function;
        std::tuple<Arguments...> arguments;
        std::unique_ptr<std::promise<ReturnType>> promise;
        Callback callback = nullptr;
        /** --------------------------------------------------------------------------------------- Execute Callback
         * @brief Passes the function result and retained arguments to the callback.
         */
        template<size_t... Indices>
        void execute_callback(std::index_sequence<Indices...>) {
            if constexpr (std::is_void_v<ReturnType>) {
                std::apply(function, arguments);
                callback(std::move(std::get<Indices>(arguments))...);
            } else {
                decltype(auto) result = std::apply(function, arguments);
                callback(std::forward<ReturnType>(result),
                    std::move(std::get<Indices>(arguments))...);
            }
        }
        /** --------------------------------------------------------------------------------------- Execute from Void
         * @brief Executes an owned invocation and publishes its optional future.
         * @param pointer The owned invocation.
         */
        static void execute_from_void(void* pointer) {
            auto& invocation = *static_cast<Invocation*>(pointer);
            try {
                if constexpr (std::is_void_v<ReturnType>) {
                    std::apply(invocation.function, std::move(invocation.arguments));
                    if (invocation.promise) invocation.promise->set_value();
                } else if (invocation.promise) {
                    invocation.promise->set_value(
                        std::apply(invocation.function, std::move(invocation.arguments)));
                } else {
                    static_cast<void>(
                        std::apply(invocation.function, std::move(invocation.arguments)));
                }
            } catch (...) {
                if (!invocation.promise) throw;
                invocation.promise->set_exception(std::current_exception());
            }
        }
        /** --------------------------------------------------------------------------------------- Callback from Void
         * @brief Executes the function before transferring its retained arguments to the callback.
         * @param pointer The owned invocation.
         */
        static void callback_from_void(void* pointer) {
            static_cast<Invocation*>(pointer)->execute_callback(
                std::index_sequence_for<Arguments...>{});
        }
        /** --------------------------------------------------------------------------------------- Delete from Void
         * @brief Deletes a typed invocation from its erased pointer.
         * @param pointer The owned invocation to destroy.
         */
        static void delete_from_void(void* pointer) {
            delete static_cast<Invocation*>(pointer);
        }
    };
    /** ------------------------------------------------------------------------------------------- Borrowed Invocation
     * @brief Holds borrowed handler and completion contexts.
     */
    struct BorrowedInvocation;
    /// @brief Owned invocation storage.
    void* invocation_ = nullptr;
    /// @brief Concrete invocation entry point.
    void (*execute_)(void*) = nullptr;
    /// @brief Concrete invocation destructor.
    void (*deleter_)(void*) = nullptr;
public:
    /** ------------------------------------------------------------------------------------------- Default Constructor
     * @brief Constructs an empty Order for assignment from the worker queue.
     */
    Order() noexcept = default;
    /** ------------------------------------------------------------------------------------------- Borrowed Order
     * @brief Borrows contexts that remain alive through execution and the optional callback.
     * @param run The function to run with the borrowed context.
     * @param context The borrowed context.
     * @param done The callback function to invoke when done.
     * @param done_context The context for the done callback.
     */
    Order(
        void (*run)(void*),
        void* context,
        void (*done)(void*) = nullptr,
        void* done_context = nullptr
    );
    /** ------------------------------------------------------------------------------------------- Order With Future
     * @brief Owns arguments and publishes the function's result or exception through a future.
     * @param function The concrete function to invoke.
     * @param future Receives the future for this invocation.
     * @param arguments The arguments to pass to the function.
     */
    template<typename Function, typename... Arguments>
        requires (std::is_pointer_v<std::decay_t<Function>>
            && std::is_function_v<std::remove_pointer_t<std::decay_t<Function>>>
            && std::is_invocable_v<std::decay_t<Function>, std::decay_t<Arguments>...>)
    explicit Order(
        Function function,
        std::unique_ptr<std::future<std::invoke_result_t<std::decay_t<Function>,
            std::decay_t<Arguments>...>>>& future,
        Arguments&&... arguments
    ) {
        using Invoke = Invocation<std::decay_t<Function>, std::decay_t<Arguments>...>;
        auto promise = std::make_unique<std::promise<typename Invoke::ReturnType>>();
        auto result = std::make_unique<std::future<typename Invoke::ReturnType>>(
            promise->get_future());
        auto* invocation = new Invoke{function,
            std::tuple<std::decay_t<Arguments>...>(std::forward<Arguments>(arguments)...),
            std::move(promise)};
        invocation_ = invocation;
        execute_ = &Invoke::execute_from_void;
        deleter_ = &Invoke::delete_from_void;
        future = std::move(result);
    }
    /** ------------------------------------------------------------------------------------------- Order With Callback
     * @brief Calls a function before giving its result and retained arguments to the callback.
     * @param function The concrete function to invoke.
     * @param callback Receives the result followed by the owned arguments, omitting a void result.
     * @param arguments The arguments to pass to the function.
     */
    template<typename Function, typename... Arguments>
        requires (std::is_pointer_v<std::decay_t<Function>>
            && std::is_function_v<std::remove_pointer_t<std::decay_t<Function>>>
            && std::is_invocable_v<std::decay_t<Function>, std::decay_t<Arguments>&...>)
    explicit Order(
        Function function,
        typename ResultCallback<std::invoke_result_t<std::decay_t<Function>,
            std::decay_t<Arguments>...>, std::decay_t<Arguments>...>::Type callback,
        Arguments&&... arguments
    ) {
        using Invoke = Invocation<std::decay_t<Function>, std::decay_t<Arguments>...>;
        auto* invocation = new Invoke{function,
            std::tuple<std::decay_t<Arguments>...>(std::forward<Arguments>(arguments)...),
            nullptr, callback};
        invocation_ = invocation;
        execute_ = &Invoke::callback_from_void;
        deleter_ = &Invoke::delete_from_void;
    }
    /** ------------------------------------------------------------------------------------------- Order Without Callback
     * @brief Owns decayed arguments with explicit std::ref borrowing through a concrete function.
     * @param function The concrete function to invoke.
     * @param arguments The arguments to pass to the function.
     */
    template<typename Function, typename... Arguments>
        requires (std::is_pointer_v<std::decay_t<Function>>
            && std::is_function_v<std::remove_pointer_t<std::decay_t<Function>>>
            && std::is_invocable_v<std::decay_t<Function>, std::decay_t<Arguments>...>)
    explicit Order(Function function, Arguments&&... arguments) {
        using Invoke = Invocation<std::decay_t<Function>, std::decay_t<Arguments>...>;
        auto* invocation = new Invoke{function,
            std::tuple<std::decay_t<Arguments>...>(std::forward<Arguments>(arguments)...),
            nullptr};
        invocation_ = invocation;
        execute_ = &Invoke::execute_from_void;
        deleter_ = &Invoke::delete_from_void;
    }
    /** ------------------------------------------------------------------------------------------- No Copy */
    Order(const Order&) = delete;
    Order& operator=(const Order&) = delete;
    Order(Order&& other) noexcept;
    Order& operator=(Order&& other) noexcept;
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Destroys the Order and releases its owned arguments.
     */
    ~Order();
    /** ------------------------------------------------------------------------------------------- Execute
     * @brief Executes once and logs failures not delivered through a future.
     */
    void execute() noexcept;
};
/** --------------------------------------------------------------------------------------------------------- Kitchen
 * @class Kitchen
 * @brief Schedules move-only Orders on a shared worker queue.
 */
class Kitchen {
private:
    /// @brief Queue for orders awaiting execution.
    moodycamel::BlockingConcurrentQueue<Order> order_queue_;
    /// @brief Workers processing the queue.
    std::vector<std::unique_ptr<std::thread>> threads_;
    /// @brief Requests worker exit after accepted orders finish.
    std::atomic<bool> stop_{false};
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Starts one worker for each reported hardware thread.
     */
    Kitchen();
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Completes queued orders and joins the workers.
     */
    ~Kitchen();
    /** ------------------------------------------------------------------------------------------- Work
     * @brief Processes queued orders until shutdown has emptied the queue.
     * @param kitchen The shared queue owner.
     */
    static void work(Kitchen* kitchen);
    /** ------------------------------------------------------------------------------------------- Submit Internal
     * @brief Transfers an Order into the worker queue.
     * @param order The Order to submit.
     */
    void submit_(Order&& order);
    /** ------------------------------------------------------------------------------------------- Submit Bulk Internal
     * @brief Transfers an array of Orders into the worker queue.
     * @param orders The array of Orders to submit.
     * @param count The number of Orders in the array.
     */
    void submit_bulk_(Order* orders, size_t count);
    friend struct KitchenInitializer;
public:
    /** ------------------------------------------------------------------------------------------- Instance Access
     * @brief Returns the shared Kitchen instance.
     * @return Reference to the Kitchen instance.
     */
    static Kitchen& inst();
    /** ------------------------------------------------------------------------------------------- No Copy/Move */
    Kitchen(const Kitchen&) = delete;
    Kitchen& operator=(const Kitchen&) = delete;
    Kitchen(Kitchen&&) = delete;
    Kitchen& operator=(Kitchen&&) = delete;
    /** ------------------------------------------------------------------------------------------- Submit
     * @brief Moves an Order to the worker queue or throws if allocation fails.
     * @param order The Order to submit.
     */
    static void submit(Order&& order) {
        inst().submit_(std::move(order));
    }
    /** ------------------------------------------------------------------------------------------- Submit Borrowed
     * @brief Queues borrowed contexts that remain alive until their callbacks return.
     * @param run The function to execute.
     * @param context The context to pass to the function.
     * @param done The optional completion callback.
     * @param done_context The context to pass to the callback.
     */
    static void submit(
        void (*run)(void*),
        void* context,
        void (*done)(void*) = nullptr,
        void* done_context = nullptr
    ) {
        submit(Order(run, context, done, done_context));
    }
    /** ------------------------------------------------------------------------------------------- Submit Forwarding
     * @brief Queues a concrete function with owned arguments and an optional future or callback.
     * @param function The function to execute.
     * @param arguments The constructor arguments for the Order.
     */
    template<typename Function, typename... Arguments>
        requires std::is_constructible_v<Order, Function, Arguments...>
    static void submit(Function&& function, Arguments&&... arguments) {
        submit(Order(std::forward<Function>(function), std::forward<Arguments>(arguments)...));
    }
    /** ------------------------------------------------------------------------------------------- Submit With Future
     * @brief Queues a function and returns its asynchronous result through the supplied future.
     * @param function The function to execute.
     * @param future Receives the future for this invocation.
     * @param arguments The arguments to pass to the function.
     */
    template<typename Function, typename... Arguments>
        requires (std::is_pointer_v<std::decay_t<Function>>
            && std::is_function_v<std::remove_pointer_t<std::decay_t<Function>>>
            && std::is_invocable_v<std::decay_t<Function>, std::decay_t<Arguments>...>)
    static void submit(
        Function function,
        std::unique_ptr<std::future<std::invoke_result_t<std::decay_t<Function>,
            std::decay_t<Arguments>...>>>& future,
        Arguments&&... arguments
    ) {
        submit(Order(function, future, std::forward<Arguments>(arguments)...));
    }
    /** ------------------------------------------------------------------------------------------- Submit With Callback
     * @brief Queues a function whose result and owned arguments are passed to a callback.
     * @param function The function to execute.
     * @param callback Receives the result and the owned arguments.
     * @param arguments The arguments to pass to the function.
     */
    template<typename Function, typename... Arguments>
        requires (std::is_pointer_v<std::decay_t<Function>>
            && std::is_function_v<std::remove_pointer_t<std::decay_t<Function>>>
            && std::is_invocable_v<std::decay_t<Function>, std::decay_t<Arguments>...>)
    static void submit(
        Function function,
        void (*callback)(std::invoke_result_t<std::decay_t<Function>,
            std::decay_t<Arguments>...>, std::decay_t<Arguments>...),
        Arguments&&... arguments
    ) {
        submit(Order(function, callback, std::forward<Arguments>(arguments)...));
    }
    /** ------------------------------------------------------------------------------------------- Submit Bulk
     * @brief Moves an array of Orders to the queue or throws if allocation fails.
     * @param orders The array of Orders to submit.
     * @param count The number of Orders in the array.
     */
    static void submit_bulk(Order* orders, size_t count) {
        inst().submit_bulk_(orders, count);
    }
};
/** --------------------------------------------------------------------------------------------------------- Kitchen Initializer
 * @struct KitchenInitializer
 * @brief Keeps the Kitchen alive through static owners in every including translation unit.
 */
struct KitchenInitializer {
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Retains the shared Kitchen instance.
     */
    KitchenInitializer();
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Releases the Kitchen after its final owner is destroyed.
     */
    ~KitchenInitializer();
};
/** --------------------------------------------------------------------------------------------------------- Initializer Instance
 * @brief Retains the Kitchen through this translation unit's static owners.
 */
static KitchenInitializer kitchen_initializer;
} // namespace buffetalligator
