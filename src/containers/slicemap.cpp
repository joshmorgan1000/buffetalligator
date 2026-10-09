/** --------------------------------------------------------------------------------------------------------- Slice Map
 * @file slicemap.cpp
 * @brief Michael lock-free hash buckets sorted by hash with hazard-protected payload replacement.
 */
#include <alligator.hpp>
#include <alligator/containers.hpp>
#include <alligator/dispatch.hpp>
#include <simd.hpp>
#include <array>
#include <bit>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace buffetalligator {
namespace {
/** --------------------------------------------------------------------------------------------------------- Retired Map Object
 * @struct RetiredMapObject
 * @brief Couples retired storage with its concrete destructor.
 */
struct RetiredMapObject {
    /// @brief The pointer to the retired map object.
    void* pointer;
    /// @brief The destructor function for the retired map object.
    void (*destroy)(void*);
};
/// @brief Hazard slot a lookup leaves armed on the index it searched.
constexpr size_t kPinnedStateSlot = 2;
/** --------------------------------------------------------------------------------------------------------- Map Hazard Record
 * @struct MapHazardRecord
 * @brief Separates independently written thread announcements onto distinct cache lines.
 */
struct alignas(128) MapHazardRecord {
    /// @brief Indicates whether this hazard record is currently claimed by a thread.
    std::atomic<bool> claimed{false};
    /// @brief The array of hazard pointers associated with this record.
    std::array<std::atomic<void*>, SliceMap::kHazardPtrsPerThread> pointers{};
};
/** --------------------------------------------------------------------------------------------------------- Map Orphan
 * @struct MapOrphan
 * @brief Transfers a protected retirement out of an exiting thread.
 */
struct MapOrphan {
    /// @brief The retired map object associated with this orphan.
    RetiredMapObject retired;
    /// @brief The next orphan in the linked list.
    MapOrphan* next;
};
/** --------------------------------------------------------------------------------------------------------- Map Hazard Domain
 * @struct MapHazardDomain
 * @brief Owns reusable thread records and retirements whose originating threads have exited.
 */
struct MapHazardDomain {
    /// @brief The array of reusable hazard records for this domain.
    std::array<MapHazardRecord, SliceMap::kHazardMaxThreads> records{};
    /// @brief The high water mark for claimed hazard records.
    std::atomic<size_t> high_water{0};
    /// @brief The array of hazard records used for traversals.
    std::array<MapHazardRecord, SliceMap::kHazardMaxThreads> traversals{};
    /// @brief The high water mark for claimed traversal records.
    std::atomic<size_t> traversal_high_water{0};
    /// @brief The number of active traversals.
    std::atomic<size_t> active_traversals{0};
    /// @brief The linked list of orphaned map objects.
    std::atomic<MapOrphan*> orphans{nullptr};
    /** ------------------------------------------------------------------------------------------- Orphan
     * @brief Publishes an orphan without popping or recycling a concurrently observed head.
     * @param node The orphaned map object to publish.
     */
    void orphan(MapOrphan* node) {
        node->next = orphans.load(std::memory_order_relaxed);
        while (!orphans.compare_exchange_weak(node->next, node,
            std::memory_order_release, std::memory_order_relaxed)) {}
    }
};
constinit MapHazardDomain map_hazards;
/** --------------------------------------------------------------------------------------------------------- Claim Record
 * @brief Claims one reusable hazard record and publishes its scan boundary before announcing pointers.
 * @param records The array of hazard records to claim from.
 * @param high_water The high water mark for claimed records.
 * @param exhausted The error message to throw if no records are available.
 * @return A pointer to the claimed hazard record.
 */
MapHazardRecord* claim_record(
    std::array<MapHazardRecord, SliceMap::kHazardMaxThreads>& records,
    std::atomic<size_t>& high_water,
    const char* exhausted
) {
    for (size_t index = 0; index < records.size(); ++index) {
        bool available = false;
        if (!records[index].claimed.compare_exchange_strong(available, true,
            std::memory_order_acquire, std::memory_order_relaxed)) continue;
        size_t previous = high_water.load(std::memory_order_seq_cst);
        while (previous <= index && !high_water.compare_exchange_weak(
            previous, index + 1, std::memory_order_seq_cst)) {}
        return &records[index];
    }
    throw std::length_error(exhausted);
}
/** --------------------------------------------------------------------------------------------------------- CPU Relax
 * @brief Issues the architecture's pause hint instead of a de-prioritizing yield syscall.
 */
inline void cpu_relax() {
#if defined(__aarch64__)
    asm volatile("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}
/** --------------------------------------------------------------------------------------------------------- Map Thread Context
 * @struct MapThreadContext
 * @brief Reclaims private retirements against globally ordered hazard announcements.
 */
struct MapThreadContext {
    /// @brief The hazard record associated with this thread context.
    MapHazardRecord* record = nullptr;
    /// @brief The list of retired map objects that are pending reclamation.
    std::vector<RetiredMapObject> retired;
    /// @brief The list of map objects that are ready to be reclaimed.
    std::vector<RetiredMapObject> reclaimable;
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Claims a reusable record before announcing any protected pointer.
     */
    MapThreadContext() {
        retired.reserve(SliceMap::kHazardMaxThreads * SliceMap::kHazardPtrsPerThread
            + SliceMap::kRetireBatch);
        record = claim_record(map_hazards.records, map_hazards.high_water,
            "SliceMap hazard domain has 256 simultaneously registered threads");
    }
    /** ------------------------------------------------------------------------------------------- Collect
     * @brief Orders removals against announcements before destroying unprotected objects.
     */
    void collect() {
        MapOrphan* orphaned = map_hazards.orphans.exchange(nullptr, std::memory_order_acquire);
        const size_t records = map_hazards.high_water.load(std::memory_order_seq_cst);
        const size_t traversals = map_hazards.traversal_high_water.load(std::memory_order_seq_cst);
        const size_t active_traversals = map_hazards.active_traversals.load(std::memory_order_seq_cst);
        const bool pinned =
            record->pointers[kPinnedStateSlot].load(std::memory_order_seq_cst) != nullptr;
        if (records <= 1 && active_traversals == 0 && !pinned && orphaned == nullptr) {
            for (const auto& object : retired) object.destroy(object.pointer);
            retired.clear();
            return;
        }
        std::array<uint64_t, SliceMap::kHazardMaxThreads * (SliceMap::kHazardPtrsPerThread + 2)>
            active;
        for (size_t index = 0; index < records; ++index) {
            for (size_t slot = 0; slot < SliceMap::kHazardPtrsPerThread; ++slot) {
                active[index * SliceMap::kHazardPtrsPerThread + slot] = reinterpret_cast<uintptr_t>(
                    map_hazards.records[index].pointers[slot].load(std::memory_order_seq_cst));
            }
        }
        const size_t thread_count = records * SliceMap::kHazardPtrsPerThread;
        for (size_t index = 0; index < traversals; ++index) {
            for (size_t slot = 0; slot < 2; ++slot) {
                active[thread_count + index * 2 + slot] = reinterpret_cast<uintptr_t>(
                    map_hazards.traversals[index].pointers[slot].load(std::memory_order_seq_cst));
            }
        }
        const size_t pointer_count = thread_count + traversals * 2;
        const size_t scan_count = (pointer_count + 7) & ~size_t(7);
        std::fill(active.begin() + pointer_count,
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
     * @tparam Object The type of the object being retired.
     * @param pointer A pointer to the object to be retired.
     * @param eager Whether to attempt eager reclamation.
     */
    template<typename Object>
    void retire(Object* pointer, bool eager) {
        retired.push_back({pointer, &destroy<Object>});
        if ((eager && map_hazards.high_water.load(std::memory_order_seq_cst) <= 1)
            || retired.size() >= SliceMap::kRetireBatch) collect();
    }
    /** ------------------------------------------------------------------------------------------- Destroy
     * @brief Invokes the original object's concrete destructor.
     * @param pointer A pointer to the object to be destroyed.
     */
    template<typename Object>
    static void destroy(void* pointer) { delete static_cast<Object*>(pointer); }
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Clears announcements before releasing the record and transferring protected
     * retirements.
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
 * @return The thread-local MapThreadContext instance.
 */
MapThreadContext& map_thread() {
    thread_local MapThreadContext context;
    return context;
}
/** --------------------------------------------------------------------------------------------------------- Announce
 * @brief Publishes a source pointer into a hazard slot and revalidates it until the two agree.
 * @tparam Object The type of the object being protected.
 * @param slot The announcement slot to use for protection.
 * @param source The atomic source to protect.
 * @return A pointer to the protected object.
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
 * @struct MapHazard
 * @brief Scoped announcement that retirement cannot miss and that clears when the reader leaves.
 */
struct MapHazard {
    /// @brief The announcement slot used to protect the object.
    std::atomic<void*>& announcement;
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs a new MapHazard and claims an announcement slot.
     * @param slot The index of the announcement slot to claim.
    */
    explicit MapHazard(size_t slot) : announcement(map_thread().record->pointers[slot]) {}
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Destroys the MapHazard and clears the announcement slot.
    */
    ~MapHazard() { clear(); }
    /** ------------------------------------------------------------------------------------------- Clear
     * @brief Clears the announcement slot used by this MapHazard.
    */
    void clear() { announcement.store(nullptr, std::memory_order_seq_cst); }
    /** ------------------------------------------------------------------------------------------- Protect
     * @brief Protects the given source by announcing it in the announcement slot.
     * @tparam Object The type of the object being protected.
     * @param source The atomic source to protect.
     * @return A pointer to the protected object.
    */
    template<typename Object>
    Object* protect(const std::atomic<Object*>& source) { return announce(announcement, source); }
};
/** --------------------------------------------------------------------------------------------------------- Map Lookup Pin
 * @struct MapLookupPin
 * @brief Announcement a lookup leaves armed so its row survives until this thread's next lookup.
 */
struct MapLookupPin {
    /// @brief The announcement slot used to protect the lookup's row.
    std::atomic<void*>& announcement;
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs a new MapLookupPin and claims an announcement slot.
     * @param slot The index of the announcement slot to claim.
    */
    explicit MapLookupPin(size_t slot) : announcement(map_thread().record->pointers[slot]) {}
    /** ------------------------------------------------------------------------------------------- Clear
     * @brief Clears the announcement slot used by this MapLookupPin.
    */
    void clear() { announcement.store(nullptr, std::memory_order_seq_cst); }
    /** ------------------------------------------------------------------------------------------- Protect
     * @brief Protects the given source by announcing it in the announcement slot.
     * @tparam Object The type of the object being protected.
     * @param source The atomic source to protect.
     * @return A pointer to the protected object.
    */
    template<typename Object>
    Object* protect(const std::atomic<Object*>& source) { return announce(announcement, source); }
};
/** --------------------------------------------------------------------------------------------------------- Map Traversal Hazard
 * @struct MapTraversalHazard
 * @brief Keeps callback traversal announcements independent of nested map operations.
 */
struct MapTraversalHazard {
    /// @brief The record used to track this traversal hazard.
    MapHazardRecord* record = claim_record(map_hazards.traversals,
        map_hazards.traversal_high_water, "SliceMap has 256 simultaneously active traversals");
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs a new MapTraversalHazard and claims a traversal record.
    */
    MapTraversalHazard() { map_hazards.active_traversals.fetch_add(1, std::memory_order_seq_cst); }
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Destroys the MapTraversalHazard and releases its traversal record.
    */
    ~MapTraversalHazard() {
        record->pointers[1].store(nullptr, std::memory_order_seq_cst);
        record->pointers[0].store(nullptr, std::memory_order_seq_cst);
        map_hazards.active_traversals.fetch_sub(1, std::memory_order_seq_cst);
        record->claimed.store(false, std::memory_order_release);
    }
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
    /** ------------------------------------------------------------------------------------------- Payload
     * @struct Payload
     * @brief Owns one immutable Slice claim and its optional typed destructor.
     */
    struct Payload {
        /// @brief The slice owned by this payload.
        Slice slice;
        /// @brief The finalizer responsible for cleaning up the slice.
        Finalizer finalizer;
        /** ----------------------------------------------------------------------------- Constructor
         * @brief Constructs a new Payload with the given slice and finalizer.
         * @param value The slice to be owned by this payload.
         * @param destructor The finalizer responsible for cleaning up the slice.
         */
        Payload(Slice&& value, Finalizer destructor)
        : slice(std::move(value)), finalizer(destructor) {}
        /** ----------------------------------------------------------------------------- Destructor
         * @brief Releases the resources held by the payload.
         */
        ~Payload() { if (finalizer) finalizer(slice.data<void>()); }
        /** ----------------------------------------------------------------------------- Retain Slice
         * @brief Retains this protected payload's existing arena entry for a
         * callback-local handle.
         * @return A handle to the retained slice.
         */
        Slice retain_slice() const {
            if (!slice.is_null()) {
                SliceEntry::from_slice(slice)->owners.fetch_add(1, std::memory_order_relaxed);
            }
            return SliceHandle::adopt(slice.id());
        }
    };
    /** ------------------------------------------------------------------------------------------- Node
     * @struct Node
     * @brief Chains one identifier into its bucket with a replaceable payload and position.
     */
    struct Node {
        /// @brief The hashed value of the node's identifier.
        const uint64_t hashed;
        /// @brief Pointer to the next node in the chain.
        std::atomic<Node*> next{nullptr};
        /// @brief Pointer to the current payload of the node.
        std::atomic<Payload*> current;
        /// @brief The position of the node within its segment.
        std::atomic<size_t> position{kPending};
        /// @brief Indicates whether the node has been published.
        std::atomic<bool> published{false};
        /** ----------------------------------------------------------------------------- Constructor
         * @brief Constructs a new Node with the specified hash and payload.
         * @param hash The hashed value of the node's identifier.
         * @param payload Pointer to the initial payload of the node.
         */
        Node(uint64_t hash, Payload* payload) : hashed(hash), current(payload) {}
        /** ----------------------------------------------------------------------------- Destructor
         * @brief Destroys the Node and releases its current payload.
         */
        ~Node() { delete current.load(std::memory_order_relaxed); }
        /** ----------------------------------------------------------------------------- Identifier
         * @brief Retrieves the original identifier of the node by reversing the hash.
         * @return The original identifier of the node.
         */
        int64_t identifier() const {
            return static_cast<int64_t>(hashed * kHashInverse);
        }
    };
    /** ------------------------------------------------------------------------------------------- Table
     * @struct Table
     * @brief Maps hash prefixes onto shared chains whose pending splits carry the mark bit.
     */
    struct Table {
        /// @brief The shift value used for calculating bucket indices.
        const size_t shift;
        /// @brief The number of buckets in the table.
        const size_t bucket_count;
        /// @brief The array of atomic bucket pointers representing the table's buckets.
        const std::unique_ptr<std::atomic<uintptr_t>[]> buckets;
        /// @brief Pointer to the retired table, if any.
        Table* retired;
        /** ----------------------------------------------------------------------------- Constructor
         * @brief Constructs a new Table with the specified number of buckets.
         * @param count The number of buckets in the table.
         */
        explicit Table(size_t count)
        : shift(std::numeric_limits<size_t>::digits - std::countr_zero(count))
        , bucket_count(count)
        , buckets(new std::atomic<uintptr_t>[count]())
        , retired(nullptr) {}
    };
    /** ------------------------------------------------------------------------------------------- Segment
     * @struct Segment
     * @brief Reserves stable position cells without relocating previously published cells.
     */
    struct Segment {
        /// @brief The array of atomic node pointers representing the segment's cells.
        std::unique_ptr<std::atomic<Node*>[]> cells;
        /** ----------------------------------------------------------------------------- Constructor
         * @brief Constructs a new Segment with the specified number of cells.
         * @param count The number of cells in the segment.
         */
        explicit Segment(size_t count) : cells(new std::atomic<Node*>[count]{}) {}
    };
    /** ------------------------------------------------------------------------------------------- Pointer View
     * @struct PointerView
     * @brief Publishes immutable pointer metadata for concurrent readers between map mutations.
     */
    struct PointerView {
        /// @brief The array of slice pointers representing the published rows.
        Slice pointers;
        /// @brief The revision number of the pointer view.
        size_t revision;
        /// @brief The number of published rows.
        size_t count;
        /** ----------------------------------------------------------------------------- Constructor
         * @brief Constructs a new PointerView with the specified capacity, revision,
         * and row count.
         * @param capacity The capacity of the slice array.
         * @param version The revision number of the pointer view.
         * @param rows The number of published rows.
         */
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
     * @param hint The initial hint for the number of rows to reserve.
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
     * @param identifier The signed identifier to hash.
     * @return A 64-bit hash value corresponding to the identifier.
     */
    static uint64_t hash(int64_t identifier) {
        return static_cast<uint64_t>(identifier) * UINT64_C(11400714819323198485);
    }
    /** ------------------------------------------------------------------------------------------- Cell
     * @brief Installs the exponentially sized segment needed by a new stable position.
     * @param index The index of the position cell for which to install the segment.
     * @return A pointer to the atomic node pointer corresponding to the specified index.
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
     * @param index The index of the position cell to read.
     * @return A pointer to the node at the specified position, or nullptr if not yet published.
     */
    Node* entry_at(size_t index) const {
        const size_t segment_index = std::bit_width(index + 1) - 1;
        Segment* segment = segments_[segment_index].load(std::memory_order_acquire);
        return segment ? segment->cells[index - ((size_t(1) << segment_index) - 1)]
            .load(std::memory_order_acquire) : nullptr;
    }
    /** ------------------------------------------------------------------------------------------- Find
     * @brief Walks one bucket's sorted chain for the exact hash of an identifier.
     * @param hashed The hash of the identifier to find.
     * @return A pointer to the node with the matching hash, or nullptr if not found.
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
     * @param table The table containing the bucket to split.
     * @param bucket The index of the bucket to split.
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
     * @brief Keeps exclusive resize ownership until the doubled table's bucket splits complete.
     * @param superseded The table being replaced by the resized table.
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
            for (size_t index = 0; index < superseded->bucket_count; ++index) {
                const uintptr_t head = superseded->buckets[index]
                    .load(std::memory_order_relaxed) & ~kMark;
                replacement->buckets[2 * index].store(head, std::memory_order_relaxed);
                replacement->buckets[2 * index + 1].store(head | kMark, std::memory_order_relaxed);
            }
            Table* published = replacement.release();
            published->retired = superseded;
            table_.store(published, std::memory_order_seq_cst);
#if defined(BUFFETALLIGATOR_TEST_MAP_GROWTH)
            BUFFETALLIGATOR_TEST_MAP_GROWTH(*this, *published);
#endif
            for (size_t bucket = 1; bucket < published->bucket_count; bucket += 2) {
                split(published, bucket);
            }
            resizing_.store(false, std::memory_order_release);
            return;
        }
        resizing_.store(false, std::memory_order_release);
    }
    /** ------------------------------------------------------------------------------------------- Upsert
     * @brief Links one distinct node or atomically exchanges an existing node's payload.
     * @param identifier The unique identifier for the node.
     * @param payload The payload to be associated with the node.
     * @return A Publication indicating the position and whether it was newly inserted.
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
 * @param capacity The initial capacity of the slice map.
 */
SliceMap::SliceMap(size_t capacity) : state_(new State(capacity)), expected_(capacity) {}
/** --------------------------------------------------------------------------------------------------------- Move Constructor
 * @brief Transfers the complete index without moving any published payload.
 * @param other The source SliceMap to move from.
 */
SliceMap::SliceMap(SliceMap&& other) noexcept
: state_(other.state_.exchange(nullptr, std::memory_order_relaxed))
, expected_(other.expected_.exchange(0, std::memory_order_relaxed))
, publish_context_(other.publish_context_)
, publish_hook_(other.publish_hook_) {}
/** --------------------------------------------------------------------------------------------------------- Move Assignment
 * @brief Reclaims destination ownership and transfers a quiescent source index.
 * @param other The source SliceMap to move from.
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
 * @param state The state of the slice map.
 * @param index The index of the row being published.
 * @param inserted Whether the row was newly inserted.
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
    if (!node) return Slice();
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
/** --------------------------------------------------------------------------------------------------------- IDs Internal
 * @brief Gathers one protected publication prefix directly through its immutable position segments.
 */
std::vector<int64_t> SliceMap::ids_internal() const {
    MapHazard hazard(0);
    State* state = hazard.protect(state_);
    std::vector<int64_t> identifiers;
    if (!state) return identifiers;
    state->advance_completed();
    size_t remaining = state->completed_.load(std::memory_order_acquire);
    identifiers.reserve(remaining);
    for (size_t segment_index = 0; remaining; ++segment_index) {
        const size_t count = std::min(remaining, size_t(1) << segment_index);
        const State::Segment* segment = state->segments_[segment_index]
            .load(std::memory_order_relaxed);
        for (size_t index = 0; index < count; ++index) {
            identifiers.push_back(segment->cells[index].load(std::memory_order_relaxed)
                ->identifier());
        }
        remaining -= count;
    }
    return identifiers;
}
/** --------------------------------------------------------------------------------------------------------- For Each
 * @brief Dispatches protected callback ranges across one captured state generation and joins them.
 * @param callback Receives protected callback-scoped Slice copies in unspecified invocation order.
 * @param context Shared callback context whose concurrent accesses must be synchronized.
 */
void SliceMap::for_each(
    void (*callback)(int64_t id, Slice* slice, void* context),
    void* context
) const {
    MapTraversalHazard hazard;
    State* state = announce(hazard.record->pointers[0], state_);
    if (!state) return;
    state->advance_completed();
    const size_t count = state->completed_.load(std::memory_order_acquire);
    if (!count) return;
#if defined(__APPLE__)
    const size_t available_workers = std::thread::hardware_concurrency();
    if (!available_workers) throw std::runtime_error("SliceMap cannot probe hardware worker count");
#else
    const size_t available_workers = static_cast<size_t>(omp_get_max_threads());
#endif
    /** ------------------------------------------------------------------------------------------- Invocation
     * @struct Invocation
     * @brief Shares the captured row prefix and the first callback failure until dispatch
     * completes.
     */
    struct Invocation {
        /// @brief The captured state and callback information for a single invocation.
        State* state;
        /// @brief The total number of completed rows to be processed.
        size_t count;
        /// @brief The number of worker threads to be used for this invocation.
        size_t workers;
        /// @brief The callback function to be invoked for each slice.
        void (*callback)(int64_t, Slice*, void*);
        /// @brief The user-defined context passed to the callback.
        void* context;
        /// @brief Indicates whether the callback has failed.
        std::atomic<bool> failed{false};
        /// @brief Stores the first exception thrown by the callback, if any.
        std::exception_ptr failure{};
        /** --------------------------------------------------------------------------------------- Run
         * @brief Visits one contiguous worker range with independent callback payload protection.
         * @param worker The index of the worker thread executing this run.
         */
        void run(size_t worker) {
            try {
                MapTraversalHazard payload_hazard;
                const size_t stride = count / workers;
                const size_t extra = count % workers;
                const size_t first = worker * stride + std::min(worker, extra);
                size_t remaining = stride + (worker < extra);
                size_t segment_index = std::bit_width(first + 1) - 1;
                size_t offset = first - ((size_t(1) << segment_index) - 1);
                while (remaining) {
                    const size_t rows = std::min(remaining, (size_t(1) << segment_index) - offset);
                    const State::Segment* segment = state->segments_[segment_index]
                        .load(std::memory_order_relaxed);
                    for (size_t index = offset; index < offset + rows; ++index) {
                        State::Node* node = segment->cells[index].load(std::memory_order_relaxed);
                        State::Payload* payload = announce(payload_hazard.record->pointers[1],
                            node->current);
                        Slice slice = payload->retain_slice();
                        callback(node->identifier(), &slice, context);
                    }
                    remaining -= rows;
                    ++segment_index;
                    offset = 0;
                }
            } catch (...) {
                bool available = false;
                if (failed.compare_exchange_strong(available, true, std::memory_order_relaxed)) {
                    failure = std::current_exception();
                }
            }
        }
    } invocation{state, count, std::min(count, available_workers), callback, context};
    /** ------------------------------------------------------------------------------------------- Worker
     * @struct Worker
     * @brief Borrows the invocation through the platform dispatch callable.
     */
    struct Worker {
        /// @brief The shared invocation containing the state and callback information.
        Invocation* invocation;
        /** --------------------------------------------------------------------------------------- Invoke
         * @brief Runs one borrowed worker range through the shared invocation.
         * @param index The index of the worker thread executing this run.
         */
        void operator()(size_t index) const { invocation->run(index); }
    };
    dispatch_for(0, invocation.workers, Worker{&invocation});
    if (invocation.failure) std::rethrow_exception(invocation.failure);
}
/** --------------------------------------------------------------------------------------------------------- Published
 * @brief Tests a position cell that stays null until its row finishes publication.
 * @param slot The slot to check for publication.
 * @return True if the slot has been published, false otherwise.
 */
bool SliceMap::published(const size_t& slot) const {
    return state_.load(std::memory_order_acquire)->entry_at(slot) != nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Size
 * @brief Reads the contiguous watermark of hook-completed first publications.
 * @return The number of completed first publications.
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
 * @return The currently reserved row positions.
 */
size_t SliceMap::capacity() const noexcept {
    MapHazard hazard(0);
    State* state = hazard.protect(state_);
    return state ? state->reserved_.load(std::memory_order_acquire) : 0;
}
/** --------------------------------------------------------------------------------------------------------- Pointer View
 * @brief Publishes one cached pointer view for concurrent readers while writers and reset are quiescent.
 * @return A pointer to the array of published slice pointers.
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
 * @param count The number of completed rows to wait for.
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
 * @param other The source SliceMap to merge from.
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
