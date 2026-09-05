#pragma once
/** --------------------------------------------------------------------------------------------------------- SliceFriend
 * @file slicefriend.hpp
 * @brief The worker order type shared by the arena internals.
 */
#include <logging.hpp>
#include <buffetalligator.hpp>
#include <array>
#include <atomic>
#include <exception>
#include <future>
#include <thread>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- BuffetOrder
 * @struct BuffetOrder
 * @brief Represents an order for the buffet with a promise, task, and context. The deleter is
 * invoked by the worker after the task runs; the destructor never calls it so self-deleting
 * deleters execute exactly once.
 */
struct BuffetOrder {
    std::promise<void*>* promise;
    void* (*task)(void*);
    void* context;
    void (*deleter)(BuffetOrder*);
    BuffetOrder(
        void* context_,
        void* (*task_)(void*),
        void (*deleter_)(BuffetOrder*)
    ) : promise(nullptr)
    , task(task_)
    , context(context_)
    , deleter(deleter_) {}
    ~BuffetOrder() = default;
    BuffetOrder(const BuffetOrder&) = delete;
    BuffetOrder& operator=(const BuffetOrder&) = delete;
    BuffetOrder(BuffetOrder&&) = delete;
    BuffetOrder& operator=(BuffetOrder&&) = delete;
    std::future<void*> get_future() {
        return promise ? promise->get_future() : std::future<void*>();
    }
    static std::pair<BuffetOrder*, std::future<void*>> create(
        void* context,
        void* (*task)(void*),
        void (*deleter)(BuffetOrder*)
    ) {
        auto order = new BuffetOrder(context, task, deleter);
        order->promise = new std::promise<void*>();
        return {order, order->promise->get_future()};
    }
};
/** --------------------------------------------------------------------------------------------------------- Ensure Heap Buffet Builtins
 * @brief Registers the built-in heap and aligned-heap placements exactly once so they always
 * hold the stable identifiers 0 and 1; defined in the library.
 */
void ensure_heap_buffet_builtins();
} // namespace buffetalligator
