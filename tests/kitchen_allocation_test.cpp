/** --------------------------------------------------------------------------------------------------------- Kitchen Allocation Test
 * @file kitchen_allocation_test.cpp
 * @brief Counts submitting-thread new calls on successful warmed wrappers, excluding user allocations.
 */
#include <alligator/kitchen.hpp>
#include "functional_support.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <latch>
#include <new>
#include <thread>
#include <utility>

namespace {
thread_local bool track_allocations = false;
thread_local size_t tracked_allocations = 0;
/** --------------------------------------------------------------------------------------------------------- Allocate
 * @brief Counts allocating new calls on the measured thread while preserving zero-size allocation semantics.
 */
void* allocate(size_t bytes) {
    if (track_allocations) {
        ++tracked_allocations;
    }
    void* address = std::malloc(bytes == 0 ? 1 : bytes);
    if (!address) {
        throw std::bad_alloc();
    }
    return address;
}
/** --------------------------------------------------------------------------------------------------------- Allocate Aligned
 * @brief Counts aligned new calls through the portable POSIX allocator on macOS and Linux.
 */
void* allocate_aligned(size_t bytes, std::align_val_t alignment) {
    if (track_allocations) {
        ++tracked_allocations;
    }
    void* address = nullptr;
    if (posix_memalign(&address, static_cast<size_t>(alignment), bytes == 0 ? 1 : bytes) != 0) {
        throw std::bad_alloc();
    }
    return address;
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- New
 * @brief Intercepts ordinary allocation without changing the production allocation path.
 */
void* operator new(size_t bytes) {
    return allocate(bytes);
}
/** --------------------------------------------------------------------------------------------------------- New Array
 * @brief Intercepts array allocation without changing the production allocation path.
 */
void* operator new[](size_t bytes) {
    return allocate(bytes);
}
/** --------------------------------------------------------------------------------------------------------- New Aligned
 * @brief Intercepts extended-alignment allocation.
 */
void* operator new(size_t bytes, std::align_val_t alignment) {
    return allocate_aligned(bytes, alignment);
}
/** --------------------------------------------------------------------------------------------------------- New Array Aligned
 * @brief Intercepts extended-alignment array allocation.
 */
void* operator new[](size_t bytes, std::align_val_t alignment) {
    return allocate_aligned(bytes, alignment);
}
/** --------------------------------------------------------------------------------------------------------- New Nothrow
 * @brief Preserves the nothrow allocation contract while recording the allocating call.
 */
void* operator new(size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return allocate(bytes);
    } catch (...) {
        return nullptr;
    }
}
/** --------------------------------------------------------------------------------------------------------- New Array Nothrow
 * @brief Preserves the nothrow array contract while recording the allocating call.
 */
void* operator new[](size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return allocate(bytes);
    } catch (...) {
        return nullptr;
    }
}
/** --------------------------------------------------------------------------------------------------------- New Aligned Nothrow
 * @brief Preserves the aligned nothrow contract while recording the allocating call.
 */
void* operator new(size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return allocate_aligned(bytes, alignment);
    } catch (...) {
        return nullptr;
    }
}
/** --------------------------------------------------------------------------------------------------------- New Array Aligned Nothrow
 * @brief Preserves the aligned nothrow array contract while recording the allocating call.
 */
void* operator new[](size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return allocate_aligned(bytes, alignment);
    } catch (...) {
        return nullptr;
    }
}
/** --------------------------------------------------------------------------------------------------------- Delete
 * @brief Releases storage acquired by the ordinary replacement allocator.
 */
void operator delete(void* address) noexcept {
    std::free(address);
}
/** --------------------------------------------------------------------------------------------------------- Delete Array
 * @brief Releases storage acquired by the array replacement allocator.
 */
void operator delete[](void* address) noexcept {
    std::free(address);
}
/** --------------------------------------------------------------------------------------------------------- Delete Sized
 * @brief Releases sized storage through the same replacement allocator.
 */
void operator delete(void* address, size_t) noexcept {
    std::free(address);
}
/** --------------------------------------------------------------------------------------------------------- Delete Array Sized
 * @brief Releases sized array storage through the same replacement allocator.
 */
void operator delete[](void* address, size_t) noexcept {
    std::free(address);
}
/** --------------------------------------------------------------------------------------------------------- Delete Aligned
 * @brief Releases extended-alignment storage through its POSIX allocation family.
 */
void operator delete(void* address, std::align_val_t) noexcept {
    std::free(address);
}
/** --------------------------------------------------------------------------------------------------------- Delete Array Aligned
 * @brief Releases extended-alignment array storage through its POSIX allocation family.
 */
void operator delete[](void* address, std::align_val_t) noexcept {
    std::free(address);
}
/** --------------------------------------------------------------------------------------------------------- Delete Sized Aligned
 * @brief Releases sized extended-alignment storage through its POSIX allocation family.
 */
void operator delete(void* address, size_t, std::align_val_t) noexcept {
    std::free(address);
}
/** --------------------------------------------------------------------------------------------------------- Delete Array Sized Aligned
 * @brief Releases sized extended-alignment array storage through its POSIX allocation family.
 */
void operator delete[](void* address, size_t, std::align_val_t) noexcept {
    std::free(address);
}
/** --------------------------------------------------------------------------------------------------------- Delete Nothrow
 * @brief Releases storage after failed construction through nothrow new.
 */
void operator delete(void* address, const std::nothrow_t&) noexcept {
    std::free(address);
}
/** --------------------------------------------------------------------------------------------------------- Delete Array Nothrow
 * @brief Releases array storage after failed construction through nothrow new.
 */
void operator delete[](void* address, const std::nothrow_t&) noexcept {
    std::free(address);
}
/** --------------------------------------------------------------------------------------------------------- Delete Aligned Nothrow
 * @brief Releases aligned storage after failed construction through nothrow new.
 */
void operator delete(void* address, std::align_val_t, const std::nothrow_t&) noexcept {
    std::free(address);
}
/** --------------------------------------------------------------------------------------------------------- Delete Array Aligned Nothrow
 * @brief Releases aligned array storage after failed construction through nothrow new.
 */
void operator delete[](void* address, std::align_val_t, const std::nothrow_t&) noexcept {
    std::free(address);
}
namespace {
using buffetalligator::Kitchen;
using buffetalligator::Order;
using buffetalligator::OrderCompletion;
using functional::require;
constexpr size_t team_size = 4;
constexpr size_t measured_rounds = 32;
/** --------------------------------------------------------------------------------------------------------- Allocation Window
 * @brief Records only the submitting thread's allocating new calls during one successful operation.
 */
class AllocationWindow {
private:
    size_t& recorded_;

public:
    explicit AllocationWindow(size_t& recorded) : recorded_(recorded) {
        tracked_allocations = 0;
        track_allocations = true;
    }
    AllocationWindow(const AllocationWindow&) = delete;
    AllocationWindow& operator=(const AllocationWindow&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Ends observation before assertions, logging, and synchronization can allocate.
     */
    ~AllocationWindow() {
        track_allocations = false;
        recorded_ = tracked_allocations;
    }
};
/** --------------------------------------------------------------------------------------------------------- Check Interception
 * @brief Proves ordinary, array, and aligned allocations are visible to the submitting-thread counter.
 */
void check_interception() {
    size_t allocations = 0;
    std::array<void*, 4> addresses{};
    {
        AllocationWindow window(allocations);
        addresses[0] = ::operator new(13);
        addresses[1] = ::operator new[](19);
        addresses[2] = ::operator new(128, std::align_val_t(128));
        addresses[3] = ::operator new[](256, std::align_val_t(128));
    }
    require(allocations == addresses.size(),
            "allocation interception missed an allocating new call");
    ::operator delete(addresses[0]);
    ::operator delete[](addresses[1]);
    ::operator delete(addresses[2], std::align_val_t(128));
    ::operator delete[](addresses[3], std::align_val_t(128));
}
/** --------------------------------------------------------------------------------------------------------- Await
 * @brief Acquires completion outside the measured interval with a bounded readiness deadline.
 */
void await(OrderCompletion& completion) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!completion.ready()) {
        require(std::chrono::steady_clock::now() < deadline,
                "allocation test Order did not complete");
        std::this_thread::yield();
    }
    completion.wait();
}
/** --------------------------------------------------------------------------------------------------------- Write Result
 * @brief Stores one typed result without user-owned allocations.
 */
void write_result(uint64_t* output, uint64_t value) {
    *output = value;
}
/** --------------------------------------------------------------------------------------------------------- Count Callback
 * @brief Counts completion callbacks without user-owned allocations.
 */
void count_callback(std::atomic<size_t>* callbacks) {
    callbacks->fetch_add(1, std::memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Warm Orders
 * @brief Initializes producer tokens and existing arena claims before observing wrapper allocation.
 */
void warm_orders() {
    Kitchen::inst().prepare_producer();
    uint64_t result = 0;
    std::atomic<size_t> callbacks{0};
    for (size_t index = 0; index < measured_rounds; ++index) {
        OrderCompletion completion;
        Order order(&write_result, &result, uint64_t(1));
        order.add_callback(&count_callback, &callbacks);
        order.complete_with(completion);
        if (index % 2 == 0) {
            Kitchen::inst().submit(std::move(order));
        } else {
            Kitchen::inst().submit_waiting(std::move(order));
        }
        await(completion);
    }
    require(result == 1 && callbacks.load() == measured_rounds, "Order warmup did not execute");
    Kitchen::inst().drain();
}
/** --------------------------------------------------------------------------------------------------------- Orders
 * @brief Checks successful variadic construction, callbacks, and prepared compute and waiting submissions.
 */
void orders() {
    LOG_INFO_STREAM
        << "Checking warmed Order construction and submissions for submitting-thread new calls";
    uint64_t result = 0;
    std::atomic<size_t> callbacks{0};
    for (size_t index = 0; index < measured_rounds; ++index) {
        OrderCompletion completion;
        Order order;
        size_t construction_allocations = 0;
        {
            AllocationWindow window(construction_allocations);
            order = Order::create(&write_result, &result, index + uint64_t(10));
            order.add_callback(&count_callback, &callbacks);
            order.complete_with(completion);
        }
        require(construction_allocations == 0,
                "Order or callback construction called allocating new");
        size_t submission_allocations = 0;
        {
            AllocationWindow window(submission_allocations);
            if (index % 2 == 0) {
                Kitchen::inst().submit(std::move(order));
            } else {
                Kitchen::inst().submit_waiting(std::move(order));
            }
        }
        require(submission_allocations == 0, "prepared Kitchen submission called allocating new");
        await(completion);
        require(result == index + 10 && callbacks.load() == index + 1,
                "allocation-free Order did not execute its handler and callback");
    }
}
/** --------------------------------------------------------------------------------------------------------- Bulk Orders
 * @brief Checks a prepared batch accepts arena-backed Orders without submitting-thread new calls.
 */
void bulk_orders() {
    LOG_INFO_STREAM << "Checking warmed bulk submission for submitting-thread new calls";
    std::array<uint64_t, measured_rounds> results{};
    std::array<OrderCompletion, measured_rounds> completions;
    std::array<Order, measured_rounds> orders;
    std::atomic<size_t> callbacks{0};
    size_t allocations = 0;
    {
        AllocationWindow window(allocations);
        for (size_t index = 0; index < orders.size(); ++index) {
            orders[index] = Order(&write_result, &results[index], index + uint64_t(100));
            orders[index].add_callback(&count_callback, &callbacks);
            orders[index].complete_with(completions[index]);
        }
        Kitchen::inst().submit_bulk(orders.data(), orders.size());
    }
    require(allocations == 0, "prepared bulk construction or submission called allocating new");
    for (size_t index = 0; index < completions.size(); ++index) {
        await(completions[index]);
        require(results[index] == index + 100, "allocation-free bulk submission lost a result");
    }
    require(callbacks.load() == orders.size(), "allocation-free bulk submission lost a callback");
}
/** --------------------------------------------------------------------------------------------------------- Fanout Result
 * @brief Writes one separate result per persistent team participant.
 */
void fanout_result(size_t rank,
                   size_t count,
                   std::array<uint64_t, team_size>* const& output,
                   const uint64_t& value) {
    require(count == team_size && rank < count, "allocation test fanout received an invalid rank");
    (*output)[rank] = value + rank;
}
/** --------------------------------------------------------------------------------------------------------- Fanout
 * @brief Checks a registered and warmed team accepts repeated invocations without submitting-thread new.
 */
void fanout() {
    LOG_INFO_STREAM << "Checking registered fanout invocation for submitting-thread new calls";
    auto registration =
        Kitchen::inst().register_fanout(team_size, &fanout_result, measured_rounds);
    OrderCompletion warm_completion;
    std::array<uint64_t, team_size> results{};
    registration.invoke(warm_completion, &results, uint64_t(1));
    await(warm_completion);
    for (size_t index = 0; index < measured_rounds; ++index) {
        OrderCompletion completion;
        size_t allocations = 0;
        bool accepted = true;
        {
            AllocationWindow window(allocations);
            if (index % 2 == 0) {
                registration.invoke(completion, &results, index + uint64_t(200));
            } else {
                accepted = registration.try_invoke(completion, &results, index + uint64_t(200));
            }
        }
        require(allocations == 0, "registered fanout invocation called allocating new");
        require(accepted, "empty warmed fanout queue rejected an invocation");
        await(completion);
        for (size_t rank = 0; rank < results.size(); ++rank) {
            require(results[rank] == index + 200 + rank,
                    "allocation-free fanout invocation lost a participant result");
        }
    }
    registration.drain();
}
/** --------------------------------------------------------------------------------------------------------- Chained Fanout Result
 * @brief Publishes one result from preparation data written before fanout starts.
 */
void chained_fanout_result(size_t rank,
                           size_t count,
                           uint64_t* const& prepared,
                           std::array<uint64_t, team_size>* const& output) {
    require(count == team_size && rank < count, "allocation chain received an invalid rank");
    (*output)[rank] = *prepared + rank;
}
/** --------------------------------------------------------------------------------------------------------- Chained Orders
 * @brief Warms and measures preparation-to-fanout-to-callback construction and submission without new.
 */
void chained_orders() {
    LOG_INFO_STREAM << "Checking warmed Order continuations for submitting-thread new calls";
    auto registration = Kitchen::inst().register_fanout(team_size, &chained_fanout_result, 2);
    for (size_t index = 0; index <= measured_rounds; ++index) {
        uint64_t prepared = 0;
        std::array<uint64_t, team_size> results{};
        std::atomic<size_t> callbacks{0};
        OrderCompletion parallel_done;
        std::latch terminal(1);
        size_t allocations = 0;
        {
            AllocationWindow window(allocations);
            Order final_order(&count_callback, &callbacks);
            final_order.complete_with(terminal);
            Order parallel = registration.order(parallel_done, &prepared, &results);
            parallel.then(std::move(final_order));
            Order initial(&write_result, &prepared, index + uint64_t(300));
            initial.then(std::move(parallel));
            Kitchen::inst().submit(std::move(initial));
        }
        if (index != 0) {
            require(allocations == 0, "warmed continuation construction or submission allocated");
        }
        terminal.wait();
        await(parallel_done);
        require(callbacks.load() == 1, "allocation-free continuation lost its terminal callback");
        for (size_t rank = 0; rank < results.size(); ++rank) {
            require(
                results[rank] == index + 300 + rank,
                "allocation-free continuation did not publish preparation to every fanout rank");
        }
    }
    registration.drain();
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Measures successful wrapper calls after setup without counting user copies or arena provisioning.
 */
int main() {
    try {
        LOG_INFO_STREAM << "Measuring successful warmed wrappers on the submitting thread; "
                        << "user argument allocations and background arena provisioning are "
                           "outside this check";
        check_interception();
        warm_orders();
        orders();
        bulk_orders();
        fanout();
        chained_orders();
        Kitchen::inst().drain();
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Kitchen allocation regression failed: " << error.what();
        return 1;
    }
    LOG_INFO_STREAM << "Kitchen warmed wrapper allocation checks passed";
    return 0;
}
