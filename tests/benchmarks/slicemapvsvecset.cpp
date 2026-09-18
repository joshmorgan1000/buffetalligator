/** --------------------------------------------------------------------------------------------------------- SliceMap vs VecSet
 * @file slicemapvsvecset.cpp
 * @brief Correctness-gated comparison of full-contract map contenders with a fixed-capacity control.
 */
#include <logging.hpp>
#include <loggingutils.hpp>
#include <alligator.hpp>
#include <simd.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <atomic>
#include <barrier>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
using IDType = int64_t;
using buffetalligator::SliceMap;
using buffetalligator::Slice;
/** --------------------------------------------------------------------------------------------------------- VecSet
 * @brief Dense fixed-capacity set with atomic index publication and per-row blocking update gates.
 * @tparam Sorted Whether quiescent drains arrange survivors in descending score order.
 */
template<bool Sorted = false>
class VecSet {
private:
    using ID = int64_t;
    static constexpr ID SENTINEL = -1;
    static constexpr uint32_t EMPTY = UINT32_MAX;
    static constexpr uint32_t BUSY = UINT32_MAX - 1;
    Slice ids_;
    Slice payloads_;
    Slice raws_;
    Slice scores_;
    std::unique_ptr<std::atomic<uint32_t>[]> index_;
    std::unique_ptr<std::atomic_flag[]> gates_;
    size_t index_mask_ = 0;
    size_t index_shift_ = 0;
    std::atomic<size_t> claimed_{0};
    size_t capacity_ = 0;
    /** ------------------------------------------------------------------------------------------- Row Guard
     * @brief Orders in-place Slice replacement against readers retaining the same row.
     */
    class RowGuard {
    private:
        std::atomic_flag& gate_;
    public:
        explicit RowGuard(std::atomic_flag& gate) : gate_(gate) {
            while (gate_.test_and_set(std::memory_order_acquire)) {
                gate_.wait(true, std::memory_order_relaxed);
            }
        }
        ~RowGuard() {
            gate_.clear(std::memory_order_release);
            gate_.notify_one();
        }
        RowGuard(const RowGuard&) = delete;
        RowGuard& operator=(const RowGuard&) = delete;
    };
    ID* ids() { return ids_.data<ID>(); }
    const ID* ids() const { return ids_.data<ID>(); }
    Slice* payloads() { return payloads_.data<Slice>(); }
    const Slice* payloads() const { return payloads_.data<Slice>(); }
    float* scores() { return scores_.data<float>(); }
    const float* scores() const { return scores_.data<float>(); }
    void** raws() { return raws_.data<void*>(); }
    /** ------------------------------------------------------------------------------------------- Index Size
     * @brief Sizes the open-addressed index for at most half occupancy.
     */
    static size_t index_size_for(size_t capacity) {
        size_t size = 16;
        while (size < capacity * 2) size <<= 1;
        return size;
    }
    /** ------------------------------------------------------------------------------------------- Bucket Of
     * @brief Selects a probe start from the high bits of the Fibonacci product.
     */
    size_t bucket_of(ID identifier) const {
        return static_cast<size_t>(static_cast<uint64_t>(identifier)
            * 11400714819323198485ull >> index_shift_);
    }
    /** ------------------------------------------------------------------------------------------- Clear Index
     * @brief Resets index entries while every worker is quiescent.
     */
    void clear_index() {
        for (size_t bucket = 0; bucket <= index_mask_; ++bucket) {
            index_[bucket].store(EMPTY, std::memory_order_relaxed);
        }
    }
public:
    /** ------------------------------------------------------------------------------------------- Row
     * @brief Borrows a row only while producers, updates, and drains are quiescent.
     */
    struct Row {
        ID id;
        Slice* payload;
    };
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Reserves capacity for distinct keys rather than the number of writes.
     */
    explicit VecSet(size_t capacity)
    : ids_(capacity * sizeof(ID)), payloads_(capacity * sizeof(Slice)),
      raws_(capacity * sizeof(void*)), scores_(capacity * sizeof(float)),
      index_(new std::atomic<uint32_t>[index_size_for(capacity)]),
      gates_(new std::atomic_flag[capacity]),
      index_mask_(index_size_for(capacity) - 1),
      index_shift_(64 - static_cast<size_t>(std::countr_zero(index_size_for(capacity)))),
      capacity_(capacity) {
        std::memset(ids(), 0xFF, capacity * sizeof(ID));
        std::memset(raws(), 0, capacity * sizeof(void*));
        std::memset(scores(), 0, capacity * sizeof(float));
        new (payloads()) Slice[capacity];
        clear_index();
    }
    VecSet(const VecSet&) = delete;
    VecSet& operator=(const VecSet&) = delete;
    /** ------------------------------------------------------------------------------------------- Move
     * @brief Transfers row ownership while both sets are quiescent.
     */
    VecSet(VecSet&& other) noexcept
    : ids_(std::move(other.ids_)), payloads_(std::move(other.payloads_)),
      raws_(std::move(other.raws_)), scores_(std::move(other.scores_)),
      index_(std::move(other.index_)), gates_(std::move(other.gates_)),
      index_mask_(other.index_mask_), index_shift_(other.index_shift_),
      claimed_(other.claimed_.load(std::memory_order_relaxed)), capacity_(other.capacity_) {
        other.capacity_ = 0;
        other.claimed_.store(0, std::memory_order_relaxed);
    }
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Releases every owned Slice after all users of the set have stopped.
     */
    ~VecSet() {
        if (capacity_ != 0) {
            for (size_t slot = 0; slot < capacity_; ++slot) payloads()[slot].~Slice();
        }
    }
    /** ------------------------------------------------------------------------------------------- Insert
     * @brief Publishes a new key or replaces its payload and score under the row's gate.
     */
    void insert(ID identifier, Slice&& payload, float row_score = 0.0f) {
        size_t probe = bucket_of(identifier);
        for (;;) {
            uint32_t entry = index_[probe].load(std::memory_order_acquire);
            if (entry == EMPTY) {
                if (!index_[probe].compare_exchange_strong(entry, BUSY,
                    std::memory_order_acq_rel, std::memory_order_acquire)) continue;
                const size_t slot = claimed_.fetch_add(1, std::memory_order_relaxed);
                if (slot >= capacity_) {
                    claimed_.fetch_sub(1, std::memory_order_relaxed);
                    index_[probe].store(EMPTY, std::memory_order_release);
                    index_[probe].notify_all();
                    throw std::length_error("VecSet distinct-key capacity exceeded.");
                }
                payloads()[slot] = std::move(payload);
                raws()[slot] = payloads()[slot].raw();
                scores()[slot] = row_score;
                ids()[slot] = identifier;
                index_[probe].store(static_cast<uint32_t>(slot), std::memory_order_release);
                index_[probe].notify_all();
                return;
            }
            if (entry == BUSY) {
                index_[probe].wait(BUSY, std::memory_order_acquire);
                continue;
            }
            if (ids()[entry] == identifier) {
                RowGuard guard(gates_[entry]);
                payloads()[entry] = std::move(payload);
                raws()[entry] = payloads()[entry].raw();
                scores()[entry] = row_score;
                return;
            }
            probe = (probe + 1) & index_mask_;
        }
    }
    /** ------------------------------------------------------------------------------------------- Merge
     * @brief Moves only live rows from a quiescent source with source values winning overlaps.
     */
    VecSet& merge(VecSet& other) {
        if (&other == this) return *this;
        const size_t count = other.size();
        for (size_t slot = 0; slot < count; ++slot) {
            insert(other.ids()[slot], std::move(other.payloads()[slot]), other.scores()[slot]);
            other.ids()[slot] = SENTINEL;
            other.raws()[slot] = nullptr;
            other.scores()[slot] = 0;
        }
        other.clear_index();
        other.claimed_.store(0, std::memory_order_release);
        return *this;
    }
    /** ------------------------------------------------------------------------------------------- Size
     * @brief Returns distinct reserved rows, exact after publishers have finished.
     */
    size_t size() const { return claimed_.load(std::memory_order_acquire); }
    /** ------------------------------------------------------------------------------------------- Drain To
     * @brief Moves the best-scoring survivors into a dense prefix while every worker is quiescent.
     * @param keep Maximum number of rows to retain.
     * @return The surviving row count.
     */
    size_t drain_to(size_t keep) {
        std::vector<uint32_t> live;
        live.reserve(capacity_);
        for (size_t slot = 0; slot < size(); ++slot) {
            live.push_back(static_cast<uint32_t>(slot));
        }
        keep = std::min(keep, live.size());
        /** --------------------------------------------------------------------------------- Score Greater
         * @brief Orders survivor candidates by descending cached score.
         */
        struct ScoreGreater {
            const float* base;
            bool operator()(uint32_t left, uint32_t right) const {
                return base[left] > base[right];
            }
        };
        if (keep != 0 && keep < live.size()) {
            std::nth_element(live.begin(), live.begin() + static_cast<ptrdiff_t>(keep),
                live.end(), ScoreGreater{scores()});
        }
        if constexpr (Sorted) {
            std::sort(live.begin(), live.begin() + static_cast<ptrdiff_t>(keep),
                ScoreGreater{scores()});
        }
        /// The quiescent index temporarily maps each occupied slot to its permutation position.
        for (size_t position = 0; position < live.size(); ++position) {
            index_[live[position]].store(static_cast<uint32_t>(position),
                std::memory_order_relaxed);
        }
        for (size_t target = 0; target < keep; ++target) {
            const size_t source = live[target];
            if (source == target) continue;
            const uint32_t displaced = index_[target].load(std::memory_order_relaxed);
            live[displaced] = static_cast<uint32_t>(source);
            index_[source].store(displaced, std::memory_order_relaxed);
            std::swap(ids()[target], ids()[source]);
            std::swap(payloads()[target], payloads()[source]);
            std::swap(raws()[target], raws()[source]);
            std::swap(scores()[target], scores()[source]);
            live[target] = static_cast<uint32_t>(target);
            index_[target].store(static_cast<uint32_t>(target), std::memory_order_relaxed);
        }
        for (size_t slot = keep; slot < capacity_; ++slot) {
            payloads()[slot].free();
            ids()[slot] = SENTINEL;
            raws()[slot] = nullptr;
            scores()[slot] = 0.0f;
        }
        for (size_t bucket = 0; bucket <= index_mask_; ++bucket) {
            index_[bucket].store(EMPTY, std::memory_order_relaxed);
        }
        for (size_t slot = 0; slot < keep; ++slot) {
            size_t probe = bucket_of(ids()[slot]);
            while (index_[probe].load(std::memory_order_relaxed) != EMPTY) {
                probe = (probe + 1u) & index_mask_;
            }
            index_[probe].store(static_cast<uint32_t>(slot), std::memory_order_relaxed);
        }
        claimed_.store(keep, std::memory_order_release);
        return keep;
    }
    /** ------------------------------------------------------------------------------------------- Find
     * @brief Finds a published key through the index while first insertions and updates overlap.
     */
    int64_t find(ID identifier) const {
        size_t probe = bucket_of(identifier);
        for (;;) {
            const uint32_t entry = index_[probe].load(std::memory_order_acquire);
            if (entry == EMPTY || entry == BUSY) return -1;
            if (ids()[entry] == identifier) return static_cast<int64_t>(entry);
            probe = (probe + 1) & index_mask_;
        }
    }
    /** ------------------------------------------------------------------------------------------- Find SIMD
     * @brief Scans the occupied ID prefix with the existing SIMD kernel after publishers finish.
     */
    int64_t find_simd(ID identifier) const {
        return buffetalligator::SIMDMisc::find_id(ids(), size(), identifier);
    }
    /** ------------------------------------------------------------------------------------------- Retain
     * @brief Retains a published payload while excluding replacement of its Slice handle.
     */
    Slice retain(size_t slot) const {
        RowGuard guard(gates_[slot]);
        return payloads()[slot].slice();
    }
    /** ------------------------------------------------------------------------------------------- Row Access
     * @brief Borrows a row during quiescent merge, drain, and validation operations.
     */
    Row row(size_t slot) { return Row{ids()[slot], payloads() + slot}; }
    /** ------------------------------------------------------------------------------------------- Score Access
     * @brief Reads the score while excluding a simultaneous update.
     */
    float score(size_t slot) const {
        RowGuard guard(gates_[slot]);
        return scores()[slot];
    }
    /** ------------------------------------------------------------------------------------------- Score Write
     * @brief Changes a score while excluding a simultaneous payload update.
     */
    void set_score(size_t slot, float value) {
        RowGuard guard(gates_[slot]);
        scores()[slot] = value;
    }
    /** ------------------------------------------------------------------------------------------- Published Check
     * @brief Checks dense row occupancy while all workers are quiescent.
     */
    bool published(size_t slot) const { return slot < size(); }
    size_t capacity() const noexcept { return capacity_; }
    const ID* id_data() const { return ids(); }
    /** ------------------------------------------------------------------------------------------- Kernel View
     * @brief Borrows the maintained pointer array while every writer and drain is quiescent.
     */
    template<typename P>
    P* const* data() const {
        return reinterpret_cast<P* const*>(raws_.data<void*>());
    }
};
using buffetalligator::Slice;
constexpr size_t PAYLOAD_BYTES = 64u;      ///< Payload claim per row; identical on all sides.
size_t LOOKUPS = 65536u;                   ///< Lookups per measurement, configurable at startup.
constexpr size_t VIEW_REPS = 65536u;       ///< View extractions per measurement.
constexpr size_t DUP_RATE = 4u;            ///< One in every `DUP_RATE` duplicate-bench writes replays.
/** --------------------------------------------------------------------------------------------------------- Bench Hazard Record
 * @brief One claimed announcement slot on its own cache line.
 */
struct alignas(128) BenchHazardRecord {
    std::atomic<bool> claimed{false};
    std::atomic<void*> pointer{nullptr};
};
/** --------------------------------------------------------------------------------------------------------- Bench Hazards
 * @brief Shared hazard domain for the lock-free contenders with reclaimed records and SIMD scans.
 */
class BenchHazards {
private:
    inline static std::array<BenchHazardRecord, 256> records_{};
    inline static std::atomic<size_t> high_water_{0};
public:
    /** ------------------------------------------------------------------------------------------- Guard
     * @brief Claims a reusable record for this thread and clears it on exit.
     */
    class Guard {
    private:
        std::atomic<void*>* slot_ = nullptr;
        std::atomic<bool>* claimed_ = nullptr;
    public:
        Guard() {
            for (auto& record : records_) {
                bool available = false;
                if (!record.claimed.compare_exchange_strong(available, true,
                    std::memory_order_acquire, std::memory_order_relaxed)) continue;
                slot_ = &record.pointer;
                claimed_ = &record.claimed;
                size_t index = static_cast<size_t>(&record - records_.data());
                size_t previous = high_water_.load(std::memory_order_seq_cst);
                while (previous <= index && !high_water_.compare_exchange_weak(
                    previous, index + 1, std::memory_order_seq_cst)) {}
                return;
            }
            throw std::length_error("Bench hazard domain exhausted 256 concurrent threads");
        }
        ~Guard() {
            slot_->store(nullptr, std::memory_order_seq_cst);
            claimed_->store(false, std::memory_order_release);
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        /** ----------------------------------------------------------------------------------- Protect
         * @brief Publishes then revalidates so retirement cannot miss this reader.
         */
        template<typename T>
        T* protect(const std::atomic<T*>& source) {
            T* pointer;
            do {
                pointer = source.load(std::memory_order_seq_cst);
                slot_->store(pointer, std::memory_order_seq_cst);
            } while (pointer != source.load(std::memory_order_seq_cst));
            return pointer;
        }
        void clear() { slot_->store(nullptr, std::memory_order_seq_cst); }
    };
    /** ------------------------------------------------------------------------------------------- Active
     * @brief Reports whether any thread currently announces the candidate pointer.
     */
    static bool active(const void* candidate) {
        const size_t records = high_water_.load(std::memory_order_seq_cst);
        for (size_t index = 0; index < records; ++index) {
            if (records_[index].pointer.load(std::memory_order_seq_cst) == candidate) return true;
        }
        return false;
    }
};
/** --------------------------------------------------------------------------------------------------------- Bench Guard
 * @brief One claimed hazard record per thread, shared by the lock-free contenders.
 */
BenchHazards::Guard& bench_guard() {
    thread_local BenchHazards::Guard claimed;
    return claimed;
}
/** --------------------------------------------------------------------------------------------------------- Bench Relax
 * @brief Issues the architecture pause hint for bounded contention waits.
 */
inline void bench_relax() {
#if defined(__aarch64__)
    __builtin_arm_yield();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}
/** --------------------------------------------------------------------------------------------------------- Position Ledger
 * @brief Shared stable-position, contiguous-watermark, wait, and publish-hook machinery.
 */
template<typename Node>
class PositionLedger {
public:
    /** ----------------------------------------------------------------------------------------------- Segment
     * @brief Reserves stable position cells without relocating published cells.
     */
    struct Segment {
        std::unique_ptr<std::atomic<Node*>[]> cells;
        explicit Segment(size_t count) : cells(new std::atomic<Node*>[count]{}) {}
    };
    /** ----------------------------------------------------------------------------------------------- View
     * @brief Publishes immutable pointer metadata between quiescent mutations.
     */
    struct View {
        Slice pointers;
        size_t revision;
        size_t count;
        View* pending_next;
        View(size_t capacity, size_t version, size_t rows)
        : pointers(capacity * sizeof(void*)), revision(version), count(rows),
          pending_next(nullptr) {}
    };
    using Hook = void (*)(void*, void*, const size_t&);
    static constexpr size_t kPending = SIZE_MAX;
    std::array<std::atomic<Segment*>, std::numeric_limits<size_t>::digits> segments{};
    std::atomic<size_t> reserved{0};
    alignas(128) std::atomic<size_t> positions{0};
    alignas(128) std::atomic<size_t> completed{0};
    std::atomic<size_t> waiters{0};
    std::atomic<size_t> revision{0};
    std::atomic<View*> cached_view{nullptr};
    View* pending_views{nullptr};
    size_t expected{0};
    void* publish_context = nullptr;
    Hook publish_hook = nullptr;
    /** ----------------------------------------------------------------------------------------------- Cell
     * @brief Installs the exponentially sized segment a new stable position needs.
     */
    std::atomic<Node*>* cell(size_t index) {
        const size_t segment_index = std::bit_width(index + 1) - 1;
        const size_t segment_size = size_t(1) << segment_index;
        Segment* segment = segments[segment_index].load(std::memory_order_acquire);
        if (!segment) {
            auto candidate = std::make_unique<Segment>(segment_size);
            if (segments[segment_index].compare_exchange_strong(segment, candidate.get(),
                std::memory_order_release, std::memory_order_acquire)) segment = candidate.release();
        }
        size_t capacity = reserved.load(std::memory_order_relaxed);
        const size_t next_capacity = segment_size + (segment_size - 1);
        while (capacity <= index && !reserved.compare_exchange_weak(capacity, next_capacity,
            std::memory_order_release, std::memory_order_relaxed)) {}
        return &segment->cells[index - (segment_size - 1)];
    }
    /** ----------------------------------------------------------------------------------------------- Entry At
     * @brief Reads a position cell that stays null until its row finishes publication.
     */
    Node* entry_at(size_t index) const {
        const size_t segment_index = std::bit_width(index + 1) - 1;
        Segment* segment = segments[segment_index].load(std::memory_order_acquire);
        return segment ? segment->cells[index - ((size_t(1) << segment_index) - 1)]
            .load(std::memory_order_acquire) : nullptr;
    }
    /** ----------------------------------------------------------------------------------------------- Advance
     * @brief Extends the contiguous hook-completed watermark and releases parked waiters.
     */
    void advance() {
        size_t seen = completed.load(std::memory_order_seq_cst);
        for (;;) {
            size_t reached = seen;
            while (reached < positions.load(std::memory_order_acquire)) {
                Node* node = entry_at(reached);
                if (!node || !node->published.load(std::memory_order_seq_cst)) break;
                ++reached;
            }
            if (reached == seen) return;
            if (completed.compare_exchange_weak(seen, reached,
                std::memory_order_seq_cst, std::memory_order_seq_cst)) {
                if (waiters.load(std::memory_order_seq_cst)) completed.notify_all();
                seen = reached;
                continue;
            }
        }
    }
    /** ----------------------------------------------------------------------------------------------- Complete
     * @brief Fires the publish hook, marks the row complete, and advances from the boundary.
     */
    void complete(Node* node, size_t index, bool inserted, void* owner) {
        if (publish_hook) publish_hook(owner, publish_context, index);
        if (!inserted) {
            revision.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        node->published.store(true, std::memory_order_seq_cst);
        if (index == completed.load(std::memory_order_seq_cst)) advance();
    }
    /** ----------------------------------------------------------------------------------------------- Wait
     * @brief Parks until count distinct rows complete publication including their hooks.
     */
    void wait_until_full(size_t count) {
        size_t current = completed.load(std::memory_order_acquire);
        if (current >= count) return;
        waiters.fetch_add(1, std::memory_order_seq_cst);
        current = completed.load(std::memory_order_seq_cst);
        while (current < count) {
            completed.wait(current, std::memory_order_acquire);
            current = completed.load(std::memory_order_seq_cst);
        }
        waiters.fetch_sub(1, std::memory_order_seq_cst);
    }
    /** ----------------------------------------------------------------------------------------------- Publish
     * @brief Assigns one dense position and stores its cell after a successful link.
     */
    size_t publish(Node* node) {
        const size_t position = positions.fetch_add(1, std::memory_order_relaxed);
        node->position.store(position, std::memory_order_release);
        cell(position)->store(node, std::memory_order_release);
        return position;
    }
    /** ----------------------------------------------------------------------------------------------- Reset
     * @brief Clears position state while all workers are quiescent.
     */
    void reset() {
        for (auto& segment : segments) delete segment.exchange(nullptr, std::memory_order_relaxed);
        reserved.store(0, std::memory_order_relaxed);
        positions.store(0, std::memory_order_relaxed);
        completed.store(0, std::memory_order_relaxed);
        revision.fetch_add(1, std::memory_order_relaxed);
        retire_view(cached_view.exchange(nullptr, std::memory_order_relaxed));
        collect_pending_views();
    }
    /** ----------------------------------------------------------------------------------------------- Retire View
     * @brief Frees a superseded view immediately unless a reader still announces it.
     */
    void retire_view(View* view) {
        if (!view) return;
        if (!BenchHazards::active(view)) {
            delete view;
            return;
        }
        view->pending_next = pending_views;
        pending_views = view;
    }
    /** ----------------------------------------------------------------------------------------------- Collect Views
     * @brief Retries views that were protected at their retirement time.
     */
    void collect_pending_views() {
        View* pending = pending_views;
        pending_views = nullptr;
        while (pending != nullptr) {
            View* next = pending->pending_next;
            retire_view(pending);
            pending = next;
        }
    }
    /** ----------------------------------------------------------------------------------------------- Destructor
     * @brief Releases position cells and cached views after all users stop.
     */
    ~PositionLedger() {
        for (auto& segment : segments) delete segment.load(std::memory_order_relaxed);
        delete cached_view.load(std::memory_order_relaxed);
        while (pending_views != nullptr) {
            View* next = pending_views->pending_next;
            delete pending_views;
            pending_views = next;
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- HazardMap
 * @class HazardMap
 * @brief Full-contract lock-free map: validated unique-key chain inserts with hazard-protected
 * bounded payload reclamation and the shared stable-position ledger.
 */
class HazardMap {
public:
    /** ------------------------------------------------------------------------------------------- Payload
     * @struct Payload
     * @brief Indirect claim so replacement exchanges one pointer while readers retain the old one.
     */
    struct Payload {
        Slice slice;
    };
    /** ------------------------------------------------------------------------------------------- Node
     * @struct Node
     * @brief One keyed row chained into its bucket; nodes live until reset or destruction.
     */
    struct Node {
        const uint64_t hashed;             ///< Fibonacci hash of the identifier; order within chain.
        std::atomic<Node*> next{nullptr};  ///< Next node in the bucket chain.
        std::atomic<Payload*> current;     ///< Replaceable payload; hazard-protected on read.
        std::atomic<size_t> position{PositionLedger<Node>::kPending};
        std::atomic<bool> published{false};
        Node(uint64_t hash, Payload* payload) : hashed(hash), current(payload) {}
        ~Node() { delete current.load(std::memory_order_relaxed); }
    };
    using Hook = PositionLedger<Node>::Hook;
private:
    static constexpr uint64_t kHashInverse = UINT64_C(17428512612931826493);
    struct Retired {
        Payload* payload;
        Retired* next;
    };
    static constexpr size_t RETIRE_BATCH = 64u;
    std::unique_ptr<std::atomic<Node*>[]> buckets_;
    size_t mask_;
    std::atomic<Retired*> retired_{nullptr};
    std::atomic<size_t> retire_count_{0};
    PositionLedger<Node> ledger_;
    /** ------------------------------------------------------------------------------------------- Bucket Of
     * @brief Fibonacci-hash an identifier onto its bucket.
     */
    size_t bucket_of(uint64_t hashed) const {
        return static_cast<size_t>(hashed >> 40) & mask_;
    }
    /** ------------------------------------------------------------------------------------------- Retire
     * @brief Batches a replaced payload and collects once the threshold is crossed.
     */
    void retire(Payload* payload) {
        Retired* node = new Retired{payload, nullptr};
        node->next = retired_.load(std::memory_order_relaxed);
        while (!retired_.compare_exchange_weak(node->next, node,
            std::memory_order_release, std::memory_order_relaxed)) {}
        if ((retire_count_.fetch_add(1, std::memory_order_acq_rel) + 1) % RETIRE_BATCH == 0) {
            collect();
        }
    }
    /** ------------------------------------------------------------------------------------------- Collect
     * @brief Frees retired payloads no reader announces and requeues the protected ones.
     */
    void collect() {
        Retired* popped = retired_.exchange(nullptr, std::memory_order_acquire);
        Retired* protected_head = nullptr;
        while (popped != nullptr) {
            Retired* next = popped->next;
            if (BenchHazards::active(popped->payload)) {
                popped->next = protected_head;
                protected_head = popped;
            } else {
                delete popped->payload;
                delete popped;
            }
            popped = next;
        }
        while (protected_head != nullptr) {
            Retired* next = protected_head->next;
            protected_head->next = retired_.load(std::memory_order_relaxed);
            while (!retired_.compare_exchange_weak(protected_head->next, protected_head,
                std::memory_order_release, std::memory_order_relaxed)) {}
            protected_head = next;
        }
    }
public:
    struct Insertion { Node* node; bool inserted; };
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Sizes the bucket array to roughly one row per bucket, rounded to a power of two.
     */
    explicit HazardMap(size_t capacity) {
        size_t buckets = 64u;
        while (buckets < capacity) buckets <<= 1u;
        mask_ = buckets - 1u;
        buckets_ = std::make_unique<std::atomic<Node*>[]>(buckets);
    }
    HazardMap(const HazardMap&) = delete;
    HazardMap& operator=(const HazardMap&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Frees every node and drains retirement after readers have stopped.
     */
    ~HazardMap() {
        clear_core();
        Retired* popped = retired_.exchange(nullptr, std::memory_order_acquire);
        while (popped != nullptr) {
            Retired* next = popped->next;
            delete popped->payload;
            delete popped;
            popped = next;
        }
    }
    /** ------------------------------------------------------------------------------------------- Upsert
     * @brief Links one distinct node or exchanges an existing node's payload.
     */
    Insertion upsert(int64_t identifier, Slice&& payload) {
        const uint64_t hashed = static_cast<uint64_t>(identifier) * 11400714819323198485ull;
        auto owned = std::make_unique<Payload>(Payload{std::move(payload)});
        std::unique_ptr<Node> candidate;
        for (;;) {
            Node* head = buckets_[bucket_of(hashed)].load(std::memory_order_acquire);
            Node* node = head;
            while (node != nullptr && node->hashed != hashed) {
                node = node->next.load(std::memory_order_acquire);
            }
            if (node != nullptr) {
                Payload* previous = node->current.exchange(owned.release(),
                    std::memory_order_seq_cst);
                if (previous) retire(previous);
                return {node, false};
            }
            if (!candidate) candidate = std::make_unique<Node>(hashed, nullptr);
            candidate->next.store(head, std::memory_order_relaxed);
            candidate->current.store(owned.get(), std::memory_order_relaxed);
            if (buckets_[bucket_of(hashed)].compare_exchange_weak(head, candidate.get(),
                std::memory_order_acq_rel, std::memory_order_acquire)) {
                owned.release();
                return {candidate.release(), true};
            }
            candidate->current.store(nullptr, std::memory_order_relaxed);
        }
    }
    /** ------------------------------------------------------------------------------------------- Add Slice
     * @brief Publishes a unique key or replaces its payload at the existing stable position.
     */
    void add_slice(int64_t identifier, Slice payload) {
        const Insertion result = upsert(identifier, std::move(payload));
        if (result.inserted) {
            const size_t position = ledger_.publish(result.node);
            ledger_.complete(result.node, position, true, this);
            return;
        }
        size_t position = result.node->position.load(std::memory_order_acquire);
        while (position == PositionLedger<Node>::kPending) {
            bench_relax();
            position = result.node->position.load(std::memory_order_acquire);
        }
        ledger_.complete(result.node, position, false, this);
    }
    /** ------------------------------------------------------------------------------------------- Find
     * @brief Returns the key's stable position or -1 when absent or still publishing.
     */
    int64_t find(int64_t identifier) const {
        const uint64_t hashed = static_cast<uint64_t>(identifier) * 11400714819323198485ull;
        Node* node = buckets_[bucket_of(hashed)].load(std::memory_order_acquire);
        while (node != nullptr && node->hashed != hashed) {
            node = node->next.load(std::memory_order_acquire);
        }
        if (node == nullptr) return -1;
        const size_t position = node->position.load(std::memory_order_acquire);
        return position == PositionLedger<Node>::kPending
            ? -1 : static_cast<int64_t>(position);
    }
    /** ------------------------------------------------------------------------------------------- Get Slice
     * @brief Retains the unique row's payload behind a hazard announcement.
     */
    Slice get_slice(int64_t identifier) const {
        const uint64_t hashed = static_cast<uint64_t>(identifier) * 11400714819323198485ull;
        Node* node = buckets_[bucket_of(hashed)].load(std::memory_order_acquire);
        while (node != nullptr && node->hashed != hashed) {
            node = node->next.load(std::memory_order_acquire);
        }
        return node == nullptr ? Slice() : retain(node);
    }
    /** ------------------------------------------------------------------------------------------- Retain
     * @brief Copies the node's payload claim inside a hazard window.
     */
    Slice retain(Node* node) const {
        Payload* payload = bench_guard().protect(node->current);
        Slice retained = payload->slice;
        bench_guard().clear();
        return retained;
    }
    /** ------------------------------------------------------------------------------------------- Raw Of
     * @brief Borrows the payload pointer while writers and reset are quiescent.
     */
    void* raw_of(Node* node) const {
        return node->current.load(std::memory_order_relaxed)->slice.data<void>();
    }
    /** ------------------------------------------------------------------------------------------- ID Of
     * @brief Recovers the identifier through the modular inverse of the hash constant.
     */
    int64_t id_of(Node* node) const {
        return static_cast<int64_t>(node->hashed * kHashInverse);
    }
    /** ------------------------------------------------------------------------------------------- Steal
     * @brief Moves a quiescent node's payload out for merge transfer.
     */
    Slice steal(Node* node) {
        Payload* payload = node->current.exchange(nullptr, std::memory_order_relaxed);
        Slice moved = std::move(payload->slice);
        delete payload;
        return moved;
    }
    /** ------------------------------------------------------------------------------------------- Clear Core
     * @brief Deletes every node and drains retirement while all workers are quiescent.
     */
    void clear_core() {
        for (size_t bucket = 0; bucket <= mask_; ++bucket) {
            Node* node = buckets_[bucket].exchange(nullptr, std::memory_order_acq_rel);
            while (node != nullptr) {
                Node* next = node->next.load(std::memory_order_relaxed);
                delete node;
                node = next;
            }
        }
        collect();
        Retired* popped = retired_.exchange(nullptr, std::memory_order_acquire);
        while (popped != nullptr) {
            Retired* next = popped->next;
            delete popped->payload;
            delete popped;
            popped = next;
        }
    }
    /** ------------------------------------------------------------------------------------------- Slice At
     * @brief Retains a published position's payload during concurrent replacement.
     */
    Slice slice_at(size_t index) const {
        Node* node = ledger_.entry_at(index);
        return node == nullptr ? Slice() : retain(node);
    }
    /** ------------------------------------------------------------------------------------------- Payload At
     * @brief Borrows a quiescent position's payload without creating another claim.
     */
    void* payload_at(size_t index) const {
        Node* node = ledger_.entry_at(index);
        return node == nullptr ? nullptr : raw_of(node);
    }
    /** ------------------------------------------------------------------------------------------- Identifier At
     * @brief Reads a stable position's immutable identifier while reset is quiescent.
     */
    int64_t identifier_at(size_t index) const {
        Node* node = ledger_.entry_at(index);
        return node == nullptr ? -1 : id_of(node);
    }
    /** ------------------------------------------------------------------------------------------- Published
     * @brief Tests a position cell that stays null until its row finishes publication.
     */
    bool published(const size_t& slot) const { return ledger_.entry_at(slot) != nullptr; }
    size_t size() const { return ledger_.completed.load(std::memory_order_acquire); }
    size_t capacity() const noexcept {
        return ledger_.reserved.load(std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Data
     * @brief Returns the cached contiguous pointer view, rebuilding it after mutations.
     */
    template<typename P = void>
    P** data() {
        ledger_.collect_pending_views();
        const size_t revision = ledger_.revision.load(std::memory_order_relaxed);
        const size_t count = ledger_.completed.load(std::memory_order_relaxed);
        using View = PositionLedger<Node>::View;
        View* current = ledger_.cached_view.load(std::memory_order_acquire);
        if (current && revision == current->revision && count == current->count) {
            return current->pointers.template data<P*>();
        }
        auto replacement = std::make_unique<View>(
            ledger_.reserved.load(std::memory_order_relaxed), revision, count);
        void** pointers = replacement->pointers.template data<void*>();
        for (size_t index = 0; index < count; ++index) {
            pointers[index] = raw_of(ledger_.entry_at(index));
        }
        const size_t capacity = ledger_.reserved.load(std::memory_order_relaxed);
        for (size_t index = count; index < capacity; ++index) pointers[index] = nullptr;
        View* expected = current;
        if (ledger_.cached_view.compare_exchange_strong(expected, replacement.get(),
            std::memory_order_seq_cst)) {
            ledger_.retire_view(expected);
            return replacement.release()->pointers.template data<P*>();
        }
        return expected->pointers.template data<P*>();
    }
    /** ------------------------------------------------------------------------------------------- Wait
     * @brief Parks until count distinct rows complete publication including their hooks.
     */
    void wait_until_full(size_t count) { ledger_.wait_until_full(count); }
    void expect(const size_t& count) { ledger_.expected = count; }
    void wait() { ledger_.wait_until_full(ledger_.expected); }
    /** ------------------------------------------------------------------------------------------- Merge
     * @brief Moves rows in source order into this map, then clears the source.
     */
    void merge(HazardMap& other) {
        const size_t count = other.ledger_.completed.load(std::memory_order_acquire);
        for (size_t index = 0; index < count; ++index) {
            Node* node = other.ledger_.entry_at(index);
            add_slice(other.id_of(node), other.steal(node));
        }
        other.reset();
    }
    /** ------------------------------------------------------------------------------------------- Reset
     * @brief Clears every row and restarts dense positions while all workers are quiescent.
     */
    void reset() {
        clear_core();
        ledger_.reset();
    }
    void on_publish(Hook hook, void* context) {
        ledger_.publish_hook = hook;
        ledger_.publish_context = context;
    }
    static void gc() {}
};
/** --------------------------------------------------------------------------------------------------------- MutexMap
 * @class MutexMap
 * @brief The full-contract control: dense rows and a keyed index under one mutex, with the same
 * stable positions, contiguous pointer view, wait semantics, and publish hooks as SliceMap.
 */
class MutexMap {
private:
    using Hook = void (*)(void*, void*, const size_t&);
    /** ------------------------------------------------------------------------------------------- Row
     * @struct Row
     * @brief One keyed row at its stable dense position.
     */
    struct Row {
        int64_t identifier;
        Slice payload;
    };
    std::vector<Row> rows_;
    std::unordered_map<int64_t, size_t> index_;
    std::vector<void*> view_;
    size_t view_revision_ = SIZE_MAX;
    size_t revision_ = 0;
    size_t expected_ = 0;
    void* publish_context_ = nullptr;
    Hook publish_hook_ = nullptr;
    mutable std::mutex mutex_;
    std::condition_variable published_cv_;
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Reserves for the expected row count; positions are dense vector indices.
     */
    explicit MutexMap(size_t capacity) {
        rows_.reserve(capacity);
        index_.reserve(capacity);
    }
    MutexMap(const MutexMap&) = delete;
    MutexMap& operator=(const MutexMap&) = delete;
    /** ------------------------------------------------------------------------------------------- Add Slice
     * @brief Publishes a unique key or replaces its payload at the existing stable position.
     */
    void add_slice(int64_t identifier, Slice payload) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = index_.find(identifier);
        if (found != index_.end()) {
            rows_[found->second].payload = std::move(payload);
            ++revision_;
            if (publish_hook_) publish_hook_(this, publish_context_, found->second);
            return;
        }
        index_.emplace(identifier, rows_.size());
        rows_.push_back(Row{identifier, std::move(payload)});
        if (publish_hook_) publish_hook_(this, publish_context_, rows_.size() - 1);
        published_cv_.notify_all();
    }
    /** ------------------------------------------------------------------------------------------- Find
     * @brief Returns the key's stable position or -1 when absent.
     */
    int64_t find(int64_t identifier) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = index_.find(identifier);
        return found == index_.end() ? -1 : static_cast<int64_t>(found->second);
    }
    /** ------------------------------------------------------------------------------------------- Get Slice
     * @brief Retains the row's payload under the gate.
     */
    Slice get_slice(int64_t identifier) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = index_.find(identifier);
        return found == index_.end() ? Slice() : rows_[found->second].payload.slice();
    }
    /** ------------------------------------------------------------------------------------------- Slice At
     * @brief Retains a published position's payload under the gate.
     */
    Slice slice_at(size_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return index < rows_.size() ? rows_[index].payload.slice() : Slice();
    }
    /** ------------------------------------------------------------------------------------------- Payload At
     * @brief Borrows a quiescent position's payload without creating another claim.
     */
    void* payload_at(size_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return index < rows_.size()
            ? const_cast<void*>(rows_[index].payload.data<void>()) : nullptr;
    }
    /** ------------------------------------------------------------------------------------------- Identifier At
     * @brief Reads a stable position's immutable identifier under the gate.
     */
    int64_t identifier_at(size_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return index < rows_.size() ? rows_[index].identifier : -1;
    }
    /** ------------------------------------------------------------------------------------------- Published
     * @brief Tests dense row occupancy under the gate.
     */
    bool published(const size_t& slot) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return slot < rows_.size();
    }
    /** ------------------------------------------------------------------------------------------- Size
     * @brief Counts distinct published rows under the gate.
     */
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return rows_.size();
    }
    /** ------------------------------------------------------------------------------------------- Capacity
     * @brief Reports the reserved row storage.
     */
    size_t capacity() const noexcept { return rows_.capacity(); }
    /** ------------------------------------------------------------------------------------------- Data
     * @brief Returns the contiguous pointer view, rebuilding it after mutations.
     */
    template<typename P = void>
    P** data() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (view_revision_ != revision_) {
            view_.clear();
            for (const Row& row : rows_) {
                view_.push_back(const_cast<void*>(row.payload.data<void>()));
            }
            view_revision_ = revision_;
        }
        return view_.data();
    }
    /** ------------------------------------------------------------------------------------------- Wait
     * @brief Parks until count distinct rows finish publication including their hooks.
     */
    void wait_until_full(size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        published_cv_.wait(lock, [this, count] { return rows_.size() >= count; });
    }
    void expect(const size_t& count) {
        std::lock_guard<std::mutex> lock(mutex_);
        expected_ = count;
    }
    void wait() { wait_until_full(expected_); }
    /** ------------------------------------------------------------------------------------------- Merge
     * @brief Moves rows in source order into this map, then clears the source.
     */
    void merge(MutexMap& other) {
        std::vector<Row> moved;
        {
            std::lock_guard<std::mutex> lock(other.mutex_);
            moved = std::move(other.rows_);
            other.index_.clear();
            other.rows_.clear();
            ++other.revision_;
            other.view_revision_ = SIZE_MAX;
        }
        for (Row& row : moved) add_slice(row.identifier, std::move(row.payload));
    }
    /** ------------------------------------------------------------------------------------------- Reset
     * @brief Clears every row and restarts dense positions under the gate.
     */
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        rows_.clear();
        index_.clear();
        ++revision_;
        view_revision_ = SIZE_MAX;
    }
    void on_publish(Hook hook, void* context) {
        std::lock_guard<std::mutex> lock(mutex_);
        publish_hook_ = hook;
        publish_context_ = context;
    }
    static void gc() {}
};
/** --------------------------------------------------------------------------------------------------------- VersionMap
 * @class VersionMap
 * @brief Full-contract lock-free map: head-insert unique keys with atomic version replacement and
 * bounded hazard-protected reclamation, sharing the stable-position ledger.
 */
class VersionMap {
public:
    /** ------------------------------------------------------------------------------------------- Version
     * @struct Version
     * @brief One immutable payload claim; replaced versions retire in bounded batches.
     */
    struct Version {
        Slice slice;
        Version* retired_next;
    };
    /** ------------------------------------------------------------------------------------------- Node
     * @struct Node
     * @brief One keyed row chained into its bucket; nodes live until reset or destruction.
     */
    struct Node {
        const int64_t identifier;
        std::atomic<Version*> current;
        Node* next;
        std::atomic<size_t> position{PositionLedger<Node>::kPending};
        std::atomic<bool> published{false};
        Node(int64_t id, Version* version, Node* link)
        : identifier(id), current(version), next(link) {}
    };
    using Hook = PositionLedger<Node>::Hook;
private:
    static constexpr size_t RETIRE_BATCH = 64u;
    std::unique_ptr<std::atomic<Node*>[]> buckets_;
    size_t bucket_count_ = 64;
    size_t shift_ = 58;
    std::atomic<Version*> retired_{nullptr};
    std::atomic<size_t> retire_count_{0};
    std::atomic<size_t> released_{0};
    PositionLedger<Node> ledger_;
    /** ------------------------------------------------------------------------------------------- Bucket Of
     * @brief Fibonacci-hash an identifier onto its bucket.
     */
    size_t bucket_of(int64_t identifier) const {
        return static_cast<uint64_t>(identifier) * 11400714819323198485ull >> shift_;
    }
    /** ------------------------------------------------------------------------------------------- Retire
     * @brief Batches one replaced version and collects once the threshold is crossed.
     */
    void retire(Version* version) {
        version->retired_next = retired_.load(std::memory_order_relaxed);
        while (!retired_.compare_exchange_weak(version->retired_next, version,
            std::memory_order_release, std::memory_order_relaxed)) {}
        if ((retire_count_.fetch_add(1, std::memory_order_acq_rel) + 1) % RETIRE_BATCH == 0) {
            collect_retired();
        }
    }
    /** ------------------------------------------------------------------------------------------- Collect Retired
     * @brief Frees unannounced versions immediately and requeues the protected ones.
     */
    void collect_retired() {
        Version* popped = retired_.exchange(nullptr, std::memory_order_acquire);
        Version* protected_head = nullptr;
        while (popped != nullptr) {
            Version* next = popped->retired_next;
            if (BenchHazards::active(popped)) {
                popped->retired_next = protected_head;
                protected_head = popped;
            } else {
                released_.fetch_add(1, std::memory_order_relaxed);
                delete popped;
            }
            popped = next;
        }
        while (protected_head != nullptr) {
            Version* next = protected_head->retired_next;
            protected_head->retired_next = retired_.load(std::memory_order_relaxed);
            while (!retired_.compare_exchange_weak(protected_head->retired_next, protected_head,
                std::memory_order_release, std::memory_order_relaxed)) {}
            protected_head = next;
        }
    }
public:
    struct Insertion { Node* node; bool inserted; };
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Sizes the bucket array to roughly one row per bucket, rounded to a power of two.
     */
    explicit VersionMap(size_t hint) {
        while (bucket_count_ < hint) bucket_count_ <<= 1;
        shift_ = 64 - static_cast<size_t>(std::countr_zero(bucket_count_));
        buckets_ = std::make_unique<std::atomic<Node*>[]>(bucket_count_);
    }
    VersionMap(const VersionMap&) = delete;
    VersionMap& operator=(const VersionMap&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Frees every node and drains retirement after readers have stopped.
     */
    ~VersionMap() {
        clear_core();
        Version* popped = retired_.exchange(nullptr, std::memory_order_acquire);
        while (popped != nullptr) {
            Version* next = popped->retired_next;
            released_.fetch_add(1, std::memory_order_relaxed);
            delete popped;
            popped = next;
        }
    }
    /** ------------------------------------------------------------------------------------------- Released
     * @brief Counts versions freed by bounded reclamation so leak checks stay exact.
     */
    size_t released() const { return released_.load(std::memory_order_acquire); }
    /** ------------------------------------------------------------------------------------------- Collect
     * @brief Collects replaced versions and returns how many this call freed.
     */
    size_t collect() {
        const size_t before = released_.load(std::memory_order_relaxed);
        collect_retired();
        return released_.load(std::memory_order_relaxed) - before;
    }
    /** ------------------------------------------------------------------------------------------- Upsert
     * @brief Links one distinct node or atomically replaces its current version.
     */
    Insertion upsert(int64_t identifier, Slice&& payload) {
        auto version = std::make_unique<Version>(Version{std::move(payload), nullptr});
        std::unique_ptr<Node> pending;
        auto& bucket = buckets_[bucket_of(identifier)];
        Node* head = bucket.load(std::memory_order_acquire);
        for (;;) {
            for (Node* node = head; node != nullptr; node = node->next) {
                if (node->identifier != identifier) continue;
                Version* previous = node->current.load(std::memory_order_acquire);
                while (!node->current.compare_exchange_weak(previous, version.get(),
                    std::memory_order_acq_rel, std::memory_order_acquire)) {}
                version.release();
                retire(previous);
                return {node, false};
            }
            if (!pending) pending.reset(new Node{identifier, version.get(), head});
            pending->next = head;
            if (bucket.compare_exchange_weak(head, pending.get(),
                std::memory_order_acq_rel, std::memory_order_acquire)) {
                Node* linked = pending.release();
                version.release();
                return {linked, true};
            }
        }
    }
    /** ------------------------------------------------------------------------------------------- Add Slice
     * @brief Publishes a unique key or replaces its payload at the existing stable position.
     */
    void add_slice(int64_t identifier, Slice payload) {
        const Insertion result = upsert(identifier, std::move(payload));
        if (result.inserted) {
            const size_t position = ledger_.publish(result.node);
            ledger_.complete(result.node, position, true, this);
            return;
        }
        size_t position = result.node->position.load(std::memory_order_acquire);
        while (position == PositionLedger<Node>::kPending) {
            bench_relax();
            position = result.node->position.load(std::memory_order_acquire);
        }
        ledger_.complete(result.node, position, false, this);
    }
    /** ------------------------------------------------------------------------------------------- Find
     * @brief Returns the key's stable position or -1 when absent or still publishing.
     */
    int64_t find(int64_t identifier) const {
        Node* node = buckets_[bucket_of(identifier)].load(std::memory_order_acquire);
        for (; node != nullptr; node = node->next) {
            if (node->identifier != identifier) continue;
            const size_t position = node->position.load(std::memory_order_acquire);
            return position == PositionLedger<Node>::kPending
                ? -1 : static_cast<int64_t>(position);
        }
        return -1;
    }
    /** ------------------------------------------------------------------------------------------- Get Slice
     * @brief Retains the unique row's current payload behind a hazard announcement.
     */
    Slice get_slice(int64_t identifier) const {
        Node* node = buckets_[bucket_of(identifier)].load(std::memory_order_acquire);
        for (; node != nullptr; node = node->next) {
            if (node->identifier != identifier) continue;
            return retain(node);
        }
        return {};
    }
    /** ------------------------------------------------------------------------------------------- Retain
     * @brief Copies the current version's claim inside a hazard window.
     */
    Slice retain(Node* node) const {
        Version* version = bench_guard().protect(node->current);
        Slice retained = version->slice.slice();
        bench_guard().clear();
        return retained;
    }
    /** ------------------------------------------------------------------------------------------- Raw Of
     * @brief Borrows the payload pointer while writers and reset are quiescent.
     */
    void* raw_of(Node* node) const {
        return node->current.load(std::memory_order_relaxed)->slice.data<void>();
    }
    /** ------------------------------------------------------------------------------------------- ID Of
     * @brief Returns the node's immutable identifier.
     */
    int64_t id_of(Node* node) const { return node->identifier; }
    /** ------------------------------------------------------------------------------------------- Steal
     * @brief Moves a quiescent node's payload out for merge transfer.
     */
    Slice steal(Node* node) {
        Version* version = node->current.exchange(nullptr, std::memory_order_relaxed);
        Slice moved = std::move(version->slice);
        delete version;
        return moved;
    }
    /** ------------------------------------------------------------------------------------------- Clear Core
     * @brief Deletes every node and drains retirement while all workers are quiescent.
     */
    void clear_core() {
        for (size_t bucket = 0; bucket < bucket_count_; ++bucket) {
            Node* node = buckets_[bucket].load(std::memory_order_relaxed);
            while (node != nullptr) {
                Node* next = node->next;
                delete node->current.load(std::memory_order_relaxed);
                delete node;
                node = next;
            }
            buckets_[bucket].store(nullptr, std::memory_order_relaxed);
        }
        collect_retired();
        Version* popped = retired_.exchange(nullptr, std::memory_order_acquire);
        while (popped != nullptr) {
            Version* next = popped->retired_next;
            released_.fetch_add(1, std::memory_order_relaxed);
            delete popped;
            popped = next;
        }
    }
    /** ------------------------------------------------------------------------------------------- Slice At
     * @brief Retains a published position's payload during concurrent replacement.
     */
    Slice slice_at(size_t index) const {
        Node* node = ledger_.entry_at(index);
        return node == nullptr ? Slice() : retain(node);
    }
    /** ------------------------------------------------------------------------------------------- Payload At
     * @brief Borrows a quiescent position's payload without creating another claim.
     */
    void* payload_at(size_t index) const {
        Node* node = ledger_.entry_at(index);
        return node == nullptr ? nullptr : raw_of(node);
    }
    /** ------------------------------------------------------------------------------------------- Identifier At
     * @brief Reads a stable position's immutable identifier while reset is quiescent.
     */
    int64_t identifier_at(size_t index) const {
        Node* node = ledger_.entry_at(index);
        return node == nullptr ? -1 : id_of(node);
    }
    /** ------------------------------------------------------------------------------------------- Published
     * @brief Tests a position cell that stays null until its row finishes publication.
     */
    bool published(const size_t& slot) const { return ledger_.entry_at(slot) != nullptr; }
    size_t size() const { return ledger_.completed.load(std::memory_order_acquire); }
    size_t capacity() const noexcept {
        return ledger_.reserved.load(std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Data
     * @brief Returns the cached contiguous pointer view, rebuilding it after mutations.
     */
    template<typename P = void>
    P** data() {
        ledger_.collect_pending_views();
        const size_t revision = ledger_.revision.load(std::memory_order_relaxed);
        const size_t count = ledger_.completed.load(std::memory_order_relaxed);
        using View = PositionLedger<Node>::View;
        View* current = ledger_.cached_view.load(std::memory_order_acquire);
        if (current && revision == current->revision && count == current->count) {
            return current->pointers.template data<P*>();
        }
        auto replacement = std::make_unique<View>(
            ledger_.reserved.load(std::memory_order_relaxed), revision, count);
        void** pointers = replacement->pointers.template data<void*>();
        for (size_t index = 0; index < count; ++index) {
            pointers[index] = raw_of(ledger_.entry_at(index));
        }
        const size_t capacity = ledger_.reserved.load(std::memory_order_relaxed);
        for (size_t index = count; index < capacity; ++index) pointers[index] = nullptr;
        View* expected = current;
        if (ledger_.cached_view.compare_exchange_strong(expected, replacement.get(),
            std::memory_order_seq_cst)) {
            ledger_.retire_view(expected);
            return replacement.release()->pointers.template data<P*>();
        }
        return expected->pointers.template data<P*>();
    }
    /** ------------------------------------------------------------------------------------------- Wait
     * @brief Parks until count distinct rows complete publication including their hooks.
     */
    void wait_until_full(size_t count) { ledger_.wait_until_full(count); }
    void expect(const size_t& count) { ledger_.expected = count; }
    void wait() { ledger_.wait_until_full(ledger_.expected); }
    /** ------------------------------------------------------------------------------------------- Merge
     * @brief Moves rows in source order into this map, then clears the source.
     */
    void merge(VersionMap& other) {
        const size_t count = other.ledger_.completed.load(std::memory_order_acquire);
        for (size_t index = 0; index < count; ++index) {
            Node* node = other.ledger_.entry_at(index);
            add_slice(other.id_of(node), other.steal(node));
        }
        other.reset();
    }
    /** ------------------------------------------------------------------------------------------- Reset
     * @brief Clears every row and restarts dense positions while all workers are quiescent.
     */
    void reset() {
        clear_core();
        ledger_.reset();
    }
    void on_publish(Hook hook, void* context) {
        ledger_.publish_hook = hook;
        ledger_.publish_context = context;
    }
    static void gc() {}
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
        pool.back().get_as<uint64_t>() = i;
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
/** --------------------------------------------------------------------------------------------------------- Section
 * @struct Section
 * @brief Reports indexed and SIMD lookups separately while preserving the original competitors.
 */
struct ComparisonRow {
    std::string workload;
    std::string label;
    std::string unit;
    std::array<std::string, 6> values;
    std::string notes;
};
std::vector<ComparisonRow> results;
std::vector<ComparisonRow> summaries;
constexpr std::array<const char*, 6> candidate_names{
    "VecSet indexed", "SliceMap", "HazardMap", "unordered_map+mutex", "VecSet SIMD", "VersionMap"
};
/** --------------------------------------------------------------------------------------------------------- Section
 * @brief Collects compact candidate rows with separate workload metadata.
 */
struct Section {
    std::string title;
    std::string unit;
    std::vector<ComparisonRow> rows;
    Section(const std::string& section_title, const std::string& measurement_unit)
    : title(section_title), unit(measurement_unit) {}
    void add(
        const std::string& label, const std::string& vecset, const std::string& slicemap,
        const std::string& hazard, const std::string& mutex, const std::string& notes = "",
        const std::string& simd = "N/A", const std::string& version = "N/A"
    ) {
        rows.push_back({title.substr(0, title.find(" - ")), label, unit,
            {vecset, slicemap, hazard, mutex, simd, version}, notes});
    }
};
/** --------------------------------------------------------------------------------------------------------- Number
 * @brief Formats a measurement with two decimals.
 */
std::string num(double value) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(2) << value;
    return output.str();
}
/** --------------------------------------------------------------------------------------------------------- Print Section
 * @brief Prints readable candidate rows and saves the largest case for the final summary.
 */
void print_section(const Section& section) {
    LOG_INFO_STREAM << section.title;
    std::vector<std::vector<std::string>> columns{
        {"workload"}, {"candidate"}, {section.unit + " median [min,max]"}
    };
    for (const auto& row : section.rows) {
        for (size_t candidate = 0; candidate < row.values.size(); ++candidate) {
            columns[0].push_back(row.label);
            columns[1].push_back(candidate_names[candidate]);
            columns[2].push_back(row.values[candidate]);
        }
        results.push_back(row);
    }
    threadsafe_logger::logging::print_table(columns, {}, threadsafe_logger::logging::TTYCYAN);
    if (!section.rows.empty()) {
        LOG_INFO_STREAM << section.rows.back().notes;
        summaries.push_back(section.rows.back());
    }
}
/** --------------------------------------------------------------------------------------------------------- Print Summary
 * @brief Ranks the two lowest medians per section without equating append and replacement semantics.
 */
void print_summary(const std::string& csv_path) {
    std::vector<std::vector<std::string>> columns{
        {"largest case per section"}, {"rank"}, {"container"}, {"median"}
    };
    for (const auto& row : summaries) {
        std::vector<std::pair<double, size_t>> ranked;
        ranked.reserve(row.values.size());
        for (size_t candidate = 0; candidate < row.values.size(); ++candidate) {
            if (row.workload == "view") continue;
            if (row.workload == "duplicates" && candidate == 2) continue;
            if (row.values[candidate].empty() || row.values[candidate][0] < '0'
                || row.values[candidate][0] > '9') continue;
            ranked.emplace_back(std::stod(row.values[candidate]), candidate);
        }
        std::sort(ranked.begin(), ranked.end());
        if (ranked.empty()) {
            columns[0].push_back(row.workload + " " + row.label);
            columns[1].push_back("N/A");
            columns[2].push_back("different operations");
            columns[3].push_back("N/A");
        }
        for (size_t place = 0; place < std::min(size_t(2), ranked.size()); ++place) {
            const auto [elapsed, candidate] = ranked[place];
            columns[0].push_back(row.workload + " " + row.label);
            columns[1].push_back(place == 0 ? "1st" : "2nd");
            columns[2].push_back(candidate_names[candidate]);
            columns[3].push_back(num(elapsed) + " " + row.unit);
        }
    }
    LOG_INFO_STREAM << "FINAL SUMMARY: first and second by measured median, largest row count per section.";
    LOG_INFO_STREAM << "unordered_map+mutex = std::unordered_map<int64_t, Slice> protected by one mutex; "
        << "the benchmark adds no mutex around SliceMap.";
    threadsafe_logger::logging::print_table(columns, {}, threadsafe_logger::logging::TTYCYAN);
    LOG_INFO_STREAM << "Ranks apply to the measured workload; equal medians or overlapping ranges are inconclusive.";
    LOG_INFO_STREAM << "SliceMap, HazardMap, VersionMap, and MutexMap all provide the full "
        << "contract: unique keys, stable positions, cached pointer views, waits, hooks, and merge; "
        << "HazardMap and VersionMap reclaim through bounded hazard batches; VecSet remains a "
        << "fixed-capacity control with blocking row gates; no concurrent erase is tested.";
    if (!csv_path.empty()) {
        std::ofstream output(csv_path);
        output << "workload,shape,candidate,unit,median_min_max,notes\n";
        for (const auto& row : results) {
            for (size_t candidate = 0; candidate < row.values.size(); ++candidate) {
                output << std::quoted(row.workload) << ',' << std::quoted(row.label) << ','
                    << std::quoted(candidate_names[candidate]) << ',' << std::quoted(row.unit) << ','
                    << std::quoted(row.values[candidate]) << ',' << std::quoted(row.notes) << '\n';
            }
        }
        output.close();
        if (!output) throw std::runtime_error("Could not write comparison CSV: " + csv_path);
        LOG_INFO_STREAM << "All measured cases: " << csv_path;
    }
}
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Makes a failed contract invalidate the run even in optimized builds.
 */
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
/** --------------------------------------------------------------------------------------------------------- Samples
 * @brief Reports the median and range of measured samples without including warmups.
 */
struct Samples {
    std::vector<uint64_t> values;
    std::string format(double divisor) const {
        auto sorted = values;
        std::sort(sorted.begin(), sorted.end());
        const size_t middle = sorted.size() / 2;
        const double median = sorted.size() % 2 ? double(sorted[middle])
            : (double(sorted[middle - 1]) + double(sorted[middle])) / 2;
        return num(median / divisor) + " [" + num(sorted.front() / divisor)
            + "," + num(sorted.back() / divisor) + "]";
    }
};
/** --------------------------------------------------------------------------------------------------------- Progress
 * @brief Reports active work once per second and aborts a stalled case after its time limit.
 */
class Progress {
private:
    std::string label_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool done_ = false;
    std::thread worker_;
    static void monitor(Progress* progress) {
        std::unique_lock lock(progress->mutex_);
        const auto start = std::chrono::steady_clock::now();
        while (!progress->done_) {
            progress->changed_.wait_for(lock, std::chrono::seconds(1));
            if (progress->done_) return;
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count();
            LOG_INFO_STREAM << progress->label_ << ": working (" << elapsed << "s)...";
            if (elapsed >= 300) {
                LOG_ERROR_STREAM << "Case exceeded 300 seconds: " << progress->label_;
                std::abort();
            }
        }
    }
public:
    explicit Progress(std::string label)
    : label_(std::move(label)), worker_(&Progress::monitor, this) {
        LOG_INFO_STREAM << label_;
    }
    ~Progress() {
        {
            std::lock_guard lock(mutex_);
            done_ = true;
        }
        changed_.notify_one();
        worker_.join();
    }
};
/** --------------------------------------------------------------------------------------------------------- Worker Team
 * @brief Reuses workers and measures earliest entry through latest completion after a start barrier.
 */
class WorkerTeam {
private:
    struct alignas(128) Timing { uint64_t start = 0; uint64_t finish = 0; };
    std::barrier<> gate_;
    std::vector<Timing> timings_;
    std::vector<std::thread> workers_;
    void (*operation_)(void*, size_t) = nullptr;
    void* context_ = nullptr;
    static void worker(WorkerTeam* team, size_t index) {
        for (;;) {
            team->gate_.arrive_and_wait();
            if (team->operation_ == nullptr) return;
            team->timings_[index].start = now_ns();
            try {
                team->operation_(team->context_, index);
            } catch (const std::exception& error) {
                LOG_ERROR_STREAM << "Worker failed: " << error.what();
                std::abort();
            }
            team->timings_[index].finish = now_ns();
            team->gate_.arrive_and_wait();
        }
    }
public:
    explicit WorkerTeam(size_t workers)
    : gate_(static_cast<std::ptrdiff_t>(workers + 1)), timings_(workers) {
        workers_.reserve(workers);
        try {
            for (size_t index = 0; index < workers; ++index) {
                workers_.emplace_back(&WorkerTeam::worker, this, index);
            }
        } catch (const std::exception& error) {
            LOG_ERROR_STREAM << "Cannot start worker team: " << error.what();
            std::abort();
        }
    }
    ~WorkerTeam() {
        operation_ = nullptr;
        gate_.arrive_and_wait();
        for (auto& worker : workers_) worker.join();
    }
    size_t size() const { return workers_.size(); }
    uint64_t measure(void (*operation)(void*, size_t), void* context) {
        operation_ = operation;
        context_ = context;
        gate_.arrive_and_wait();
        gate_.arrive_and_wait();
        uint64_t start = timings_.front().start, finish = timings_.front().finish;
        for (const auto& timing : timings_) {
            start = std::min(start, timing.start);
            finish = std::max(finish, timing.finish);
        }
        return finish - start;
    }
};
/** --------------------------------------------------------------------------------------------------------- Insert Row
 * @brief Dispatches statically to each contender's existing insertion operation.
 */
template<typename Container>
void insert_row(Container& container, int64_t identifier, Slice&& payload) {
    if constexpr (std::is_same_v<Container, VecSet<>>) {
        container.insert(identifier, std::move(payload));
    } else {
        container.add_slice(identifier, std::move(payload));
    }
}
/** --------------------------------------------------------------------------------------------------------- Retain Row
 * @brief Makes all lookups return one retained Slice while reclamation remains quiescent.
 */
template<typename Container, bool Simd = false>
Slice retain_row(Container& container, int64_t identifier) {
    if constexpr (std::is_same_v<Container, VecSet<>>) {
        const int64_t slot = Simd ? container.find_simd(identifier) : container.find(identifier);
        return slot < 0 ? Slice() : container.retain(static_cast<size_t>(slot));
    } else {
        return container.get_slice(identifier);
    }
}
/** --------------------------------------------------------------------------------------------------------- Validate Rows
 * @brief Checks actual payload identities after timing instead of counting successful lookups alone.
 */
template<typename Container>
void validate_rows(Container& container, const std::vector<int64_t>& identifiers) {
    for (size_t index = 0; index < identifiers.size(); ++index) {
        const Slice found = retain_row(container, identifiers[index]);
        require(found && found.get_as<uint64_t>() == index, "Lookup returned the wrong payload.");
    }
}
/** --------------------------------------------------------------------------------------------------------- Range Begin
 * @brief Splits every item exactly once even when the workload is not divisible by worker count.
 */
size_t range_begin(size_t count, size_t worker, size_t workers) {
    return count / workers * worker + std::min(count % workers, worker);
}
/** --------------------------------------------------------------------------------------------------------- Gather Work
 * @brief Publishes a disjoint input range without allocating payloads or checking results in the timer.
 */
template<typename Container>
struct GatherWork {
    Container& container;
    const std::vector<int64_t>& identifiers;
    std::vector<Slice>& payloads;
    size_t producers;
    static void run(void* context, size_t worker) {
        auto& work = *static_cast<GatherWork*>(context);
        const size_t first = range_begin(work.identifiers.size(), worker, work.producers);
        const size_t last = range_begin(work.identifiers.size(), worker + 1, work.producers);
        for (size_t index = first; index < last; ++index) {
            insert_row(work.container, work.identifiers[index], std::move(work.payloads[index]));
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Gather Sample
 * @brief Measures insertion with initial storage reserved and validates every row afterwards.
 */
template<typename Container>
uint64_t gather_sample(WorkerTeam& team, const std::vector<int64_t>& identifiers, size_t capacity) {
    auto payloads = payload_pool(identifiers.size());
    Container container(capacity);
    GatherWork<Container> work{container, identifiers, payloads, team.size()};
    const uint64_t elapsed = team.measure(&GatherWork<Container>::run, &work);
    validate_rows(container, identifiers);
    for (const Slice& payload : payloads) require(payload.is_null(), "Insertion retained its source.");
    return elapsed;
}
/** --------------------------------------------------------------------------------------------------------- bench_gather
 * @brief Alternates equal insertion workloads on persistent teams with two untimed warmups.
 */
void bench_gather(size_t count, size_t producers, size_t rounds, Section& out) {
    Progress progress("Gather n=" + std::to_string(count) + " producers=" + std::to_string(producers));
    const auto identifiers = scattered_ids(count);
    WorkerTeam team(producers);
    std::array<Samples, 5> samples;
    for (size_t round = 0; round < rounds + 2; ++round) {
        for (size_t order = 0; order < samples.size(); ++order) {
            const size_t candidate = (round + order) % samples.size();
            uint64_t elapsed = 0;
            switch (candidate) {
                case 0: elapsed = gather_sample<VecSet<>>(team, identifiers, count); break;
                case 1: elapsed = gather_sample<SliceMap>(team, identifiers, count); break;
                case 2: elapsed = gather_sample<HazardMap>(team, identifiers, count); break;
                case 3: elapsed = gather_sample<MutexMap>(team, identifiers, count); break;
                case 4: elapsed = gather_sample<VersionMap>(team, identifiers, count); break;
            }
            if (round >= 2) samples[candidate].values.push_back(elapsed);
        }
    }
    const double divisor = producers == 1 ? double(count) : 1000.0;
    out.add("n=" + std::to_string(count) + " p=" + std::to_string(producers),
        samples[0].format(divisor), samples[1].format(divisor),
        samples[2].format(divisor), samples[3].format(divisor),
        "storage reserved; node allocation included; payloads verified", "N/A",
        samples[4].format(divisor));
}
/** --------------------------------------------------------------------------------------------------------- bench_fill
 * @brief Measures single-worker insertion with container construction outside the timer.
 */
void bench_fill(size_t count, size_t rounds, Section& out) {
    bench_gather(count, 1, rounds, out);
}
/** --------------------------------------------------------------------------------------------------------- bench_mp_gather
 * @brief Measures concurrent insertion without thread creation or payload allocation.
 */
void bench_mp_gather(size_t count, size_t producers, size_t rounds, Section& out) {
    bench_gather(count, producers, rounds, out);
}
/** --------------------------------------------------------------------------------------------------------- Query Pattern
 * @brief Selects deterministic hit, miss, locality, and occupancy workloads.
 */
enum class QueryPattern { scattered, sequential, miss, mixed, hot, sparse };
/** --------------------------------------------------------------------------------------------------------- Queries
 * @brief Carries precomputed query IDs and independently expected payload identities.
 */
struct Queries {
    std::vector<int64_t> identifiers;
    std::vector<int64_t> expected;
    Queries(const std::vector<int64_t>& rows, QueryPattern pattern) {
        identifiers.reserve(LOOKUPS);
        expected.reserve(LOOKUPS);
        for (size_t index = 0; index < LOOKUPS; ++index) {
            size_t row = index % rows.size();
            if (pattern == QueryPattern::hot) row = index % std::min(size_t(8), rows.size());
            else if (pattern != QueryPattern::sequential) row = (index * 7919u) % rows.size();
            const bool missing = pattern == QueryPattern::miss
                || (pattern == QueryPattern::mixed && index % 2 != 0);
            identifiers.push_back(missing ? -2 - static_cast<int64_t>(index) : rows[row]);
            expected.push_back(missing ? -1 : static_cast<int64_t>(row));
        }
    }
    /** ------------------------------------------------------------------------------------------- Validate
     * @brief Validates every returned claim after the measured lookup phase completes.
     */
    void validate(const std::vector<Slice>& output) const {
        for (size_t index = 0; index < output.size(); ++index) {
            if (expected[index] < 0) require(!output[index], "Missing query returned a payload.");
            else require(output[index] && output[index].get_as<uint64_t>()
                == static_cast<uint64_t>(expected[index]), "Query returned the wrong payload.");
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Lookup Work
 * @brief Returns retained claims for disjoint query ranges through either indexed or SIMD lookup.
 */
template<typename Container, bool Simd = false>
struct LookupWork {
    Container& container;
    const Queries& queries;
    std::vector<Slice>& output;
    size_t readers;
    static void run(void* context, size_t worker) {
        auto& work = *static_cast<LookupWork*>(context);
        const size_t first = range_begin(work.output.size(), worker, work.readers);
        const size_t last = range_begin(work.output.size(), worker + 1, work.readers);
        for (size_t index = first; index < last; ++index) {
            work.output[index] = retain_row<Container, Simd>(work.container, work.queries.identifiers[index]);
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Lookup Sample
 * @brief Excludes query generation, output allocation, validation, and final claim release from timing.
 */
template<typename Container, bool Simd = false>
uint64_t lookup_sample(WorkerTeam& team, Container& container, const Queries& queries) {
    std::vector<Slice> output(queries.identifiers.size());
    LookupWork<Container, Simd> work{container, queries, output, team.size()};
    const uint64_t elapsed = team.measure(&LookupWork<Container, Simd>::run, &work);
    queries.validate(output);
    return elapsed;
}
/** --------------------------------------------------------------------------------------------------------- Populate
 * @brief Prepares identical tagged payloads for a post-gather lookup comparison.
 */
template<typename Container>
void populate(Container& container, const std::vector<int64_t>& identifiers) {
    auto payloads = payload_pool(identifiers.size());
    for (size_t index = 0; index < identifiers.size(); ++index) {
        insert_row(container, identifiers[index], std::move(payloads[index]));
    }
}
/** --------------------------------------------------------------------------------------------------------- bench_lookup
 * @brief Compares equivalent retained-payload lookups over several query and occupancy patterns.
 */
void bench_lookup(
    size_t count, size_t readers, size_t rounds, QueryPattern pattern,
    const std::string& pattern_name, Section& out
) {
    Progress progress("Lookup " + pattern_name + " n=" + std::to_string(count)
        + " readers=" + std::to_string(readers));
    const auto identifiers = scattered_ids(count);
    const size_t capacity = pattern == QueryPattern::sparse ? count * 4 : count;
    const Queries queries(identifiers, pattern);
    VecSet set(capacity);
    SliceMap map(capacity);
    HazardMap hazard(capacity);
    MutexMap mutex_map(capacity);
    VersionMap version_map(capacity);
    populate(set, identifiers);
    populate(map, identifiers);
    populate(hazard, identifiers);
    populate(mutex_map, identifiers);
    populate(version_map, identifiers);
    WorkerTeam team(readers);
    std::array<Samples, 6> samples;
    for (size_t round = 0; round < rounds + 2; ++round) {
        for (size_t order = 0; order < samples.size(); ++order) {
            const size_t candidate = (round + order) % samples.size();
            if (candidate == 1 && readers >= SliceMap::kHazardMaxThreads) continue;
            if (candidate == 2 && readers > 64) continue;
            uint64_t elapsed = 0;
            switch (candidate) {
                case 0: elapsed = lookup_sample(team, set, queries); break;
                case 1: elapsed = lookup_sample(team, map, queries); break;
                case 2: elapsed = lookup_sample(team, hazard, queries); break;
                case 3: elapsed = lookup_sample(team, mutex_map, queries); break;
                case 4: elapsed = lookup_sample<VecSet<>, true>(team, set, queries); break;
                case 5: elapsed = lookup_sample(team, version_map, queries); break;
            }
            if (round >= 2) samples[candidate].values.push_back(elapsed);
        }
    }
    out.add(pattern_name + " n=" + std::to_string(count) + " c=" + std::to_string(readers),
        samples[0].format(LOOKUPS),
        samples[1].values.empty() ? "N/A reader limit" : samples[1].format(LOOKUPS),
        samples[2].values.empty() ? "N/A reader limit" : samples[2].format(LOOKUPS),
        samples[3].format(LOOKUPS), "median [min,max]; retained Slice for every hit",
        samples[4].format(LOOKUPS), samples[5].format(LOOKUPS));
}
/** --------------------------------------------------------------------------------------------------------- Stream Work
 * @brief Polls for disjoint result IDs while independent producers publish unique rows.
 */
template<typename Container>
struct StreamWork {
    GatherWork<Container> gather;
    std::vector<Slice>& output;
    size_t consumers;
    static void run(void* context, size_t worker) {
        auto& work = *static_cast<StreamWork*>(context);
        if (worker < work.gather.producers) {
            GatherWork<Container>::run(&work.gather, worker);
            return;
        }
        const size_t reader = worker - work.gather.producers;
        const size_t first = range_begin(work.output.size(), reader, work.consumers);
        const size_t last = range_begin(work.output.size(), reader + 1, work.consumers);
        for (size_t index = first; index < last; ++index) {
            Slice found;
            while (!(found = retain_row(work.gather.container, work.gather.identifiers[index]))) {
                std::this_thread::yield();
            }
            work.output[index] = std::move(found);
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Stream Sample
 * @brief Checks every delivered payload outside the timed publication and polling interval.
 */
template<typename Container>
uint64_t stream_sample(
    WorkerTeam& team, const std::vector<int64_t>& identifiers, size_t producers, size_t consumers
) {
    auto payloads = payload_pool(identifiers.size());
    std::vector<Slice> output(identifiers.size());
    Container container(identifiers.size());
    StreamWork<Container> work{{container, identifiers, payloads, producers}, output, consumers};
    const uint64_t elapsed = team.measure(&StreamWork<Container>::run, &work);
    for (size_t index = 0; index < output.size(); ++index) {
        require(output[index] && output[index].get_as<uint64_t>() == index,
            "Concurrent lookup returned the wrong payload.");
        require(payloads[index].is_null(), "Concurrent insertion retained its source.");
    }
    return elapsed;
}
/** --------------------------------------------------------------------------------------------------------- bench_stream
 * @brief Compares overlapping producers/readers only for candidates with synchronized ID lookup.
 */
void bench_stream(size_t count, size_t producers, size_t consumers, size_t rounds, Section& out) {
    Progress progress("Stream n=" + std::to_string(count) + " p=" + std::to_string(producers)
        + " c=" + std::to_string(consumers));
    const auto identifiers = scattered_ids(count);
    WorkerTeam team(producers + consumers);
    std::array<Samples, 5> samples;
    for (size_t round = 0; round < rounds + 2; ++round) {
        for (size_t order = 0; order < samples.size(); ++order) {
            const size_t candidate = (round + order) % samples.size();
            if (candidate == 1 && consumers > 64) continue;
            uint64_t elapsed = 0;
            switch (candidate) {
                case 0: elapsed = stream_sample<VecSet<>>(team, identifiers, producers, consumers); break;
                case 1: elapsed = stream_sample<HazardMap>(team, identifiers, producers, consumers); break;
                case 2: elapsed = stream_sample<MutexMap>(team, identifiers, producers, consumers); break;
                case 3: elapsed = stream_sample<VersionMap>(team, identifiers, producers, consumers); break;
                case 4: elapsed = stream_sample<SliceMap>(team, identifiers, producers, consumers); break;
            }
            if (round >= 2) samples[candidate].values.push_back(elapsed);
        }
    }
    out.add("n=" + std::to_string(count) + " p=" + std::to_string(producers)
        + " c=" + std::to_string(consumers), samples[0].format(1000),
        samples[4].format(1000),
        samples[1].values.empty() ? "N/A reader limit" : samples[1].format(1000),
        samples[2].format(1000), "median [min,max]; append-only; polling included",
        "N/A unsafe ID scan", samples[3].format(1000));
}
/** --------------------------------------------------------------------------------------------------------- bench_growth
 * @brief Tests exceeding the initial sizing hint without overrunning fixed-capacity contenders.
 */
void bench_growth(size_t count, size_t producers, size_t rounds, Section& out) {
    Progress progress("Growth n=" + std::to_string(count) + " p=" + std::to_string(producers));
    const auto identifiers = scattered_ids(count);
    const size_t initial = std::max(size_t(1), count / 8);
    WorkerTeam team(producers);
    std::array<Samples, 4> samples;
    for (size_t round = 0; round < rounds + 2; ++round) {
        for (size_t order = 0; order < samples.size(); ++order) {
            const size_t candidate = (round + order) % samples.size();
            const uint64_t elapsed = candidate == 0
                ? gather_sample<HazardMap>(team, identifiers, initial)
                : candidate == 1 ? gather_sample<MutexMap>(team, identifiers, initial)
                : candidate == 2 ? gather_sample<VersionMap>(team, identifiers, initial)
                : gather_sample<SliceMap>(team, identifiers, initial);
            if (round >= 2) samples[candidate].values.push_back(elapsed);
        }
    }
    out.add("n=" + std::to_string(count) + " initial=" + std::to_string(initial)
        + " p=" + std::to_string(producers), "N/A fixed", samples[3].format(count),
        samples[0].format(count), samples[1].format(count),
        "nodes grow beyond the hint; unordered_map may rehash", "N/A fixed", samples[2].format(count));
}
/** --------------------------------------------------------------------------------------------------------- View Sample
 * @brief Times cached pointer-list access or full rebuilding and validates every borrowed payload.
 */
template<typename Container>
uint64_t view_sample(Container& container, size_t count, size_t repetitions, uintptr_t& sink) {
    const uint64_t start = now_ns();
    for (size_t repeat = 0; repeat < repetitions; ++repeat) {
        sink ^= reinterpret_cast<uintptr_t>(container.template data<void>()[repeat % count]);
    }
    const uint64_t elapsed = now_ns() - start;
    const void* const* pointers = container.template data<void>();
    std::vector<bool> seen(count, false);
    for (size_t index = 0; index < count; ++index) {
        require(pointers[index] != nullptr, "Pointer view contains a null row.");
        const uint64_t identifier = *static_cast<const uint64_t*>(pointers[index]);
        require(identifier < count && !seen[identifier], "Pointer view lost or duplicated a payload.");
        seen[identifier] = true;
    }
    return elapsed;
}
/** --------------------------------------------------------------------------------------------------------- View
 * @brief Reports cached access separately from rebuilt lists without choosing a common winner.
 */
void bench_view(size_t count, size_t rounds, Section& output) {
    Progress progress("Pointer views n=" + std::to_string(count));
    const auto identifiers = scattered_ids(count);
    VecSet set(count);
    SliceMap map(count);
    HazardMap hazard(count);
    MutexMap mutex_map(count);
    VersionMap version_map(count);
    populate(set, identifiers);
    populate(map, identifiers);
    populate(hazard, identifiers);
    populate(mutex_map, identifiers);
    populate(version_map, identifiers);
    const size_t walk_repeats = std::max(size_t(1), VIEW_REPS / count);
    uintptr_t sink = 0;
    std::array<Samples, 5> samples;
    Samples cold_map;
    for (size_t round = 0; round < rounds + 2; ++round) {
        for (size_t order = 0; order < samples.size(); ++order) {
            const size_t candidate = (round + order) % samples.size();
            uint64_t elapsed = 0;
            switch (candidate) {
                case 0: elapsed = view_sample(set, count, VIEW_REPS, sink); break;
                case 1: elapsed = view_sample(map, count, VIEW_REPS, sink); break;
                case 2: elapsed = view_sample(hazard, count, walk_repeats, sink); break;
                case 3: elapsed = view_sample(mutex_map, count, walk_repeats, sink); break;
                case 4: elapsed = view_sample(version_map, count, walk_repeats, sink); break;
            }
            if (round >= 2) samples[candidate].values.push_back(elapsed);
        }
        map.add_slice(identifiers[0], map.get_slice(identifiers[0]));
        const uint64_t rebuilt = view_sample(map, count, 1, sink);
        if (round >= 2) cold_map.values.push_back(rebuilt);
    }
    output.add("n=" + std::to_string(count), samples[0].format(VIEW_REPS),
        samples[1].format(VIEW_REPS), samples[2].format(walk_repeats),
        samples[3].format(walk_repeats),
        "cached list access versus full rebuild; sink=" + std::to_string(sink),
        "N/A", samples[4].format(walk_repeats));
    output.add("n=" + std::to_string(count) + " rebuild after replacement", "N/A cached",
        cold_map.format(1), samples[2].format(walk_repeats), samples[3].format(walk_repeats),
        "SliceMap includes allocation and full pointer-view refresh; replacement excluded",
        "N/A", samples[4].format(walk_repeats));
}
/** --------------------------------------------------------------------------------------------------------- Combine Sample
 * @brief Moves uneven input partitions into one destination and validates every resulting claim.
 */
template<typename Container>
uint64_t combine_sample(size_t count, size_t parts) {
    const auto identifiers = scattered_ids(count);
    auto payloads = payload_pool(count);
    std::vector<std::unique_ptr<Container>> sources;
    for (size_t part = 0; part < parts; ++part) {
        const size_t first = range_begin(count, part, parts);
        const size_t last = range_begin(count, part + 1, parts);
        sources.push_back(std::make_unique<Container>(last - first));
        for (size_t index = first; index < last; ++index) {
            insert_row(*sources.back(), identifiers[index], std::move(payloads[index]));
        }
    }
    Container destination(count);
    const uint64_t start = now_ns();
    for (auto& source : sources) {
        destination.merge(*source);
    }
    const uint64_t elapsed = now_ns() - start;
    validate_rows(destination, identifiers);
    for (size_t part = 0; part < parts; ++part) {
        const size_t first = range_begin(count, part, parts);
        const size_t last = range_begin(count, part + 1, parts);
        for (size_t index = first; index < last; ++index) {
            require(!retain_row(*sources[part], identifiers[index]), "Merge left a row in its source.");
        }
    }
    return elapsed;
}
/** --------------------------------------------------------------------------------------------------------- Combine
 * @brief Compares unique-key merges with rotating order and verified source ownership.
 */
void bench_combine(size_t count, size_t parts, size_t rounds, Section& output) {
    Progress progress("Combine n=" + std::to_string(count) + " parts=" + std::to_string(parts));
    std::array<Samples, 5> samples;
    for (size_t round = 0; round < rounds + 2; ++round) {
        for (size_t order = 0; order < samples.size(); ++order) {
            const size_t candidate = (round + order) % samples.size();
            uint64_t elapsed = 0;
            switch (candidate) {
                case 0: elapsed = combine_sample<VecSet<>>(count, parts); break;
                case 1: elapsed = combine_sample<SliceMap>(count, parts); break;
                case 2: elapsed = combine_sample<HazardMap>(count, parts); break;
                case 3: elapsed = combine_sample<MutexMap>(count, parts); break;
                case 4: elapsed = combine_sample<VersionMap>(count, parts); break;
            }
            if (round >= 2) samples[candidate].values.push_back(elapsed);
        }
    }
    output.add("n=" + std::to_string(count) + " parts=" + std::to_string(parts),
        samples[0].format(1000), samples[1].format(1000), samples[2].format(1000),
        samples[3].format(1000), "unique keys; destination reserved; source ownership and payloads verified",
        "N/A", samples[4].format(1000));
}
/** --------------------------------------------------------------------------------------------------------- Duplicate Sample
 * @brief Measures sequential replacement including VersionMap's quiescent history collection.
 */
template<typename Container>
uint64_t duplicate_sample(const std::vector<int64_t>& writes) {
    auto payloads = payload_pool(writes.size());
    Container container(writes.size());
    const uint64_t start = now_ns();
    for (size_t index = 0; index < writes.size(); ++index) {
        insert_row(container, writes[index], std::move(payloads[index]));
    }
    if constexpr (std::is_same_v<Container, VersionMap>) container.collect();
    const uint64_t elapsed = now_ns() - start;
    std::unordered_map<int64_t, size_t> expected;
    for (size_t index = 0; index < writes.size(); ++index) expected[writes[index]] = index;
    for (const auto& [identifier, index] : expected) {
        const Slice found = retain_row(container, identifier);
        require(found && found.get_as<uint64_t>() == index, "Replacement returned the wrong value.");
    }
    require(container.size() == expected.size(),
        "Replacement consumed a distinct-key slot or created duplicate keys.");
    for (const Slice& payload : payloads) require(!payload, "Replacement did not transfer ownership.");
    return elapsed;
}
/** --------------------------------------------------------------------------------------------------------- Duplicate Work
 * @brief Overlaps competing updates to shared keys with readers retaining payload claims.
 */
template<typename Container>
struct DuplicateWork {
    Container& container;
    std::vector<Slice>& payloads;
    std::vector<Slice>& output;
    size_t distinct;
    size_t producers;
    size_t consumers;
    static void run(void* context, size_t worker) {
        auto& work = *static_cast<DuplicateWork*>(context);
        const bool producer = worker < work.producers;
        const size_t lane = producer ? worker : worker - work.producers;
        const size_t lanes = producer ? work.producers : work.consumers;
        const size_t first = range_begin(work.payloads.size(), lane, lanes);
        const size_t last = range_begin(work.payloads.size(), lane + 1, lanes);
        for (size_t index = first; index < last; ++index) {
            const int64_t identifier = static_cast<int64_t>(index % work.distinct);
            if (producer) insert_row(work.container, identifier, std::move(work.payloads[index]));
            else work.output[index] = retain_row(work.container, identifier);
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Concurrent Duplicate Sample
 * @brief Verifies retained versions and distinct-key storage after contended updates.
 */
template<typename Container>
uint64_t concurrent_duplicate_sample(
    WorkerTeam& team, size_t count, size_t distinct, size_t producers, size_t consumers
) {
    Container container(distinct);
    auto initial = payload_pool(distinct);
    for (size_t index = 0; index < distinct; ++index) {
        insert_row(container, static_cast<int64_t>(index), std::move(initial[index]));
    }
    const Slice retained = retain_row(container, int64_t(0));
    auto payloads = payload_pool(count);
    std::vector<Slice> output(count);
    DuplicateWork<Container> work{container, payloads, output, distinct, producers, consumers};
    uint64_t elapsed = team.measure(&DuplicateWork<Container>::run, &work);
    if constexpr (std::is_same_v<Container, VersionMap>) {
        const uint64_t start = now_ns();
        container.collect();
        elapsed += now_ns() - start;
        require(container.released() == count, "Version reclamation lost or leaked a history.");
        require(container.collect() == 0, "Version reclamation left unretired history.");
    }
    for (size_t index = 0; index < count; ++index) {
        require(!payloads[index], "Concurrent replacement did not transfer its source.");
        require(output[index] && output[index].get_as<uint64_t>() < count
            && output[index].get_as<uint64_t>() % distinct == index % distinct,
            "Concurrent replacement returned a torn, missing, or wrong-key payload.");
    }
    require(retained && retained.get_as<uint64_t>() == 0,
        "Replacement or collection invalidated an earlier retained claim.");
    auto final_payloads = payload_pool(distinct);
    for (size_t index = 0; index < distinct; ++index) {
        final_payloads[index].get_as<uint64_t>() += count;
        insert_row(container, static_cast<int64_t>(index), std::move(final_payloads[index]));
        const Slice found = retain_row(container, static_cast<int64_t>(index));
        require(found && found.get_as<uint64_t>() == count + index, "Final replacement was lost.");
    }
    require(container.size() == distinct,
        "Concurrent duplicates exhausted row capacity or created duplicate keys.");
    return elapsed;
}
/** --------------------------------------------------------------------------------------------------------- Duplicates
 * @brief Measures unique-key replacement without treating append-only containers as equivalent maps.
 */
void bench_duplicates(size_t count, size_t rounds, Section& output) {
    Progress progress("Sequential duplicate replacement n=" + std::to_string(count));
    const auto writes = write_list(count);
    std::array<Samples, 5> samples;
    for (size_t round = 0; round < rounds + 2; ++round) {
        for (size_t order = 0; order < samples.size(); ++order) {
            const size_t candidate = (round + order) % samples.size();
            uint64_t elapsed = 0;
            switch (candidate) {
                case 0: elapsed = duplicate_sample<VecSet<>>(writes); break;
                case 1: elapsed = duplicate_sample<MutexMap>(writes); break;
                case 2: elapsed = duplicate_sample<VersionMap>(writes); break;
                case 3: elapsed = duplicate_sample<SliceMap>(writes); break;
                case 4: elapsed = duplicate_sample<HazardMap>(writes); break;
            }
            if (round >= 2) samples[candidate].values.push_back(elapsed);
        }
    }
    output.add("n=" + std::to_string(count) + " sequential", samples[0].format(count),
        samples[3].format(count), samples[4].format(count), samples[1].format(count),
        "25% replacements; every contender enforces unique keys", "N/A", samples[2].format(count));
}
/** --------------------------------------------------------------------------------------------------------- Concurrent Duplicates
 * @brief Compares contended replacement and retained reads with equivalent ownership contracts.
 */
void bench_concurrent_duplicates(
    size_t count, size_t producers, size_t consumers, size_t rounds, Section& output
) {
    Progress progress("Concurrent duplicates n=" + std::to_string(count)
        + " p=" + std::to_string(producers) + " c=" + std::to_string(consumers));
    const size_t distinct = std::min(size_t(64), count / 4);
    WorkerTeam team(producers + consumers);
    std::array<Samples, 5> samples;
    for (size_t round = 0; round < rounds + 2; ++round) {
        for (size_t order = 0; order < samples.size(); ++order) {
            const size_t candidate = (round + order) % samples.size();
            uint64_t elapsed = 0;
            switch (candidate) {
                case 0: elapsed = concurrent_duplicate_sample<VecSet<>>(
                    team, count, distinct, producers, consumers); break;
                case 1: elapsed = concurrent_duplicate_sample<MutexMap>(
                    team, count, distinct, producers, consumers); break;
                case 2: elapsed = concurrent_duplicate_sample<VersionMap>(
                    team, count, distinct, producers, consumers); break;
                case 3: elapsed = concurrent_duplicate_sample<SliceMap>(
                    team, count, distinct, producers, consumers); break;
                case 4: elapsed = concurrent_duplicate_sample<HazardMap>(
                    team, count, distinct, producers, consumers); break;
            }
            if (round >= 2) samples[candidate].values.push_back(elapsed);
        }
    }
    output.add("n=" + std::to_string(count) + " p=" + std::to_string(producers)
        + " c=" + std::to_string(consumers), samples[0].format(1000),
        samples[3].format(1000), samples[4].format(1000), samples[1].format(1000),
        "shared-key updates plus equal retained reads; unique keys enforced everywhere",
        "N/A", samples[2].format(1000));
}
/** --------------------------------------------------------------------------------------------------------- Check Ownership
 * @brief Checks boundary IDs and retained payload lifetime using each contender's public operations.
 */
template<typename Container>
void check_ownership() {
    const std::vector<int64_t> identifiers{INT64_MIN, INT64_MAX, -2, 0, 8, 15, 16, 31};
    Slice retained;
    {
        Container container(identifiers.size());
        populate(container, identifiers);
        validate_rows(container, identifiers);
        require(!retain_row(container, int64_t(999)), "Absent boundary ID unexpectedly matched.");
        retained = retain_row(container, identifiers[3]);
    }
    require(retained && retained.get_as<uint64_t>() == 3,
        "Container destruction invalidated a retained payload.");
}
/** --------------------------------------------------------------------------------------------------------- Check Drain
 * @brief Checks every five-row score permutation, sparse slots, zero drains, and refill ownership.
 */
template<bool Sorted>
void check_drain() {
    constexpr size_t capacity = 8;
    std::array<size_t, 5> ranks{0, 1, 2, 3, 4};
    do {
        for (size_t requested = 0; requested <= ranks.size() + 1; ++requested) {
            const size_t keep = std::min(requested, ranks.size());
            VecSet<Sorted> set(capacity);
            auto payloads = payload_pool(ranks.size());
            const Slice retained = payloads.front().slice();
            for (size_t index = 0; index < ranks.size(); ++index) {
                set.insert(static_cast<int64_t>(100 + index),
                    std::move(payloads[index]), static_cast<float>(ranks[index]));
            }
            require(set.drain_to(requested) == keep && set.size() == keep,
                "Drain returned the wrong size.");
            for (size_t index = 0; index < ranks.size(); ++index) {
                const int64_t slot = set.find(static_cast<int64_t>(100 + index));
                if (ranks[index] < ranks.size() - keep) {
                    require(slot == -1, "Drain retained an evicted ID.");
                    continue;
                }
                require(slot >= 0 && static_cast<size_t>(slot) < keep,
                    "Drain lost a selected ID or left a hole.");
                const auto row = set.row(static_cast<size_t>(slot));
                require(row.payload->template get_as<uint64_t>() == index,
                    "Drain changed a surviving payload.");
                require(set.score(static_cast<size_t>(slot)) == ranks[index],
                    "Drain separated a score from its payload.");
                require(set.template data<void>()[slot] == row.payload->raw(),
                    "Drain left a stale raw pointer.");
                if constexpr (Sorted) {
                    require(static_cast<size_t>(slot) == ranks.size() - 1 - ranks[index],
                        "Sorted drain returned the wrong order.");
                }
            }
            for (size_t slot = keep; slot < capacity; ++slot) {
                require(!set.published(slot) && !*set.row(slot).payload
                    && set.template data<void>()[slot] == nullptr,
                    "Drain retained ownership or a raw pointer in its tail.");
            }
            require(retained.get_as<uint64_t>() == 0,
                "Drain invalidated an externally retained payload.");
            auto refill = payload_pool(capacity - keep);
            for (size_t index = 0; index < refill.size(); ++index) {
                set.insert(static_cast<int64_t>(200 + index), std::move(refill[index]));
                require(set.find(static_cast<int64_t>(200 + index)) == int64_t(keep + index),
                    "Drain did not recycle its tail.");
            }
            require(set.size() == capacity, "Refill after drain lost rows.");
            require(set.drain_to(0) == 0 && set.size() == 0, "Drain to zero left live rows.");
        }
    } while (std::next_permutation(ranks.begin(), ranks.end()));
}
/** --------------------------------------------------------------------------------------------------------- Check Duplicate Capacity
 * @brief Verifies that replacing one key leaves room for another distinct key.
 */
void check_duplicate_capacity() {
    VecSet<> set(8);
    auto payloads = payload_pool(set.capacity() * 4);
    for (size_t write = 0; write < payloads.size(); ++write) {
        set.insert(-1, std::move(payloads[write]), static_cast<float>(write));
        require(set.size() == 1, "Duplicate insertion left multiple live rows.");
        const int64_t found = set.find(-1);
        require(found >= 0 && set.retain(static_cast<size_t>(found)).get_as<uint64_t>() == write,
            "Duplicate insertion did not preserve the last payload.");
        require(set.find_simd(-1) == found, "SIMD lookup rejected a valid negative ID.");
    }
    auto distinct = payload_pool(8);
    for (size_t index = 0; index < 7; ++index) {
        set.insert(static_cast<int64_t>(index), std::move(distinct[index]));
    }
    bool rejected = false;
    try { set.insert(99, std::move(distinct[7])); }
    catch (const std::length_error&) { rejected = true; }
    require(rejected && set.size() == 8 && distinct[7],
        "Distinct-key overflow did not preserve capacity and source ownership.");
    set.insert(-1, std::move(distinct[7]));
    require(set.size() == 8 && set.retain(static_cast<size_t>(set.find(-1))).get_as<uint64_t>() == 7,
        "Replacing a key in a full set failed.");
}
/** --------------------------------------------------------------------------------------------------------- Concurrent First Inserts
 * @brief Races first insertion of the same keys before testing replacement of existing rows.
 */
template<typename Container>
void check_concurrent_first_inserts(WorkerTeam& team) {
    constexpr size_t count = 4096;
    constexpr size_t distinct = 17;
    for (size_t repeat = 0; repeat < 5; ++repeat) {
        Container container(distinct);
        std::vector<int64_t> identifiers(count);
        for (size_t index = 0; index < count; ++index) {
            identifiers[index] = static_cast<int64_t>(index % distinct) - 1;
        }
        auto payloads = payload_pool(count);
        GatherWork<Container> work{container, identifiers, payloads, team.size()};
        team.measure(&GatherWork<Container>::run, &work);
        for (size_t index = 0; index < distinct; ++index) {
            const Slice found = retain_row(container, static_cast<int64_t>(index) - 1);
            require(found && found.get_as<uint64_t>() < count
                && found.get_as<uint64_t>() % distinct == index,
                "Concurrent first insertion published the wrong payload.");
        }
        require(container.size() == distinct, "Concurrent first insertion duplicated a key.");
        if constexpr (std::is_same_v<Container, VersionMap>) {
            container.collect();
            require(container.released() == count - distinct,
                "First-insert reclamation count is wrong.");
        }
    }
}
/** --------------------------------------------------------------------------------------------------------- Overlapping Merge
 * @brief Checks source-wins merging, retained old values, transferred scores, and source reuse.
 */
void check_overlapping_merge() {
    auto payloads = payload_pool(4);
    VecSet left(2);
    VecSet right(2);
    VecSet destination(3);
    left.insert(-1, std::move(payloads[0]), 2.0f);
    left.insert(10, std::move(payloads[1]), 1.0f);
    right.insert(-1, std::move(payloads[2]), 3.0f);
    const Slice retained = left.retain(static_cast<size_t>(left.find(-1)));
    destination.merge(left);
    destination.merge(right);
    require(destination.size() == 2 && left.size() == 0 && right.size() == 0,
        "Overlapping merge failed set size or source emptiness.");
    const size_t slot = static_cast<size_t>(destination.find(-1));
    require(destination.retain(slot).get_as<uint64_t>() == 2 && destination.score(slot) == 3.0f,
        "Overlapping merge lost source value or score.");
    require(retained.get_as<uint64_t>() == 0, "Merge invalidated an external claim.");
    left.insert(20, std::move(payloads[3]));
    require(left.size() == 1 && left.find(-1) == -1, "Merge left stale source index entries.");
    destination.drain_to(1);
    require(destination.size() == 1 && destination.find(-1) >= 0 && destination.find(10) == -1,
        "Drain after overlapping merge kept the wrong row.");
}
/** --------------------------------------------------------------------------------------------------------- bench_contracts
 * @brief Qualifies lookup ownership and both drain variants before interpreting performance results.
 */
void bench_contracts(size_t workers) {
    Progress progress("Container ownership and drain contracts");
    check_ownership<VecSet<>>();
    check_ownership<SliceMap>();
    check_ownership<HazardMap>();
    check_ownership<MutexMap>();
    check_ownership<VersionMap>();
    size_t failures = 0;
    try {
        check_drain<false>();
        LOG_INFO_STREAM << "VecSet drain: PASS";
    } catch (const std::exception& error) {
        ++failures;
        LOG_ERROR_STREAM << "VecSet drain: FAIL: " << error.what();
    }
    try {
        check_drain<true>();
        LOG_INFO_STREAM << "Sorted VecSet drain: PASS";
    } catch (const std::exception& error) {
        ++failures;
        LOG_ERROR_STREAM << "Sorted VecSet drain: FAIL: " << error.what();
    }
    try {
        check_duplicate_capacity();
        LOG_INFO_STREAM << "VecSet duplicate capacity: PASS";
    } catch (const std::exception& error) {
        ++failures;
        LOG_ERROR_STREAM << "VecSet duplicate capacity: FAIL: " << error.what();
    }
    require(failures == 0,
        "Container candidates failed correctness; no release winner can be selected.");
    const size_t producers = std::max(size_t(2), workers);
    const size_t consumers = std::max(size_t(2), workers);
    WorkerTeam team(producers + consumers);
    check_concurrent_first_inserts<VecSet<>>(team);
    check_concurrent_first_inserts<SliceMap>(team);
    check_concurrent_first_inserts<HazardMap>(team);
    check_concurrent_first_inserts<VersionMap>(team);
    check_concurrent_first_inserts<MutexMap>(team);
    check_overlapping_merge();
    const auto identifiers = scattered_ids(257);
    stream_sample<VecSet<>>(team, identifiers, producers, consumers);
    stream_sample<SliceMap>(team, identifiers, producers, consumers);
    stream_sample<VersionMap>(team, identifiers, producers, consumers);
    concurrent_duplicate_sample<VecSet<>>(team, 4096, 17, producers, consumers);
    concurrent_duplicate_sample<SliceMap>(team, 4096, 17, producers, consumers);
    concurrent_duplicate_sample<HazardMap>(team, 4096, 17, producers, consumers);
    concurrent_duplicate_sample<VersionMap>(team, 4096, 17, producers, consumers);
    concurrent_duplicate_sample<MutexMap>(team, 4096, 17, producers, consumers);
    combine_sample<VecSet<>>(131, 8);
    combine_sample<SliceMap>(131, 8);
    combine_sample<HazardMap>(131, 8);
    combine_sample<MutexMap>(131, 8);
    combine_sample<VersionMap>(131, 8);
    LOG_INFO_STREAM << "Concurrent publication, replacement, retention, collection, uneven merge: PASS ("
        << producers + consumers << " workers)";
}
/** --------------------------------------------------------------------------------------------------------- Settings
 * @brief Selects workload groups without modifying the implementations or reducing hardware by default.
 */
struct Settings {
    std::string group = "all";
    std::string csv_path;
    std::vector<size_t> sizes{64, 256, 512, 2048, 16384, 65536};
    size_t rounds = 5;
    size_t workers = std::thread::hardware_concurrency();
    bool help = false;
    bool selected(const char* name) const { return group == "all" || group == name; }
};
/** --------------------------------------------------------------------------------------------------------- Parse Settings
 * @brief Exposes reproducible row, repetition, query, and worker counts for each workload group.
 */
Settings parse_settings(int count, char** arguments) {
    Settings settings;
    for (int index = 1; index < count; ++index) {
        const std::string_view option(arguments[index]);
        if (option == "--help") { settings.help = true; continue; }
        require(index + 1 < count, "Missing option value; use --help.");
        const std::string_view value(arguments[++index]);
        if (option == "--case") { settings.group = value; continue; }
        if (option == "--csv") { settings.csv_path = value; continue; }
        size_t number = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && number > 0,
            "Expected a positive integer option value.");
        if (option == "--rows") settings.sizes = {number};
        else if (option == "--rounds") settings.rounds = number;
        else if (option == "--workers") settings.workers = number;
        else if (option == "--lookups") LOOKUPS = number;
        else throw std::runtime_error("Unknown option: " + std::string(option));
    }
    require(settings.workers != 0, "CPU count is unavailable; supply --workers.");
    require(settings.workers < static_cast<size_t>(PTRDIFF_MAX), "Worker count is too large.");
    require(settings.workers <= (SliceMap::kHazardMaxThreads - 1) / 2,
        "SliceMap contracts use twice --workers plus a coordinator; --workers must be at most 127.");
    require(settings.rounds <= SIZE_MAX - 2, "Round count overflow.");
    require(LOOKUPS <= SIZE_MAX / 7919u && LOOKUPS < static_cast<size_t>(INT64_MAX),
        "Lookup count is too large.");
    for (const size_t rows : settings.sizes) {
        require(rows >= 8 && rows <= UINT32_MAX / 8u, "Row counts must be between 8 and UINT32_MAX/8.");
    }
    bool known = false;
    for (const auto name : {"all", "contracts", "fill", "gather", "lookup", "stream",
        "growth", "view", "combine", "duplicates"}) {
        known = known || settings.group == name;
    }
    require(known, "Unknown --case; use --help.");
    return settings;
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs selected comparisons and returns failure when any tested contract is violated.
 */
int main(int count, char** arguments) {
    try {
        const Settings settings = parse_settings(count, arguments);
        if (settings.help) {
            LOG_INFO_STREAM << "Usage: slicemapvsvecset [--case all|contracts|fill|gather|lookup|"
                << "stream|growth|view|combine|duplicates] [--rows N] [--rounds N] [--workers N]"
                << " [--lookups N] [--csv PATH]; defaults: 5 measured rounds, 65536 queries, hardware worker count.";
            return 0;
        }
        LOG_INFO_STREAM << "Experimental comparison: corrected VecSet and immutable-version map included.";
        LOG_INFO_STREAM << "Workers=" << settings.workers << ", rounds=" << settings.rounds
            << ", lookups=" << LOOKUPS << ", compiler=" << __VERSION__;
#ifdef NDEBUG
        LOG_INFO_STREAM << "Optimized-build assertions disabled; benchmark checks remain active.";
#else
        LOG_WARN_STREAM << "Assertions enabled; do not treat this as a Release performance result.";
#endif
        LOG_INFO_STREAM << "All timings: 2 warmups, rotating order, median [min,max]; "
            << "payload setup and thread creation excluded; node allocation included in insertion.";
        LOG_INFO_STREAM << "SIMD column scans VecSet's existing ID array; it is a lookup variant.";
        LOG_INFO_STREAM << "SliceMap uses Michael sorted bucket chains; HazardMap and VersionMap "
            << "carry the same position and view contract on their own lock-free cores; every "
            << "contender enforces unique keys and bounded reclamation.";
        std::vector<size_t> workers{1, 2, settings.workers};
        std::sort(workers.begin(), workers.end());
        workers.erase(std::unique(workers.begin(), workers.end()), workers.end());
        bench_contracts(settings.workers);
        if (settings.selected("fill")) {
            Section section("fill - single worker, reserved storage", "ns/row");
            for (const size_t rows : settings.sizes) bench_fill(rows, settings.rounds, section);
            print_section(section);
        }
        if (settings.selected("gather")) {
            for (const size_t producers : workers) {
                Section section("gather - concurrent producers, reserved storage", producers == 1
                    ? "ns/row" : "us/batch");
                for (const size_t rows : settings.sizes) {
                    bench_mp_gather(rows, producers, settings.rounds, section);
                }
                print_section(section);
            }
        }
        if (settings.selected("lookup")) {
            constexpr std::array<const char*, 6> patterns{
                "scattered-hit", "sequential-hit", "miss", "mixed-50", "hot-eight", "quarter-full"
            };
            for (const size_t readers : workers) {
                for (size_t pattern = 0; pattern < patterns.size(); ++pattern) {
                    Section section("lookup - retained payload, aggregate cost per operation", "ns/op");
                    for (const size_t rows : settings.sizes) {
                        bench_lookup(rows, readers, settings.rounds, static_cast<QueryPattern>(pattern),
                            patterns[pattern], section);
                    }
                    print_section(section);
                }
            }
        }
        if (settings.selected("stream")) {
            std::vector<std::pair<size_t, size_t>> shapes{{1, 1}};
            if (settings.workers > 1) {
                shapes.emplace_back(1, settings.workers - 1);
                shapes.emplace_back(settings.workers - 1, 1);
                shapes.emplace_back(settings.workers / 2, settings.workers - settings.workers / 2);
            }
            std::sort(shapes.begin(), shapes.end());
            shapes.erase(std::unique(shapes.begin(), shapes.end()), shapes.end());
            for (const auto& [producers, consumers] : shapes) {
                Section section("stream - overlapping producers and polling readers", "us/batch");
                for (const size_t rows : settings.sizes) {
                    bench_stream(rows, producers, consumers, settings.rounds, section);
                }
                print_section(section);
            }
        }
        if (settings.selected("growth")) {
            for (const size_t producers : workers) {
                Section section("growth - insert beyond initial sizing hint", "ns/row");
                for (const size_t rows : settings.sizes) {
                    bench_growth(rows, producers, settings.rounds, section);
                }
                print_section(section);
            }
        }
        if (settings.selected("view")) {
            Section section("view - cached list access versus rebuilding a list", "ns/view");
            for (const size_t rows : settings.sizes) bench_view(rows, settings.rounds, section);
            print_section(section);
        }
        if (settings.selected("combine")) {
            Section section("combine - verified unique-input merge", "us/merge");
            for (const size_t rows : settings.sizes) {
                for (const size_t parts : {2u, 4u, 8u}) {
                    bench_combine(rows, parts, settings.rounds, section);
                }
            }
            print_section(section);
        }
        if (settings.selected("duplicates")) {
            Section section("duplicates - sequential replacement", "ns/write");
            for (const size_t rows : settings.sizes) bench_duplicates(rows, settings.rounds, section);
            print_section(section);
            for (const size_t total : workers) {
                if (total < 2) continue;
                Section concurrent("duplicates - concurrent replacement and retained reads", "us/batch");
                for (const size_t rows : settings.sizes) {
                    bench_concurrent_duplicates(rows, total / 2, total - total / 2,
                        settings.rounds, concurrent);
                }
                print_section(concurrent);
            }
        }
        if (!summaries.empty()) print_summary(settings.csv_path);
        return 0;
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Comparison failed: " << error.what();
        return 1;
    }
}
