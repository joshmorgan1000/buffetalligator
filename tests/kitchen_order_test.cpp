/** --------------------------------------------------------------------------------------------------------- Kitchen Order Test
 * @file kitchen_order_test.cpp
 * @brief Exercises owned arguments, standard futures, callbacks, and bulk Order submission.
 */
#include <alligator/kitchen.hpp>
#include "functional_support.hpp"
#include "kitchen_test_support.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace {
using buffetalligator::Kitchen;
using buffetalligator::Order;
using functional::require;
static_assert(!std::is_copy_constructible_v<Order>);
static_assert(!std::is_copy_assignable_v<Order>);
static_assert(std::is_nothrow_move_constructible_v<Order>);
static_assert(std::is_nothrow_move_assignable_v<Order>);
static_assert(noexcept(std::declval<Order&>().execute()));
/** --------------------------------------------------------------------------------------------------------- Await
 * @brief Requires a submitted future to become ready before retrieving its value or exception.
 */
template<typename Value>
void await(std::unique_ptr<std::future<Value>>& future) {
    require(future != nullptr, "submission did not publish its future");
    require(future->wait_for(std::chrono::seconds(5)) == std::future_status::ready,
        "submitted Order did not complete");
}
/** --------------------------------------------------------------------------------------------------------- Add
 * @brief Returns the sum of two copied arguments.
 */
uint64_t add(uint64_t left, uint64_t right) {
    return left + right;
}
/** --------------------------------------------------------------------------------------------------------- Assign
 * @brief Writes a result through an explicitly borrowed reference.
 */
void assign(uint64_t& output, uint64_t value) {
    output = value;
}
/** --------------------------------------------------------------------------------------------------------- Consume
 * @brief Consumes a move-only argument and returns its value.
 */
uint64_t consume(std::unique_ptr<uint64_t> value) {
    return *value;
}
/** --------------------------------------------------------------------------------------------------------- Produce
 * @brief Returns a move-only result through a standard future.
 */
std::unique_ptr<uint64_t> produce(uint64_t value) {
    return std::make_unique<uint64_t>(value);
}
/** --------------------------------------------------------------------------------------------------------- Reference
 * @brief Returns the caller's explicitly borrowed value by reference.
 */
uint64_t& reference(uint64_t& value) {
    return value;
}
/** --------------------------------------------------------------------------------------------------------- Futures
 * @brief Checks typed, void, move-only, and reference results through the public submission API.
 */
void futures() {
    LOG_INFO_STREAM << "Checking standard futures and owned function arguments";
    std::unique_ptr<std::future<uint64_t>> result;
    uint64_t copied = 35;
    Order pending(&add, result, copied, uint64_t{7});
    copied = 100;
    Kitchen::submit(std::move(pending));
    await(result);
    require(result->get() == 42, "Order did not own its copied argument");
    uint64_t output = 0;
    std::unique_ptr<std::future<void>> completed;
    Kitchen::submit(&assign, completed, std::ref(output), uint64_t{73});
    await(completed);
    completed->get();
    require(output == 73, "void future completed before its borrowed output was written");
    Kitchen::submit(&consume, result, std::make_unique<uint64_t>(91));
    await(result);
    require(result->get() == 91, "Order did not transfer its move-only argument");
    std::unique_ptr<std::future<std::unique_ptr<uint64_t>>> owned_result;
    Kitchen::submit(&produce, owned_result, uint64_t{109});
    await(owned_result);
    require(*owned_result->get() == 109, "future lost a move-only result");
    std::unique_ptr<std::future<uint64_t&>> borrowed_result;
    Kitchen::submit(&reference, borrowed_result, std::ref(output));
    await(borrowed_result);
    require(&borrowed_result->get() == &output, "reference future changed the result identity");
}
/** --------------------------------------------------------------------------------------------------------- Callback State
 * @brief Records values observed by a typed callback.
 */
struct CallbackState {
    std::latch completed{1};
    uint64_t result = 0;
    uint64_t argument = 0;
};
/** --------------------------------------------------------------------------------------------------------- Double Value
 * @brief Produces a typed result while retaining the callback's borrowed observation state.
 */
uint64_t double_value(uint64_t value, CallbackState*) {
    return value * 2;
}
/** --------------------------------------------------------------------------------------------------------- Receive Value
 * @brief Receives the function result followed by its original arguments.
 */
void receive_value(uint64_t result, uint64_t argument, CallbackState* state) {
    state->result = result;
    state->argument = argument;
    kitchen_test::latch_arrive(&state->completed);
}
/** --------------------------------------------------------------------------------------------------------- Write Value
 * @brief Writes a value before its void-returning invocation calls the completion function.
 */
void write_value(uint64_t value, CallbackState* state) {
    state->result = value;
}
/** --------------------------------------------------------------------------------------------------------- Receive Void
 * @brief Receives the original arguments after a void-returning invocation.
 */
void receive_void(uint64_t value, CallbackState* state) {
    require(state->result == value, "void callback ran before its handler");
    state->argument = value;
    kitchen_test::latch_arrive(&state->completed);
}
/** --------------------------------------------------------------------------------------------------------- Read Owned
 * @brief Reads an owned argument without consuming it before the callback receives ownership.
 */
uint64_t read_owned(const std::unique_ptr<uint64_t>& value, CallbackState*) {
    return *value + 1;
}
/** --------------------------------------------------------------------------------------------------------- Receive Owned
 * @brief Receives ownership of the retained argument after its handler returns.
 */
void receive_owned(uint64_t result, std::unique_ptr<uint64_t> value, CallbackState* state) {
    state->result = result;
    state->argument = *value;
    kitchen_test::latch_arrive(&state->completed);
}
/** --------------------------------------------------------------------------------------------------------- Text Length
 * @brief Consumes a string value while its Order retains the original callback argument.
 */
uint64_t text_length(std::string text, CallbackState*) {
    return text.size();
}
/** --------------------------------------------------------------------------------------------------------- Receive Text
 * @brief Verifies a value-taking handler preserved its string for the callback.
 */
void receive_text(uint64_t result, std::string text, CallbackState* state) {
    require(text == "owned callback string", "handler consumed the callback's original string");
    state->result = result;
    state->argument = text.size();
    kitchen_test::latch_arrive(&state->completed);
}
/** --------------------------------------------------------------------------------------------------------- Callbacks
 * @brief Checks typed and void callbacks with result-first argument ordering.
 */
void callbacks() {
    LOG_INFO_STREAM << "Checking function pointer callbacks";
    CallbackState value_state;
    Kitchen::submit(&double_value, &receive_value, uint64_t{21}, &value_state);
    value_state.completed.wait();
    require(value_state.result == 42 && value_state.argument == 21,
        "typed callback did not receive the result and original arguments");
    CallbackState void_state;
    Kitchen::submit(&write_value, &receive_void, uint64_t{81}, &void_state);
    void_state.completed.wait();
    require(void_state.result == 81 && void_state.argument == 81,
        "void callback did not receive its original arguments");
    CallbackState owned_state;
    Kitchen::submit(&read_owned, &receive_owned, std::make_unique<uint64_t>(99), &owned_state);
    owned_state.completed.wait();
    require(owned_state.result == 100 && owned_state.argument == 99,
        "callback did not receive its retained move-only argument");
    CallbackState text_state;
    Kitchen::submit(&text_length, &receive_text, std::string("owned callback string"), &text_state);
    text_state.completed.wait();
    require(text_state.result == 21 && text_state.argument == 21,
        "callback did not receive its original string and matching result");
}
/** --------------------------------------------------------------------------------------------------------- Owned Argument
 * @brief Records destruction after transferring unique argument ownership between Orders.
 */
struct alignas(128) OwnedArgument {
    size_t* destroyed;
    uint64_t value;
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Captures the caller's lifetime observation and argument value.
     */
    OwnedArgument(size_t& count, uint64_t payload) : destroyed(&count), value(payload) {}
    OwnedArgument(const OwnedArgument&) = delete;
    OwnedArgument& operator=(const OwnedArgument&) = delete;
    /** ------------------------------------------------------------------------------------------- Move
     * @brief Transfers the sole destruction responsibility.
     */
    OwnedArgument(OwnedArgument&& other) noexcept
        : destroyed(std::exchange(other.destroyed, nullptr)), value(other.value) {}
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Counts release by the owning argument.
     */
    ~OwnedArgument() {
        if (destroyed) ++*destroyed;
    }
};
/** --------------------------------------------------------------------------------------------------------- Consume Owned
 * @brief Consumes an aligned argument and updates an explicitly borrowed output.
 */
void consume_owned(OwnedArgument argument, uint64_t& output) {
    require(reinterpret_cast<uintptr_t>(&argument) % alignof(OwnedArgument) == 0,
        "Order lost its argument alignment");
    output = argument.value;
}
/** --------------------------------------------------------------------------------------------------------- Moves
 * @brief Verifies move ownership, single execution, and abandoned invocation cleanup.
 */
void moves() {
    LOG_INFO_STREAM << "Checking move-only Order ownership and argument cleanup";
    size_t replaced_destroyed = 0;
    size_t retained_destroyed = 0;
    uint64_t output = 0;
    {
        Order source(&consume_owned, OwnedArgument(retained_destroyed, 93), std::ref(output));
        Order moved(std::move(source));
        source.execute();
        require(output == 0, "moving an Order left its source executable");
        Order destination(&consume_owned, OwnedArgument(replaced_destroyed, 17), std::ref(output));
        destination = std::move(moved);
        require(replaced_destroyed == 1, "move assignment retained the replaced invocation");
        moved.execute();
        require(output == 0, "move assignment left its source executable");
        destination.execute();
        require(output == 93, "moved Order lost its argument");
        output = 101;
        destination.execute();
        require(output == 101, "an Order executed its consumed invocation twice");
    }
    require(retained_destroyed == 1, "moved Order released its argument more than once");
    size_t abandoned_destroyed = 0;
    {
        Order abandoned(&consume_owned, OwnedArgument(abandoned_destroyed, 19), std::ref(output));
    }
    require(abandoned_destroyed == 1 && output == 101,
        "destroying an unsubmitted Order executed work or leaked its argument");
    Order empty;
    empty.execute();
}
/** --------------------------------------------------------------------------------------------------------- Fail Value
 * @brief Throws an identifiable failure from a value-returning function.
 */
uint64_t fail_value() {
    throw std::runtime_error("expected Order failure");
}
/** --------------------------------------------------------------------------------------------------------- Fail Void
 * @brief Throws an identifiable failure from a void-returning function.
 */
void fail_void() {
    throw std::runtime_error("expected void Order failure");
}
/** --------------------------------------------------------------------------------------------------------- Fail Borrowed
 * @brief Records execution before throwing from a borrowed-context handler.
 */
void fail_borrowed(void* context) {
    static_cast<std::atomic<size_t>*>(context)->fetch_add(1, std::memory_order_relaxed);
    throw std::runtime_error("expected borrowed Order failure");
}
/** --------------------------------------------------------------------------------------------------------- Expect Failure
 * @brief Requires a standard future to deliver the original handler exception.
 */
template<typename Value>
void expect_failure(std::unique_ptr<std::future<Value>>& result) {
    await(result);
    bool caught = false;
    try {
        result->get();
    } catch (const std::runtime_error&) {
        caught = true;
    }
    require(caught, "future did not deliver its handler's exception");
}
/** --------------------------------------------------------------------------------------------------------- Exceptions
 * @brief Verifies both result forms propagate exceptions and later submissions remain usable.
 */
void exceptions() {
    LOG_INFO_STREAM << "Checking standard future exceptions and worker recovery";
    std::unique_ptr<std::future<uint64_t>> value_result;
    Kitchen::submit(&fail_value, value_result);
    expect_failure(value_result);
    std::unique_ptr<std::future<void>> void_result;
    Kitchen::submit(&fail_void, void_result);
    expect_failure(void_result);
    std::atomic<size_t> borrowed_calls{0};
    std::latch borrowed_done{1};
    Kitchen::submit(&fail_borrowed, &borrowed_calls, &kitchen_test::latch_arrive, &borrowed_done);
    borrowed_done.wait();
    require(borrowed_calls.load(std::memory_order_relaxed) == 1,
        "borrowed handler failure lost execution or its completion callback");
    Kitchen::submit(&add, value_result, uint64_t{8}, uint64_t{9});
    await(value_result);
    require(value_result->get() == 17, "handler exceptions prevented subsequent execution");
}
/** --------------------------------------------------------------------------------------------------------- Count Borrowed
 * @brief Counts one borrowed invocation before its done callback runs.
 */
void count_borrowed(void* context) {
    static_cast<std::atomic<size_t>*>(context)->fetch_add(1, std::memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Bulk
 * @brief Moves a batch into the queue and verifies exactly-once borrowed completion callbacks.
 */
void bulk() {
    LOG_INFO_STREAM << "Checking borrowed contexts and bulk Order moves";
    constexpr size_t count = 256;
    std::array<std::atomic<size_t>, count> executions{};
    std::latch completed(count);
    {
        std::array<Order, count> orders;
        for (size_t index = 0; index < count; ++index) {
            orders[index] = Order(&count_borrowed, &executions[index],
                &kitchen_test::latch_arrive, &completed);
        }
        Kitchen::submit_bulk(orders.data(), orders.size());
    }
    completed.wait();
    for (const auto& execution : executions) {
        require(execution.load(std::memory_order_relaxed) == 1,
            "bulk submission lost an Order or executed one more than once");
    }
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs the public Order construction and Kitchen submission contracts.
 */
int main() {
    futures();
    callbacks();
    moves();
    exceptions();
    bulk();
    LOG_INFO_STREAM << "Kitchen Order checks passed";
    return 0;
}
