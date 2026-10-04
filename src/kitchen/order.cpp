/** --------------------------------------------------------------------------------------------------------- Order
 * @file order.cpp
 * @brief Executes arena-owned Orders and publishes completion after owned state is released.
 */
#include <alligator/kitchen.hpp>
#include <logging.hpp>
#include <utility>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Order Completion Finish
 * @brief Publishes the result before notifying observers and relinquishing completion access.
 * @param error The first execution or callback exception, if any.
 */
void OrderCompletion::finish(std::exception_ptr error) noexcept {
    error_ = std::move(error);
    completed_.count_down();
}
/** --------------------------------------------------------------------------------------------------------- Order Completion Wait
 * @brief Acquires completed writes through the latch and rethrows the first failure.
 */
void OrderCompletion::wait() const {
    completed_.wait();
    if (error_) std::rethrow_exception(error_);
}
/** --------------------------------------------------------------------------------------------------------- Order Countdown Constructor
 * @brief Arms a batch countdown and marks an empty batch complete immediately.
 * @param count The number of arrivals required to release observers.
 */
OrderCountdown::OrderCountdown(uint32_t count) : pending_(count) {}
/** --------------------------------------------------------------------------------------------------------- Order Countdown Arrive
 * @brief Publishes every arrival's writes and relinquishes countdown access on the last arrival.
 * @param countdown The caller-owned countdown receiving this arrival.
 */
void OrderCountdown::arrive(void* countdown) {
    static_cast<OrderCountdown*>(countdown)->pending_.count_down();
}
/** --------------------------------------------------------------------------------------------------------- Order Countdown Wait
 * @brief Acquires all callback writes after the last arrival stops accessing this countdown.
 */
void OrderCountdown::wait() { pending_.wait(); }
/** --------------------------------------------------------------------------------------------------------- Order Countdown Rearm
 * @brief Resets a drained batch countdown before externally synchronized submissions resume.
 * @param count The number of arrivals required for the next batch.
 */
void OrderCountdown::rearm(uint32_t count) {
    std::destroy_at(&pending_);
    std::construct_at(&pending_, count);
}
/** --------------------------------------------------------------------------------------------------------- Order Claim
 * @brief Claims host-accessible invocation storage from the existing arena.
 * @param bytes The number of bytes required by the invocation.
 * @return The arena claim holding invocation storage.
 */
Slice Order::claim(size_t bytes) {
    return Slice(
        bytes,
        BuffetDescriptors::descriptor_for(static_cast<AlignedHeapBuffer*>(nullptr))
    );
}
/** --------------------------------------------------------------------------------------------------------- Borrowed Order Constructor
 * @brief Borrows handler and completion contexts without allocating invocation storage.
 * @param run The handler to execute.
 * @param context The caller-owned handler context.
 * @param done The optional completion callback.
 * @param done_context The caller-owned callback context.
 */
Order::Order(
    void (*run)(void*),
    void* context,
    void (*done)(void*),
    void* done_context
) noexcept
: context_(context)
, callback_context_(done_context)
, run_(run)
, callback_(done) {}
/** --------------------------------------------------------------------------------------------------------- Order Move Constructor
 * @brief Transfers invocation ownership and leaves the source empty without signaling completion.
 * @param other The Order whose invocation ownership is transferred.
 */
Order::Order(Order&& other) noexcept
: storage_(std::move(other.storage_))
, callback_storage_(std::move(other.callback_storage_))
, context_(std::exchange(other.context_, nullptr))
, callback_context_(std::exchange(other.callback_context_, nullptr))
, run_(std::exchange(other.run_, nullptr))
, destroy_(std::exchange(other.destroy_, nullptr))
, callback_(std::exchange(other.callback_, nullptr))
, destroy_callback_(std::exchange(other.destroy_callback_, nullptr))
, parallel_(std::exchange(other.parallel_, nullptr))
, completion_(std::exchange(other.completion_, nullptr))
, continuation_(std::exchange(other.continuation_, false))
, destination_(std::exchange(other.destination_, nullptr))
, latch_(std::exchange(other.latch_, nullptr)) {}
/** --------------------------------------------------------------------------------------------------------- Order Move Assignment
 * @brief Releases the previous invocation before transferring ownership from the source.
 * @param other The Order whose invocation ownership is transferred.
 * @return This Order after ownership transfer.
 */
Order& Order::operator=(Order&& other) noexcept {
    if (this != &other) {
        clear_payload();
        clear_callback();
        storage_ = std::move(other.storage_);
        callback_storage_ = std::move(other.callback_storage_);
        context_ = std::exchange(other.context_, nullptr);
        callback_context_ = std::exchange(other.callback_context_, nullptr);
        run_ = std::exchange(other.run_, nullptr);
        destroy_ = std::exchange(other.destroy_, nullptr);
        callback_ = std::exchange(other.callback_, nullptr);
        destroy_callback_ = std::exchange(other.destroy_callback_, nullptr);
        parallel_ = std::exchange(other.parallel_, nullptr);
        completion_ = std::exchange(other.completion_, nullptr);
        continuation_ = std::exchange(other.continuation_, false);
        destination_ = std::exchange(other.destination_, nullptr);
        latch_ = std::exchange(other.latch_, nullptr);
    }
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Order Destructor
 * @brief Releases unsubmitted invocation state without invoking callbacks or signaling completion.
 */
Order::~Order() {
    clear_payload();
    clear_callback();
}
/** --------------------------------------------------------------------------------------------------------- Order Clear Payload
 * @brief Destroys owned arguments before releasing their arena claim and executable pointers.
 */
void Order::clear_payload() noexcept {
    if (destroy_) destroy_(context_);
    storage_ = Slice{};
    context_ = nullptr;
    run_ = nullptr;
    destroy_ = nullptr;
    parallel_ = nullptr;
    destination_ = nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Order Clear Callback
 * @brief Destroys callback arguments before releasing their arena claim and callback pointers.
 */
void Order::clear_callback() noexcept {
    if (destroy_callback_) destroy_callback_(callback_context_);
    callback_storage_ = Slice{};
    callback_context_ = nullptr;
    callback_ = nullptr;
    destroy_callback_ = nullptr;
    continuation_ = false;
}
/** --------------------------------------------------------------------------------------------------------- Order Then
 * @brief Owns the next Order in arena storage until this Order completes successfully.
 * @param next The Order to schedule after successful execution.
 * @return This Order with the owned continuation installed.
 */
Order& Order::then(Order&& next) {
    Slice replacement;
    Order* continuation = construct<Order>(replacement, std::move(next));
    clear_callback();
    callback_storage_ = std::move(replacement);
    callback_context_ = continuation;
    callback_ = nullptr;
    destroy_callback_ = &destroy<Order>;
    continuation_ = true;
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Order Execute
 * @brief Runs one handler and completes cleanup and callbacks while retaining its first failure.
 */
void Order::execute() noexcept {
    std::exception_ptr error;
    try {
        run_(context_);
    } catch (...) {
        error = std::current_exception();
    }
    finish(std::move(error));
}
/** --------------------------------------------------------------------------------------------------------- Order Finish
 * @brief Releases owned state around the callback before publishing completion or logging failure.
 * @param error The first execution or callback exception, if any.
 */
void Order::finish(std::exception_ptr error) noexcept {
    OrderCompletion* completion = std::exchange(completion_, nullptr);
    std::latch* latch = std::exchange(latch_, nullptr);
    clear_payload();
    if (continuation_) {
        Order next(std::move(*static_cast<Order*>(callback_context_)));
        clear_callback();
        if (error) {
            next.finish(error);
        } else {
            try {
                Kitchen::inst().submit(std::move(next));
            } catch (...) {
                error = std::current_exception();
                next.finish(error);
            }
        }
    } else {
        if (callback_) {
            try {
                callback_(callback_context_);
            } catch (...) {
                if (!error) error = std::current_exception();
            }
        }
        clear_callback();
    }
    if (completion) {
        completion->finish(std::move(error));
    } else if (error) {
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& failure) {
            LOG_ERROR_STREAM << "Kitchen Order failed: " << failure.what();
        } catch (...) {
            LOG_ERROR_STREAM << "Kitchen Order failed with a nonstandard exception";
        }
    }
    if (latch) latch->count_down();
}
} // namespace buffetalligator
