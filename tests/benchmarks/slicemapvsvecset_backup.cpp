#include <cstdio>
/** --------------------------------------------------------------------------------------------------------- SliceMap vs VecSet
 * @file experiment_slicemap_vs_vecset.cpp
 * @brief Benchmark ring for the fetch-result carrier: SliceMap (hazard-pointer slot channel),
 * VecSet (sentinel-published unique-id set with a lock-free id index), a hazard-pointer lock-free
 * bucket map (Maged Michael, "Hazard Pointers", IEEE TPDS 2004), and a vanilla
 * `std::unordered_map` + mutex control, on the real pipeline verbs: gathering scattered rows
 * (single and multi producer), streaming consumption across thread counts, extracting
 * kernel-facing pointer-list views, combining multiple sets into one, and duplicate-heavy writes
 * with set-semantics assertions.
 */
#include <logging.hpp>
#include <loggingutils.hpp>
#include <alligator.hpp>
#include <simd.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
using IDType = int64_t;
using buffetalligator::SliceMap;
using buffetalligator::Slice;
/** --------------------------------------------------------------------------------------------------------- VecSet
 * @class VecSet
 * @brief Fixed-capacity result set of unique (ID, Slice) rows with per-slot publication. IDs are
 * stored as int64 regardless of the caller's ID category - only the publish (add) method is
 * templated, widening uint32 losslessly on the way in, so the whole engine reads one consistent
 * type.
 * @tparam Sorted When true, `drain_to` compacts survivors best-score-first, so slot order is
 * score order and the final rerank is a prefix read; defaults to unordered.
 */
template <bool Sorted = false>
class VecSet {
private:
    /// @brief The one internal ID width; uint32 ids widen losslessly at publish.
    using ID = int64_t;
    /// @brief Unpublished-slot marker; all-ones is never a valid ID.
    static constexpr ID SENTINEL = -1;
    /// @brief Empty index entry; all-ones so one byte fill marks every entry free.
    static constexpr uint32_t EMPTY = 0xFFFFFFFFu;
    /// @brief One ID per slot, sentinel-filled until its row is published.
    Slice ids_;
    /// @brief One payload Slice per slot, null until its row is published.
    Slice payloads_;
    /// @brief One raw payload pointer per slot - the kernel-facing view, null until published.
    Slice raws_;
    /// @brief One cached score per slot, zero until the producing kernel writes it; drains read
    /// it and nothing ever re-sorts it.
    Slice scores_;
    /// @brief Lock-free id-to-slot index; one row per id lives at the slot its entry names. The
    /// entries carry slot numbers, which capacity bounds well below 32 bits, so IDs keep their
    /// full 64-bit width no matter how large the vector ids themselves get.
    std::unique_ptr<std::atomic<uint32_t>[]> index_;
    /// @brief Probe mask; the index holds a power-of-two entry count at most half full.
    size_t index_mask_ = 0;
    /// @brief High-bit shift that turns the Fibonacci product into an index bucket.
    size_t index_shift_ = 0;
    /// @brief Producer slot claims; claiming and publishing are separate steps.
    std::atomic<size_t> claimed_{0};
    /// @brief Number of slots.
    size_t capacity_ = 0;
    /// @brief Typed base of the ID array.
    ID* ids() { return ids_.data<ID>(); }
    const ID* ids() const { return ids_.data<ID>(); }
    /// @brief Typed base of the payload array.
    Slice* payloads() { return payloads_.data<Slice>(); }
    /// @brief Typed base of the score array.
    float* scores() { return scores_.data<float>(); }
    const float* scores() const { return scores_.data<float>(); }
    /// @brief Typed base of the raw-pointer array.
    void** raws() { return raws_.data<void*>(); }
    /** ------------------------------------------------------------------------------------------- Index Size
     * @brief The power-of-two entry count keeping the index at most half full for `capacity` rows.
     */
    static size_t index_size_for(size_t capacity) {
        size_t size = 16u;
        while (size < capacity * 2u) size <<= 1u;
        return size;
    }
    /** ------------------------------------------------------------------------------------------- Bucket Of
     * @brief Fibonacci-hash an ID onto its probe start; the multiply scatters into the high bits.
     */
    size_t bucket_of(const ID& id) const {
        return static_cast<size_t>(static_cast<uint64_t>(id) * 11400714819323198485ull >> index_shift_);
    }
    /** ------------------------------------------------------------------------------------------- Install
     * @brief The row's id lives at `slot`; returns the index's slot for that id - a fresh install
     * when the id is new, or the surviving slot when a row with the id landed first. Occupied
     * entries - duplicates and collisions alike - probe on a plain acquire load that never takes
     * the line exclusive; only an actually-claimed empty entry pays the RMW. The owner
     * release-stores its ID before installing, so the id we read on the dedup path is always
     * visible.
     * @param id The row's ID.
     * @param slot The claimed slot holding the row.
     * @return The slot the index owns for this ID.
     */
    int64_t install(const ID& id, size_t slot) {
        size_t probe = bucket_of(id);
        for (;;) {
            uint32_t expected = index_[probe].load(std::memory_order_acquire);
            if (expected == EMPTY
                && index_[probe].compare_exchange_strong(
                    expected, static_cast<uint32_t>(slot),
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return static_cast<int64_t>(slot);
            }
            if (expected != EMPTY && ids()[static_cast<size_t>(expected)] == id) {
                return static_cast<int64_t>(expected);
            }
            probe = (probe + 1u) & index_mask_;
        }
    }
public:
    /** ------------------------------------------------------------------------------------------- Row
     * @struct Row
     * @brief The one single-vector concept: an ID and its payload bytes.
     */
    struct Row {
        ID id;           ///< The vector's ID.
        Slice* payload;  ///< Borrowed payload; valid while the owning VecSet lives.
    };
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Claims slots for exactly the rows the request will produce; there is no resize.
     * @param capacity Number of rows this set will carry.
     */
    explicit VecSet(size_t capacity)
    : ids_(capacity * sizeof(ID))
    , payloads_(capacity * sizeof(Slice))
    , raws_(capacity * sizeof(void*))
    , scores_(capacity * sizeof(float))
    , index_(new std::atomic<uint32_t>[index_size_for(capacity)])
    , index_mask_(index_size_for(capacity) - 1u)
    , index_shift_(64u - static_cast<size_t>(__builtin_ctzll(index_size_for(capacity))))
    , capacity_(capacity) {
        /// Sentinel and EMPTY are all-ones, so one byte fill marks every slot and entry free.
        std::memset(ids(), 0xFF, capacity * sizeof(ID));
        std::memset(index_.get(), 0xFF, (index_mask_ + 1u) * sizeof(uint32_t));
        new (payloads()) Slice[capacity_];
    }
    /** ------------------------------------------------------------------------------------------- Move Only Semantics
     * @brief The payload slots hold live claims, so the set moves rather than copies.
     */
    VecSet(const VecSet&) = delete;
    VecSet& operator=(const VecSet&) = delete;
    VecSet(VecSet&& other) noexcept
    : ids_(std::move(other.ids_))
    , payloads_(std::move(other.payloads_))
    , raws_(std::move(other.raws_))
    , scores_(std::move(other.scores_))
    , index_(std::move(other.index_))
    , index_mask_(other.index_mask_)
    , index_shift_(other.index_shift_)
    , claimed_(other.claimed_.load(std::memory_order_acquire))
    , capacity_(other.capacity_) {
        other.capacity_ = 0;
        other.index_mask_ = 0;
    }
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Releases every payload claim.
     */
    ~VecSet() {
        if (payloads_) {
            Slice* rows = payloads();
            for (size_t i = 0; i < capacity_; ++i) {
                rows[i].~Slice();
            }
        }
    }
    /** ------------------------------------------------------------------------------------------- Claim
     * @brief Claims the next `n` slots for the producer; slots publish independently.
     * @param n Number of slots to claim.
     * @return The first claimed slot.
     */
    size_t claim(const size_t& n = 1) {
        return claimed_.fetch_add(n, std::memory_order_relaxed);
    }
    /** ------------------------------------------------------------------------------------------- Publish
     * @brief Fills a claimed slot and publishes it - the one add method, and the one place the
     * caller's ID category appears; uint32 ids widen losslessly into the int64 row. The release
     * store of the ID is what makes the payload bytes visible to readers, and the index install
     * is what makes the ID unique. A duplicate absorbs last-write-wins into the surviving slot -
     * payload and score both - and the burned claim stays unpublished and reads back as sentinel.
     * Total atomic operations: 1 store, 1 acq_rel CAS, 1 release store per duplicate
     * Total branches: 1 (duplicate check)
     * Total performance score: 40
     * @param slot The claimed slot.
     * @param id The row's ID in the caller's category.
     * @param payload The row's payload, moved in.
     * @param row_score The row's cached score, written once and read by every drain; higher
     * survives drains. Defaults to zero.
     */
    void publish(const size_t& slot, const ID& id, Slice&& payload, const float& row_score = 0.0f) {
        payloads()[slot] = std::move(payload);
        raws()[slot] = payloads()[slot].template data<void>();
        scores()[slot] = row_score;
        std::atomic_ref<ID>(ids()[slot]).store(static_cast<int64_t>(id), std::memory_order_release);
        const int64_t live = install(static_cast<int64_t>(id), slot);
        if (live != static_cast<int64_t>(slot)) [[unlikely]] {
            payloads()[static_cast<size_t>(live)] = std::move(payloads()[slot]);
            raws()[static_cast<size_t>(live)] = payloads()[static_cast<size_t>(live)].template data<void>();
            scores()[static_cast<size_t>(live)] = row_score;
            raws()[slot] = nullptr;
            std::atomic_ref<ID>(ids()[slot]).store(SENTINEL, std::memory_order_release);
        }
    }
    /** ------------------------------------------------------------------------------------------- Merge
     * @brief Splices another set's rows in at memcpy speed, then rebuilds the spliced range's
     * index entries with plain stores; runs under the drain protocol, never concurrent with
     * producers or readers. Still a set merge: duplicate ids last-write-wins, the spliced
     * payload replaces the survivor's and the burned slot goes back to sentinel.
     * Total atomic operations: 1 release store
     * Total branches: 2 per spliced row
     * @param other The set to drain into this one.
     * @return This set, holding both sets' rows.
     */
    VecSet& merge(VecSet& other) {
        const size_t count = other.capacity_;
        const size_t first = claim(count);
        /// Bitwise claim transfer: copy the row bytes, then stamp the source set free so its
        /// destructor releases nothing and its index holds no stale rows.
        std::memcpy(ids() + first, other.ids(), count * sizeof(ID));
        std::memcpy(raws() + first, other.raws(), count * sizeof(void*));
        std::memcpy(payloads() + first, other.payloads(), count * sizeof(Slice));
        std::memcpy(scores() + first, other.scores(), count * sizeof(float));
        std::memset(other.ids(), 0xFF, count * sizeof(ID));
        std::memset(other.raws(), 0, count * sizeof(void*));
        std::memset(other.payloads(), 0xFF, count * sizeof(Slice));
        std::memset(other.scores(), 0, count * sizeof(float));
        std::memset(other.index_.get(), 0xFF, (other.index_mask_ + 1u) * sizeof(uint32_t));
        other.claimed_.store(0, std::memory_order_release);
        for (size_t i = 0; i < count; ++i) {
            const size_t slot = first + i;
            const ID id = ids()[slot];
            if (id == SENTINEL) continue;
            size_t probe = bucket_of(id);
            for (;;) {
                const uint32_t entry = index_[probe].load(std::memory_order_acquire);
                if (entry == EMPTY) {
                    index_[probe].store(static_cast<uint32_t>(slot), std::memory_order_release);
                    break;
                }
                if (ids()[entry] == id) {
                    payloads()[entry] = std::move(payloads()[slot]);
                    raws()[entry] = payloads()[entry].template data<void>();
                    scores()[entry] = scores()[slot];
                    raws()[slot] = nullptr;
                    ids()[slot] = SENTINEL;
                    break;
                }
                probe = (probe + 1u) & index_mask_;
            }
        }
        return *this;
    }
    /** ------------------------------------------------------------------------------------------- Size
     * @brief The live row count, scanned; approximate under concurrent producers, exact under
     * the drain protocol.
     * @return Published, non-evicted row count.
     */
    size_t size() const {
        size_t rows = 0;
        const ID* id_base = ids();
        for (size_t slot = 0; slot < capacity_; ++slot) {
            rows += id_base[slot] != SENTINEL ? 1u : 0u;
        }
        return rows;
    }
    /** ------------------------------------------------------------------------------------------- Drain To
     * @brief Evicts the worst-scoring rows down to `keep` survivors - the high-water/low-water
     * cache step: publish until the set is full, then keep the best. Survivors compact into a
     * dense prefix, evicted claims release, and claimed slots reset so producers recycle the
     * freed slots; runs under the drain protocol, never concurrent with producers. Equal scores
     * keep an arbitrary subset - the final rerank is the caller's partial sort.
     * Total atomic operations: 1 release store
     * Total branches: 2 per live row
     * @param keep The number of best-scoring rows to retain.
     * @return The surviving row count.
     */
    size_t drain_to(size_t keep) {
        std::vector<uint32_t> live;
        live.reserve(capacity_);
        for (size_t slot = 0; slot < capacity_; ++slot) {
            if (ids()[slot] != SENTINEL) live.push_back(static_cast<uint32_t>(slot));
        }
        if (live.size() <= keep) return live.size();
        /// Best scores first; a local functor over the score base keeps the dispatch concrete.
        struct ScoreGreater {
            const float* base;
            bool operator()(uint32_t left, uint32_t right) const { return base[left] > base[right]; }
        };
        std::nth_element(live.begin(), live.begin() + static_cast<ptrdiff_t>(keep), live.end(),
            ScoreGreater{scores()});
        /// Sorted sets keep the prefix best-score-first, so slot order is score order.
        if constexpr (Sorted) {
            std::sort(live.begin(), live.begin() + static_cast<ptrdiff_t>(keep), ScoreGreater{scores()});
        }
        const size_t surviving = keep;
        /// Ascending survivor slots never move over an unprocessed row, so compaction is safe.
        for (size_t target = 0; target < surviving; ++target) {
            const size_t source = live[target];
            if (source == target) continue;
            std::memcpy(ids() + target, ids() + source, sizeof(ID));
            std::memcpy(raws() + target, raws() + source, sizeof(void*));
            std::memcpy(payloads() + target, payloads() + source, sizeof(Slice));
            scores()[target] = scores()[source];
        }
        /// Everything past the prefix is dead: release the tail's claims and stamp it unpublished.
        std::memset(ids() + surviving, 0xFF, (capacity_ - surviving) * sizeof(ID));
        std::memset(raws() + surviving, 0, (capacity_ - surviving) * sizeof(void*));
        std::memset(payloads() + surviving, 0xFF, (capacity_ - surviving) * sizeof(Slice));
        std::memset(scores() + surviving, 0, (capacity_ - surviving) * sizeof(float));
        /// Rebuild the index over the survivors with plain stores; the drain protocol owns it.
        std::memset(index_.get(), 0xFF, (index_mask_ + 1u) * sizeof(uint32_t));
        for (size_t slot = 0; slot < surviving; ++slot) {
            size_t probe = bucket_of(ids()[slot]);
            while (index_[probe].load(std::memory_order_relaxed) != EMPTY) {
                probe = (probe + 1u) & index_mask_;
            }
            index_[probe].store(static_cast<uint32_t>(slot), std::memory_order_release);
        }
        claimed_.store(surviving, std::memory_order_release);
        return surviving;
    }
    /** ------------------------------------------------------------------------------------------- Find
     * @brief Locates the row holding `id` through the lock-free index; a miss means the row has
     * not landed (yet) or another row already carries the id's slot in the probe path.
     * @param id The ID to look for.
     * @return The row's slot, or -1 when it has not been published.
     */
    int64_t find(const ID& id) const {
        size_t probe = bucket_of(id);
        for (;;) {
            const uint32_t entry = index_[probe].load(std::memory_order_acquire);
            if (entry == EMPTY) {
                return -1;
            }
            if (std::atomic_ref<ID>(const_cast<ID&>(ids()[static_cast<size_t>(entry)]))
                .load(std::memory_order_acquire) == id) {
                return static_cast<int64_t>(entry);
            }
            probe = (probe + 1u) & index_mask_;
        }
    }
    /** ------------------------------------------------------------------------------------------- Row Access
     * @brief The published row at a slot.
     * @param slot Slot returned by `find` or walked by a consumer.
     * @return The (ID, payload) row.
     */
    Row row(const size_t& slot) {
        return Row{ids()[slot], payloads() + slot};
    }
    /** ------------------------------------------------------------------------------------------- Score Access
     * @brief The row's cached score - written at publish or by the producing kernel afterwards,
     * read by every drain. Compute once, read forever after.
     * @param slot Slot returned by `find` or walked by a consumer.
     * @return The cached score.
     */
    float score(const size_t& slot) const { return scores()[slot]; }
    /** ------------------------------------------------------------------------------------------- Score Write
     * @brief Writes the row's cached score after the fact - the compute-once hook for kernels
     * that score the row after it lands.
     * @param slot Slot returned by `find` or walked by a consumer.
     * @param value The score; higher survives drains.
     */
    void set_score(const size_t& slot, const float& value) { scores()[slot] = value; }
    /** ------------------------------------------------------------------------------------------- Published Check
     * @brief Whether a slot's row has landed.
     * @param slot The slot to check.
     * @return True once the slot's ID has been release-stored.
     */
    bool published(const size_t& slot) const {
        return std::atomic_ref<ID>(const_cast<ID&>(ids()[slot]))
            .load(std::memory_order_acquire) != SENTINEL;
    }
    /** ------------------------------------------------------------------------------------------- Capacity
     * @brief Number of slots in the set.
     * @return The capacity.
     */
    size_t capacity() const noexcept { return capacity_; }
    /** ------------------------------------------------------------------------------------------- ID Base
     * @brief The raw ID array, for SIMD passes over the whole set.
     * @return Typed pointer to `capacity()` IDs; unpublished slots read as the sentinel.
     */
    const ID* id_data() const { return ids(); }
    /** ------------------------------------------------------------------------------------------- Kernel View
     * @brief The kernel-facing pointer list: one payload data pointer per slot, in slot order,
     * maintained at publish time so extraction is free.
     * @return Const pointer list with `capacity()` entries; unpublished slots read as nullptr.
     */
    template <typename P>
    P* const* data() const {
        return reinterpret_cast<P* const*>(raws_.data<void*>());
    }
};
using buffetalligator::Slice;
constexpr size_t PAYLOAD_BYTES = 64u;      ///< Payload claim per row; identical on all sides.
constexpr size_t LOOKUPS = 65536u;         ///< Lookups per measurement.
constexpr size_t VIEW_REPS = 65536u;       ///< View extractions per measurement.
constexpr size_t DUP_RATE = 4u;            ///< One in every `DUP_RATE` duplicate-bench writes replays.
/** --------------------------------------------------------------------------------------------------------- HazardMap
 * @class HazardMap
 * @brief Lock-free bucketed map with hazard-pointer-protected reads: inserts are one CAS at the
 * bucket head, reads publish each node into a per-thread hazard slot and re-validate before
 * dereferencing, paying the protocol's store-load fence per hop. Not a set - duplicates chain.
 */
class HazardMap {
private:
    /** ------------------------------------------------------------------------------------------- Node
     * @struct Node
     * @brief One (ID, payload) row chained into its bucket.
     */
    struct Node {
        int64_t id;                    ///< The row's ID.
        Slice payload;         ///< The row's payload claim.
        std::atomic<Node*> next{nullptr};  ///< Next node in the bucket chain.
    };
    /// @brief Fixed hazard-slot table; each thread gets one slot, claimed on first use.
    static constexpr size_t MAX_THREADS = 64u;
    inline static std::atomic<Node*> hazards_[MAX_THREADS] = {};
    /// @brief Monotone thread-slot registration counter.
    inline static std::atomic<size_t> registered_{0};
    /// @brief Bucket heads.
    std::unique_ptr<std::atomic<Node*>[]> buckets_;
    /// @brief Bucket index mask; bucket count is a power of two.
    size_t mask_;
    /** ------------------------------------------------------------------------------------------- my_hazard
     * @brief This thread's hazard slot.
     */
    static std::atomic<Node*>& my_hazard() {
        thread_local size_t slot = registered_.fetch_add(1, std::memory_order_relaxed) % MAX_THREADS;
        return hazards_[slot];
    }
    /** ------------------------------------------------------------------------------------------- bucket_of
     * @brief Fibonacci-hash an ID onto its bucket.
     */
    size_t bucket_of(int64_t id) const {
        return (static_cast<uint64_t>(id) * 11400714819323198485ull >> 40) & mask_;
    }
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Sizes the bucket array to roughly one row per bucket, rounded to a power of two.
     */
    explicit HazardMap(size_t capacity) {
        size_t buckets = 64u;
        while (buckets < capacity) {
            buckets <<= 1u;
        }
        mask_ = buckets - 1u;
        buckets_ = std::make_unique<std::atomic<Node*>[]>(buckets);
    }
    HazardMap(const HazardMap&) = delete;
    HazardMap& operator=(const HazardMap&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Frees every chained node; the request is over, so no reader can hold a hazard.
     */
    ~HazardMap() {
        for (size_t b = 0; b <= mask_; ++b) {
            Node* node = buckets_[b].load(std::memory_order_acquire);
            while (node != nullptr) {
                Node* next = node->next.load(std::memory_order_relaxed);
                delete node;
                node = next;
            }
        }
    }
    /** ------------------------------------------------------------------------------------------- insert
     * @brief Publishes one row with a single CAS at its bucket head.
     */
    void insert(int64_t id, Slice&& payload) {
        Node* node = new Node{id, std::move(payload)};
        std::atomic<Node*>& head = buckets_[bucket_of(id)];
        Node* old_head = head.load(std::memory_order_acquire);
        do {
            node->next.store(old_head, std::memory_order_relaxed);
        } while (!head.compare_exchange_weak(
            old_head, node, std::memory_order_release, std::memory_order_acquire));
    }
    /** ------------------------------------------------------------------------------------------- find
     * @brief Hazard-protected traversal: publish the candidate, re-validate the link that led
     * to it, then dereference.
     */
    Slice* find(int64_t id) {
        std::atomic<Node*>& hazard = my_hazard();
        std::atomic<Node*>* source = &buckets_[bucket_of(id)];
        Node* node = source->load(std::memory_order_acquire);
        while (node != nullptr) {
            hazard.store(node, std::memory_order_seq_cst);
            if (source->load(std::memory_order_acquire) != node) {
                node = source->load(std::memory_order_acquire);
                continue;
            }
            if (node->id == id) {
                hazard.store(nullptr, std::memory_order_release);
                return &node->payload;
            }
            source = &node->next;
            node = source->load(std::memory_order_acquire);
        }
        hazard.store(nullptr, std::memory_order_release);
        return nullptr;
    }
    /** ------------------------------------------------------------------------------------------- view_into
     * @brief Walks every bucket into a kernel-facing pointer list; order is bucket order.
     */
    size_t view_into(std::vector<void*>& out) {
        out.clear();
        for (size_t b = 0; b <= mask_; ++b) {
            Node* node = buckets_[b].load(std::memory_order_acquire);
            while (node != nullptr) {
                out.push_back(node->payload.data<void>());
                node = node->next.load(std::memory_order_acquire);
            }
        }
        return out.size();
    }
    /** ------------------------------------------------------------------------------------------- drain_into
     * @brief Moves every row into another map, emptying this one.
     */
    void drain_into(HazardMap& destination) {
        for (size_t b = 0; b <= mask_; ++b) {
            Node* node = buckets_[b].exchange(nullptr, std::memory_order_acq_rel);
            while (node != nullptr) {
                destination.insert(node->id, std::move(node->payload));
                Node* next = node->next.load(std::memory_order_relaxed);
                delete node;
                node = next;
            }
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- MutexMap
 * @class MutexMap
 * @brief The vanilla control: `std::unordered_map` under one mutex - the shape being replaced.
 * Unique keys by container contract; `insert_or_assign` makes duplicates last-write-wins.
 */
class MutexMap {
private:
    std::unordered_map<int64_t, Slice> rows_;  ///< Keyed rows under the gate.
    mutable std::mutex mutex_;                         ///< The one gate.
public:
    /** ----------------------------------------------------------------------------------------- Constructor
     * @brief Reserves for the expected row count; there is no other capacity notion.
     */
    explicit MutexMap(size_t capacity) {
        rows_.reserve(capacity);
    }
    MutexMap(const MutexMap&) = delete;
    MutexMap& operator=(const MutexMap&) = delete;
    /** ----------------------------------------------------------------------------------------- insert
     * @brief Inserts or overwrites one row under the gate.
     */
    void insert(int64_t id, Slice&& payload) {
        std::lock_guard<std::mutex> guard(mutex_);
        rows_.insert_or_assign(id, std::move(payload));
    }
    /** ----------------------------------------------------------------------------------------- find
     * @brief Copies a shared view of the row's payload out under the gate; null when absent.
     */
    Slice find(int64_t id) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = rows_.find(id);
        return found == rows_.end() ? Slice() : found->second.slice();
    }
    /** ----------------------------------------------------------------------------------------- view_into
     * @brief Walks the map into a kernel-facing pointer list under the gate.
     */
    size_t view_into(std::vector<void*>& out) {
        std::lock_guard<std::mutex> guard(mutex_);
        out.clear();
        out.reserve(rows_.size());
        for (auto& entry : rows_) {
            out.push_back(entry.second.data<void>());
        }
        return out.size();
    }
    /** ----------------------------------------------------------------------------------------- drain_into
     * @brief Moves every row into another map under the destination's gate, emptying this one.
     */
    void drain_into(MutexMap& destination) {
        std::lock_guard<std::mutex> guard(destination.mutex_);
        for (auto& entry : rows_) {
            destination.rows_.insert_or_assign(entry.first, std::move(entry.second));
        }
        rows_.clear();
    }
};
/** --------------------------------------------------------------------------------------------------------- now_ns
 * @brief Monotonic nanosecond timestamp.
 */
inline uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
/** --------------------------------------------------------------------------------------------------------- scattered_ids
 * @brief Deterministic non-adjacent IDs - Fibonacci-hashed indices, matching how coarse-filter
 * survivors actually look (scattered, never sequential).
 */
std::vector<int64_t> scattered_ids(size_t count) {
    std::vector<int64_t> ids(count);
    for (size_t i = 0; i < count; ++i) {
        ids[i] = static_cast<int64_t>((i * 11400714819323198485ull) >> 24);
    }
    return ids;
}
/** --------------------------------------------------------------------------------------------------------- payload_pool
 * @brief Pre-claims one payload slice per row so the timed region measures the container, not
 * the arena.
 */
std::vector<Slice> payload_pool(size_t count) {
    std::vector<Slice> pool;
    pool.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        pool.emplace_back(PAYLOAD_BYTES);
    }
    return pool;
}
/** --------------------------------------------------------------------------------------------------------- write_list
 * @brief The duplicate-bench write schedule: the first `count - count / DUP_RATE` entries are
 * the distinct IDs, the tail replays them so every replayed ID is written exactly twice.
 */
std::vector<int64_t> write_list(size_t count) {
    const std::vector<int64_t> ids = scattered_ids(count);
    const size_t distinct = count - count / DUP_RATE;
    std::vector<int64_t> writes(count);
    for (size_t i = 0; i < count; ++i) {
        writes[i] = i < distinct ? ids[i] : ids[i - distinct];
    }
    return writes;
}
/** --------------------------------------------------------------------------------------------------------- write_tag
 * @brief The byte stamped into each write's payload; distinct writes of one ID must differ, so
 * the surviving tag proves which write won.
 */
uint8_t write_tag(size_t index) {
    return static_cast<uint8_t>(index % 251u + 1u);
}
/** --------------------------------------------------------------------------------------------------------- tag_pool
 * @brief A payload pool with every write's tag stamped at byte zero.
 */
std::vector<Slice> tag_pool(const std::vector<int64_t>& writes) {
    std::vector<Slice> pool = payload_pool(writes.size());
    for (size_t i = 0; i < pool.size(); ++i) {
        pool[i].data<uint8_t>()[0] = write_tag(i);
    }
    return pool;
}
/** --------------------------------------------------------------------------------------------------------- Section
 * @struct Section
 * @brief One benchmark table: six columns of cells, header row first, printed with cyan borders.
 */
struct Section {
    std::string title;                              ///< The log title printed above the table.
    std::vector<std::vector<std::string>> columns;  ///< Column-major cells; row zero is the header.
    /** ------------------------------------------------------------------------------------------- Section
     * @brief Lays out the columns under their unit-labeled headers.
     * @param section_title The log title printed above the table.
     * @param unit The measurement unit shared by every row of the section.
     */
    Section(const std::string& section_title, const std::string& unit)
    : title(section_title)
    , columns{
        {"bench"}, {"vecset " + unit}, {"slicemap " + unit},
        {"hazard " + unit}, {"mutex " + unit}, {"notes"}
    } {}
    /** ------------------------------------------------------------------------------------------- add
     * @brief Appends one measured row.
     */
    void add(
        const std::string& label,
        const std::string& vecset,
        const std::string& slicemap,
        const std::string& hazard,
        const std::string& mutex,
        const std::string& notes = ""
    ) {
        columns[0].push_back(label);
        columns[1].push_back(vecset);
        columns[2].push_back(slicemap);
        columns[3].push_back(hazard);
        columns[4].push_back(mutex);
        columns[5].push_back(notes);
    }
};
/** --------------------------------------------------------------------------------------------------------- num
 * @brief Formats one measurement with two decimals.
 */
std::string num(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value;
    return out.str();
}
/** --------------------------------------------------------------------------------------------------------- print_section
 * @brief Logs the title and prints the section's table with cyan borders.
 */
void print_section(const Section& section) {
    LOG_INFO_STREAM << section.title;
    threadsafe_logger::logging::print_table(section.columns, {}, threadsafe_logger::logging::TTYCYAN);
}
/** --------------------------------------------------------------------------------------------------------- bench_fill
 * @brief Single-thread gather: N publishes into each container.
 */
void bench_fill(size_t count, size_t rounds, Section& out) {
    const std::vector<int64_t> ids = scattered_ids(count);
    uint64_t vecset_ns = 0;
    uint64_t slicemap_ns = 0;
    uint64_t hazard_ns = 0;
    uint64_t mutex_ns = 0;
    for (size_t round = 0; round < rounds; ++round) {
        std::vector<Slice> pool = payload_pool(count);
        const uint64_t vecset_start = now_ns();
        VecSet set(count);
        for (size_t i = 0; i < count; ++i) {
            set.publish(set.claim(), ids[i], std::move(pool[i]));
        }
        vecset_ns += now_ns() - vecset_start;
        std::vector<Slice> map_pool = payload_pool(count);
        const uint64_t slicemap_start = now_ns();
        SliceMap map(count);
        for (size_t i = 0; i < count; ++i) {
            map.add_slice(ids[i], std::move(map_pool[i]));
        }
        slicemap_ns += now_ns() - slicemap_start;
        std::vector<Slice> hazard_pool = payload_pool(count);
        const uint64_t hazard_start = now_ns();
        HazardMap hazard(count);
        for (size_t i = 0; i < count; ++i) {
            hazard.insert(ids[i], std::move(hazard_pool[i]));
        }
        hazard_ns += now_ns() - hazard_start;
        std::vector<Slice> mutex_pool = payload_pool(count);
        const uint64_t mutex_start = now_ns();
        MutexMap mutex_map(count);
        for (size_t i = 0; i < count; ++i) {
            mutex_map.insert(ids[i], std::move(mutex_pool[i]));
        }
        mutex_ns += now_ns() - mutex_start;
    }
    out.add("n=" + std::to_string(count),
        num(static_cast<double>(vecset_ns) / static_cast<double>(rounds * count)),
        num(static_cast<double>(slicemap_ns) / static_cast<double>(rounds * count)),
        num(static_cast<double>(hazard_ns) / static_cast<double>(rounds * count)),
        num(static_cast<double>(mutex_ns) / static_cast<double>(rounds * count)));
}
/** --------------------------------------------------------------------------------------------------------- bench_mp_gather
 * @brief Multi-producer gather: `producers` threads split the batch and publish concurrently,
 * the substrate-parallel-emit shape.
 */
void bench_mp_gather(size_t count, size_t producers, size_t rounds, Section& out) {
    const std::vector<int64_t> ids = scattered_ids(count);
    uint64_t vecset_ns = 0;
    uint64_t slicemap_ns = 0;
    uint64_t hazard_ns = 0;
    uint64_t mutex_ns = 0;
    for (size_t round = 0; round < rounds; ++round) {
        {
            std::vector<Slice> pool = payload_pool(count);
            VecSet set(count);
            const uint64_t start = now_ns();
            std::vector<std::thread> threads;
            for (size_t p = 0; p < producers; ++p) {
                threads.emplace_back([&set, &ids, &pool, p, producers, count]() {
                    for (size_t i = p; i < count; i += producers) {
                        set.publish(set.claim(), ids[i], std::move(pool[i]));
                    }
                });
            }
            for (std::thread& thread : threads) {
                thread.join();
            }
            vecset_ns += now_ns() - start;
        }
        {
            std::vector<Slice> pool = payload_pool(count);
            SliceMap map(count);
            const uint64_t start = now_ns();
            std::vector<std::thread> threads;
            for (size_t p = 0; p < producers; ++p) {
                threads.emplace_back([&map, &ids, &pool, p, producers, count]() {
                    for (size_t i = p; i < count; i += producers) {
                        map.add_slice(ids[i], std::move(pool[i]));
                    }
                });
            }
            for (std::thread& thread : threads) {
                thread.join();
            }
            slicemap_ns += now_ns() - start;
        }
        {
            std::vector<Slice> pool = payload_pool(count);
            HazardMap hazard(count);
            const uint64_t start = now_ns();
            std::vector<std::thread> threads;
            for (size_t p = 0; p < producers; ++p) {
                threads.emplace_back([&hazard, &ids, &pool, p, producers, count]() {
                    for (size_t i = p; i < count; i += producers) {
                        hazard.insert(ids[i], std::move(pool[i]));
                    }
                });
            }
            for (std::thread& thread : threads) {
                thread.join();
            }
            hazard_ns += now_ns() - start;
        }
        {
            std::vector<Slice> pool = payload_pool(count);
            MutexMap mutex_map(count);
            const uint64_t start = now_ns();
            std::vector<std::thread> threads;
            for (size_t p = 0; p < producers; ++p) {
                threads.emplace_back([&mutex_map, &ids, &pool, p, producers, count]() {
                    for (size_t i = p; i < count; i += producers) {
                        mutex_map.insert(ids[i], std::move(pool[i]));
                    }
                });
            }
            for (std::thread& thread : threads) {
                thread.join();
            }
            mutex_ns += now_ns() - start;
        }
    }
    out.add("n=" + std::to_string(count) + " p=" + std::to_string(producers),
        num(static_cast<double>(vecset_ns) / static_cast<double>(rounds) / 1000.0),
        num(static_cast<double>(slicemap_ns) / static_cast<double>(rounds) / 1000.0),
        num(static_cast<double>(hazard_ns) / static_cast<double>(rounds) / 1000.0),
        num(static_cast<double>(mutex_ns) / static_cast<double>(rounds) / 1000.0));
}
/** --------------------------------------------------------------------------------------------------------- bench_lookup
 * @brief Post-gather hit lookups, single thread, IDs revisited in scattered order.
 */
void bench_lookup(size_t count, Section& out) {
    const std::vector<int64_t> ids = scattered_ids(count);
    std::vector<Slice> pool = payload_pool(count);
    VecSet set(count);
    for (size_t i = 0; i < count; ++i) {
        set.publish(set.claim(), ids[i], std::move(pool[i]));
    }
    std::vector<Slice> map_pool = payload_pool(count);
    SliceMap map(count);
    for (size_t i = 0; i < count; ++i) {
        map.add_slice(ids[i], std::move(map_pool[i]));
    }
    std::vector<Slice> hazard_pool = payload_pool(count);
    HazardMap hazard(count);
    for (size_t i = 0; i < count; ++i) {
        hazard.insert(ids[i], std::move(hazard_pool[i]));
    }
    std::vector<Slice> mutex_pool = payload_pool(count);
    MutexMap mutex_map(count);
    for (size_t i = 0; i < count; ++i) {
        mutex_map.insert(ids[i], std::move(mutex_pool[i]));
    }
    uint64_t vecset_hits = 0;
    const uint64_t vecset_start = now_ns();
    for (size_t i = 0; i < LOOKUPS; ++i) {
        vecset_hits += set.find(ids[(i * 7919u) % count]) >= 0 ? 1u : 0u;
    }
    const uint64_t vecset_ns = now_ns() - vecset_start;
    uint64_t slicemap_hits = 0;
    const uint64_t slicemap_start = now_ns();
    for (size_t i = 0; i < LOOKUPS; ++i) {
        Slice found = map.get_slice(ids[(i * 7919u) % count]);
        slicemap_hits += found ? 1u : 0u;
    }
    const uint64_t slicemap_ns = now_ns() - slicemap_start;
    uint64_t hazard_hits = 0;
    const uint64_t hazard_start = now_ns();
    for (size_t i = 0; i < LOOKUPS; ++i) {
        hazard_hits += hazard.find(ids[(i * 7919u) % count]) != nullptr ? 1u : 0u;
    }
    const uint64_t hazard_ns = now_ns() - hazard_start;
    uint64_t mutex_hits = 0;
    const uint64_t mutex_start = now_ns();
    for (size_t i = 0; i < LOOKUPS; ++i) {
        Slice found = mutex_map.find(ids[(i * 7919u) % count]);
        mutex_hits += found ? 1u : 0u;
    }
    const uint64_t mutex_ns = now_ns() - mutex_start;
    const bool all_hit = vecset_hits == LOOKUPS && slicemap_hits == LOOKUPS
        && hazard_hits == LOOKUPS && mutex_hits == LOOKUPS;
    out.add("n=" + std::to_string(count),
        num(static_cast<double>(vecset_ns) / static_cast<double>(LOOKUPS)),
        num(static_cast<double>(slicemap_ns) / static_cast<double>(LOOKUPS)),
        num(static_cast<double>(hazard_ns) / static_cast<double>(LOOKUPS)),
        num(static_cast<double>(mutex_ns) / static_cast<double>(LOOKUPS)),
        all_hit ? "all hit" : "HIT MISS");
}
/** --------------------------------------------------------------------------------------------------------- bench_stream
 * @brief One producer publishes scattered rows while `consumers` threads hunt disjoint ID
 * subsets, consuming each row the moment it lands.
 */
void bench_stream(size_t count, size_t consumers, size_t rounds, Section& out) {
    const std::vector<int64_t> ids = scattered_ids(count);
    uint64_t vecset_ns = 0;
    uint64_t slicemap_ns = 0;
    uint64_t hazard_ns = 0;
    uint64_t mutex_ns = 0;
    for (size_t round = 0; round < rounds; ++round) {
        {
            std::vector<Slice> pool = payload_pool(count);
            VecSet set(count);
            const uint64_t start = now_ns();
            std::vector<std::thread> threads;
            for (size_t c = 0; c < consumers; ++c) {
                threads.emplace_back([&set, &ids, c, consumers, count]() {
                    for (size_t i = c; i < count; i += consumers) {
                        while (set.find(ids[i]) < 0) {
                            std::this_thread::yield();
                        }
                    }
                });
            }
            for (size_t i = 0; i < count; ++i) {
                set.publish(set.claim(), ids[i], std::move(pool[i]));
            }
            for (std::thread& thread : threads) {
                thread.join();
            }
            vecset_ns += now_ns() - start;
        }
        {
            std::vector<Slice> pool = payload_pool(count);
            SliceMap map(count);
            const uint64_t start = now_ns();
            std::vector<std::thread> threads;
            for (size_t c = 0; c < consumers; ++c) {
                threads.emplace_back([&map, &ids, c, consumers, count]() {
                    for (size_t i = c; i < count; i += consumers) {
                        while (!map.get_slice(ids[i])) {
                            std::this_thread::yield();
                        }
                    }
                });
            }
            for (size_t i = 0; i < count; ++i) {
                map.add_slice(ids[i], std::move(pool[i]));
            }
            for (std::thread& thread : threads) {
                thread.join();
            }
            slicemap_ns += now_ns() - start;
        }
        {
            std::vector<Slice> pool = payload_pool(count);
            HazardMap hazard(count);
            const uint64_t start = now_ns();
            std::vector<std::thread> threads;
            for (size_t c = 0; c < consumers; ++c) {
                threads.emplace_back([&hazard, &ids, c, consumers, count]() {
                    for (size_t i = c; i < count; i += consumers) {
                        while (hazard.find(ids[i]) == nullptr) {
                            std::this_thread::yield();
                        }
                    }
                });
            }
            for (size_t i = 0; i < count; ++i) {
                hazard.insert(ids[i], std::move(pool[i]));
            }
            for (std::thread& thread : threads) {
                thread.join();
            }
            hazard_ns += now_ns() - start;
        }
        {
            std::vector<Slice> pool = payload_pool(count);
            MutexMap mutex_map(count);
            const uint64_t start = now_ns();
            std::vector<std::thread> threads;
            for (size_t c = 0; c < consumers; ++c) {
                threads.emplace_back([&mutex_map, &ids, c, consumers, count]() {
                    for (size_t i = c; i < count; i += consumers) {
                        while (!mutex_map.find(ids[i])) {
                            std::this_thread::yield();
                        }
                    }
                });
            }
            for (size_t i = 0; i < count; ++i) {
                mutex_map.insert(ids[i], std::move(pool[i]));
            }
            for (std::thread& thread : threads) {
                thread.join();
            }
            mutex_ns += now_ns() - start;
        }
    }
    out.add("n=" + std::to_string(count) + " c=" + std::to_string(consumers),
        num(static_cast<double>(vecset_ns) / static_cast<double>(rounds) / 1000.0),
        num(static_cast<double>(slicemap_ns) / static_cast<double>(rounds) / 1000.0),
        num(static_cast<double>(hazard_ns) / static_cast<double>(rounds) / 1000.0),
        num(static_cast<double>(mutex_ns) / static_cast<double>(rounds) / 1000.0));
}
/** --------------------------------------------------------------------------------------------------------- bench_view
 * @brief Extracts the kernel-facing pointer list repeatedly - the hand-off to the SIMD kernels.
 * VecSet and SliceMap maintain the list natively; the hazard map and the mutex control walk.
 */
void bench_view(size_t count, Section& out) {
    const std::vector<int64_t> ids = scattered_ids(count);
    std::vector<Slice> pool = payload_pool(count);
    VecSet set(count);
    for (size_t i = 0; i < count; ++i) {
        set.publish(set.claim(), ids[i], std::move(pool[i]));
    }
    std::vector<Slice> map_pool = payload_pool(count);
    SliceMap map(count);
    for (size_t i = 0; i < count; ++i) {
        map.add_slice(ids[i], std::move(map_pool[i]));
    }
    std::vector<Slice> hazard_pool = payload_pool(count);
    HazardMap hazard(count);
    for (size_t i = 0; i < count; ++i) {
        hazard.insert(ids[i], std::move(hazard_pool[i]));
    }
    std::vector<Slice> mutex_pool = payload_pool(count);
    MutexMap mutex_map(count);
    for (size_t i = 0; i < count; ++i) {
        mutex_map.insert(ids[i], std::move(mutex_pool[i]));
    }
    /// Fold the first pointer of each extracted view into a sink so extraction cannot be elided.
    uintptr_t sink = 0;
    const uint64_t vecset_start = now_ns();
    for (size_t i = 0; i < VIEW_REPS; ++i) {
        float* const* rows = set.data<float>();
        sink ^= reinterpret_cast<uintptr_t>(rows[i % count]);
    }
    const uint64_t vecset_ns = now_ns() - vecset_start;
    const uint64_t slicemap_start = now_ns();
    for (size_t i = 0; i < VIEW_REPS; ++i) {
        float** rows = map.data<float>();
        sink ^= reinterpret_cast<uintptr_t>(rows[i % count]);
    }
    const uint64_t slicemap_ns = now_ns() - slicemap_start;
    std::vector<void*> walked;
    walked.reserve(count);
    /// The hazard walk is O(n) per extraction, so it takes proportionally fewer reps.
    const size_t walk_reps = VIEW_REPS / count > 0 ? VIEW_REPS / count : 1u;
    const uint64_t hazard_start = now_ns();
    for (size_t i = 0; i < walk_reps; ++i) {
        hazard.view_into(walked);
        sink ^= reinterpret_cast<uintptr_t>(walked[i % count]);
    }
    const uint64_t hazard_ns = now_ns() - hazard_start;
    const uint64_t mutex_start = now_ns();
    for (size_t i = 0; i < walk_reps; ++i) {
        mutex_map.view_into(walked);
        sink ^= reinterpret_cast<uintptr_t>(walked[i % count]);
    }
    const uint64_t mutex_ns = now_ns() - mutex_start;
    out.add("n=" + std::to_string(count),
        num(static_cast<double>(vecset_ns) / static_cast<double>(VIEW_REPS)),
        num(static_cast<double>(slicemap_ns) / static_cast<double>(VIEW_REPS)),
        num(static_cast<double>(hazard_ns) / static_cast<double>(walk_reps)),
        num(static_cast<double>(mutex_ns) / static_cast<double>(walk_reps)),
        "sink=" + std::to_string(sink & 0xFu));
}
/** --------------------------------------------------------------------------------------------------------- bench_combine
 * @brief Combines `parts` gathered sets into one, the multi-scanner / multi-shard merge shape.
 */
void bench_combine(size_t count, size_t parts, size_t rounds, Section& out) {
    const std::vector<int64_t> ids = scattered_ids(count);
    const size_t part_rows = count / parts;
    uint64_t vecset_ns = 0;
    uint64_t slicemap_ns = 0;
    uint64_t hazard_ns = 0;
    uint64_t mutex_ns = 0;
    for (size_t round = 0; round < rounds; ++round) {
        {
            std::vector<VecSet<>> sources;
            sources.reserve(parts);
            for (size_t p = 0; p < parts; ++p) {
                std::vector<Slice> pool = payload_pool(part_rows);
                sources.emplace_back(part_rows);
                for (size_t i = 0; i < part_rows; ++i) {
                    sources[p].publish(sources[p].claim(), ids[p * part_rows + i], std::move(pool[i]));
                }
            }
            const uint64_t start = now_ns();
            VecSet combined(count);
            for (size_t p = 0; p < parts; ++p) {
                combined.merge(sources[p]);
            }
            vecset_ns += now_ns() - start;
        }
        {
            std::vector<std::unique_ptr<SliceMap>> sources;
            for (size_t p = 0; p < parts; ++p) {
                std::vector<Slice> pool = payload_pool(part_rows);
                sources.push_back(std::make_unique<SliceMap>(part_rows));
                for (size_t i = 0; i < part_rows; ++i) {
                    sources[p]->add_slice(ids[p * part_rows + i], std::move(pool[i]));
                }
            }
            const uint64_t start = now_ns();
            SliceMap combined(count);
            for (size_t p = 0; p < parts; ++p) {
                combined + *sources[p];
            }
            slicemap_ns += now_ns() - start;
        }
        {
            std::vector<std::unique_ptr<HazardMap>> sources;
            for (size_t p = 0; p < parts; ++p) {
                std::vector<Slice> pool = payload_pool(part_rows);
                sources.push_back(std::make_unique<HazardMap>(part_rows));
                for (size_t i = 0; i < part_rows; ++i) {
                    sources[p]->insert(ids[p * part_rows + i], std::move(pool[i]));
                }
            }
            const uint64_t start = now_ns();
            HazardMap combined(count);
            for (size_t p = 0; p < parts; ++p) {
                sources[p]->drain_into(combined);
            }
            hazard_ns += now_ns() - start;
        }
        {
            std::vector<std::unique_ptr<MutexMap>> sources;
            for (size_t p = 0; p < parts; ++p) {
                std::vector<Slice> pool = payload_pool(part_rows);
                sources.push_back(std::make_unique<MutexMap>(part_rows));
                for (size_t i = 0; i < part_rows; ++i) {
                    sources[p]->insert(ids[p * part_rows + i], std::move(pool[i]));
                }
            }
            const uint64_t start = now_ns();
            MutexMap combined(count);
            for (size_t p = 0; p < parts; ++p) {
                sources[p]->drain_into(combined);
            }
            mutex_ns += now_ns() - start;
        }
    }
    out.add("n=" + std::to_string(count) + " parts=" + std::to_string(parts),
        num(static_cast<double>(vecset_ns) / static_cast<double>(rounds) / 1000.0),
        num(static_cast<double>(slicemap_ns) / static_cast<double>(rounds) / 1000.0) + " DQ",
        num(static_cast<double>(hazard_ns) / static_cast<double>(rounds) / 1000.0) + " DQ",
        num(static_cast<double>(mutex_ns) / static_cast<double>(rounds) / 1000.0),
        "set merge: vecset mutex; DQ = splice/reinsert, no dedup by id");
}
/** --------------------------------------------------------------------------------------------------------- bench_duplicates
 * @brief Duplicate-heavy writes with set-semantics assertions: a quarter of the writes replay
 * earlier IDs, so every container is measured and checked on the path that makes a set a set.
 * VecSet must keep exactly one row per ID with the last write surviving; the mutex control must
 * match; SliceMap is a channel, so it must retain the duplicates instead.
 */
void bench_duplicates(size_t count, size_t rounds, Section& out) {
    const std::vector<int64_t> writes = write_list(count);
    const size_t distinct = count - count / DUP_RATE;
    /// The expected surviving tag per ID: the tag of whichever write came last.
    std::unordered_map<int64_t, uint8_t> expected;
    expected.reserve(distinct);
    for (size_t i = 0; i < count; ++i) {
        expected[writes[i]] = write_tag(i);
    }
    uint64_t vecset_ns = 0;
    uint64_t slicemap_ns = 0;
    uint64_t hazard_ns = 0;
    uint64_t mutex_ns = 0;
    bool vecset_set = true;
    bool mutex_set = true;
    bool slicemap_channel = true;
    for (size_t round = 0; round < rounds; ++round) {
        {
            std::vector<Slice> pool = tag_pool(writes);
            VecSet set(count);
            const uint64_t start = now_ns();
            for (size_t i = 0; i < count; ++i) {
                set.publish(set.claim(), writes[i], std::move(pool[i]));
            }
            vecset_ns += now_ns() - start;
            if (round == 0) {
                size_t landed = 0u;
                for (size_t slot = 0; slot < count; ++slot) {
                    landed += set.published(slot) ? 1u : 0u;
                }
                for (const auto& want : expected) {
                    const int64_t slot = set.find(want.first);
                    if (slot < 0
                        || set.row(static_cast<size_t>(slot)).payload->data<uint8_t>()[0] != want.second) {
                        vecset_set = false;
                        break;
                    }
                }
                if (landed != distinct) vecset_set = false;
                /// Merge must dedup too: overlap two sets on one id, last write wins the splice.
                std::vector<Slice> merge_pool = payload_pool(4u);
                merge_pool[0].data<uint8_t>()[0] = 1u;
                merge_pool[1].data<uint8_t>()[0] = 2u;
                merge_pool[2].data<uint8_t>()[0] = 3u;
                merge_pool[3].data<uint8_t>()[0] = 4u;
                VecSet left(2u);
                VecSet right(2u);
                left.publish(left.claim(), int64_t(900), std::move(merge_pool[0]), 5.0f);
                left.publish(left.claim(), int64_t(901), std::move(merge_pool[1]), 2.0f);
                right.publish(right.claim(), int64_t(900), std::move(merge_pool[2]), 9.0f);
                VecSet both(4u);
                both.merge(left);
                both.merge(right);
                const int64_t merged_slot = both.find(int64_t(900));
                if (merged_slot < 0
                    || both.row(static_cast<size_t>(merged_slot)).payload->data<uint8_t>()[0] != 3u
                    || both.score(static_cast<size_t>(merged_slot)) != 9.0f
                    || both.score(static_cast<size_t>(both.find(int64_t(901)))) != 2.0f) {
                    vecset_set = false;
                }
                /// Drain keeps the best score, evicts the rest, and recycles the freed slots.
                if (both.drain_to(1u) != 1u
                    || both.find(int64_t(901)) >= 0
                    || both.score(static_cast<size_t>(both.find(int64_t(900)))) != 9.0f) {
                    vecset_set = false;
                }
                both.publish(both.claim(), int64_t(902), std::move(merge_pool[3]), 1.0f);
                if (both.find(int64_t(902)) < 0 || both.size() != 2u) {
                    vecset_set = false;
                }
            }
        }
        {
            std::vector<Slice> pool = tag_pool(writes);
            SliceMap map(count);
            const uint64_t start = now_ns();
            for (size_t i = 0; i < count; ++i) {
                map.add_slice(writes[i], std::move(pool[i]));
            }
            slicemap_ns += now_ns() - start;
            if (round == 0) {
                if (map.size() != count) {
                    slicemap_channel = false;
                }
                for (const auto& want : expected) {
                    if (!map.get_slice(want.first)) {
                        slicemap_channel = false;
                        break;
                    }
                }
            }
        }
        {
            std::vector<Slice> pool = tag_pool(writes);
            HazardMap hazard(count);
            const uint64_t start = now_ns();
            for (size_t i = 0; i < count; ++i) {
                hazard.insert(writes[i], std::move(pool[i]));
            }
            hazard_ns += now_ns() - start;
        }
        {
            std::vector<Slice> pool = tag_pool(writes);
            MutexMap mutex_map(count);
            const uint64_t start = now_ns();
            for (size_t i = 0; i < count; ++i) {
                mutex_map.insert(writes[i], std::move(pool[i]));
            }
            mutex_ns += now_ns() - start;
            if (round == 0) {
                for (const auto& want : expected) {
                    Slice found = mutex_map.find(want.first);
                    if (!found || found.data<uint8_t>()[0] != want.second) {
                        mutex_set = false;
                        break;
                    }
                }
            }
        }
    }
    out.add("n=" + std::to_string(count) + " distinct=" + std::to_string(distinct),
        num(static_cast<double>(vecset_ns) / static_cast<double>(rounds * count)),
        num(static_cast<double>(slicemap_ns) / static_cast<double>(rounds * count)),
        num(static_cast<double>(hazard_ns) / static_cast<double>(rounds * count)),
        num(static_cast<double>(mutex_ns) / static_cast<double>(rounds * count)),
        std::string("vecset_set=") + (vecset_set ? "OK" : "FAIL")
            + " mutex_set=" + (mutex_set ? "OK" : "FAIL")
            + " channel=" + (slicemap_channel ? "OK" : "FAIL"));
}
} // namespace
int main() {
    std::fputs("AUDIT 2026-09-12: EXPERIMENT / PROOF OF CONCEPT; not an application test or benchmark.\n", stderr);
    const size_t sizes[] = {64u, 256u, 512u, 2048u, 16384u, 65536u};
    {
        Section section("fill - single-thread publish, per-row rate", "ns/row");
        for (const size_t count : sizes) {
            bench_fill(count, count <= 2048u ? 200u : 25u, section);
        }
        print_section(section);
    }
    {
        Section section("mpgather - concurrent producers, per-batch time", "us/batch");
        for (const size_t producers : {1u, 2u, 4u, 8u}) {
            bench_mp_gather(2048u, producers, 50u, section);
        }
        bench_mp_gather(65536u, 8u, 10u, section);
        print_section(section);
    }
    {
        Section section("lookup - post-gather hits, per-lookup time", "ns/hit");
        for (const size_t count : sizes) {
            bench_lookup(count, section);
        }
        print_section(section);
    }
    {
        Section section("stream - producer plus spinning consumers, per-batch time", "us/batch");
        for (const size_t consumers : {1u, 2u, 4u, 8u}) {
            bench_stream(256u, consumers, 50u, section);
        }
        for (const size_t consumers : {1u, 2u, 4u, 8u}) {
            bench_stream(2048u, consumers, 50u, section);
        }
        bench_stream(65536u, 8u, 10u, section);
        print_section(section);
    }
    {
        Section section("view - kernel-facing pointer list extraction", "ns/view");
        for (const size_t count : sizes) {
            bench_view(count, section);
        }
        print_section(section);
    }
    {
        Section section("combine - multi-shard SET MERGE (slicemap/hazard splice, disqualified)", "us/merge");
        for (const size_t parts : {2u, 4u, 8u}) {
            bench_combine(2048u, parts, 100u, section);
        }
        bench_combine(65536u, 4u, 10u, section);
        print_section(section);
    }
    {
        Section section("dups - 25% duplicate writes with set-semantics assertions", "ns/row");
        for (const size_t count : sizes) {
            bench_duplicates(count, count <= 2048u ? 100u : 25u, section);
        }
        print_section(section);
    }
    return 0;
}