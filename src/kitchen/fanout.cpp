/** --------------------------------------------------------------------------------------------------------- Fanout Orders
 * @file fanout.cpp
 * @brief Dedicated persistent teams execute registered fanout Orders from bounded queues.
 */
#include <alligator/kitchen.hpp>
#include <logging.hpp>
#include <moodycamel/blockingconcurrentqueue.h>
#include <chrono>
#include <exception>
#include <latch>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Fanout Implementation
 * @struct FanoutTeam::Impl
 * @brief Owns one bounded producer lane and its persistent execution team.
 */
struct FanoutTeam::Impl {
    /// @brief Kitchen retaining accepted invocation accounting.
    Kitchen& kitchen_;
    /// @brief Maximum number of queued invocations.
    const size_t capacity_;
    /// @brief Preallocated storage for pending invocations.
    moodycamel::BlockingConcurrentQueue<Order> queue_;
    /// @brief Shared producer registration for this team.
    moodycamel::ProducerToken producer_;
    /// @brief Serializes access to the shared producer token.
    std::mutex producer_mutex_;
    /// @brief Persistent threads participating in every invocation.
    std::vector<std::thread> workers_;
    /// @brief Counts completed producer registrations during startup.
    std::latch startup_ready_;
    /// @brief Releases workers after registration succeeds or fails.
    std::latch startup_release_{1};
    /// @brief Serializes startup error publication.
    std::mutex startup_mutex_;
    /// @brief Retains the first worker registration failure.
    std::exception_ptr startup_error_;
    /// @brief Alternating invocation latches whose storage remains inline.
    std::optional<std::latch> rounds_[2];
    /// @brief Completion latch published for the current invocation.
    std::latch* current_round_ = nullptr;
    /// @brief Number of invocations awaiting the coordinator.
    std::atomic<size_t> queued_{0};
    /// @brief Accepted invocations whose callbacks remain incomplete.
    std::atomic<size_t> outstanding_{0};
    /// @brief Published invocation generation observed by every participant.
    std::atomic<uint32_t> generation_{0};
    /// @brief Signals parked participants to leave the team.
    std::atomic<bool> stopping_{false};
    /// @brief Elects the participant publishing the first invocation failure.
    std::atomic_flag failed_ = ATOMIC_FLAG_INIT;
    /// @brief Retains the current invocation's first failure.
    std::exception_ptr error_;
    /// @brief Invocation shared by every participant in the current generation.
    Order current_;
    /** ------------------------------------------------------------------------------------------- Thread Count
     * @brief Rejects thread counts outside the standard latch's supported participation range.
     * @param threads The requested number of participating threads.
     * @return The validated thread count.
     */
    static size_t thread_count(size_t threads) {
        if (threads == 0) ALLIGATOR_KITCHEN_THROW("Fanout thread count must be positive");
        if (threads > static_cast<size_t>(std::latch::max()))
            ALLIGATOR_KITCHEN_THROW("Fanout thread count exceeds the standard latch capacity");
        return threads;
    }
    /** ------------------------------------------------------------------------------------------- Queue Capacity
     * @brief Rejects empty or unrepresentable queue storage before constructing the queue.
     * @param capacity The requested number of pending invocations.
     * @return The validated pending capacity.
     */
    static size_t queue_capacity(size_t capacity) {
        if (capacity == 0) ALLIGATOR_KITCHEN_THROW("Fanout queue capacity must be positive");
        if (capacity > std::numeric_limits<size_t>::max() / (sizeof(Order) * 4))
            ALLIGATOR_KITCHEN_THROW("Fanout queue capacity exceeds addressable storage");
        return capacity;
    }
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Provisions queue storage and starts exactly the requested number of persistent
     * threads.
     * @param threads The number of participating threads.
     * @param capacity The maximum number of pending invocations.
     */
    Impl(size_t threads, size_t capacity)
    : kitchen_(Kitchen::inst())
    , capacity_(queue_capacity(capacity))
    , queue_(capacity_, 1, 0)
    , producer_(queue_)
    , workers_(thread_count(threads))
    , startup_ready_(static_cast<std::ptrdiff_t>(workers_.size())) {
        if (!producer_.valid()) {
            ALLIGATOR_KITCHEN_THROW("Fanout could not allocate its producer queue registration");
        }
        const size_t prepared_capacity = capacity_ + decltype(queue_)::BLOCK_SIZE;
        for (size_t index = 0; index < prepared_capacity; ++index) {
            if (!queue_.enqueue(producer_, Order{})) {
                ALLIGATOR_KITCHEN_THROW("Fanout could not preallocate its invocation queue");
            }
        }
        Order prepared;
        for (size_t index = 0; index < prepared_capacity; ++index) queue_.wait_dequeue(prepared);
        try {
            for (size_t rank = 0; rank < workers_.size(); ++rank)
                workers_[rank] = std::thread(&Impl::work, this, rank);
            startup_ready_.wait();
            if (startup_error_) std::rethrow_exception(startup_error_);
            startup_release_.count_down();
        } catch (...) {
            stopping_.store(true, std::memory_order_release);
            startup_release_.count_down();
            for (std::thread& worker : workers_) {
                if (worker.joinable()) worker.join();
            }
            throw;
        }
    }
    /** ------------------------------------------------------------------------------------------- Work
     * @brief Prepares descendant submission tokens before joining the accepted registration.
     * @param rank The participant's stable team rank.
     */
    void work(size_t rank) noexcept {
        try {
            kitchen_.prepare_producer();
        } catch (...) {
            std::lock_guard lock(startup_mutex_);
            if (!startup_error_) startup_error_ = std::current_exception();
        }
        startup_ready_.count_down();
        startup_release_.wait();
        if (stopping_.load(std::memory_order_acquire)) return;
        if (rank == 0) consume();
        else participate(rank);
    }
    /** ------------------------------------------------------------------------------------------- Execute Rank
     * @brief Runs one participant and retains the first failure for invocation completion.
     * @param rank The participant's stable team rank.
     */
    void execute_rank(size_t rank) noexcept {
        try {
            current_.invoke_rank(rank, workers_.size());
        } catch (...) {
            if (!failed_.test_and_set(std::memory_order_relaxed))
                error_ = std::current_exception();
        }
    }
    /** ------------------------------------------------------------------------------------------- Participate
     * @brief Parks between generations and arrives at the invocation's latch after execution.
     * @param rank The participant's stable team rank.
     */
    void participate(size_t rank) noexcept {
        uint32_t observed = 0;
        for (;;) {
            generation_.wait(observed, std::memory_order_acquire);
            observed = generation_.load(std::memory_order_acquire);
            if (stopping_.load(std::memory_order_acquire)) return;
            std::latch& completed = *current_round_;
            execute_rank(rank);
            completed.count_down();
        }
    }
    /** ------------------------------------------------------------------------------------------- Consume
     * @brief Publishes queued invocations to the team and retires them after every rank returns.
     */
    void consume() noexcept {
        moodycamel::ConsumerToken consumer(queue_);
        size_t round = 0;
        for (;;) {
            queue_.wait_dequeue(consumer, current_);
            if (!current_) return;
            queued_.fetch_sub(1, std::memory_order_relaxed);
            failed_.clear(std::memory_order_relaxed);
            // Each latch survives the next round so all count_down calls return before reuse.
            current_round_ = &rounds_[round].emplace(
                static_cast<std::ptrdiff_t>(workers_.size())
            );
            generation_.fetch_add(1, std::memory_order_release);
            generation_.notify_all();
            execute_rank(0);
            current_round_->arrive_and_wait();
            round ^= 1;
            current_.finish(std::exchange(error_, {}));
            kitchen_.finish_order();
            outstanding_.fetch_sub(1, std::memory_order_release);
        }
    }
    /** ------------------------------------------------------------------------------------------- Try Submit
     * @brief Publishes one invocation using preallocated storage or leaves it untouched when
     * full.
     * @param order The invocation transferred on acceptance.
     * @return Whether the queue accepted the invocation.
     */
    bool try_submit(Order&& order) {
        std::lock_guard lock(producer_mutex_);
        if (queued_.load(std::memory_order_relaxed) == capacity_) return false;
        queued_.fetch_add(1, std::memory_order_relaxed);
        outstanding_.fetch_add(1, std::memory_order_relaxed);
        kitchen_.accept_order();
        if (queue_.try_enqueue(producer_, std::move(order))) return true;
        kitchen_.finish_order();
        outstanding_.fetch_sub(1, std::memory_order_release);
        queued_.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }
    /** ------------------------------------------------------------------------------------------- Drain
     * @brief Waits for accepted invocations and their callbacks while reporting long waits.
     */
    void drain() {
        auto report_at = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (outstanding_.load(std::memory_order_acquire) != 0) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= report_at) {
                LOG_INFO_STREAM << "Kitchen fanout: completing "
                    << outstanding_.load(std::memory_order_relaxed) << " accepted orders";
                report_at = now + std::chrono::seconds(1);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    /** ------------------------------------------------------------------------------------------- Stop Participants
     * @brief Publishes a terminal generation to every parked participant.
     */
    void stop_participants() noexcept {
        stopping_.store(true, std::memory_order_release);
        generation_.fetch_add(1, std::memory_order_release);
        generation_.notify_all();
    }
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Drains accepted work before waking and joining every persistent thread.
     */
    ~Impl() {
        drain();
        if (!queue_.try_enqueue(producer_, Order{})) {
            LOG_ERROR_STREAM << "Kitchen fanout could not signal its drained queue for shutdown";
            std::terminate();
        }
        workers_[0].join();
        stop_participants();
        for (size_t rank = 1; rank < workers_.size(); ++rank) workers_[rank].join();
    }
};
/** --------------------------------------------------------------------------------------------------------- Fanout Constructor
 * @brief Registers a persistent team with preallocated pending invocation storage.
 * @param threads The number of participating threads.
 * @param capacity The maximum number of pending invocations.
 */
FanoutTeam::FanoutTeam(size_t threads, size_t capacity)
: impl_(std::make_unique<Impl>(threads, capacity)) {}
/** --------------------------------------------------------------------------------------------------------- Fanout Destructor
 * @brief Drains and releases the registered team.
 */
FanoutTeam::~FanoutTeam() = default;
/** --------------------------------------------------------------------------------------------------------- Fanout Submit
 * @brief Transfers an invocation to the registered team or reports a full pending queue.
 * @param order The invocation transferred on acceptance.
 */
void FanoutTeam::submit(Order&& order) {
    if (!try_submit(std::move(order)))
        ALLIGATOR_KITCHEN_THROW(
            "Fanout queue is full; drain or increase its registration capacity"
        );
}
/** --------------------------------------------------------------------------------------------------------- Fanout Try Submit
 * @brief Transfers an invocation only when pending queue capacity is available.
 * @param order The invocation transferred on acceptance.
 * @return Whether the queue accepted the invocation.
 */
bool FanoutTeam::try_submit(Order&& order) {
    return impl_->try_submit(std::move(order));
}
/** --------------------------------------------------------------------------------------------------------- Fanout Drain
 * @brief Waits for accepted invocations and their completion callbacks.
 */
void FanoutTeam::drain() {
    impl_->drain();
}
/** --------------------------------------------------------------------------------------------------------- Fanout Threads
 * @brief Returns the registered number of persistent participant threads.
 * @return The participating thread count.
 */
size_t FanoutTeam::threads() const noexcept {
    return impl_->workers_.size();
}
} // namespace buffetalligator
