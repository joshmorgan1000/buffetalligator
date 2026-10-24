/** --------------------------------------------------------------------------------------------------------- Chomp Queue
 * @file chompqueue.cpp
 * @brief Per-producer segment chains whose pushes publish with one store, drained by one or two
 * consumers that switch from single to bulk dispatch at the high-water mark.
 *
 * - Each producer appends Chomp ids to its own chain of 4 KiB segments and publishes with a store
 *   of its tail, so producers share no written cache line.
 * - Consumers read a lane under a consumer-only lock and return drained segments to its producer.
 * - Depth is the sum of tail minus head over lanes, rescanned every quarter high-water mark.
 */
#include <alligator.hpp>
#include <alligator/containers.hpp>
#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

namespace buffetalligator {
namespace {
constexpr uint64_t SEGMENT_SLOTS = 1024;  ///< Chomp ids per segment.
constexpr uint32_t NULL_ID = 0xFFFFFFFFu;
/** --------------------------------------------------------------------------------------------------------- Segment
 * @struct Segment
 * @brief One link of a lane's chain, reused once every id in it has been read.
 */
struct alignas(64) Segment {
    std::atomic<Segment*> next{nullptr};  ///< Successor in the chain or the spare list.
    alignas(64) uint32_t ids[SEGMENT_SLOTS];  ///< Published Chomp ids.
};
} // namespace
/** --------------------------------------------------------------------------------------------------------- Lane
 * @struct Lane
 * @brief One producer's chain, with producer and consumer fields on separate cache lines.
 */
struct alignas(128) ChompQueue::Lane {
    std::atomic<uint64_t> tail{0};  ///< Items published, stored only by the producer.
    uint32_t* write_slot = nullptr;  ///< Next id slot in the producer's segment.
    uint32_t* write_end = nullptr;  ///< End of the producer's segment.
    Segment* write = nullptr;  ///< Segment the producer fills.
    Segment* cache = nullptr;  ///< Spare segments the producer took in one exchange.
    uint64_t published = 0;  ///< Producer copy of tail.
    alignas(128) std::atomic<uint64_t> head{0};  ///< Items read, stored under the consumer lock.
    std::atomic_flag busy;  ///< Consumer-only lock; producers never touch it.
    Segment* read = nullptr;  ///< Segment holding head.
    uint64_t read_end = SEGMENT_SLOTS;  ///< Index one past the read segment.
    alignas(128) std::atomic<Segment*> spare{nullptr};  ///< Drained segments returned by consumers.
    std::atomic<bool> bound{false};  ///< Whether a Producer handle holds this lane.
    /** ------------------------------------------------------------------------------------------- Grow
     * @brief Links a spare or new segment once the producer's segment is full.
     */
    void grow();
    /** ------------------------------------------------------------------------------------------- Take
     * @brief Moves up to count published ids into output under the consumer lock.
     * @return The Chomps moved.
     */
    size_t take(Chomp* output, size_t count);
};
/** --------------------------------------------------------------------------------------------------------- State
 * @struct State
 * @brief Lanes, thresholds, handlers, and the wake protocol shared by every handle.
 */
struct ChompQueue::State {
    std::unique_ptr<Lane[]> lanes;
    std::unique_ptr<std::atomic<bool>[]> consumers_bound;  ///< Whether a Consumer holds each index.
    size_t producers;
    size_t consumers;
    size_t high_water;
    size_t max_batch;
    size_t refresh;  ///< Singles dispatched between full depth rescans.
    Single single;
    Bulk bulk;
    alignas(128) std::atomic<uint32_t> sleepers{0};  ///< Parked consumers; producers only read it.
    alignas(128) std::atomic<uint32_t> signal{0};  ///< Bumped to wake parked consumers.
    alignas(128) std::atomic<bool> closed{false};  ///< Set once producers have finished.
};
/** --------------------------------------------------------------------------------------------------------- Grow
 * @brief Takes the returned spares in one exchange when the local cache is empty.
 */
void ChompQueue::Lane::grow() {
    if (!cache) cache = spare.exchange(nullptr, std::memory_order_acquire);
    Segment* next = cache ? cache : new Segment;
    if (cache) cache = cache->next.load(std::memory_order_relaxed);
    next->next.store(nullptr, std::memory_order_relaxed);
    write->next.store(next, std::memory_order_release);
    write = next;
    write_slot = next->ids;
    write_end = next->ids + SEGMENT_SLOTS;
}
/** --------------------------------------------------------------------------------------------------------- Take
 * @brief Copies ids a segment run at a time and returns each drained segment to the producer.
 */
size_t ChompQueue::Lane::take(Chomp* output, size_t count) {
    while (busy.test_and_set(std::memory_order_acquire)) busy.wait(true, std::memory_order_relaxed);
    uint64_t position = head.load(std::memory_order_relaxed);
    const size_t taken = static_cast<size_t>(std::min<uint64_t>(count,
        tail.load(std::memory_order_acquire) - position));
    for (size_t done = 0; done < taken;) {
        if (position == read_end) {
            Segment* drained = read;
            read = drained->next.load(std::memory_order_acquire);
            read_end += SEGMENT_SLOTS;
            Segment* top = spare.load(std::memory_order_relaxed);
            do {
                drained->next.store(top, std::memory_order_relaxed);
            } while (!spare.compare_exchange_weak(top, drained, std::memory_order_release,
                std::memory_order_relaxed));
        }
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(taken - done, read_end - position));
        const uint32_t* source = read->ids + (position & (SEGMENT_SLOTS - 1));
        for (size_t index = 0; index < chunk; ++index) output[done + index].id_ = source[index];
        position += chunk;
        done += chunk;
    }
    head.store(position, std::memory_order_release);
    busy.clear(std::memory_order_release);
    busy.notify_one();
    return taken;
}
/** --------------------------------------------------------------------------------------------------------- Constructor
 * @brief Gives every lane its first segment and fixes the thresholds and handlers.
 */
ChompQueue::ChompQueue(
    size_t producers,
    size_t consumers,
    size_t high_water,
    size_t max_batch,
    Single single,
    Bulk bulk
) : state_(nullptr) {
    if (!producers || consumers < 1 || consumers > 2 || !high_water || !max_batch || !single || !bulk)
        ALLIGATOR_THROW("ChompQueue needs producers, one or two consumers, positive thresholds, "
            "and both handlers");
    state_ = new State;
    State& state = *state_;
    state.lanes = std::make_unique<Lane[]>(producers);
    state.consumers_bound = std::make_unique<std::atomic<bool>[]>(consumers);
    state.producers = producers;
    state.consumers = consumers;
    state.high_water = high_water;
    state.max_batch = max_batch;
    state.refresh = std::max<size_t>(1, high_water / 4);
    state.single = single;
    state.bulk = bulk;
    for (size_t index = 0; index < producers; ++index) {
        Lane& lane = state.lanes[index];
        lane.write = lane.read = new Segment;
        lane.write_slot = lane.write->ids;
        lane.write_end = lane.write->ids + SEGMENT_SLOTS;
    }
}
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Releases every undelivered Chomp, then every segment in the chains and spare lists.
 */
ChompQueue::~ChompQueue() {
    State& state = *state_;
    for (size_t index = 0; index < state.producers; ++index) {
        Lane& lane = state.lanes[index];
        const uint64_t tail = lane.tail.load(std::memory_order_acquire);
        Segment* segment = lane.read;
        uint64_t segment_end = lane.read_end;
        for (uint64_t head = lane.head.load(std::memory_order_relaxed); head < tail; ++head) {
            if (head == segment_end) {
                segment = segment->next.load(std::memory_order_relaxed);
                segment_end += SEGMENT_SLOTS;
            }
            Chomp undelivered;
            undelivered.id_ = segment->ids[head & (SEGMENT_SLOTS - 1)];
        }
        for (Segment* list : {lane.read, lane.spare.load(std::memory_order_acquire), lane.cache}) {
            while (list) {
                Segment* next = list->next.load(std::memory_order_relaxed);
                delete list;
                list = next;
            }
        }
    }
    delete state_;
}
/** --------------------------------------------------------------------------------------------------------- Producer
 * @brief Binds an unheld producer lane.
 */
ChompQueue::Producer ChompQueue::producer(size_t index) {
    if (index >= state_->producers || state_->lanes[index].bound.exchange(true, std::memory_order_acquire))
        ALLIGATOR_THROW("ChompQueue producer index is out of range or already bound");
    return Producer(&state_->lanes[index], state_);
}
/** --------------------------------------------------------------------------------------------------------- Consumer
 * @brief Binds an unheld consumer index.
 */
ChompQueue::Consumer ChompQueue::consumer(size_t index) {
    if (index >= state_->consumers
        || state_->consumers_bound[index].exchange(true, std::memory_order_acquire))
        ALLIGATOR_THROW("ChompQueue consumer index is out of range or already bound");
    return Consumer(state_, index);
}
/** --------------------------------------------------------------------------------------------------------- Close
 * @brief Publishes closure and wakes every parked consumer.
 */
void ChompQueue::close() noexcept {
    state_->closed.store(true, std::memory_order_seq_cst);
    state_->signal.fetch_add(1, std::memory_order_release);
    state_->signal.notify_all();
}
/** --------------------------------------------------------------------------------------------------------- Reset
 * @brief Reopens the queue for another round of producers.
 */
void ChompQueue::reset() noexcept { state_->closed.store(false, std::memory_order_release); }
/** --------------------------------------------------------------------------------------------------------- Producer Destructor
 * @brief Releases the lane binding; every push is already published.
 */
ChompQueue::Producer::~Producer() { lane_->bound.store(false, std::memory_order_release); }
/** --------------------------------------------------------------------------------------------------------- Push
 * @brief Writes the id, publishes it with one tail store, and wakes a parked consumer if any.
 */
void ChompQueue::Producer::push(Chomp&& chomp) {
    Lane& lane = *lane_;
    if (lane.write_slot == lane.write_end) lane.grow();
    *lane.write_slot++ = std::exchange(chomp.id_, NULL_ID);
    lane.tail.store(++lane.published, std::memory_order_seq_cst);
    if (state_->sleepers.load(std::memory_order_seq_cst)) {
        state_->signal.fetch_add(1, std::memory_order_release);
        state_->signal.notify_all();
    }
}
/** --------------------------------------------------------------------------------------------------------- Push Bulk
 * @brief Writes the ids a segment at a time, publishing each segment's run with one tail store.
 */
void ChompQueue::Producer::push(std::span<Chomp> chomps) {
    Lane& lane = *lane_;
    for (size_t done = 0; done < chomps.size();) {
        if (lane.write_slot == lane.write_end) lane.grow();
        const size_t chunk = std::min<size_t>(chomps.size() - done,
            static_cast<size_t>(lane.write_end - lane.write_slot));
        for (size_t index = 0; index < chunk; ++index)
            lane.write_slot[index] = std::exchange(chomps[done + index].id_, NULL_ID);
        lane.write_slot += chunk;
        lane.published += chunk;
        done += chunk;
        lane.tail.store(lane.published, std::memory_order_seq_cst);
    }
    if (state_->sleepers.load(std::memory_order_seq_cst)) {
        state_->signal.fetch_add(1, std::memory_order_release);
        state_->signal.notify_all();
    }
}
/** --------------------------------------------------------------------------------------------------------- Consumer Destructor
 * @brief Releases the consumer index.
 */
ChompQueue::Consumer::~Consumer() {
    state_->consumers_bound[index_].store(false, std::memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Run
 * @brief Rescans lane depths; at or above the high-water mark it fills one bulk batch across lanes,
 * below it dispatches singles until the next rescan, and with nothing published it parks.
 */
void ChompQueue::Consumer::run(void* context) {
    State& state = *state_;
    const size_t producers = state.producers;
    std::vector<uint64_t> available(producers);
    std::unique_ptr<Chomp[]> batch(new Chomp[state.max_batch]);
    size_t lane = index_ % producers;
    for (;;) {
        const bool closed = state.closed.load(std::memory_order_seq_cst);
        uint64_t depth = 0;
        for (size_t index = 0; index < producers; ++index) {
            const Lane& scanned = state.lanes[index];
            available[index] = scanned.tail.load(std::memory_order_seq_cst)
                - scanned.head.load(std::memory_order_relaxed);
            depth += available[index];
        }
        if (depth == 0) {
            if (closed) return;
            // Producers read sleepers after their tail store, so a push either shows here or wakes us.
            const uint32_t epoch = state.signal.load(std::memory_order_acquire);
            state.sleepers.fetch_add(1, std::memory_order_seq_cst);
            bool published = state.closed.load(std::memory_order_seq_cst);
            for (size_t index = 0; index < producers && !published; ++index) {
                const Lane& scanned = state.lanes[index];
                published = scanned.tail.load(std::memory_order_seq_cst)
                    != scanned.head.load(std::memory_order_relaxed);
            }
            if (!published) state.signal.wait(epoch, std::memory_order_acquire);
            state.sleepers.fetch_sub(1, std::memory_order_relaxed);
            continue;
        }
        if (depth >= state.high_water) {
            size_t count = 0;
            for (size_t scanned = 0; scanned < producers && count < state.max_batch; ++scanned) {
                if (available[lane])
                    count += state.lanes[lane].take(batch.get() + count, state.max_batch - count);
                lane = lane + 1 == producers ? 0 : lane + 1;
            }
            if (count) state.bulk(context, std::span<Chomp>(batch.get(), count));
            for (size_t index = 0; index < count; ++index) batch[index].free();
            continue;
        }
        // Remaining never exceeds the summed estimates, so some lane always has one available.
        for (uint64_t remaining = std::min<uint64_t>(depth, state.refresh); remaining;) {
            while (!available[lane]) lane = lane + 1 == producers ? 0 : lane + 1;
            Chomp item;
            if (!state.lanes[lane].take(&item, 1)) {
                remaining -= std::min(remaining, available[lane]);
                available[lane] = 0;
                continue;
            }
            --available[lane];
            --remaining;
            state.single(context, std::move(item));
        }
    }
}
} // namespace buffetalligator
