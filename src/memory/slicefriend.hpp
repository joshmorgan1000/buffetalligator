#pragma once
/** --------------------------------------------------------------------------------------------------------- SliceFriend
 * @file slicefriend.hpp
 * @brief The worker order type shared by the arena internals.
 */
#include <logging.hpp>
#include <buffetalligator.hpp>
#include <memory/alligator.hpp>
#include <array>
#include <atomic>
#include <exception>
#include <future>
#include <thread>
#include <memory>

namespace buffetalligator {

class SliceFriend {
public:
    /** --------------------------------------------------------------------------------------------------------- BuffetOrder
     * @struct BuffetOrder
     * @brief Represents an order for the buffet with a promise, task, and context. The deleter is
     * invoked by the worker after the task runs; the destructor never calls it so self-deleting
     * deleters execute exactly once.
     */
    struct BuffetOrder {
        void* encapsulated = nullptr;
        void* (*convert_and_execute)(void*, const BuffetOrder*);
        void (*main)(BuffetOrder*) = nullptr;
        void (*deleter)(BuffetOrder*);
        template<typename RetType, typename Func, typename... Args>
            requires std::is_invocable_r_v<RetType, Func, Args...>
        struct EncapsulatedTask {
            Func func;
            std::tuple<Args...> args;
            EncapsulatedTask(
                Func f,
                Args... a
            ) : func(f), args(std::make_tuple(a...)) {
                return_type_converter = [](void* result) -> RetType {
                    return *static_cast<RetType*>(result);
                };
            }
            RetType (*return_type_converter)(void* result);
            RetType execute() {
                return std::apply(func, args);
            }
            std::promise<RetType> promise;
            std::future<RetType> get_future() {
                return promise.get_future();
            }
            void execute_and_set_promise() {
                try {
                    RetType result = execute();
                    promise.set_value(result);
                } catch (...) {
                    promise.set_exception(std::current_exception());
                }
            }
        };
        template<typename RetType, typename Func, typename... Args>
            requires std::is_invocable_r_v<RetType, Func, Args...>
        static std::pair<std::unique_ptr<BuffetOrder>, std::future<RetType>> bind(Func f, Args... a) {
            std::unique_ptr<BuffetOrder> order = std::make_unique<BuffetOrder>();
            order->encapsulated = new EncapsulatedTask<RetType, Func, Args...>(f, a...);
            auto future = static_cast<EncapsulatedTask<RetType, Func, Args...>*>(order->encapsulated)->get_future();
            order->convert_and_execute = [](void* encapsulated, const BuffetOrder*) -> void* {
                auto task = static_cast<EncapsulatedTask<RetType, Func, Args...>*>(encapsulated);
                RetType result = task->execute();
                return static_cast<void*>(new RetType(result));
            };
            order->main = [](BuffetOrder* myself) {
                auto task = static_cast<EncapsulatedTask<RetType, Func, Args...>*>(myself->encapsulated);
                RetType result = task->execute();
                task->promise.set_value(std::move(result));
            };
            order->deleter = [](BuffetOrder* order) {
                delete static_cast<EncapsulatedTask<RetType, Func, Args...>*>(order->encapsulated);
            };
            return {std::move(order), std::move(future)};
        }
        void execute() const {
            if (convert_and_execute) {
                convert_and_execute(encapsulated, this);
            }
        }
        BuffetOrder() = default;
        BuffetOrder(const BuffetOrder&) = delete;
        BuffetOrder& operator=(const BuffetOrder&) = delete;
        BuffetOrder(BuffetOrder&&) = delete;
        BuffetOrder& operator=(BuffetOrder&&) = delete;
        ~BuffetOrder() {
            if (deleter) {
                deleter(this);
            }
        }
    };
    /**
     * @brief A placeholder function for demonstration purposes.
     * @param ptr A void pointer parameter.
     */
    static void do_somthing_fun(void* ptr);
    /** --------------------------------------------------------------------------------------------------------- Execute async
     * @brief Executes a function asynchronously by enqueuing it as a BuffetOrder.
     * @tparam RetType The return type of the function.
     * @tparam Func The type of the function to execute.
     * @tparam Args The types of the arguments to pass to the function.
     * @param f The function to execute.
     * @param a The arguments to pass to the function.
     * @return A std::future representing the result of the asynchronous execution.
     */
    template<typename RetType, typename Func, typename... Args>
    inline static std::future<RetType> execute_async(Func f, Args... a) {
        auto [order, future] = BuffetOrder::bind<RetType>(f, a...);
        do_somthing_fun(order.release());
        return std::move(future);
    }
};
} // namespace buffetalligator
