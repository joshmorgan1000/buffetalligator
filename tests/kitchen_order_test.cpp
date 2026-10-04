/** --------------------------------------------------------------------------------------------------------- Kitchen Order Test
 * @file kitchen_order_test.cpp
 * @brief Exercises owned Order arguments, Slice publication, and persistent fanout teams.
 */
#include <alligator.hpp>
#include <alligator/kitchen.hpp>
#include "functional_support.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <exception>
#include <functional>
#include <latch>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace {
using buffetalligator::Kitchen;
using buffetalligator::Order;
using buffetalligator::OrderCompletion;
using buffetalligator::Slice;
using functional::require;
constexpr size_t team_size = 4;
constexpr size_t queued_rounds = 8;
static_assert(!std::is_copy_constructible_v<Order>);
static_assert(std::is_nothrow_move_constructible_v<Order>);
static_assert(noexcept(std::declval<Order&>().execute()));
/** --------------------------------------------------------------------------------------------------------- Watchdog
 * @brief Bounds blocking teardown and thread joins without imposing timing on test ordering.
 */
class Watchdog {
private:
    std::binary_semaphore finished_{0};
    std::thread thread_;
    /** ------------------------------------------------------------------------------------------- Monitor
     * @brief Aborts a deadlocked regression after its global deadline expires.
     */
    static void monitor(Watchdog* watchdog) {
        require(watchdog->finished_.try_acquire_for(std::chrono::seconds(30)),
                "Kitchen Order regression exceeded its deadline");
    }

public:
    Watchdog() : thread_(&Watchdog::monitor, this) {
    }
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Stops the deadline monitor after every regression has completed.
     */
    ~Watchdog() {
        finished_.release();
        thread_.join();
    }
};
/** --------------------------------------------------------------------------------------------------------- Await Ready
 * @brief Bounds readiness observation before exercising the completion's blocking wait.
 */
void await_ready(OrderCompletion& completion) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!completion.ready()) {
        require(std::chrono::steady_clock::now() < deadline, "Order completion was not published");
        std::this_thread::yield();
    }
}
/** --------------------------------------------------------------------------------------------------------- Await
 * @brief Waits for successful completion with a bounded readiness deadline.
 */
void await(OrderCompletion& completion) {
    await_ready(completion);
    completion.wait();
}
/** --------------------------------------------------------------------------------------------------------- Lifetime
 * @brief Records destruction of one resource whose tracking state remains on the caller's stack.
 */
struct Lifetime {
    std::atomic<size_t> destroyed{0};
};
/** --------------------------------------------------------------------------------------------------------- Owned Argument
 * @brief Transfers unique ownership without requiring a heap allocation.
 */
struct alignas(128) OwnedArgument {
    Lifetime* lifetime;
    uint64_t value;
    OwnedArgument(Lifetime& tracking, uint64_t payload) : lifetime(&tracking), value(payload) {
    }
    OwnedArgument(const OwnedArgument&) = delete;
    OwnedArgument& operator=(const OwnedArgument&) = delete;
    OwnedArgument(OwnedArgument&& other) noexcept
        : lifetime(std::exchange(other.lifetime, nullptr)), value(other.value) {
    }
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Records release by the unique owning argument.
     */
    ~OwnedArgument() {
        if (lifetime) {
            lifetime->destroyed.fetch_add(1, std::memory_order_release);
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Consume
 * @brief Combines an owned argument and a copied value through an explicitly borrowed output.
 */
void consume(OwnedArgument argument, uint64_t addition, uint64_t& output) {
    require(reinterpret_cast<uintptr_t>(&argument) % alignof(OwnedArgument) == 0,
            "owned argument did not retain its alignment");
    require(argument.lifetime->destroyed.load(std::memory_order_acquire) == 0,
            "Order invoked a destroyed argument");
    output = argument.value + addition;
}
/** --------------------------------------------------------------------------------------------------------- Complete
 * @brief Checks handler destruction precedes callback invocation and consumes callback ownership.
 */
void complete(OwnedArgument argument, Lifetime* handler_lifetime, uint64_t* output) {
    require(handler_lifetime->destroyed.load(std::memory_order_acquire) == 1,
            "callback ran before the handler argument was released");
    require(*output == 42, "callback did not observe the handler result");
    *output += argument.value;
}
/** --------------------------------------------------------------------------------------------------------- Assign
 * @brief Writes one result through a borrowed destination.
 */
void assign(uint64_t& output, uint64_t value) {
    output = value;
}
/** --------------------------------------------------------------------------------------------------------- Variadics And Lifetimes
 * @brief Checks move-only handler and callback ownership with explicit borrowing and one-shot completion.
 */
void variadics_and_lifetimes() {
    LOG_INFO_STREAM << "Checking variadic arguments, callbacks, and one-shot completion";
    Kitchen& kitchen = Kitchen::inst();
    kitchen.prepare_producer();
    Lifetime handler_lifetime;
    Lifetime callback_lifetime;
    uint64_t output = 0;
    uint64_t addition = 7;
    OrderCompletion completion;
    require(!completion.ready(), "new completion was already ready");
    Order order(&consume, OwnedArgument(handler_lifetime, 35), addition, std::ref(output));
    addition = 100;
    order.add_callback(&complete, OwnedArgument(callback_lifetime, 8), &handler_lifetime, &output);
    order.complete_with(completion);
    kitchen.submit(std::move(order));
    await(completion);
    require(output == 50, "variadic handler or callback lost an argument");
    require(handler_lifetime.destroyed.load() == 1 && callback_lifetime.destroyed.load() == 1,
            "completed Order retained or duplicated argument ownership");
    OrderCompletion waiting_completion;
    Order waiting = Order::create(&assign, std::ref(output), uint64_t(73));
    waiting.complete_with(waiting_completion);
    kitchen.submit_waiting(std::move(waiting));
    await(waiting_completion);
    require(output == 73, "waiting Order did not receive its variadic arguments");
    Lifetime abandoned_lifetime;
    Lifetime abandoned_callback;
    {
        Order abandoned(&consume, OwnedArgument(abandoned_lifetime, 1), uint64_t(2),
                        std::ref(output));
        abandoned.add_callback(&consume, OwnedArgument(abandoned_callback, 3), uint64_t(4),
                               std::ref(output));
        Order moved(std::move(abandoned));
        Order destination;
        destination = std::move(moved);
    }
    require(output == 73 && abandoned_lifetime.destroyed.load() == 1 &&
                abandoned_callback.destroyed.load() == 1,
            "unsubmitted moved Order invoked work or leaked its argument ownership");
    Lifetime replaced_lifetime;
    Lifetime retained_lifetime;
    Order replaced(&consume, OwnedArgument(replaced_lifetime, 2), uint64_t(3), std::ref(output));
    Order replacement(&consume, OwnedArgument(retained_lifetime, 4), uint64_t(5),
                      std::ref(output));
    replaced = std::move(replacement);
    require(replaced_lifetime.destroyed.load() == 1 && retained_lifetime.destroyed.load() == 0,
            "move assignment did not release the replaced argument exactly once");
    OrderCompletion direct_completion;
    replaced.complete_with(direct_completion);
    replaced.execute();
    await(direct_completion);
    require(output == 9 && retained_lifetime.destroyed.load() == 1,
            "direct execution lost ownership after move assignment");
}
/** --------------------------------------------------------------------------------------------------------- Order Failure
 * @brief Identifies an exception raised by a scheduled handler or callback without allocating text.
 */
struct OrderFailure : std::exception {
    const char* what() const noexcept override {
        return "intentional Order failure";
    }
};
/** --------------------------------------------------------------------------------------------------------- Callback Failure
 * @brief Distinguishes callback failure from an earlier handler failure.
 */
struct CallbackFailure : std::exception {
    const char* what() const noexcept override {
        return "intentional callback failure";
    }
};
/** --------------------------------------------------------------------------------------------------------- Fail
 * @brief Raises a typed exception while holding one move-only argument.
 */
void fail(OwnedArgument argument) {
    require(argument.lifetime->destroyed.load() == 0, "failing handler received a dead argument");
    throw OrderFailure{};
}
/** --------------------------------------------------------------------------------------------------------- Fail Callback
 * @brief Confirms handler cleanup before reporting a distinct callback exception.
 */
void fail_callback(OwnedArgument argument, Lifetime* handler_lifetime) {
    require(handler_lifetime->destroyed.load() == 1,
            "exception callback ran before handler argument cleanup");
    require(argument.lifetime->destroyed.load() == 0, "callback received a destroyed argument");
    throw CallbackFailure{};
}
/** --------------------------------------------------------------------------------------------------------- Await Failure
 * @brief Requires completion to preserve the handler's exact exception type.
 */
template <typename Exception = OrderFailure> void await_failure(OrderCompletion& completion) {
    await_ready(completion);
    bool caught = false;
    try {
        completion.wait();
    } catch (const Exception&) {
        caught = true;
    }
    require(caught, "Order completion did not rethrow the handler's exception");
}
/** --------------------------------------------------------------------------------------------------------- Exceptions
 * @brief Checks worker survival and resource release after handler and callback exceptions.
 */
void exceptions() {
    LOG_INFO_STREAM << "Checking handler and callback exception reporting";
    Lifetime handler_lifetime;
    Lifetime callback_lifetime;
    Lifetime failing_callback_lifetime;
    OrderCompletion completion;
    Order order(&fail, OwnedArgument(handler_lifetime, 1));
    order.add_callback(&fail_callback, OwnedArgument(failing_callback_lifetime, 3),
                       &handler_lifetime);
    order.complete_with(completion);
    Kitchen::inst().submit(std::move(order));
    await_failure(completion);
    require(handler_lifetime.destroyed.load() == 1 &&
                failing_callback_lifetime.destroyed.load() == 1,
            "throwing handler skipped its callback or leaked an argument");
    OrderCompletion callback_completion;
    uint64_t output = 0;
    Order callback_failure(&assign, std::ref(output), uint64_t(81));
    callback_failure.add_callback(&fail_callback, OwnedArgument(callback_lifetime, 2),
                                  &handler_lifetime);
    callback_failure.complete_with(callback_completion);
    Kitchen::inst().submit(std::move(callback_failure));
    await_failure<CallbackFailure>(callback_completion);
    require(output == 81 && callback_lifetime.destroyed.load() == 1,
            "throwing callback changed handler execution or leaked its argument");
    OrderCompletion recovered_completion;
    Order recovered(&assign, std::ref(output), uint64_t(91));
    recovered.complete_with(recovered_completion);
    Kitchen::inst().submit(std::move(recovered));
    await(recovered_completion);
    require(output == 91, "worker or completion did not recover after a reported exception");
}
/** --------------------------------------------------------------------------------------------------------- Slice Result
 * @brief Records a produced Slice's ownership identity before its return.
 */
struct SliceResult {
    void* address = nullptr;
    uint32_t identifier = UINT32_MAX;
};
/** --------------------------------------------------------------------------------------------------------- Produce Slice
 * @brief Creates a result whose original storage must become the destination's storage.
 */
Slice produce_slice(uint64_t value, SliceResult* produced) {
    Slice result(sizeof(uint64_t));
    result.get_as<uint64_t>() = value;
    produced->address = result.raw();
    produced->identifier = result.id();
    return result;
}
/** --------------------------------------------------------------------------------------------------------- Slice Publication
 * @brief Checks result ownership moves to the destination while its old aliases remain valid.
 */
void slice_publication() {
    LOG_INFO_STREAM << "Checking zero-copy Slice result publication";
    Slice destination(sizeof(uint64_t));
    destination.get_as<uint64_t>() = 17;
    Slice previous = destination;
    SliceResult produced;
    OrderCompletion completion;
    Order order =
        Order::create_slice_populator(&produce_slice, &destination, uint64_t(93), &produced);
    order.complete_with(completion);
    Kitchen::inst().submit_waiting(std::move(order));
    await(completion);
    require(destination.raw() == produced.address && destination.id() == produced.identifier,
            "Slice publication copied payload or acquired another result identifier");
    require(destination.get_as<uint64_t>() == 93 && previous.get_as<uint64_t>() == 17,
            "Slice publication lost returned data or retargeted an existing alias");
    OrderCompletion facade_completion;
    Kitchen::inst().submit_slice(&produce_slice, destination, facade_completion, uint64_t(109),
                                 &produced);
    await(facade_completion);
    require(destination.raw() == produced.address && destination.id() == produced.identifier &&
                destination.get_as<uint64_t>() == 109,
            "Kitchen Slice submission did not publish the handler's original result");
}
/** --------------------------------------------------------------------------------------------------------- Bulk Orders
 * @brief Checks move-only order arrays survive batch submission and producer-side destruction.
 */
void bulk_orders() {
    LOG_INFO_STREAM << "Checking move-only bulk submission";
    std::array<uint64_t, queued_rounds> outputs{};
    std::array<Lifetime, queued_rounds> lifetimes;
    std::array<OrderCompletion, queued_rounds> completions;
    {
        std::array<Order, queued_rounds> orders;
        for (size_t index = 0; index < orders.size(); ++index) {
            orders[index] = Order(&consume, OwnedArgument(lifetimes[index], index), uint64_t(100),
                                  std::ref(outputs[index]));
            orders[index].complete_with(completions[index]);
        }
        Kitchen::inst().submit_bulk(orders.data(), orders.size());
    }
    for (size_t index = 0; index < completions.size(); ++index) {
        await(completions[index]);
        require(outputs[index] == index + 100 && lifetimes[index].destroyed.load() == 1,
                "bulk submission lost, duplicated, or prematurely destroyed an Order");
    }
}
/** --------------------------------------------------------------------------------------------------------- Fanout Round
 * @brief Tracks each participant and a shared argument in one deterministic team invocation.
 */
struct FanoutRound {
    std::array<std::atomic<size_t>, team_size> visits{};
    std::atomic<size_t> entered{0};
    std::atomic<bool> released{true};
    std::atomic<const void*> argument_address{nullptr};
    Lifetime lifetime;
    uint64_t expected = 0;
    size_t failing_rank = SIZE_MAX;
};
/** --------------------------------------------------------------------------------------------------------- Fanout Handler
 * @brief Checks unique ranks and shared argument ownership before awaiting the caller's gate.
 */
void fanout_handler(size_t rank,
                    size_t count,
                    FanoutRound* const& round,
                    const OwnedArgument& argument) {
    require(count == team_size && rank < count, "fanout rank or team size was incorrect");
    require(round->visits[rank].fetch_add(1, std::memory_order_relaxed) == 0,
            "fanout invoked a rank more than once");
    require(reinterpret_cast<uintptr_t>(&argument) % alignof(OwnedArgument) == 0,
            "fanout arena argument did not retain its extended alignment");
    require(argument.value == round->expected && argument.lifetime == &round->lifetime &&
                round->lifetime.destroyed.load(std::memory_order_acquire) == 0,
            "fanout moved or destroyed a shared argument before every participant used it");
    const void* first_address = nullptr;
    round->argument_address.compare_exchange_strong(first_address, &argument);
    require(round->argument_address.load() == &argument,
            "fanout copied its shared argument for individual participants");
    round->entered.fetch_add(1, std::memory_order_release);
    round->released.wait(false, std::memory_order_acquire);
    if (rank == round->failing_rank) {
        throw OrderFailure{};
    }
}
using RegisteredFanout = decltype(Kitchen::inst().register_fanout(team_size, &fanout_handler));
/** --------------------------------------------------------------------------------------------------------- Await Team
 * @brief Requires every parked participant to wake before the caller releases the team.
 */
void await_team(FanoutRound& round) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (round.entered.load(std::memory_order_acquire) != team_size) {
        require(std::chrono::steady_clock::now() < deadline,
                "fanout did not start all registered participants concurrently");
        std::this_thread::yield();
    }
}
/** --------------------------------------------------------------------------------------------------------- Check Round
 * @brief Verifies all team ranks executed once and the invocation released its argument.
 */
void check_round(FanoutRound& round) {
    for (const std::atomic<size_t>& visits : round.visits) {
        require(visits.load() == 1, "fanout omitted a registered rank");
    }
    require(round.lifetime.destroyed.load() == 1,
            "fanout retained or duplicated its shared argument ownership");
}
/** --------------------------------------------------------------------------------------------------------- Fanout Isolation
 * @brief Queues independent invocations while a complete fanout team is held outside Kitchen workers.
 */
void fanout_isolation() {
    LOG_INFO_STREAM << "Checking queued fanout teams and independent Kitchen progress";
    std::array<FanoutRound, queued_rounds> rounds;
    std::array<OrderCompletion, queued_rounds> completions;
    auto fanout = Kitchen::inst().register_fanout(team_size, &fanout_handler, queued_rounds);
    require(fanout.threads() == team_size, "fanout registration changed the requested team size");
    rounds[0].released.store(false);
    for (size_t index = 0; index < rounds.size(); ++index) {
        rounds[index].expected = index + 200;
        fanout.invoke(completions[index], &rounds[index],
                      OwnedArgument(rounds[index].lifetime, rounds[index].expected));
    }
    await_team(rounds[0]);
    require(!completions[0].ready(), "fanout completed while its handlers were still blocked");
    uint64_t ordinary_result = 0;
    OrderCompletion ordinary_completion;
    Order ordinary(&assign, std::ref(ordinary_result), uint64_t(99));
    ordinary.complete_with(ordinary_completion);
    Kitchen::inst().submit(std::move(ordinary));
    await(ordinary_completion);
    require(ordinary_result == 99, "blocked fanout prevented ordinary Kitchen work");
    for (size_t index = 1; index < rounds.size(); ++index) {
        require(rounds[index].entered.load() == 0,
                "queued fanout invocation overlapped the preceding invocation");
    }
    rounds[0].released.store(true, std::memory_order_release);
    rounds[0].released.notify_all();
    for (size_t index = 0; index < rounds.size(); ++index) {
        await(completions[index]);
        check_round(rounds[index]);
    }
    fanout.drain();
    for (size_t index = 0; index < queued_rounds; ++index) {
        OrderCompletion repeated;
        FanoutRound round;
        round.expected = index + 300;
        fanout.invoke(repeated, &round, OwnedArgument(round.lifetime, round.expected));
        await(repeated);
        check_round(round);
    }
}
/** --------------------------------------------------------------------------------------------------------- Produce Fanout
 * @brief Submits separate invocation state from one synchronized concurrent producer.
 */
void produce_fanout(RegisteredFanout* fanout,
                    FanoutRound* rounds,
                    OrderCompletion* completions,
                    std::barrier<>* start,
                    size_t producer) {
    start->arrive_and_wait();
    for (size_t index = producer; index < queued_rounds; index += team_size) {
        rounds[index].expected = index + 400;
        fanout->invoke(completions[index], &rounds[index],
                       OwnedArgument(rounds[index].lifetime, rounds[index].expected));
    }
}
/** --------------------------------------------------------------------------------------------------------- Concurrent Fanout
 * @brief Checks multiple producers and registration destruction drain accepted invocations.
 */
void concurrent_fanout() {
    LOG_INFO_STREAM
        << "Checking concurrent fanout producers and draining registration destruction";
    std::array<FanoutRound, queued_rounds> rounds;
    std::array<OrderCompletion, queued_rounds> completions;
    {
        auto fanout = Kitchen::inst().register_fanout(team_size, &fanout_handler, queued_rounds);
        std::barrier start(static_cast<std::ptrdiff_t>(team_size));
        std::array<std::thread, team_size> producers;
        for (size_t index = 0; index < producers.size(); ++index) {
            producers[index] = std::thread(&produce_fanout, &fanout, rounds.data(),
                                           completions.data(), &start, index);
        }
        for (std::thread& producer : producers) {
            producer.join();
        }
    }
    for (size_t index = 0; index < rounds.size(); ++index) {
        require(completions[index].ready(), "fanout destruction returned before completion");
        completions[index].wait();
        check_round(rounds[index]);
    }
}
/** --------------------------------------------------------------------------------------------------------- Fanout Capacity
 * @brief Rejects a full bounded queue without publishing completion and reuses its completion later.
 */
void fanout_capacity() {
    LOG_INFO_STREAM << "Checking exact fanout queue capacity and rejected completion reuse";
    auto fanout = Kitchen::inst().register_fanout(team_size, &fanout_handler, 2);
    std::array<FanoutRound, 3> accepted;
    std::array<OrderCompletion, 3> completions;
    accepted[0].released.store(false);
    fanout.invoke(completions[0], &accepted[0], OwnedArgument(accepted[0].lifetime, 0));
    await_team(accepted[0]);
    for (size_t index = 1; index < accepted.size(); ++index) {
        fanout.invoke(completions[index], &accepted[index],
                      OwnedArgument(accepted[index].lifetime, 0));
    }
    FanoutRound rejected;
    OrderCompletion reusable;
    require(!fanout.try_invoke(reusable, &rejected, OwnedArgument(rejected.lifetime, 0)),
            "fanout accepted more queued invocations than its registered capacity");
    require(!reusable.ready() && rejected.entered.load() == 0 &&
                rejected.lifetime.destroyed.load() == 1,
            "rejected fanout invocation ran, published completion, or retained arguments");
    accepted[0].released.store(true, std::memory_order_release);
    accepted[0].released.notify_all();
    for (size_t index = 0; index < accepted.size(); ++index) {
        await(completions[index]);
        check_round(accepted[index]);
    }
    FanoutRound recovered;
    require(fanout.try_invoke(reusable, &recovered, OwnedArgument(recovered.lifetime, 0)),
            "drained fanout queue did not accept a previously rejected completion");
    await(reusable);
    check_round(recovered);
}
/** --------------------------------------------------------------------------------------------------------- Fanout Exceptions
 * @brief Checks one failing participant does not strand peers or prevent a later invocation.
 */
void fanout_exceptions() {
    LOG_INFO_STREAM << "Checking fanout exception completion and team reuse";
    auto fanout = Kitchen::inst().register_fanout(team_size, &fanout_handler, 2);
    OrderCompletion completion;
    FanoutRound failed;
    failed.expected = 51;
    failed.failing_rank = 1;
    fanout.invoke(completion, &failed, OwnedArgument(failed.lifetime, failed.expected));
    await_failure(completion);
    check_round(failed);
    OrderCompletion recovered_completion;
    FanoutRound recovered;
    recovered.expected = 52;
    require(fanout.try_invoke(recovered_completion, &recovered,
                              OwnedArgument(recovered.lifetime, recovered.expected)),
            "empty fanout queue rejected a new invocation");
    await(recovered_completion);
    check_round(recovered);
}
/** --------------------------------------------------------------------------------------------------------- Prepared State
 * @brief Carries plain writes through preparation, all fanout participants, and final continuation.
 */
struct PreparedState {
    uint64_t prepared = 0;
    std::array<uint64_t, team_size> results{};
    bool finalized = false;
};
/** --------------------------------------------------------------------------------------------------------- Prepare State
 * @brief Produces the ordinary Order's data before its fanout continuation can execute.
 */
void prepare_state(PreparedState* state) {
    state->prepared = 67;
}
/** --------------------------------------------------------------------------------------------------------- Use Prepared State
 * @brief Requires every fanout participant to observe the preceding Order's plain writes.
 */
void use_prepared_state(size_t rank, size_t count, PreparedState* const& state) {
    require(count == team_size && rank < count, "prepared fanout received an invalid rank");
    require(state->prepared == 67, "fanout ran before its preparation Order published data");
    state->results[rank] = state->prepared + rank;
}
/** --------------------------------------------------------------------------------------------------------- Finalize State
 * @brief Requires all fanout writes before the terminal ordinary Order executes.
 */
void finalize_state(PreparedState* state) {
    for (size_t rank = 0; rank < state->results.size(); ++rank) {
        require(state->results[rank] == 67 + rank,
                "terminal Order ran before every fanout participant completed");
    }
    state->finalized = true;
}
/** --------------------------------------------------------------------------------------------------------- Fanout Continuations
 * @brief Chains ordinary preparation to fanout and a final Order using caller-owned standard latches.
 */
void fanout_continuations() {
    LOG_INFO_STREAM
        << "Checking preparation, fanout, and terminal Order chains with standard latches";
    auto fanout = Kitchen::inst().register_fanout(team_size, &use_prepared_state, 2);
    PreparedState first;
    std::latch first_done(1);
    Order preparation(&prepare_state, &first);
    preparation.then(fanout.order(first_done, &first));
    Kitchen::inst().submit(std::move(preparation));
    first_done.wait();
    for (size_t rank = 0; rank < first.results.size(); ++rank) {
        require(first.results[rank] == 67 + rank, "fanout latch missed a prepared participant");
    }
    PreparedState second;
    std::latch final_done(1);
    OrderCompletion fanout_done;
    Order final_order(&finalize_state, &second);
    final_order.complete_with(final_done);
    Order parallel = fanout.order(fanout_done, &second);
    parallel.then(std::move(final_order));
    Order initial(&prepare_state, &second);
    initial.add_callback(std::move(parallel));
    Kitchen::inst().submit(std::move(initial));
    final_done.wait();
    await(fanout_done);
    require(second.finalized, "terminal latch was counted before the final Order executed");
}
/** --------------------------------------------------------------------------------------------------------- Failed Continuation
 * @brief Skips downstream handlers after preparation failure while completing their terminal signals.
 */
void failed_continuation() {
    LOG_INFO_STREAM << "Checking failed preparation skips fanout and completes terminal observers";
    auto fanout = Kitchen::inst().register_fanout(team_size, &fanout_handler, 2);
    Lifetime preparation_lifetime;
    FanoutRound skipped;
    uint64_t final_result = 0;
    OrderCompletion fanout_done;
    OrderCompletion final_done;
    std::latch terminal(1);
    Order final_order(&assign, std::ref(final_result), uint64_t(1));
    final_order.complete_with(final_done).complete_with(terminal);
    Order parallel = fanout.order(fanout_done, &skipped, OwnedArgument(skipped.lifetime, 0));
    parallel.then(std::move(final_order));
    Order preparation(&fail, OwnedArgument(preparation_lifetime, 0));
    preparation.then(std::move(parallel));
    Kitchen::inst().submit(std::move(preparation));
    await_failure(final_done);
    terminal.wait();
    await_failure(fanout_done);
    require(skipped.entered.load() == 0 && final_result == 0,
            "failed preparation executed a downstream handler");
    require(preparation_lifetime.destroyed.load() == 1 && skipped.lifetime.destroyed.load() == 1,
            "failed continuation retained preparation or canceled fanout arguments");
}
/** --------------------------------------------------------------------------------------------------------- Full Continuation Queue
 * @brief Propagates continuation enqueue failure through terminal completion without running rejected work.
 */
void full_continuation_queue() {
    LOG_INFO_STREAM << "Checking a full fanout queue completes rejected continuation chains";
    auto fanout = Kitchen::inst().register_fanout(team_size, &fanout_handler, 1);
    std::array<FanoutRound, 2> accepted;
    std::array<OrderCompletion, 2> accepted_done;
    accepted[0].released.store(false);
    fanout.invoke(accepted_done[0], &accepted[0], OwnedArgument(accepted[0].lifetime, 0));
    await_team(accepted[0]);
    fanout.invoke(accepted_done[1], &accepted[1], OwnedArgument(accepted[1].lifetime, 0));
    FanoutRound rejected;
    uint64_t prepared = 0;
    uint64_t final_result = 0;
    OrderCompletion fanout_done;
    OrderCompletion final_done;
    std::latch terminal(1);
    Order final_order(&assign, std::ref(final_result), uint64_t(1));
    final_order.complete_with(final_done).complete_with(terminal);
    Order parallel = fanout.order(fanout_done, &rejected, OwnedArgument(rejected.lifetime, 0));
    parallel.then(std::move(final_order));
    Order preparation(&assign, std::ref(prepared), uint64_t(81));
    preparation.then(std::move(parallel));
    Kitchen::inst().submit(std::move(preparation));
    await_failure<buffetalligator::KitchenException>(final_done);
    terminal.wait();
    await_failure<buffetalligator::KitchenException>(fanout_done);
    require(prepared == 81 && rejected.entered.load() == 0 && final_result == 0,
            "full fanout queue lost preparation or executed a rejected continuation");
    require(rejected.lifetime.destroyed.load() == 1,
            "rejected fanout continuation retained its owned arguments");
    accepted[0].released.store(true, std::memory_order_release);
    accepted[0].released.notify_all();
    for (size_t index = 0; index < accepted.size(); ++index) {
        await(accepted_done[index]);
        check_round(accepted[index]);
    }
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs public Order and fanout regressions against the production Kitchen.
 */
int main() {
    Watchdog watchdog;
    try {
        variadics_and_lifetimes();
        exceptions();
        slice_publication();
        bulk_orders();
        fanout_isolation();
        concurrent_fanout();
        fanout_capacity();
        fanout_exceptions();
        fanout_continuations();
        failed_continuation();
        full_continuation_queue();
        Kitchen::inst().drain();
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Kitchen Order regression failed: " << error.what();
        return 1;
    }
    LOG_INFO_STREAM << "Kitchen Order and fanout checks passed";
    return 0;
}
