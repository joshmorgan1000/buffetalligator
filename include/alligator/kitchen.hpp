#pragma once
/** --------------------------------------------------------------------------------------------------------- Kitchen
 * @file kitchen.hpp
 * @brief Arena-owned Orders, asynchronous Slice results, and persistent worker teams.
 */
#include <alligator.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <latch>
#include <memory>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace buffetalligator {
class FanoutTeam;
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
/** --------------------------------------------------------------------------------------------------------- Order Completion
 * @class OrderCompletion
 * @brief Caller-owned completion that publishes one Order's writes and first exception.
 */
class OrderCompletion {
private:
    /// @brief Releases observers after one completed Order.
    std::latch completed_{1};
    /// @brief Stores the first exception thrown during order execution, if any.
    std::exception_ptr error_;
    /** ------------------------------------------------------------------------------------------- Finish
     * @brief Marks the OrderCompletion as finished and stores the first exception, if any.
     * @param error The exception thrown during order execution, or nullptr if none.
     */
    void finish(std::exception_ptr error) noexcept;
    /// @brief Grants the Order class access to the finish method.
    friend class Order;
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Default constructs an OrderCompletion.
     */
    OrderCompletion() = default;
    /** ------------------------------------------------------------------------------------------- No copy */
    OrderCompletion(const OrderCompletion&) = delete;
    OrderCompletion& operator=(const OrderCompletion&) = delete;
    /** ------------------------------------------------------------------------------------------- Wait
     * @brief Waits for payload cleanup and callbacks before rethrowing any failure.
     */
    void wait() const;
    /** ------------------------------------------------------------------------------------------- Ready
     * @brief Reports whether the Order has released its completion latch.
     */
    bool ready() const noexcept { return completed_.try_wait(); }
};
/** --------------------------------------------------------------------------------------------------------- Order Countdown
 * @class OrderCountdown
 * @brief Caller-owned completion for a batch whose callbacks arrive exactly once per Order.
 */
struct OrderCountdown {
private:
    /// @brief Releases observers after every expected arrival.
    std::latch pending_;
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs an OrderCountdown with the specified number of expected arrivals.
     * @param count The number of expected arrivals.
     */
    explicit OrderCountdown(uint32_t count = 1);
    /** ------------------------------------------------------------------------------------------- No copy */
    OrderCountdown(const OrderCountdown&) = delete;
    OrderCountdown& operator=(const OrderCountdown&) = delete;
    /** ------------------------------------------------------------------------------------------- Arrive
     * @brief Completes one arrival and publishes the final callback's writes.
     * @param countdown The OrderCountdown being arrived.
     */
    static void arrive(void* countdown);
    /** ------------------------------------------------------------------------------------------- Wait
     * @brief Waits until every callback has finished accessing this countdown.
     */
    void wait();
    /** ------------------------------------------------------------------------------------------- Ready
     * @brief Reports whether every expected callback has arrived at the latch.
     * @return True if every expected callback has arrived, false otherwise.
     */
    bool ready() const noexcept { return pending_.try_wait(); }
    /** ------------------------------------------------------------------------------------------- Rearm
     * @brief Reuses a drained countdown after its observers have finished.
     * @param count The new number of expected arrivals.
     */
    void rearm(uint32_t count);
};
/** --------------------------------------------------------------------------------------------------------- Order
 * @class Order
 * @brief Owns typed arguments in the Slice arena until execution and completion finish.
 */
class Order {
private:
    /// @brief Storage for the order's owned arguments.
    Slice storage_;
    /// @brief Storage for the order's callback arguments.
    Slice callback_storage_;
    /// @brief Context pointer for the order's main function.
    void* context_ = nullptr;
    /// @brief Context pointer for the order's callback function.
    void* callback_context_ = nullptr;
    /// @brief Function pointer for the order's main function.
    void (*run_)(void*) = nullptr;
    /// @brief Function pointer for destroying the order's main function context.
    void (*destroy_)(void*) noexcept = nullptr;
    /// @brief Function pointer for the order's callback function.
    void (*callback_)(void*) = nullptr;
    /// @brief Function pointer for destroying the order's callback context.
    void (*destroy_callback_)(void*) noexcept = nullptr;
    /// @brief Function pointer for the order's parallel function.
    void (*parallel_)(void*, size_t, size_t) = nullptr;
    /// @brief Pointer to the order's completion object.
    OrderCompletion* completion_ = nullptr;
    /// @brief Indicates whether the order is a continuation.
    bool continuation_ = false;
    /// @brief Pointer to the order's destination fanout team.
    FanoutTeam* destination_ = nullptr;
    /// @brief Pointer to the order's latch.
    std::latch* latch_ = nullptr;
    /** ------------------------------------------------------------------------------------------- Invocation
     * @struct Invocation
     * @brief Keeps a concrete function and its owned arguments together in arena storage.
     */
    template<typename Function, typename... Arguments>
    struct Invocation {
        /// @brief The concrete function to be invoked.
        Function function;
        /// @brief The owned arguments for the function.
        std::tuple<Arguments...> arguments;
        /// @brief Pointer to the target Slice where the invocation is stored.
        Slice* target = nullptr;
        /** ----------------------------------------------------------------------------- Constructor
         * @brief Constructs the invocation with the given function and incoming
         * arguments.
         * @param handler The concrete function to be invoked.
         * @param incoming The incoming arguments to be forwarded to the function.
         */
        template<typename... Incoming>
        Invocation(Function handler, Incoming&&... incoming)
        : function(handler), arguments(std::forward<Incoming>(incoming)...) {}
    };
    /** ------------------------------------------------------------------------------------------- Claim
     * @brief Claims host-accessible argument storage from the existing Slice arena.
     * @param bytes The number of bytes to claim from the Slice arena.
     * @return A Slice representing the claimed storage.
     */
    static Slice claim(size_t bytes);
    /** ------------------------------------------------------------------------------------------- Construct
     * @brief Constructs one typed invocation without copying its payload bytes.
     * @tparam Payload The type of the invocation payload.
     * @tparam Arguments The types of the arguments to be forwarded to the payload constructor.
     * @param storage The Slice representing the storage for the invocation.
     * @param arguments The arguments to be forwarded to the payload constructor.
     * @return A pointer to the constructed payload.
     */
    template<typename Payload, typename... Arguments>
    [[nodiscard]] static Payload* construct(Slice& storage, Arguments&&... arguments) {
        constexpr size_t padding = alignof(Payload) > 64 ? alignof(Payload) - 1 : 0;
        storage = claim(sizeof(Payload) + padding);
        void* address = storage.raw();
        size_t available = storage.size_bytes();
        std::align(alignof(Payload), sizeof(Payload), address, available);
        return std::construct_at(static_cast<Payload*>(address),
            std::forward<Arguments>(arguments)...);
    }
    /** ------------------------------------------------------------------------------------------- Destroy
     * @brief Destroys a typed invocation before returning its Slice claim.
     * @tparam Payload The type of the invocation payload.
     * @param context Pointer to the invocation payload to be destroyed.
     */
    template<typename Payload>
    static void destroy(void* context) noexcept {
        std::destroy_at(static_cast<Payload*>(context));
    }
    /** ------------------------------------------------------------------------------------------- Invoke
     * @brief Consumes each owned argument once through the concrete function signature.
     * @tparam Payload The type of the invocation payload.
     * @param context Pointer to the invocation payload to be invoked.
     */
    template<typename Payload>
    static void invoke(void* context) {
        Payload& payload = *static_cast<Payload*>(context);
        static_cast<void>(std::apply(payload.function, std::move(payload.arguments)));
    }
    /** ------------------------------------------------------------------------------------------- Invoke Slice
     * @brief Transfers the returned Slice into the caller's live destination without a data copy.
     * @tparam Payload The type of the invocation payload.
     * @param context Pointer to the invocation payload to be invoked.
     */
    template<typename Payload>
    static void invoke_slice(void* context) {
        Payload& payload = *static_cast<Payload*>(context);
        *payload.target = std::apply(payload.function, std::move(payload.arguments));
    }
    /** ------------------------------------------------------------------------------------------- Invoke Team
     * @brief Shares immutable arguments while passing each participant its rank and team size.
     * @tparam Payload The type of the invocation payload.
     * @param payload The invocation payload to be shared.
     * @param rank The rank of the current participant.
     * @param count The total number of participants.
     * @param Indices The index sequence for unpacking the payload arguments.
     */
    template<typename Payload, size_t... Indices>
    static void invoke_team(
        const Payload& payload,
        size_t rank,
        size_t count,
        std::index_sequence<Indices...>
    ) {
        static_cast<void>(
            std::invoke(
                payload.function,
                rank,
                count,
                std::get<Indices>(payload.arguments)...
            )
        );
    }
    /** ------------------------------------------------------------------------------------------- Invoke Parallel
     * @brief Enters the concrete fanout handler with the same immutable invocation for every
     * rank.
     * @tparam Payload The type of the invocation payload.
     * @param context Pointer to the invocation payload to be invoked.
     * @param rank The rank of the current participant.
     * @param count The total number of participants.
     */
    template<typename Payload>
    static void invoke_parallel(void* context, size_t rank, size_t count) {
        const Payload& payload = *static_cast<Payload*>(context);
        invoke_team(payload, rank, count,
            std::make_index_sequence<std::tuple_size_v<decltype(payload.arguments)>>{});
    }
    /** ------------------------------------------------------------------------------------------- Clear Payload
     * @brief Clears the stored invocation payload.
     */
    void clear_payload() noexcept;
    /** ------------------------------------------------------------------------------------------- Clear Callback
     * @brief Clears the stored callback.
     */
    void clear_callback() noexcept;
    /** ------------------------------------------------------------------------------------------- Invoke Rank
     * @brief Invokes the stored parallel function for the given rank and count.
     * @param rank The rank of the current participant.
     * @param count The total number of participants.
     */
    void invoke_rank(size_t rank, size_t count) { parallel_(context_, rank, count); }
    /** ------------------------------------------------------------------------------------------- Finish
     * @brief Finishes the order, optionally with an error.
     * @param error The exception pointer representing an error, if any.
     */
    void finish(std::exception_ptr error = {}) noexcept;
    /// @brief Grants access to internal members for friend classes.
    friend class FanoutTeam;
    friend class Kitchen;
    template<typename> friend class FanoutOrder;
public:
    /** ------------------------------------------------------------------------------------------- Default Constructor
     * @brief Constructs an empty Order.
     */
    Order() noexcept = default;
    /** ------------------------------------------------------------------------------------------- Borrowed Order
     * @brief Borrows raw contexts that remain alive through their execution and callbacks.
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
    ) noexcept;
    /** ------------------------------------------------------------------------------------------- Variadic Order
     * @brief Owns decayed arguments with explicit std::ref borrowing through a concrete function.
     * @param function The concrete function to invoke.
     * @param arguments The arguments to pass to the function.
     */
    template<typename Function, typename... Arguments>
        requires (std::is_pointer_v<std::decay_t<Function>>
            && std::is_function_v<std::remove_pointer_t<std::decay_t<Function>>>
            && std::is_invocable_v<std::decay_t<Function>, std::decay_t<Arguments>...>)
    explicit Order(Function function, Arguments&&... arguments) {
        using Payload = Invocation<std::decay_t<Function>, std::decay_t<Arguments>...>;
        context_ = construct<Payload>(storage_, function, std::forward<Arguments>(arguments)...);
        destroy_ = &destroy<Payload>;
        run_ = &invoke<Payload>;
    }
    /** ------------------------------------------------------------------------------------------- No copy */
    Order(const Order&) = delete;
    Order& operator=(const Order&) = delete;
    Order(Order&& other) noexcept;
    Order& operator=(Order&& other) noexcept;
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Destroys the Order and releases any owned resources.
     */
    ~Order();
    /** ------------------------------------------------------------------------------------------- Create
     * @brief Constructs an arena-owned Order from a concrete function and its arguments.
     * @tparam Function The type of the concrete function.
     * @tparam Arguments The types of the arguments to pass to the function.
     * @param function The concrete function to invoke.
     * @param arguments The arguments to pass to the function.
     * @return An Order that owns the provided function and arguments.
     */
    template<typename Function, typename... Arguments>
    static Order create(Function function, Arguments&&... arguments) {
        return Order(function, std::forward<Arguments>(arguments)...);
    }
    /** ------------------------------------------------------------------------------------------- Slice Populator
     * @brief Installs a handler's returned Slice into a destination kept alive until completion.
     * @tparam Function The type of the concrete function.
     * @tparam Arguments The types of the arguments to pass to the function.
     * @param function The concrete function to invoke.
     * @param target The destination Slice to populate.
     * @param arguments The arguments to pass to the function.
     * @return An Order that owns the provided function and arguments.
     */
    template<typename Function, typename... Arguments>
        requires std::is_same_v<std::invoke_result_t<Function, std::decay_t<Arguments>...>, Slice>
    static Order create_slice_populator(
        Function function, Slice* target, Arguments&&... arguments
    ) {
        using Payload = Invocation<Function, std::decay_t<Arguments>...>;
        Order order(function, std::forward<Arguments>(arguments)...);
        static_cast<Payload*>(order.context_)->target = target;
        order.run_ = &invoke_slice<Payload>;
        return order;
    }
private:
    /** ------------------------------------------------------------------------------------------- Fanout Invocation
     * @brief Owns immutable arguments for a registered team's concrete rank-aware handler.
     * @tparam Function The type of the concrete function.
     * @tparam Arguments The types of the arguments to pass to the function.
     * @param function The concrete function to invoke.
     * @param arguments The arguments to pass to the function.
     * @return An Order that owns the provided function and arguments.
     */
    template<typename Function, typename... Arguments>
        requires (std::is_pointer_v<Function>
            && std::is_function_v<std::remove_pointer_t<Function>>
            && std::is_invocable_v<Function, size_t, size_t, const std::decay_t<Arguments>&...>)
    static Order create_fanout(Function function, Arguments&&... arguments) {
        using Payload = Invocation<Function, std::decay_t<Arguments>...>;
        Order order;
        order.context_ = construct<Payload>(order.storage_, function,
            std::forward<Arguments>(arguments)...);
        order.destroy_ = &destroy<Payload>;
        order.parallel_ = &invoke_parallel<Payload>;
        return order;
    }
public:
    /** ------------------------------------------------------------------------------------------- Callback
     * @brief Owns callback arguments until the callback runs after payload destruction.
     * @tparam Function The type of the concrete function.
     * @tparam Arguments The types of the arguments to pass to the function.
     * @param function The concrete function to invoke.
     * @param arguments The arguments to pass to the function.
     */
    template<typename Function, typename... Arguments>
        requires (std::is_pointer_v<Function>
            && std::is_function_v<std::remove_pointer_t<Function>>
            && std::is_invocable_v<Function, std::decay_t<Arguments>...>)
    void add_callback(Function function, Arguments&&... arguments) {
        using Payload = Invocation<Function, std::decay_t<Arguments>...>;
        Slice replacement;
        Payload* payload = construct<Payload>(replacement, function,
            std::forward<Arguments>(arguments)...);
        clear_callback();
        callback_storage_ = std::move(replacement);
        callback_context_ = payload;
        callback_ = &invoke<Payload>;
        destroy_callback_ = &destroy<Payload>;
    }
    /** ------------------------------------------------------------------------------------------- Then
     * @brief Schedules the next owned Order after success or forwards failure to its completion.
     * @param next The next Order to schedule after success.
     */
    Order& then(Order&& next);
    /** ------------------------------------------------------------------------------------------- Order Callback
     * @brief Installs another owned Order as the success continuation.
     * @param next The next Order to schedule after success.
     */
    void add_callback(Order&& next) { then(std::move(next)); }
    /** ------------------------------------------------------------------------------------------- Completion
     * @brief Borrows completion storage that outlives the Order's execution and callback.
     * @param completion The completion storage to borrow.
     */
    Order& complete_with(OrderCompletion& completion) noexcept {
        completion_ = &completion;
        return *this;
    }
    /** ------------------------------------------------------------------------------------------- Latch Completion
     * @brief Counts down one caller-owned latch arrival after the Order and its callback finish.
     * @param completion The latch to count down.
     */
    Order& complete_with(std::latch& completion) noexcept {
        latch_ = &completion;
        return *this;
    }
    /** ------------------------------------------------------------------------------------------- Execute
     * @brief Executes once and reports handler or callback failures through completion or
     * logging.
     */
    void execute() noexcept;
    /** ------------------------------------------------------------------------------------------- Bool Conversion
     * @brief Returns true if the Order has a valid handler or parallel execution context.
     * @return True if the Order has a valid handler or parallel execution context.
     */
    explicit operator bool() const noexcept { return run_ != nullptr || parallel_ != nullptr; }
};
/** --------------------------------------------------------------------------------------------------------- Fanout Team
 * @class FanoutTeam
 * @brief Owns a dedicated parked team and a bounded invocation queue allocated during registration.
 */
class FanoutTeam {
private:
    /// @brief Implementation details for the FanoutTeam, hidden from the public interface.
    struct Impl;
    /// @brief Pointer to the implementation details.
    std::unique_ptr<Impl> impl_;
    /// @brief Friend declaration for the Kitchen class to allow access to private members.
    friend class Kitchen;
    /// @brief Friend declaration for the FanoutOrder class template to allow access to private members.
    template<typename> friend class FanoutOrder;
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs a FanoutTeam with the specified number of threads and queue capacity.
     * @param threads The number of threads to allocate for the team.
     * @param capacity The capacity of the preallocated invocation queue.
     */
    FanoutTeam(size_t threads, size_t capacity);
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Destroys the FanoutTeam and releases its resources.
     */
    ~FanoutTeam();
    /** ------------------------------------------------------------------------------------------- No Copy/Move */
    FanoutTeam(const FanoutTeam&) = delete;
    FanoutTeam& operator=(const FanoutTeam&) = delete;
    FanoutTeam(FanoutTeam&&) = delete;
    FanoutTeam& operator=(FanoutTeam&&) = delete;
    /** ------------------------------------------------------------------------------------------- Submit
     * @brief Accepts a rank-aware Order or throws when the preallocated queue is full.
     * @param order The rank-aware Order to submit to the team.
     */
    void submit(Order&& order);
    /** ------------------------------------------------------------------------------------------- Try Submit
     * @brief Moves an accepted Order and leaves a rejected Order untouched when the queue is
     * full.
     * @param order The rank-aware Order to try to submit to the team.
     * @return True if the Order was accepted, false if the queue was full.
     */
    bool try_submit(Order&& order);
    /** ------------------------------------------------------------------------------------------- Drain
     * @brief Waits for accepted invocations and callbacks while external producers are quiescent.
     */
    void drain();
    /** ------------------------------------------------------------------------------------------- Threads
     * @brief Returns the number of threads allocated for the team.
     * @return The number of threads.
     */
    size_t threads() const noexcept;
};
/** --------------------------------------------------------------------------------------------------------- Fanout Order
 * @class FanoutOrder
 * @brief Binds a concrete handler to a reusable team whose invocations execute serially.
 * @tparam Function The type of the handler function to bind to the team.
 */
template<typename Function>
class FanoutOrder {
private:
    /// @brief The handler function bound to the team.
    Function function_;
    /// @brief The fanout team to which the order will be submitted.
    FanoutTeam team_;
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs a FanoutOrder with the specified number of threads, handler function, and
     * queue capacity.
     * @param threads The number of threads to allocate for the fanout team.
     * @param function The handler function to bind to the team.
     * @param capacity The capacity of the fanout team's queue.
     */
    FanoutOrder(size_t threads, Function function, size_t capacity)
    : function_(function), team_(threads, capacity) {}
    /** ------------------------------------------------------------------------------------------- Prepare Order
     * @brief Creates an owned fanout invocation for later submission or another Order's callback.
     * @param completion The completion object to be notified when the order is complete.
     * @param arguments The arguments to pass to the handler function.
     * @return The created Order object.
     */
    template<typename Completion, typename... Arguments>
    Order order(Completion& completion, Arguments&&... arguments) {
        Order invocation = Order::create_fanout(function_, std::forward<Arguments>(arguments)...);
        invocation.destination_ = &team_;
        invocation.complete_with(completion);
        return invocation;
    }
    /** ------------------------------------------------------------------------------------------- Invoke
     * @brief Queues shared immutable arguments and publishes completion after every rank
     * finishes.
     * @param completion The completion object to be notified when the order is complete.
     * @param arguments The arguments to pass to the handler function.
     */
    template<typename Completion, typename... Arguments>
    void invoke(Completion& completion, Arguments&&... arguments) {
        team_.submit(order(completion, std::forward<Arguments>(arguments)...));
    }
    /** ------------------------------------------------------------------------------------------- Try Invoke
     * @brief Reports a full queue without arming completion or invoking the handler.
     * @param completion The completion object to be notified if the order could not be submitted.
     * @param arguments The arguments to pass to the handler function.
     * @return True if the order was successfully submitted, false if the queue was full.
     */
    template<typename Completion, typename... Arguments>
    bool try_invoke(Completion& completion, Arguments&&... arguments) {
        return team_.try_submit(order(completion, std::forward<Arguments>(arguments)...));
    }
    /** ------------------------------------------------------------------------------------------- Drain
     * @brief Waits for all submitted orders to complete.
     */
    void drain() { team_.drain(); }
    /** ------------------------------------------------------------------------------------------- Threads
     * @brief Returns the number of threads in the team.
     * @return The number of threads.
     */
    size_t threads() const noexcept { return team_.threads(); }
};
/** --------------------------------------------------------------------------------------------------------- Kitchen
 * @class Kitchen
 * @brief Schedules move-only Orders on prestarted compute and waiting pools.
 */
class Kitchen {
private:
    /// @brief Implementation details hidden from the public interface.
    struct Impl;
    /// @brief Pointer to the implementation details.
    std::unique_ptr<Impl> impl_;
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs the Kitchen object.
     */
    Kitchen();
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Destructs the Kitchen object.
     */
    ~Kitchen();
    /** ------------------------------------------------------------------------------------------- Accept Order
     * @brief Notifies the kitchen that a new order has been accepted.
     */
    void accept_order() noexcept;
    /** ------------------------------------------------------------------------------------------- Finish Order
     * @brief Notifies the kitchen that an order has been completed.
     */
    void finish_order() noexcept;
    /// @brief Grants access to KitchenInitializer and FanoutTeam for internal operations.
    friend struct KitchenInitializer;
    friend class FanoutTeam;
public:
    /** ------------------------------------------------------------------------------------------- Instance Access
     * @brief Singleton access to the Kitchen instance.
     * @return Reference to the Kitchen instance.
     */
    static Kitchen& inst();
    /** ------------------------------------------------------------------------------------------- No Copy/Move */
    Kitchen(const Kitchen&) = delete;
    Kitchen& operator=(const Kitchen&) = delete;
    Kitchen(Kitchen&&) = delete;
    Kitchen& operator=(Kitchen&&) = delete;
    /** ------------------------------------------------------------------------------------------- Set Max Threads
     * @brief Drains and resizes the compute team while external producers are quiescent.
     * @param threads The maximum number of threads to set for the compute team.
     */
    void set_max_threads(size_t threads);
    /** ------------------------------------------------------------------------------------------- Max Threads
     * @brief Retrieves the maximum number of threads allowed for the compute team.
     * @return The maximum number of threads.
     */
    size_t max_threads() const noexcept;
    /** ------------------------------------------------------------------------------------------- Prepare Producer
     * @brief Registers this thread's queue tokens during setup before allocation-free submissions.
     */
    void prepare_producer();
    /** ------------------------------------------------------------------------------------------- Submit
     * @brief Moves one Order to compute or throws when its preallocated queue capacity is
     * exhausted.
     * @param order The Order to submit.
     */
    void submit(Order&& order);
    /** ------------------------------------------------------------------------------------------- Try Submit
     * @brief Attempts to move one Order to compute without throwing.
     * @param order The Order to submit.
     * @return True if the Order was successfully submitted, false otherwise.
     */
    bool try_submit(Order&& order);
    /** ------------------------------------------------------------------------------------------- Submit Borrowed
     * @brief Queues borrowed contexts that remain alive until their callbacks return.
     * @param run The function to execute.
     * @param context The context to pass to the run function.
     * @param done The function to call upon completion (optional).
     * @param done_context The context to pass to the done function (optional).
     */
    void submit(
        void (*run)(void*),
        void* context,
        void (*done)(void*) = nullptr,
        void* done_context = nullptr
    );
    /** ------------------------------------------------------------------------------------------- Submit Bulk
     * @brief Moves a batch of ordinary compute Orders or rejects it without moving any record.
     * @param orders Pointer to the array of Orders to submit.
     * @param count The number of Orders in the array.
     */
    void submit_bulk(Order* orders, size_t count);
    /** ------------------------------------------------------------------------------------------- Try Submit Bulk
     * @brief Attempts to move a batch of ordinary compute Orders without throwing.
     * @param orders Pointer to the array of Orders to submit.
     * @param count The number of Orders in the array.
     * @return True if the batch was successfully submitted, false otherwise.
     */
    bool try_submit_bulk(Order* orders, size_t count);
    /** ------------------------------------------------------------------------------------------- Bulk Submit
     * @brief Moves a batch of ordinary compute Orders from a vector without throwing.
     * @param orders The vector of Orders to submit.
     */
    void submit_bulk(std::vector<Order>&& orders) { submit_bulk(orders.data(), orders.size()); }
    /** ------------------------------------------------------------------------------------------- Submit Waiting
     * @brief Moves one Order to the dedicated pool for storage or blocking operations.
     * @param order The Order to submit.
     */
    void submit_waiting(Order&& order);
    /** ------------------------------------------------------------------------------------------- Try Submit Waiting
     * @brief Attempts to move one Order to the dedicated pool for storage or blocking operations
     * without throwing.
     * @param order The Order to submit.
     * @return True if the Order was successfully submitted, false otherwise.
     */
    bool try_submit_waiting(Order&& order);
    /** ------------------------------------------------------------------------------------------- Submit Waiting Function
     * @brief Moves a function-based Order to the dedicated pool for storage or blocking operations.
     * @param run The function to run.
     * @param context The context to pass to the run function.
     * @param done The completion callback function.
     * @param done_context The context to pass to the completion callback.
     */
    void submit_waiting(
        void (*run)(void*),
        void* context,
        void (*done)(void*) = nullptr,
        void* done_context = nullptr
    );
    /** ------------------------------------------------------------------------------------------- Submit Slice
     * @brief Schedules a Slice-returning storage handler with an explicit destination and
     * completion.
     * @param function The function to execute for the Slice.
     * @param target The target Slice to populate.
     * @param completion The completion callback for the operation.
     * @param arguments Additional arguments to pass to the function.
     */
    template<typename Function, typename... Arguments>
    void submit_slice(
        Function function,
        Slice& target,
        OrderCompletion& completion,
        Arguments&&... arguments
    ) {
        Order order = Order::create_slice_populator(function, &target,
            std::forward<Arguments>(arguments)...);
        order.complete_with(completion);
        submit_waiting(std::move(order));
    }
    /** ------------------------------------------------------------------------------------------- Register Fanout
     * @brief Starts exactly threads dedicated participants with capacity queued invocations.
     * @param threads The number of threads to dedicate to the fanout.
     * @param function The function to execute for each fanout invocation.
     * @param capacity The capacity of the queued invocations for each thread.
     */
    template<typename Function>
        requires (std::is_pointer_v<Function>
            && std::is_function_v<std::remove_pointer_t<Function>>)
    FanoutOrder<Function> register_fanout(
        size_t threads,
        Function function,
        size_t capacity = 4096
    ) {
        return FanoutOrder<Function>(threads, function, capacity);
    }
    /** ------------------------------------------------------------------------------------------- Drain
     * @brief Waits for accepted Orders and fanout callbacks while external producers are
     * quiescent.
     */
    void drain();
};
/** --------------------------------------------------------------------------------------------------------- Kitchen Initializer
 * @struct KitchenInitializer
 * @brief Keeps the Kitchen alive through public static owners in every including translation unit.
 */
struct KitchenInitializer {
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Initializes the KitchenInitializer and ensures the Kitchen is alive.
     */
    KitchenInitializer();
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Destroys the KitchenInitializer and releases the Kitchen if no other owners exist.
     */
    ~KitchenInitializer();
};
/** ------------------------------------------------------------------------------------------- Kitchen Initializer Instance
 * @brief Creates a static instance of KitchenInitializer to ensure the Kitchen is alive.
 */
static KitchenInitializer kitchen_initializer;
} // namespace buffetalligator
