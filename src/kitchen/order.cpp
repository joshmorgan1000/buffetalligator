/** --------------------------------------------------------------------------------------------------------- Order
 * @file order.cpp
 * @brief Executes owned function calls and releases their arguments.
 */
#include <alligator/kitchen.hpp>
#include <logging.hpp>
#include <utility>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Borrowed Invocation
 * @brief Stores borrowed contexts for a handler and its optional completion callback.
 */
struct Order::BorrowedInvocation {
    void (*run)(void*);
    void* context;
    void (*done)(void*);
    void* done_context;
    /** ------------------------------------------------------------------------------------------- Execute
     * @brief Runs the borrowed handler and completion callback while preserving handler failures.
     * @param pointer The invocation containing the borrowed contexts.
     */
    static void execute(void* pointer) {
        auto& invocation = *static_cast<BorrowedInvocation*>(pointer);
        std::exception_ptr error;
        try {
            invocation.run(invocation.context);
        } catch (...) {
            error = std::current_exception();
        }
        if (invocation.done) invocation.done(invocation.done_context);
        if (error) std::rethrow_exception(error);
    }
    /** ------------------------------------------------------------------------------------------- Destroy
     * @brief Releases the invocation without destroying its borrowed contexts.
     * @param pointer The invocation to release.
     */
    static void destroy(void* pointer) {
        delete static_cast<BorrowedInvocation*>(pointer);
    }
};
/** --------------------------------------------------------------------------------------------------------- Borrowed Constructor
 * @brief Borrows handler and completion contexts until the invocation finishes.
 * @param run The handler to execute.
 * @param context The caller-owned handler context.
 * @param done The optional completion callback.
 * @param done_context The caller-owned completion context.
 */
Order::Order(
    void (*run)(void*),
    void* context,
    void (*done)(void*),
    void* done_context
)
: invocation_(new BorrowedInvocation{run, context, done, done_context})
, execute_(&BorrowedInvocation::execute)
, deleter_(&BorrowedInvocation::destroy) {}
/** --------------------------------------------------------------------------------------------------------- Move Constructor
 * @brief Transfers invocation ownership and leaves the source empty.
 * @param other The Order whose invocation ownership is transferred.
 */
Order::Order(Order&& other) noexcept
: invocation_(std::exchange(other.invocation_, nullptr))
, execute_(std::exchange(other.execute_, nullptr))
, deleter_(std::exchange(other.deleter_, nullptr)) {}
/** --------------------------------------------------------------------------------------------------------- Move Assignment
 * @brief Releases the previous invocation before transferring ownership from the source.
 * @param other The Order whose invocation ownership is transferred.
 * @return This Order after ownership transfer.
 */
Order& Order::operator=(Order&& other) noexcept {
    if (this != &other) {
        if (deleter_) deleter_(invocation_);
        invocation_ = std::exchange(other.invocation_, nullptr);
        execute_ = std::exchange(other.execute_, nullptr);
        deleter_ = std::exchange(other.deleter_, nullptr);
    }
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Releases owned invocation state without invoking an unsubmitted function.
 */
Order::~Order() {
    if (deleter_) deleter_(invocation_);
}
/** --------------------------------------------------------------------------------------------------------- Execute
 * @brief Runs an invocation once and logs failures not delivered through its future.
 */
void Order::execute() noexcept {
    if (!execute_) return;
    Order invocation(std::move(*this));
    try {
        invocation.execute_(invocation.invocation_);
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Kitchen Order failed: " << error.what();
    } catch (...) {
        LOG_ERROR_STREAM << "Kitchen Order failed with a nonstandard exception";
    }
}
} // namespace buffetalligator
