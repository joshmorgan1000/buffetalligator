/** --------------------------------------------------------------------------------------------------------- Slice Map
 * @file slicemap.cpp
 * @brief Michael lock-free hash buckets sorted by hash with hazard-protected payload replacement.
 */
#include <alligator.hpp>
#include "simd.hpp"
#include <array>
#include <bit>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace buffetalligator {
namespace {
/** --------------------------------------------------------------------------------------------------------- Retired Map Object
 * @brief Couples retired storage with its concrete destructor.
 */
struct RetiredMapObject {
    void* pointer;
    void (*destroy)(void*);
};
/// @brief Hazard slot a lookup leaves armed on the index it searched.
constexpr size_t kPinnedStateSlot = 2;
/** --------------------------------------------------------------------------------------------------------- Map Hazard Record
 * @brief Separates independently written thread announcements onto distinct cache lines.
 */
struct alignas(128) MapHazardRecord {
    std::atomic<bool> claimed{false};
    std::array<std::atomic<void*>, SliceMap::kHazardPtrsPerThread> pointers{};
};
/** --------------------------------------------------------------------------------------------------------- Map Orphan
 * @brief Transfers a protected retirement out of an exiting thread.
 */
struct MapOrphan {
    RetiredMapObject retired;
    MapOrphan* next;
};
/** --------------------------------------------------------------------------------------------------------- Map Hazard Domain
 * @brief Owns reusable thread records and retirements whose originating threads have exited.
 */
struct MapHazardDomain {
    std::array<MapHazardRecord, SliceMap::kHazardMaxThreads> records{};
    std::atomic<size_t> high_water{0};
    std::atomic<MapOrphan*> orphans{nullptr};
    /** ------------------------------------------------------------------------------------------- Orphan
     * @brief Publishes an orphan without popping or recycling a concurrently observed head.
     */
    void orphan(MapOrphan* node) {
        node->next = orphans.load(std::memory_order_relaxed);
        while (!orphans.compare_exchange_weak(node->next, node,
            std::memory_order_release, std::memory_order_relaxed)) {}
    }
};
constinit MapHazardDomain map_hazards;
/** --------------------------------------------------------------------------------------------------------- CPU Relax
 * @brief Issues the architecture's pause hint instead of a de-prioritizing yield syscall.
 */
inline void cpu_relax() {
#if defined(__aarch64__)
    __builtin_arm_yield();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}
/** --------------------------------------------------------------------------------------------------------- Map Thread Context
 * @brief Reclaims private retirements against globally ordered hazard announcements.
 */
struct MapThreadContext {
    MapHazardRecord* record = nullptr;
    std::vector<RetiredMapObject> retired;
    std::vector<RetiredMapObject> reclaimable;
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Claims a reusable record before announcing any protected pointer.
     */
    MapThreadContext() {
        retired.reserve(SliceMap::kHazardMaxThreads * SliceMap::kHazardPtrsPerThread
            + SliceMap::kRetireBatch);
        for (size_t index = 0; index < map_hazards.records.size(); ++index) {
            bool available = false;
            if (!map_hazards.records[index].claimed.compare_exchange_strong(available, true,
                std::memory_order_acquire, std::memory_order_relaxed)) continue;
            record = &map_hazards.records[index];
            size_t previous = map_hazards.high_water.load(std::memory_order_seq_cst);
            while (previous <= index && !map_hazards.high_water.compare_exchange_weak(
                previous, index + 1, std::memory_order_seq_cst)) {}
            return;
        }
        throw std::length_error("SliceMap hazard domain has 256 simultaneously registered threads");
    }
    /** ------------------------------------------------------------------------------------------- Collect
     * @brief Orders removals against announcements before destroying unprotected objects.
     */
    void collect() {
        MapOrphan* orphaned = map_hazards.orphans.exchange(nullptr, std::memory_order_acquire);
        const size_t records = map_hazards.high_water.load(std::memory_order_seq_cst);
        const bool pinned =
            record->pointers[kPinnedStateSlot].load(std::memory_order_seq_cst) != nullptr;
        if (records <= 1 && !pinned && orphaned == nullptr) {
            for (const auto& object : retired) object.destroy(object.pointer);
            retired.clear();
            return;
        }
        std::array<uint64_t, SliceMap::kHazardMaxThreads * SliceMap::kHazardPtrsPerThread> active;
        for (size_t index = 0; index < records; ++index) {
            for (size_t slot = 0; slot < SliceMap::kHazardPtrsPerThread; ++slot) {
                active[index * SliceMap::kHazardPtrsPerThread + slot] = reinterpret_cast<uintptr_t>(
                    map_hazards.records[index].pointers[slot].load(std::memory_order_seq_cst));
            }
        }
        const size_t scan_count = (records * SliceMap::kHazardPtrsPerThread + 7) & ~size_t(7);
        std::fill(active.begin() + records * SliceMap::kHazardPtrsPerThread,
            active.begin() + scan_count, uint64_t(0));
        size_t remaining = 0;
        reclaimable.clear();
        for (const auto& object : retired) {
            if (SIMDMisc::find_id(active.data(), scan_count,
                static_cast<int64_t>(reinterpret_cast<uintptr_t>(object.pointer))) >= 0) {
                retired[remaining++] = object;
            } else {
                reclaimable.push_back(object);
            }
        }
        retired.resize(remaining);
        while (orphaned) {
            MapOrphan* next = orphaned->next;
            if (SIMDMisc::find_id(active.data(), scan_count,
                static_cast<int64_t>(reinterpret_cast<uintptr_t>(orphaned->retired.pointer))) >= 0) {
                map_hazards.orphan(orphaned);
            } else {
                reclaimable.push_back(orphaned->retired);
                delete orphaned;
            }
            orphaned = next;
        }
        for (const auto& object : reclaimable) object.destroy(object.pointer);
    }
    /** ------------------------------------------------------------------------------------------- Retire
     * @brief Reclaims promptly while alone and batches retirements under concurrent sharers.
     */
    template<typename Object>
    void retire(Object* pointer, bool eager) {
        retired.push_back({pointer, &destroy<Object>});
        if ((eager && map_hazards.high_water.load(std::memory_order_seq_cst) <= 1)
            || retired.size() >= SliceMap::kRetireBatch) collect();
    }
    /** ------------------------------------------------------------------------------------------- Destroy
     * @brief Invokes the original object's concrete destructor.
     */
    template<typename Object>
    static void destroy(void* pointer) { delete static_cast<Object*>(pointer); }
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Clears announcements before releasing the record and transferring protected retirements.
     */
    ~MapThreadContext() {
        for (auto& pointer : record->pointers) pointer.store(nullptr, std::memory_order_seq_cst);
        collect();
        for (const auto& object : retired) map_hazards.orphan(new MapOrphan{object, nullptr});
        record->claimed.store(false, std::memory_order_release);
    }
};
/** --------------------------------------------------------------------------------------------------------- Map Thread
 * @brief Registers only threads that actually access a map.
 */
MapThreadContext& map_thread() {
    thread_local MapThreadContext context;
    return context;
}
/** --------------------------------------------------------------------------------------------------------- Announce
 * @brief Publishes a source pointer into a hazard slot and revalidates it until the two agree.
 */
template<typename Object>
Object* announce(std::atomic<void*>& slot, const std::atomic<Object*>& source) {
    Object* pointer;
    do {
        pointer = source.load(std::memory_order_seq_cst);
        slot.store(pointer, std::memory_order_seq_cst);
    } while (pointer != source.load(std::memory_order_seq_cst));
    return pointer;
}
/** --------------------------------------------------------------------------------------------------------- Map Hazard
 * @brief Scoped announcement that retirement cannot miss and that clears when the reader leaves.
 */
struct MapHazard {
    std::atomic<void*>& announcement;
    explicit MapHazard(size_t slot) : announcement(map_thread().record->pointers[slot]) {}
    ~MapHazard() { clear(); }
    void clear() { announcement.store(nullptr, std::memory_order_seq_cst); }
    template<typename Object>
    Object* protect(const std::atomic<Object*>& source) { return announce(announcement, source); }
};
/** --------------------------------------------------------------------------------------------------------- Map Lookup Pin
 * @brief Announcement a lookup leaves armed so its row survives until this thread's next lookup.
 */
struct MapLookupPin {
    std::atomic<void*>& announcement;
    explicit MapLookupPin(size_t slot) : announcement(map_thread().record->pointers[slot]) {}
    void clear() { announcement.store(nullptr, std::memory_order_seq_cst); }
    template<typename Object>
    Object* protect(const std::atomic<Object*>& source) { return announce(announcement, source); }
};
} // namespace
/** --------------------------------------------------------------------------------------------------------- Slice Map State
 * @brief Keeps hash-sorted bucket chains behind doubling tables and stable position cells.
 */
struct SliceMap::State {
    struct Node;
    static constexpr size_t kPending = SIZE_MAX;
    static constexpr uintptr_t kMark = 1;
    static constexpr uint64_t kHashInverse = UINT64_C(17428512612931826493);
    static_assert(kHashInverse * UINT64_C(11400714819323198485) == 1);
    /** ----------------------------------------------------------------------------------------------------- Payload
     * @brief Owns one immutable Slice claim and its optional typed destructor.
     */
    struct Payload {
        Slice slice;
        Finalizer finalizer;
        Payload(Slice&& value, Finalizer destructor)
        : slice(std::move(value)), finalizer(destructor) {}
        ~Payload() { if (finalizer) finalizer(slice.data<void>()); }
    };
    /** ----------------------------------------------------------------------------------------------------- Node
     * @brief Chains one identifier into its bucket with a replaceable payload and stable position.
     */
    struct Node {
        const uint64_t hashed;
        std::atomic<Node*> next{nullptr};
        std::atomic<Payload*> current;
        std::atomic<size_t> position{kPending};
        std::atomic<bool> published{false};
        Node(uint64_t hash, Payload* payload) : hashed(hash), current(payload) {}
        ~Node() { delete current.load(std::memory_order_relaxed); }
        int64_t identifier() const { return static_cast<int64_t>(hashed * kHashInverse); }
    };
    /** ----------------------------------------------------------------------------------------------------- Table
     * @brief Maps hash prefixes onto shared chains whose pending splits carry the mark bit.
     */
    struct Table {
        const size_t shift;
        const size_t bucket_count;
        const std::unique_ptr<std::atomic<uintptr_t>[]> buckets;
        Table* retired;
        explicit Table(size_t count)
        : shift(std::numeric_limits<size_t>::digits - std::countr_zero(count)),
          bucket_count(count), buckets(new std::atomic<uintptr_t>[count]()), retired(nullptr) {}
    };
    /** ----------------------------------------------------------------------------------------------------- Segment
     * @brief Reserves stable position cells without relocating previously published cells.
     */
    struct Segment {
        std::unique_ptr<std::atomic<Node*>[]> cells;
        explicit Segment(size_t count) : cells(new std::atomic<Node*>[count]{}) {}
    };
    /** ----------------------------------------------------------------------------------------------------- Pointer View
     * @brief Publishes immutable pointer metadata for concurrent readers between map mutations.
     */
    struct PointerView {
        Slice pointers;
        size_t revision;
        size_t count;
        PointerView(size_t capacity, size_t version, size_t rows)
        : pointers(capacity * sizeof(void*)), revision(version), count(rows) {}
    };
    struct Publication { size_t index; bool inserted; };
    static constexpr size_t kInflightStep = size_t(1) << 32;
    static constexpr size_t kPopulationMask = kInflightStep - 1;
    std::atomic<Table*> table_;
    std::array<std::atomic<Segment*>, std::numeric_limits<size_t>::digits> segments_{};
    std::atomic<size_t> reserved_{0};
    /// @brief Low word counts linked rows; high word counts in-flight link attempts.
    alignas(128) std::atomic<size_t> positions_{0};
    alignas(128) std::atomic<size_t> completed_{0};
    std::atomic<size_t> waiters_{0};
    std::atomic<size_t> revision_{0};
    std::atomic<PointerView*> cached_view_{nullptr};
    std::atomic<bool> resizing_{false};
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Prepares the initial table and position directory without imposing a row ceiling.
     */
    explicit State(size_t hint)
    : table_(new Table(std::max(size_t(2), std::bit_ceil(std::max(size_t(1), hint))))) {
        if (hint) {
            const size_t highest = std::bit_width(hint) - 1;
            for (size_t index = 0; index <= highest; ++index) {
                segments_[index].store(new Segment(size_t(1) << index), std::memory_order_relaxed);
            }
        }
        reserved_.store(hint, std::memory_order_relaxed);
    }
    /** ------------------------------------------------------------------------------------------- Hash
     * @brief Permutes the entire signed identifier domain into a collision-free hash order.
     */
    static uint64_t hash(int64_t identifier) {
        return static_cast<uint64_t>(identifier) * UINT64_C(11400714819323198485);
    }
    /** ------------------------------------------------------------------------------------------- Cell
     * @brief Installs the exponentially sized segment needed by a new stable position.
     */
    std::atomic<Node*>* cell(size_t index) {
        const size_t segment_index = std::bit_width(index + 1) - 1;
        const size_t segment_size = size_t(1) << segment_index;
        Segment* segment = segments_[segment_index].load(std::memory_order_acquire);
        if (!segment) {
            auto candidate = std::make_unique<Segment>(segment_size);
            if (segments_[segment_index].compare_exchange_strong(segment, candidate.get(),
                std::memory_order_release, std::memory_order_acquire)) segment = candidate.release();
        }
        size_t capacity = reserved_.load(std::memory_order_relaxed);
        const size_t next_capacity = segment_size + (segment_size - 1);
        while (capacity <= index && !reserved_.compare_exchange_weak(capacity, next_capacity,
            std::memory_order_release, std::memory_order_relaxed)) {}
        return &segment->cells[index - (segment_size - 1)];
    }
    /** ------------------------------------------------------------------------------------------- Entry At
     * @brief Reads a position cell that stays null until its row finishes publication.
     */
    Node* entry_at(size_t index) const {
        const size_t segment_index = std::bit_width(index + 1) - 1;
        Segment* segment = segments_[segment_index].load(std::memory_order_acquire);
        return segment ? segment->cells[index - ((size_t(1) << segment_index) - 1)]
            .load(std::memory_order_acquire) : nullptr;
    }
    /** ------------------------------------------------------------------------------------------- Find
     * @brief Walks one bucket's sorted chain for the exact hash of an identifier.
     */
    Node* find(uint64_t hashed) const {
        Table* table = table_.load(std::memory_order_acquire);
        Node* node = reinterpret_cast<Node*>(table->buckets[hashed >> table->shift]
            .load(std::memory_order_acquire) & ~kMark);
        while (node && node->hashed < hashed) node = node->next.load(std::memory_order_acquire);
        return node && node->hashed == hashed ? node : nullptr;
    }
    /** ------------------------------------------------------------------------------------------- Advance Completed
     * @brief Extends the contiguous hook-completed watermark and releases parked waiters.
     */
    void advance_completed() {
        size_t seen = completed_.load(std::memory_order_seq_cst);
        for (;;) {
            size_t reached = seen;
            while (reached < (positions_.load(std::memory_order_acquire) & kPopulationMask)) {
                Node* node = entry_at(reached);
                if (!node || !node->published.load(std::memory_order_seq_cst)) break;
                ++reached;
            }
            if (reached == seen) return;
            if (completed_.compare_exchange_weak(seen, reached,
                std::memory_order_seq_cst, std::memory_order_seq_cst)) {
                if (waiters_.load(std::memory_order_seq_cst)) completed_.notify_all();
                seen = reached;
                continue;
            }
        }
    }
    /** ------------------------------------------------------------------------------------------- Split
     * @brief Repoints one marked bucket at the first chain node inside its narrower hash range.
     */
    void split(Table* table, size_t bucket) {
        const uintptr_t value = table->buckets[bucket].load(std::memory_order_acquire);
        if (!(value & kMark)) return;
        const uint64_t boundary = uint64_t(bucket) << table->shift;
        Node* node = reinterpret_cast<Node*>(value & ~kMark);
        while (node && node->hashed < boundary) node = node->next.load(std::memory_order_acquire);
        table->buckets[bucket].store(reinterpret_cast<uintptr_t>(node), std::memory_order_release);
    }
    /** ------------------------------------------------------------------------------------------- Try Resize
     * @brief Doubles the table once insertion traffic exceeds one node per bucket.
     */
    void try_resize(Table* superseded) {
        if (table_.load(std::memory_order_acquire) != superseded) return;
        auto replacement = std::make_unique<Table>(superseded->bucket_count * 2);
        bool idle = false;
        if (!resizing_.compare_exchange_strong(idle, true,
            std::memory_order_acq_rel, std::memory_order_relaxed)) return;
        if (table_.load(std::memory_order_acquire) == superseded) {
            while ((positions_.load(std::memory_order_acquire) & ~kPopulationMask) != 0) {
                cpu_relax();
            }
            auto replacement = std::make_unique<Table>(superseded->bucket_count * 2);
            for (size_t index = 0; index < superseded->bucket_count; ++index) {
                const uintptr_t head = superseded->buckets[index]
                    .load(std::memory_order_relaxed) & ~kMark;
                replacement->buckets[2 * index].store(head, std::memory_order_relaxed);
                replacement->buckets[2 * index + 1].store(head | kMark, std::memory_order_relaxed);
            }
            Table* published = replacement.release();
            published->retired = superseded;
            table_.store(published, std::memory_order_seq_cst);
            resizing_.store(false, std::memory_order_release);
            for (size_t bucket = 1; bucket < published->bucket_count; bucket += 2) {
                split(published, bucket);
            }
            return;
        }
        resizing_.store(false, std::memory_order_release);
    }
    /** ------------------------------------------------------------------------------------------- Upsert
     * @brief Links one distinct node or atomically exchanges an existing node's payload.
     */
    Publication upsert(int64_t identifier, std::unique_ptr<Payload> payload) {
        const uint64_t hashed = hash(identifier);
        std::unique_ptr<Node> candidate;
        for (;;) {
            Table* table = table_.load(std::memory_order_acquire);
            const size_t bucket = hashed >> table->shift;
            Node* node = reinterpret_cast<Node*>(table->buckets[bucket]
                .load(std::memory_order_acquire) & ~kMark);
            Node* predecessor = nullptr;
            while (node && node->hashed < hashed) {
                predecessor = node;
                node = node->next.load(std::memory_order_acquire);
            }
            if (node && node->hashed == hashed) {
                if (candidate) candidate->current.store(nullptr, std::memory_order_relaxed);
                Payload* previous = node->current.exchange(payload.release(),
                    std::memory_order_seq_cst);
                if (previous) map_thread().retire(previous, true);
                size_t position = node->position.load(std::memory_order_acquire);
                while (position == kPending) {
                    cpu_relax();
                    position = node->position.load(std::memory_order_acquire);
                }
                return {position, false};
            }
            if (resizing_.load(std::memory_order_acquire)) {
                cpu_relax();
                continue;
            }
            positions_.fetch_add(kInflightStep, std::memory_order_acq_rel);
            if (table_.load(std::memory_order_acquire) != table
                || resizing_.load(std::memory_order_acquire)) {
                positions_.fetch_sub(kInflightStep, std::memory_order_release);
                cpu_relax();
                continue;
            }
            uintptr_t head = table->buckets[bucket].load(std::memory_order_acquire);
            if (head & kMark) {
                positions_.fetch_sub(kInflightStep, std::memory_order_release);
                cpu_relax();
                continue;
            }
            node = reinterpret_cast<Node*>(head);
            predecessor = nullptr;
            while (node && node->hashed < hashed) {
                predecessor = node;
                node = node->next.load(std::memory_order_acquire);
            }
            if (node && node->hashed == hashed) {
                if (candidate) candidate->current.store(nullptr, std::memory_order_relaxed);
                positions_.fetch_sub(kInflightStep, std::memory_order_release);
                continue;
            }
            if (!candidate) candidate = std::make_unique<Node>(hashed, nullptr);
            candidate->next.store(node, std::memory_order_relaxed);
            candidate->current.store(payload.get(), std::memory_order_relaxed);
            const bool linked = predecessor
                ? predecessor->next.compare_exchange_strong(node, candidate.get(),
                    std::memory_order_acq_rel, std::memory_order_acquire)
                : table->buckets[bucket].compare_exchange_strong(head,
                    reinterpret_cast<uintptr_t>(candidate.get()),
                    std::memory_order_acq_rel, std::memory_order_acquire);
            if (!linked) {
                positions_.fetch_sub(kInflightStep, std::memory_order_release);
                continue;
            }
            Node* inserted = candidate.release();
            payload.release();
            positions_.fetch_sub(kInflightStep, std::memory_order_release);
            const size_t position = positions_.fetch_add(1, std::memory_order_relaxed)
                & kPopulationMask;
            inserted->position.store(position, std::memory_order_release);
            cell(position)->store(inserted, std::memory_order_release);
            if (position >= table->bucket_count) try_resize(table);
            return {position, true};
        }
    }
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Releases every node, payload, and superseded table after state readers leave.
     */
    ~State() {
        Table* table = table_.load(std::memory_order_relaxed);
        for (size_t bucket = 0; bucket < table->bucket_count; ++bucket) {
            const uint64_t boundary = uint64_t(bucket + 1) << table->shift;
            Node* node = reinterpret_cast<Node*>(table->buckets[bucket]
                .load(std::memory_order_relaxed) & ~kMark);
            while (node && (bucket + 1 == table->bucket_count || node->hashed < boundary)) {
                Node* next = node->next.load(std::memory_order_relaxed);
                delete node;
                node = next;
            }
        }
        while (table) {
            Table* superseded = table->retired;
            delete table;
            table = superseded;
        }
        for (auto& segment : segments_) delete segment.load(std::memory_order_relaxed);
        delete cached_view_.load(std::memory_order_relaxed);
    }
};
/** --------------------------------------------------------------------------------------------------------- Constructor
 * @brief Allocates the initial state and records the caller's default wait expectation.
 */
SliceMap::SliceMap(size_t capacity) : state_(new State(capacity)), expected_(capacity) {}
/** --------------------------------------------------------------------------------------------------------- Move Constructor
 * @brief Transfers the complete index without moving any published payload.
 */
SliceMap::SliceMap(SliceMap&& other) noexcept
: state_(other.state_.exchange(nullptr, std::memory_order_relaxed)),
  expected_(other.expected_.exchange(0, std::memory_order_relaxed)),
  publish_context_(other.publish_context_), publish_hook_(other.publish_hook_) {}
/** --------------------------------------------------------------------------------------------------------- Move Assignment
 * @brief Reclaims destination ownership and transfers a quiescent source index.
 */
SliceMap& SliceMap::operator=(SliceMap&& other) noexcept {
    if (this == &other) return *this;
    State* previous = state_.exchange(other.state_.exchange(nullptr, std::memory_order_relaxed),
        std::memory_order_seq_cst);
    expected_.store(other.expected_.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
    publish_context_ = other.publish_context_;
    publish_hook_ = other.publish_hook_;
    if (previous) map_thread().retire(previous, true);
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Drops this thread's own lookup pin on the index and retires it once other readers leave.
 */
SliceMap::~SliceMap() {
    State* previous = state_.exchange(nullptr, std::memory_order_seq_cst);
    if (!previous) return;
    MapLookupPin pin(kPinnedStateSlot);
    if (pin.announcement.load(std::memory_order_seq_cst) == previous) pin.clear();
    map_thread().retire(previous, true);
}
/** --------------------------------------------------------------------------------------------------------- Complete Publish
 * @brief Marks a row hook-complete and advances the watermark only from the boundary row.
 */
void SliceMap::complete_publish(State* state, size_t index, bool inserted) {
    if (!inserted) state->revision_.fetch_add(1, std::memory_order_relaxed);
    if (publish_hook_) publish_hook_(this, publish_context_, index);
    if (inserted) {
        state->entry_at(index)->published.store(true, std::memory_order_seq_cst);
        if (index == state->completed_.load(std::memory_order_seq_cst)) {
            state->advance_completed();
        }
    }
}
/** --------------------------------------------------------------------------------------------------------- Add Finalized
 * @brief Transfers a Slice and its typed finalizer into the unique-key publication protocol.
 */
void SliceMap::add_finalized(const int64_t& identifier, Slice&& slice, Finalizer finalizer) {
    State* state = state_.load(std::memory_order_acquire);
    const auto result = state->upsert(identifier,
        std::make_unique<State::Payload>(std::move(slice), finalizer));
    complete_publish(state, result.index, result.inserted);
}
/** --------------------------------------------------------------------------------------------------------- Find Internal
 * @brief Returns the stable position after one sorted bucket-chain walk, leaving the index pinned.
 */
int64_t SliceMap::find_internal(int64_t identifier) const {
    MapLookupPin pin(kPinnedStateSlot);
    State* state = pin.protect(state_);
    State::Node* node = state->find(State::hash(identifier));
    if (!node) return -1;
    const size_t position = node->position.load(std::memory_order_acquire);
    return position == State::kPending ? -1 : static_cast<int64_t>(position);
}
/** --------------------------------------------------------------------------------------------------------- Get Slice Internal
 * @brief Retains the selected payload before releasing its hazard announcement.
 */
Slice SliceMap::get_slice_internal(int64_t identifier) const {
    MapHazard state_hazard(0), payload_hazard(1);
    State* state = state_hazard.protect(state_);
    State::Node* node = state->find(State::hash(identifier));
    if (!node) return {};
    State::Payload* payload = payload_hazard.protect(node->current);
    return payload->slice;
}
/** --------------------------------------------------------------------------------------------------------- Slice At
 * @brief Retains a published position's payload during concurrent replacement.
 */
Slice SliceMap::slice_at(size_t index) const {
    MapHazard state_hazard(0), payload_hazard(1);
    State* state = state_hazard.protect(state_);
    return payload_hazard.protect(state->entry_at(index)->current)->slice;
}
/** --------------------------------------------------------------------------------------------------------- Payload At
 * @brief Borrows a quiescent position's payload without creating another Slice claim.
 */
void* SliceMap::payload_at(size_t index) const {
    State::Node* entry = state_.load(std::memory_order_relaxed)->entry_at(index);
    return entry ? entry->current.load(std::memory_order_relaxed)->slice.data<void>() : nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Identifier At
 * @brief Reads a stable position's immutable identifier while reset is quiescent.
 */
int64_t SliceMap::identifier_at(size_t index) const {
    State* state = state_.load(std::memory_order_acquire);
    State::Node* entry = state->entry_at(index);
    return entry ? entry->identifier() : -1;
}
/** --------------------------------------------------------------------------------------------------------- Published
 * @brief Tests a position cell that stays null until its row finishes publication.
 */
bool SliceMap::published(const size_t& slot) const {
    return state_.load(std::memory_order_acquire)->entry_at(slot) != nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Size
 * @brief Reads the contiguous watermark of hook-completed first publications.
 */
size_t SliceMap::size() const {
    MapHazard hazard(0);
    State* state = hazard.protect(state_);
    if (!state) return 0;
    state->advance_completed();
    return state->completed_.load(std::memory_order_acquire);
}
/** --------------------------------------------------------------------------------------------------------- Capacity
 * @brief Reads the current reservation while preserving concurrent reset safety.
 */
size_t SliceMap::capacity() const noexcept {
    MapHazard hazard(0);
    State* state = hazard.protect(state_);
    return state ? state->reserved_.load(std::memory_order_acquire) : 0;
}
/** --------------------------------------------------------------------------------------------------------- Pointer View
 * @brief Publishes one cached pointer view for concurrent readers while writers and reset are quiescent.
 */
void** SliceMap::pointer_view() const {
    MapHazard state_hazard(0), view_hazard(1);
    State* state = state_hazard.protect(state_);
    state->advance_completed();
    const size_t revision = state->revision_.load(std::memory_order_relaxed);
    const size_t count = state->completed_.load(std::memory_order_relaxed);
    State::PointerView* previous = view_hazard.protect(state->cached_view_);
    if (previous && revision == previous->revision && count == previous->count) {
        return previous->pointers.data<void*>();
    }
    const size_t capacity = state->reserved_.load(std::memory_order_relaxed);
    auto replacement = std::make_unique<State::PointerView>(capacity, revision, count);
    void** pointers = replacement->pointers.data<void*>();
    for (size_t index = 0; index < count; ++index) {
        pointers[index] = state->entry_at(index)->current.load(std::memory_order_relaxed)
            ->slice.data<void>();
    }
    for (size_t index = count; index < capacity; ++index) pointers[index] = nullptr;
    if (state->cached_view_.compare_exchange_strong(previous, replacement.get(),
        std::memory_order_seq_cst)) {
        replacement.release();
        view_hazard.clear();
        state_hazard.clear();
        if (previous) map_thread().retire(previous, true);
        return pointers;
    }
    return view_hazard.protect(state->cached_view_)->pointers.data<void*>();
}
/** --------------------------------------------------------------------------------------------------------- Wait Until Full
 * @brief Parks on the hook-completed watermark with no semaphore count or missed wakeup window.
 */
void SliceMap::wait_until_full(size_t count) {
    State* state = state_.load(std::memory_order_acquire);
    size_t completed = state->completed_.load(std::memory_order_acquire);
    if (completed >= count) return;
    state->waiters_.fetch_add(1, std::memory_order_seq_cst);
    completed = state->completed_.load(std::memory_order_seq_cst);
    while (completed < count) {
        state->completed_.wait(completed, std::memory_order_acquire);
        completed = state->completed_.load(std::memory_order_seq_cst);
    }
    state->waiters_.fetch_sub(1, std::memory_order_seq_cst);
}
/** --------------------------------------------------------------------------------------------------------- Reset
 * @brief Exchanges the whole index and retires its ownership after active readers finish.
 */
void SliceMap::reset() {
    State* replacement = new State(capacity());
    State* previous = state_.exchange(replacement, std::memory_order_seq_cst);
    map_thread().retire(previous, true);
}
/** --------------------------------------------------------------------------------------------------------- Merge
 * @brief Moves rows and finalizers in source order with stable-position replacement for shared keys.
 */
SliceMap& SliceMap::merge(SliceMap& other) {
    if (this == &other) return *this;
    State* source = other.state_.load(std::memory_order_relaxed);
    State* destination = state_.load(std::memory_order_relaxed);
    source->advance_completed();
    const size_t count = source->completed_.load(std::memory_order_relaxed);
    for (size_t index = 0; index < count; ++index) {
        State::Node* entry = source->entry_at(index);
        std::unique_ptr<State::Payload> payload(entry->current.exchange(nullptr,
            std::memory_order_relaxed));
        const auto result = destination->upsert(entry->identifier(), std::move(payload));
        complete_publish(destination, result.index, result.inserted);
    }
    other.reset();
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- GC
 * @brief Collects this thread's retirements and protected objects orphaned by exited threads.
 */
void SliceMap::gc() { map_thread().collect(); }
} // namespace buffetalligator
