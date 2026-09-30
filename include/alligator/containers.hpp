#pragma once
/** --------------------------------------------------------------------------------------------------------- Containers
 * @file include/alligator/containers.hpp
 * @brief Container utilities for the Alligator library.
 */
#include <alligator.hpp>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iterator>
#include <memory>
#include <new>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace buffetalligator {
class Slice;
/** --------------------------------------------------------------------------------------------------------- SliceMap
 * @class SliceMap
 * @brief Growing unique-key Michael hash table with sorted bucket chains and hazard-protected Slice
 * replacement.
 */
class SliceMap {
private:
    struct State;
    std::atomic<State*> state_;
    std::atomic<size_t> expected_;
    void* publish_context_ = nullptr;
    using Hook = void (*)(void*, void*, const size_t&);
    Hook publish_hook_ = nullptr;
    int64_t find_internal(int64_t identifier) const;
    Slice get_slice_internal(int64_t identifier) const;
    void** pointer_view() const;
    void* payload_at(size_t index) const;
    int64_t identifier_at(size_t index) const;
    void complete_publish(State* state, size_t index, bool inserted);
public:
    inline static constexpr int kHazardPtrsPerThread = 3;
    inline static constexpr int kHazardMaxThreads = 256;
    inline static constexpr size_t kRetireBatch = 32;
    using Finalizer = void (*)(void*);
    using PublishHook = Hook;
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Reserves an initial row hint without imposing a maximum key count.
     * @param capacity Initial row reservation and default wait expectation.
     */
    explicit SliceMap(size_t capacity);
    /** ------------------------------------------------------------------------------------------- Move only */
    SliceMap(const SliceMap&) = delete;
    SliceMap& operator=(const SliceMap&) = delete;
    SliceMap(SliceMap&& other) noexcept;
    SliceMap& operator=(SliceMap&& other) noexcept;
    ~SliceMap();
    /** ------------------------------------------------------------------------------------------- Add Slice
     * @brief Inserts a new key or replaces its Slice at the existing stable position.
     * @param id Row identifier.
     * @param slice Owned payload transferred into the map.
     */
    template<typename ID>
        requires std::is_convertible_v<ID, int64_t>
    void add_slice(ID id, Slice slice) {
        add_finalized(static_cast<int64_t>(id), std::move(slice), nullptr);
    }
    /** ------------------------------------------------------------------------------------------- Merge
     * @brief Transfers a quiescent source into this map with source values replacing overlapping keys.
     * @param other Source map, emptied for reuse.
     * @return This map.
     */
    SliceMap& merge(SliceMap& other);
    /** ------------------------------------------------------------------------------------------- Operator +
     * @brief Merges another map into this one using the + operator.
     * @param other Source map, emptied for reuse.
     * @return This map.
     */
    SliceMap& operator+(SliceMap& other) { return merge(other); }
    /** ------------------------------------------------------------------------------------------- Find
     * @brief Returns the key's stable position or -1 and pins the searched index on this thread
     * until its next lookup, so the located row outlives any reset or reclamation.
     */
    template<typename ID>
        requires std::is_convertible_v<ID, int64_t>
    int64_t find(const ID& id) const {
        return find_internal(static_cast<int64_t>(id));
    }
    /** ------------------------------------------------------------------------------------------- Get Slice
     * @brief Retains a payload inside its hazard window while insertions and replacements overlap.
     */
    template<typename ID>
        requires std::is_convertible_v<ID, int64_t>
    Slice get_slice(ID id) {
        return get_slice_internal(static_cast<int64_t>(id));
    }
    /** ------------------------------------------------------------------------------------------- Data
     * @brief Refreshes the cached contiguous pointer view while all map users are quiescent.
     * @return Capacity-sized borrowed view invalidated by mutation, move, or destruction.
     */
    template<typename T = void>
    T** data() { return reinterpret_cast<T**>(pointer_view()); }
    /** ------------------------------------------------------------------------------------------- Pointer View
     * @brief Returns a raw pointer to the underlying contiguous storage.
     * @return A pointer to the underlying storage.
     */
    template<typename T = void>
    const T** data() const { return reinterpret_cast<const T**>(pointer_view()); }
    /** ------------------------------------------------------------------------------------------- As
     * @brief Borrows one payload while writers and reset are quiescent.
     * @tparam T The type to which the payload should be cast.
     * @param index The index of the slot to retrieve the payload for.
     * @return A pointer to the payload cast to the specified type.
     */
    template<typename T>
    T* as(size_t index) { return static_cast<T*>(payload_at(index)); }
    /** ------------------------------------------------------------------------------------------- Const As
     * @brief Borrows one payload while writers and reset are quiescent.
     * @tparam T The type to which the payload should be cast.
     * @param index The index of the slot to retrieve the payload for.
     * @return A pointer to the payload cast to the specified type.
     */
    template<typename T>
    const T* as(size_t index) const { return static_cast<const T*>(payload_at(index)); }
    /** ------------------------------------------------------------------------------------------- Slice At
     * @brief Retains a published position while payload replacements overlap.
     * @param index The index of the slot to retrieve the slice for.
     * @return The slice corresponding to the specified slot.
     */
    Slice slice_at(size_t index) const;
    /** ------------------------------------------------------------------------------------------- ID
     * @brief Reads a published position's immutable identifier before the next reset.
     * @tparam ID The type to which the identifier should be converted.
     * @param index The index of the slot to retrieve the identifier for.
     * @return The identifier of the specified slot.
     */
    template<typename ID>
        requires std::is_convertible_v<int64_t, ID>
    ID id(size_t index) const { return static_cast<ID>(identifier_at(index)); }
    /** ------------------------------------------------------------------------------------------- Published
     * @brief Checks if a specific slot has been published.
     * @param slot The slot to check for publication.
     * @return True if the slot has been published, false otherwise.
     */
    bool published(const size_t& slot) const;
    /** ------------------------------------------------------------------------------------------- Size
     * @brief Counts completed first publications, excluding replacements and exact when writers
     * finish.
     * @return The number of completed first publications.
     */
    size_t size() const;
    /** ------------------------------------------------------------------------------------------- Capacity
     * @brief Returns the currently reserved row positions, which grow automatically.
     * @return The currently reserved row positions.
     */
    size_t capacity() const noexcept;
    /** ------------------------------------------------------------------------------------------- Expect
     * @brief Sets the expected number of distinct rows to be published.
     * @param count The expected number of distinct rows.
     */
    void expect(const size_t& count) { expected_.store(count, std::memory_order_release); }
    /** ------------------------------------------------------------------------------------------- Wait Threshold
     * @brief Retrieves the current wait threshold for distinct row publications.
     * @return The current wait threshold.
     */
    size_t wait_threshold() const { return expected_.load(std::memory_order_acquire); }
    /** ------------------------------------------------------------------------------------------- Wait
     * @brief Parks until the wait threshold of distinct rows finish publication, including their
     * publish hooks.
     */
    void wait() { wait_until_full(wait_threshold()); }
    /** ------------------------------------------------------------------------------------------- Wait
     * @brief Parks until count distinct rows finish publication, including their publish hooks.
     * @param count The number of distinct rows to wait for.
     */
    void wait_until_full(size_t count);
    /** ------------------------------------------------------------------------------------------- Reset
     * @brief Replaces the index while readers may finish on the retired state and writers are
     * quiescent.
     */
    void reset();
    /** ------------------------------------------------------------------------------------------- On Publish
     * @brief Registers a hook before use, invoked after each insertion or replacement at its
     * stable position.
     * @param hook The function to be called after each insertion or replacement.
     * @param context User-defined context passed to the hook.
     */
    void on_publish(PublishHook hook, void* context) {
        publish_hook_ = hook;
        publish_context_ = context;
    }
    /** ------------------------------------------------------------------------------------------- GC
     * @brief Reclaims this thread's retired objects and orphaned objects no reader still protects.
     */
    static void gc();
protected:
    /** ------------------------------------------------------------------------------------------- Add Finalized
     * @brief Transfers an object whose destructor runs when its row is safely reclaimed.
     * @param id The identifier for the row. Must be convertible to int64_t.
     * @param slice The slice representing the object to be finalized.
     * @param finalizer The function to be called to finalize the object.
     */
    void add_finalized(const int64_t& id, Slice&& slice, Finalizer finalizer);
};
/** ----------------------------------------------------------------------------------------------- SliceMapT
 * @class SliceMapT
 * @brief Typed SliceMap entries constructed in place and finalized after safe replacement or removal.
 * @tparam T The payload type rows carry.
 */
template<typename T>
class SliceMapT final : public SliceMap {
public:
    /** ------------------------------------------------------------------------------------------- Constructor with Capacity
     * @brief Reserves an initial row hint without limiting future growth.
     * @param capacity Initial reservation and default wait expectation.
     */
    explicit SliceMapT(size_t capacity) : SliceMap(capacity) {}
    /** ------------------------------------------------------------------------------------------- Move Only Semantics
     * @brief The payload slots hold live claims, so the set moves rather than copies.
     */
    SliceMapT(const SliceMapT&) = delete;
    SliceMapT& operator=(const SliceMapT&) = delete;
    SliceMapT(SliceMapT&& other) noexcept = default;
    SliceMapT& operator=(SliceMapT&& other) noexcept = default;
    /** ------------------------------------------------------------------------------------------- Emplace
     * @brief Constructs a typed row in place and publishes it under `id`, registering `~T()` as
     * the row's finalizer unless `T` is trivially destructible.
     * Total atomic operations: the add_slice protocol
     * Total branches: 0 (the finalizer choice resolves at compile time)
     * @param id The identifier for the row. Must be convertible to int64_t.
     * @param args Arguments forwarded to T's constructor.
     */
    template<typename ID, typename... Args>
        requires std::is_convertible_v<ID, int64_t>
    void emplace(ID id, Args&&... args) {
        Slice payload(sizeof(T));
        new (payload.data<void>()) T(std::forward<Args>(args)...);
        if constexpr (std::is_trivially_destructible_v<T>) {
            add_slice(id, std::move(payload));
        } else {
            add_finalized(static_cast<int64_t>(id), std::move(payload), &finalize);
        }
    }
    /** ------------------------------------------------------------------------------------------- Access Payload as T
     * @brief Access the payload at the given slot as its native type.
     * @param index The slot of the row to access.
     * @return A reference to the payload.
     */
    T& as(size_t index) {
        return *SliceMap::as<T>(index);
    }
    const T& as(size_t index) const {
        return *SliceMap::as<T>(index);
    }
private:
    /** ------------------------------------------------------------------------------------------- Finalize
     * @brief Type-erased destructor for emplaced payloads; the row calls it before releasing its claim.
     * @param object The payload bytes holding a constructed T.
     */
    static void finalize(void* object) {
        static_cast<T*>(object)->~T();
    }
};
/** --------------------------------------------------------------------------------------------------------- WeakSlice
 * @class WeakSlice
 * @brief A non-owning reference to an anonymous pointer.
 */
class WeakSlice {
private:
    /// @brief A std::span of bytes representing the memory block.
    std::span<std::byte> span_;
public:
    /** ----------------------------------------------------------------------------------------- Valid
     * @brief Checks if the WeakSlice is valid (non-empty).
     * @return True if the WeakSlice is valid, false otherwise.
     */
    bool valid() const { return !span_.empty(); }
    /** ----------------------------------------------------------------------------------------- Conversion to bool
     * @brief Converts the WeakSlice to a boolean value indicating its validity.
     * @return True if the WeakSlice is valid, false otherwise.
     */
    operator bool() const { return valid(); }
    /** ----------------------------------------------------------------------------------------- Null
     * @brief Checks if the WeakSlice is null (empty).
     * @return True if the WeakSlice is null, false otherwise.
     */
    bool is_null() const { return span_.empty(); }
    /** ----------------------------------------------------------------------------------------- Constructor
     * @brief Constructs a WeakSlice from a raw pointer and size.
     * @param ptr Pointer to the memory block.
     * @param size Size of the memory block in bytes.
     */
    WeakSlice(void* ptr = nullptr, size_t size = 0)
    : span_(static_cast<std::byte*>(ptr), size) {}
    /** ----------------------------------------------------------------------------------------- Constructor - Non-owning
     * @brief Constructs a WeakSlice with a specified size, but throws an exception since it
     * cannot allocate memory.
     * @param size The size of the memory block in bytes.
     * @param unused A boolean parameter to differentiate this constructor.
     */
    WeakSlice([[maybe_unused]] size_t size, bool) {
        ALLIGATOR_THROW("WeakSlice: cannot allocate memory for non-owning slice.");
    }
    /** ----------------------------------------------------------------------------------------- Constructor - Copy
     * @brief Copy constructor for WeakSlice.
     * @param other The WeakSlice object to copy from.
     */
    WeakSlice(const WeakSlice& other) : span_(other.span_) {}
    /** ----------------------------------------------------------------------------------------- Operator= Copy
     * @brief Copy assignment operator for WeakSlice.
     * @param other The WeakSlice object to copy from.
     * @return Reference to the current WeakSlice object.
     */
    WeakSlice& operator=(const WeakSlice& other) {
        if (this != &other) span_ = other.span_;
        return *this;
    }
    /** ----------------------------------------------------------------------------------------- Constructor - Move
     * @brief Move constructor for WeakSlice.
     * @param other The WeakSlice object to move from.
     */
    WeakSlice(WeakSlice&& other) noexcept : span_(std::move(other.span_)) {}
    /** ----------------------------------------------------------------------------------------- Operator= Move
     * @brief Move assignment operator for WeakSlice.
     * @param other The WeakSlice object to move from.
     * @return Reference to the current WeakSlice object.
     */
    WeakSlice& operator=(WeakSlice&& other) noexcept {
        if (this != &other) span_ = std::move(other.span_);
        return *this;
    }
    /** ----------------------------------------------------------------------------------------- Destructor
     * @brief Default destructor for WeakSlice.
     */
    ~WeakSlice() = default;
    /** ----------------------------------------------------------------------------------------- Placement
     * @brief Returns the placement of the memory block. Since this is a weak reference, we
     * don't know the actual placement.
     */
    const BuffetDescriptor* placement() const {
        return nullptr;
    }
    /** ----------------------------------------------------------------------------------------- Raw
     * @brief Returns a raw pointer to the memory block.
     */
    void* raw() { return span_.data(); }
    /** ----------------------------------------------------------------------------------------- Raw Const
     * @brief Returns a raw pointer to the memory block.
     */
    const void* raw() const { return span_.data(); }
    /** ----------------------------------------------------------------------------------------- Raw
     * @brief Returns a raw pointer to the memory block.
     */
    template<typename T>
    T* data() { return reinterpret_cast<T*>(span_.data()); }
    /** ----------------------------------------------------------------------------------------- Data
     * @brief Returns a typed pointer to the memory block.
     * @tparam T The type to cast the memory block to.
     */
    template<typename T>
    const T* data() const { return reinterpret_cast<const T*>(span_.data()); }
    /** ----------------------------------------------------------------------------------------- Size
     * @brief Returns the size of the memory block in bytes.
     */
    template<typename T>
    size_t size() const { 
        return span_.size() / sizeof(T);
    }
    /** ----------------------------------------------------------------------------------------- Size Bytes
     * @brief Returns the size of the memory block in bytes.
     */
    size_t size_bytes() const {
        return span_.size();
    }
    /** ----------------------------------------------------------------------------------------- Operator Slice
     * @brief Converts the WeakSlice to a Slice object.
     * @return A new Slice object containing the memory from the WeakSlice.
     */
    explicit operator Slice() const {
        return slice();
    }
    /** ----------------------------------------------------------------------------------------- Slice
     * @brief Creates a new Slice object representing a subrange of the memory block.
     * @param offset The starting offset of the subrange.
     * @param size The size of the subrange in bytes.
     * @return A new Slice object containing the specified subrange.
     */
    Slice slice(size_t offset = 0, size_t size = SIZE_MAX) const {
        if (offset >= span_.size()) [[unlikely]] {
            ALLIGATOR_THROW("WeakSlice::slice(): offset out of bounds");
        }
        if (size > span_.size() - offset) [[unlikely]] {
            size = span_.size() - offset;
        }
        return Slice(span_.data() + offset, size);
    }
};
/** --------------------------------------------------------------------------------------------------------- SliceQueue
 * @class SliceQueue
 * @brief Move-only Slice mailboxes with thread-bound producer and consumer handles, backed by
 * the private C queue's thread-local blocks.
 */
class SliceQueue {
private:
    void* queue_; ///< Private C queue state.
    /** ------------------------------------------------------------------------------------------- Release Descriptor
     * @brief Releases one undelivered descriptor's arena ownership at destruction time.
     */
    static void release_descriptor(void* descriptor) noexcept;
public:
    static constexpr size_t block_size = 256; ///< Slices per synchronized handoff.
    /** ------------------------------------------------------------------------------------------- Producer
     * @brief Owns one producer binding on its calling thread and flushes on destruction.
     */
    class Producer {
    private:
        void* local_; ///< Cached producer TLS address.
        explicit Producer(void* local) noexcept : local_(local) {}
        friend class SliceQueue;
    public:
        Producer(const Producer&) = delete;
        Producer& operator=(const Producer&) = delete;
        Producer(Producer&&) = delete;
        Producer& operator=(Producer&&) = delete;
        ~Producer();
        /** --------------------------------------------------------------------------------------- Push
         * @brief Moves one Slice into local staging, blocking for capacity and nulling the source.
         */
        void push(Slice&& slice) noexcept;
        /** --------------------------------------------------------------------------------------- Push Bulk
         * @brief Moves every Slice in the span into staging and nulls every source.
         */
        void push(std::span<Slice> slices) noexcept;
        /** --------------------------------------------------------------------------------------- Flush
         * @brief Publishes a partial block at a burst boundary before the producer finishes.
         */
        void flush() noexcept;
    };
    /** ------------------------------------------------------------------------------------------- Consumer
     * @brief Owns a thread-bound consumer that must drain its local blocks before destruction.
     */
    class Consumer {
    private:
        void* local_; ///< Cached consumer TLS address.
        explicit Consumer(void* local) noexcept : local_(local) {}
        friend class SliceQueue;
    public:
        Consumer(const Consumer&) = delete;
        Consumer& operator=(const Consumer&) = delete;
        Consumer(Consumer&&) = delete;
        Consumer& operator=(Consumer&&) = delete;
        ~Consumer();
        /** --------------------------------------------------------------------------------------- Pop
         * @brief Waits for a Slice and replaces output, returning false unchanged after closure.
         */
        bool pop(Slice& output) noexcept;
        /** --------------------------------------------------------------------------------------- Pop Bulk
         * @brief Replaces up to one block of outputs, returning zero for closure or an empty span.
         */
        size_t pop(std::span<Slice> output) noexcept;
    };
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Allocates fixed worker pools with capacity per producer in multiples of block_size.
     */
    SliceQueue(size_t producers, size_t consumers, size_t capacity = 4096);
    SliceQueue(const SliceQueue&) = delete;
    SliceQueue& operator=(const SliceQueue&) = delete;
    SliceQueue(SliceQueue&&) = delete;
    SliceQueue& operator=(SliceQueue&&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Releases undelivered Slices after all worker handles have been destroyed.
     */
    ~SliceQueue();
    /** ------------------------------------------------------------------------------------------- Bind Producer
     * @brief Binds a unique producer index with at most one producer binding per calling thread.
     */
    Producer producer(size_t index);
    /** ------------------------------------------------------------------------------------------- Bind Consumer
     * @brief Binds a unique consumer index with all configured consumers participating until drain.
     */
    Consumer consumer(size_t index);
    /** ------------------------------------------------------------------------------------------- Close
     * @brief Wakes consumers after all producers have finished and flushed their partial blocks.
     */
    void close() noexcept;
    /** ------------------------------------------------------------------------------------------- Reset
     * @brief Reopens a fully drained queue while every worker is quiescent.
     */
    void reset() noexcept;
};
/** --------------------------------------------------------------------------------------------------------- PriorityWords
 * @struct PriorityWords
 * @brief The packed slot word shared by the priority containers: 32 key bits above a 32-bit value.
 */
struct PriorityWords {
    /// @brief Three-way order over two 4-byte keys, negative when the left key pops first.
    using Compare = int (*)(const void* left_key, const void* right_key);
    /// @brief The all-ones word marking a free slot, which sorts last in the natural order.
    static constexpr uint64_t EMPTY = ~uint64_t(0);
    /// @brief The value reserved so no entry equals EMPTY; it is also the null pool index.
    static constexpr uint32_t NULL_VALUE = 0xFFFFFFFFu;
    /// @brief Fixed header bytes ahead of a container's words.
    static constexpr size_t HEADER_BYTES = 64;
    /** ------------------------------------------------------------------------------------------- Pack
     * @brief Packs key bits above a value into one slot word.
     * @param key_bits The 32 key bits.
     * @param value The 32-bit value.
     * @return The slot word.
     */
    static constexpr uint64_t pack(uint32_t key_bits, uint32_t value) noexcept {
        return (static_cast<uint64_t>(key_bits) << 32) | value;
    }
    /// @brief The key bits of a slot word.
    static constexpr uint32_t key_of(uint64_t word) noexcept {
        return static_cast<uint32_t>(word >> 32);
    }
    /// @brief The value of a slot word.
    static constexpr uint32_t value_of(uint64_t word) noexcept {
        return static_cast<uint32_t>(word);
    }
};
/** --------------------------------------------------------------------------------------------------------- SliceHandle
 * @struct SliceHandle
 * @brief Moves a Slice's reference in and out of a bare pool index for containers that store ids.
 */
struct SliceHandle {
    SliceHandle() = delete;
    /** ------------------------------------------------------------------------------------------- Detach
     * @brief Takes a Slice's pool index along with its reference, leaving the Slice null.
     * @param slice The Slice whose reference the container takes.
     * @return The pool index.
     */
    static uint32_t detach(Slice&& slice) noexcept {
        const uint32_t value = slice.id_;
        slice.id_ = 0xFFFFFFFFu;
        return value;
    }
    /** ------------------------------------------------------------------------------------------- Adopt
     * @brief Wraps a pool index whose reference the container holds into an owning Slice.
     * @param value The pool index.
     * @return The owning Slice.
     */
    static Slice adopt(uint32_t value) noexcept {
        Slice adopted;
        adopted.id_ = value;
        return adopted;
    }
};
/** --------------------------------------------------------------------------------------------------------- PrioritySlice
 * @class PrioritySlice
 * @brief Lock-free bounded priority queue of packed key and value words living in one Slice.
 *
 * Every entry is one 64-bit slot word holding 32 key bits in its high half and a 32-bit value in
 * its low half, so a Slice's pool index or any other 32-bit handle rides along with its key.
 * Without a comparator the unsigned word order is the priority order, so the caller hands in
 * sortable key bits and ties resolve by value; with a comparator every scan hands it pointers to
 * copies of the two 4-byte keys. A push takes a free slot, or evicts the worst entry of its
 * snapshot once every slot is taken and the newcomer beats it; a pop removes the best entry of
 * its snapshot. A 64-byte header ahead of the slots caches the current worst word, the free slot
 * count, and the last freed slot, so a push that cannot enter is rejected without a scan and a
 * push after a pop lands with one CAS. In natural order two caches behind the header hold each
 * 32-slot block's largest and smallest word, so an eviction or a pop scans the block caches and
 * then one block rather than every slot; the caches are hints that every scan re-checks. A lost
 * CAS is another thread's completed operation. Ordering is exact under push-only load and
 * relaxed under mixed load, where a pop landing between a push's scan and its CAS can let that
 * push keep a slightly worse entry.
 */
class PrioritySlice : public PriorityWords {
public:
    /// @brief Header position storing the declared capacity before granule padding.
    static constexpr size_t CAPACITY_WORD = 7;
    /// @brief Slot words per block the natural-order caches summarize.
    static constexpr size_t BLOCK_WORDS = 32;
    /** ------------------------------------------------------------------------------------------- Header Bytes
     * @brief Bytes ahead of the slot words for a capacity: the header plus two block caches.
     * @param capacity The slot count.
     * @return The byte offset of the first slot word.
     */
    static constexpr size_t header_bytes(size_t capacity) noexcept {
        const size_t blocks = (capacity + BLOCK_WORDS - 1) / BLOCK_WORDS;
        const size_t cache = (blocks * sizeof(uint64_t) + 63) & ~size_t(63);
        return HEADER_BYTES + 2 * cache;
    }
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Claims storage for a fixed number of entries, all free.
     * @param capacity The entry count the queue trims itself to.
     * @param compare The key order, or nullptr for the natural unsigned word order.
     * @param novel_buffer Whether the storage is its own buffer rather than a slab claim.
     * @param placement The placement the storage is claimed from.
     */
    explicit PrioritySlice(
        size_t capacity,
        Compare compare = nullptr,
        bool novel_buffer = false,
        const BuffetDescriptor* placement = Slice::default_placement()
    );
    /** ------------------------------------------------------------------------------------------- Constructor - Adopt
     * @brief Reads capacity from CAPACITY_WORD and rebuilds caches over occupied and EMPTY words.
     * @param storage The Slice holding the header, the block caches, and the slot words.
     * @param compare The key order, or nullptr for the natural unsigned word order.
     */
    explicit PrioritySlice(Slice storage, Compare compare = nullptr);
    /** ------------------------------------------------------------------------------------------- Move only */
    PrioritySlice(const PrioritySlice&) = delete;
    PrioritySlice& operator=(const PrioritySlice&) = delete;
    PrioritySlice(PrioritySlice&& other) noexcept;
    PrioritySlice& operator=(PrioritySlice&& other) noexcept;
    ~PrioritySlice() = default;
    /** ------------------------------------------------------------------------------------------- Push
     * @brief Enters a word by taking a free slot or evicting the worst entry it beats, rejecting
     * without a scan whenever the queue is full and the word does not beat the cached threshold.
     * @param word The packed entry.
     * @return EMPTY when a free slot was taken, the evicted word, or `word` itself when rejected.
     */
    uint64_t push(uint64_t word) noexcept;
    /** ------------------------------------------------------------------------------------------- Push Key and Value
     * @brief Enters a key and value pair.
     * @param key_bits The 32 key bits.
     * @param value The 32-bit value.
     * @return EMPTY when a free slot was taken, the evicted word, or the packed word when
     * rejected.
     */
    uint64_t push(uint32_t key_bits, uint32_t value) noexcept {
        return push(pack(key_bits, value));
    }
    /** ------------------------------------------------------------------------------------------- Pop
     * @brief Removes and returns the best entry, or EMPTY when no slot holds one.
     * @return The popped word.
     */
    uint64_t pop() noexcept;
    /** ------------------------------------------------------------------------------------------- Take
     * @brief Frees one slot and returns the word it held, EMPTY when it was already free.
     * @param index The slot.
     * @return The word the slot held.
     */
    uint64_t take(size_t index) noexcept;
    /** ------------------------------------------------------------------------------------------- Best
     * @brief Returns the best entry without removing it, or EMPTY when no slot holds one.
     * @return The best word.
     */
    uint64_t best() const noexcept;
    /** ------------------------------------------------------------------------------------------- Worst
     * @brief Returns the entry the next eviction removes, or EMPTY while a slot is free.
     * @return The worst word.
     */
    uint64_t worst() const noexcept;
    /** ------------------------------------------------------------------------------------------- Accepts
     * @brief Reports from the header, without a scan, whether pushing this word would enter.
     * @param word The packed entry.
     * @return True when a slot is free or the word beats the cached threshold.
     */
    bool accepts(uint64_t word) const noexcept;
    /** ------------------------------------------------------------------------------------------- Clear
     * @brief Frees every slot without releasing the values they held.
     */
    void clear() noexcept;
    /** ------------------------------------------------------------------------------------------- Size
     * @brief Counts the slots holding an entry, exact once every operation has completed.
     * @return The entry count.
     */
    size_t size() const noexcept;
    /// @brief The slot count.
    size_t capacity() const noexcept { return capacity_; }
    /// @brief Whether no slot holds an entry.
    bool empty() const noexcept { return best() == EMPTY; }
    /// @brief Whether every slot holds an entry.
    bool full() const noexcept { return worst() != EMPTY; }
    /// @brief The key order, or nullptr for the natural order.
    Compare compare() const noexcept { return compare_; }
    /** ------------------------------------------------------------------------------------------- Storage
     * @brief Another view of the header, block caches, and slot words, for kernels or transport.
     * @return A view sharing the storage.
     */
    Slice storage() const { return storage_.slice(); }
    /// @brief The slot words behind the header and block caches.
    const uint64_t* words() const noexcept { return words_; }
private:
    Slice storage_;  ///< The header, the block caches, and the slot words.
    uint64_t* header_ = nullptr;  ///< Threshold word, free slot count, free slot hint.
    uint64_t* max_cache_ = nullptr;  ///< Per-block largest occupied word.
    uint64_t* min_cache_ = nullptr;  ///< Per-block smallest word, EMPTY for a block with none.
    uint64_t* words_ = nullptr;  ///< The slot words.
    size_t capacity_ = 0;  ///< Slot count.
    size_t blocks_ = 0;  ///< Block count.
    Compare compare_ = nullptr;  ///< Key order, nullptr for the natural order.
    size_t block_length(size_t block) const noexcept;
    uint64_t push_natural(uint64_t word) noexcept;
    uint64_t pop_natural() noexcept;
    uint64_t push_ordered(uint64_t word) noexcept;
    uint64_t pop_ordered() noexcept;
    void fill_caches(size_t index, uint64_t word) noexcept;
    void advance_hint(size_t filled) noexcept;
    void note_free(size_t index) noexcept;
    int64_t free_from_ring() const noexcept;
    int64_t free_slot_after(size_t hint) const noexcept;
    void arm_natural() noexcept;
    void arm_natural(uint64_t threshold) noexcept;
    void arm_ordered(uint64_t threshold) noexcept;
    int64_t worst_ordered(uint64_t& worst, uint64_t& runner_up) const noexcept;
    int64_t best_ordered(uint64_t& best) const noexcept;
    bool better(uint64_t word, uint64_t than) const noexcept;
    uint64_t worse_of(uint64_t left, uint64_t right) const noexcept;
};
/** --------------------------------------------------------------------------------------------------------- HeapSlice
 * @class HeapSlice
 * @brief Bounded min-max heap of packed key and value words living in one Slice, for one owner.
 *
 * The same slot words as PrioritySlice, arranged as a min-max heap behind a 64-byte header
 * holding the element count, so the best entry pops and the worst entry is evicted in
 * logarithmic time with no scans. It is not thread-safe: one owner mutates it at a time.
 */
class HeapSlice : public PriorityWords {
public:
    /// @brief Header position storing the declared capacity before granule padding.
    static constexpr size_t CAPACITY_WORD = 1;
    /** ------------------------------------------------------------------------------------------- Header Bytes
     * @brief Bytes ahead of the heap positions, the same for every capacity.
     * @return HEADER_BYTES.
     */
    static constexpr size_t header_bytes(size_t) noexcept { return HEADER_BYTES; }
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Claims storage for a fixed number of entries, all free.
     * @param capacity The entry count the heap trims itself to.
     * @param compare The key order, or nullptr for the natural unsigned word order.
     * @param novel_buffer Whether the storage is its own buffer rather than a slab claim.
     * @param placement The placement the storage is claimed from.
     */
    explicit HeapSlice(
        size_t capacity,
        Compare compare = nullptr,
        bool novel_buffer = false,
        const BuffetDescriptor* placement = Slice::default_placement()
    );
    /** ------------------------------------------------------------------------------------------- Constructor - Adopt
     * @brief Adopts a heap with count in word zero and capacity in CAPACITY_WORD.
     * @param storage The Slice holding the header and the heap positions.
     * @param compare The key order, or nullptr for the natural unsigned word order.
     */
    explicit HeapSlice(Slice storage, Compare compare = nullptr);
    /** ------------------------------------------------------------------------------------------- Move only */
    HeapSlice(const HeapSlice&) = delete;
    HeapSlice& operator=(const HeapSlice&) = delete;
    HeapSlice(HeapSlice&& other) noexcept;
    HeapSlice& operator=(HeapSlice&& other) noexcept;
    ~HeapSlice() = default;
    /** ------------------------------------------------------------------------------------------- Push
     * @brief Enters a word, evicting the worst entry it beats once full.
     * @param word The packed entry.
     * @return EMPTY when inserted, the evicted word, or `word` itself when rejected.
     */
    uint64_t push(uint64_t word) noexcept;
    /** ------------------------------------------------------------------------------------------- Push Key and Value
     * @brief Enters a key and value pair.
     * @param key_bits The 32 key bits.
     * @param value The 32-bit value.
     * @return EMPTY when inserted, the evicted word, or the packed word when rejected.
     */
    uint64_t push(uint32_t key_bits, uint32_t value) noexcept {
        return push(pack(key_bits, value));
    }
    /** ------------------------------------------------------------------------------------------- Pop
     * @brief Removes and returns the best entry, or EMPTY when the heap is empty.
     * @return The popped word.
     */
    uint64_t pop() noexcept;
    /** ------------------------------------------------------------------------------------------- Take
     * @brief Removes the word at a heap position, EMPTY when the position is past the count.
     * @param index The heap position.
     * @return The removed word.
     */
    uint64_t take(size_t index) noexcept;
    /** ------------------------------------------------------------------------------------------- Best
     * @brief Returns the best entry without removing it, or EMPTY when the heap is empty.
     * @return The best word.
     */
    uint64_t best() const noexcept;
    /** ------------------------------------------------------------------------------------------- Worst
     * @brief Returns the entry the next eviction removes, or EMPTY while room remains.
     * @return The worst word.
     */
    uint64_t worst() const noexcept;
    /** ------------------------------------------------------------------------------------------- Accepts
     * @brief Reports whether pushing this word would enter the heap.
     * @param word The packed entry.
     * @return True while room remains or when the word beats the worst entry.
     */
    bool accepts(uint64_t word) const noexcept;
    /** ------------------------------------------------------------------------------------------- Clear
     * @brief Forgets every entry without releasing the values they held.
     */
    void clear() noexcept;
    /** ------------------------------------------------------------------------------------------- Size
     * @brief The entry count.
     * @return The entry count.
     */
    size_t size() const noexcept;
    /// @brief The position count.
    size_t capacity() const noexcept { return capacity_; }
    /// @brief Whether the heap holds no entry.
    bool empty() const noexcept { return size() == 0; }
    /// @brief Whether every position holds an entry.
    bool full() const noexcept { return size() == capacity_; }
    /// @brief The key order, or nullptr for the natural order.
    Compare compare() const noexcept { return compare_; }
    /** ------------------------------------------------------------------------------------------- Storage
     * @brief Another view of the header and heap positions, for kernels or transport.
     * @return A view sharing the storage.
     */
    Slice storage() const { return storage_.slice(); }
    /// @brief The heap positions behind the header.
    const uint64_t* words() const noexcept { return words_; }
private:
    Slice storage_;  ///< The header and the heap positions.
    uint64_t* header_ = nullptr;  ///< The element count.
    uint64_t* words_ = nullptr;  ///< The heap positions.
    size_t capacity_ = 0;  ///< Position count.
    Compare compare_ = nullptr;  ///< Key order, nullptr for the natural order.
};
/** --------------------------------------------------------------------------------------------------------- PriorityT
 * @class PriorityT
 * @brief Typed layer over a packed-word priority container: a key of at most 4 bytes and a
 * 4-byte value or an owned Slice.
 * @tparam Queue PrioritySlice or HeapSlice.
 * @tparam K The key type, trivially copyable and 1, 2 or 4 bytes wide.
 * @tparam V Slice, whose reference the container owns, or any trivially copyable 4-byte type.
 * @tparam Order The three-way key order, or nullptr for ascending arithmetic order.
 */
template<typename Queue, typename K, typename V, int (*Order)(const K*, const K*)>
class PriorityT final : public Queue {
    static_assert(
        std::is_trivially_copyable_v<K> && (sizeof(K) == 1 || sizeof(K) == 2 || sizeof(K) == 4),
        "PriorityT keys must be trivially copyable and 1, 2 or 4 bytes wide"
    );
    /// @brief Whether values are Slices whose references the container owns.
    static constexpr bool owns_slices = std::is_same_v<V, Slice>;
    static_assert(owns_slices || (std::is_trivially_copyable_v<V> && sizeof(V) == 4),
        "PriorityT values must be Slice or a trivially copyable 4-byte type");
    /// @brief The unsigned type as wide as K.
    using RawBits = std::conditional_t<sizeof(K) == 4, uint32_t,
        std::conditional_t<sizeof(K) == 2, uint16_t, uint8_t>>;
    /** ------------------------------------------------------------------------------------------- Encode
     * @brief Maps a key to its slot bits: sortable bits in natural order, raw bits under a
     * comparator.
     * @param key The key.
     * @return The 32 key bits.
     */
    static uint32_t encode(K key) noexcept {
        if constexpr (Order != nullptr || !std::is_arithmetic_v<K>) {
            return static_cast<uint32_t>(std::bit_cast<RawBits>(key));
        } else if constexpr (std::is_floating_point_v<K>) {
            const uint32_t bits = std::bit_cast<uint32_t>(key);
            return bits ^ (static_cast<uint32_t>(static_cast<int32_t>(bits) >> 31) | 0x80000000u);
        } else if constexpr (std::is_signed_v<K>) {
            return static_cast<uint32_t>(static_cast<int32_t>(key)) ^ 0x80000000u;
        } else {
            return static_cast<uint32_t>(key);
        }
    }
    /** ------------------------------------------------------------------------------------------- Decode
     * @brief Inverts encode.
     * @param bits The 32 key bits.
     * @return The key.
     */
    static K decode(uint32_t bits) noexcept {
        if constexpr (Order != nullptr || !std::is_arithmetic_v<K>) {
            return std::bit_cast<K>(static_cast<RawBits>(bits));
        } else if constexpr (std::is_floating_point_v<K>) {
            return std::bit_cast<K>(bits ^ ((bits >> 31) != 0 ? 0x80000000u : 0xFFFFFFFFu));
        } else if constexpr (std::is_signed_v<K>) {
            return static_cast<K>(static_cast<int32_t>(bits ^ 0x80000000u));
        } else {
            return static_cast<K>(bits);
        }
    }
    /** ------------------------------------------------------------------------------------------- Erased Compare
     * @brief Static trampoline handing the erased scan's key copies to Order.
     * @param left The left key bits.
     * @param right The right key bits.
     * @return Order's result.
     */
    static int erased_compare(const void* left, const void* right) {
        return Order(static_cast<const K*>(left), static_cast<const K*>(right));
    }
    /// @brief The erased comparator: nullptr keeps the natural order.
    static constexpr PriorityWords::Compare erased() noexcept {
        if constexpr (Order == nullptr) {
            return nullptr;
        } else {
            return &erased_compare;
        }
    }
public:
    /// @brief Encodes a typed key for the natural packed-word priority order.
    static uint32_t key_bits(K key) noexcept { return encode(key); }
    /// @brief Decodes a key from its packed-word bits.
    static K key_from_bits(uint32_t bits) noexcept { return decode(bits); }
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Claims storage for a fixed number of entries, all free.
     * @param capacity The entry count the container trims itself to.
     * @param novel_buffer Whether the storage is its own buffer rather than a slab claim.
     * @param placement The placement the storage is claimed from.
     */
    explicit PriorityT(
        size_t capacity,
        bool novel_buffer = false,
        const BuffetDescriptor* placement = Slice::default_placement()
    ) : Queue(capacity, erased(), novel_buffer, placement) {}
    /** ------------------------------------------------------------------------------------------- Constructor - Adopt
     * @brief Views existing storage laid out as the container expects.
     * @param storage The Slice holding the container.
     */
    explicit PriorityT(Slice storage) : Queue(std::move(storage), erased()) {}
    /** ------------------------------------------------------------------------------------------- Move only */
    PriorityT(const PriorityT&) = delete;
    PriorityT& operator=(const PriorityT&) = delete;
    PriorityT(PriorityT&& other) noexcept = default;
    /** ------------------------------------------------------------------------------------------- Move Assignment
     * @brief Releases destination payloads before transferring the source storage.
     * @param other The container whose storage and payloads are transferred.
     * @return This container.
     */
    PriorityT& operator=(PriorityT&& other) noexcept {
        if (this != &other) {
            if constexpr (owns_slices) clear();
            Queue::operator=(std::move(other));
        }
        return *this;
    }
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Releases every owned Slice still held.
     */
    ~PriorityT() { clear(); }
    /** ------------------------------------------------------------------------------------------- Push
     * @brief Enters a key and value, evicting the worst entry once full.
     * @param key The key.
     * @param value The value.
     * @return True when the entry entered, false when the container was full of better entries.
     */
    bool push(K key, V value) noexcept requires (!owns_slices) {
        const uint64_t word = PriorityWords::pack(encode(key), std::bit_cast<uint32_t>(value));
        return Queue::push(word) != word;
    }
    /** ------------------------------------------------------------------------------------------- Push Slice
     * @brief Enters a key and takes the Slice's reference, releasing whichever entry loses its
     * place.
     * @param key The key.
     * @param value The Slice the container takes ownership of.
     * @return True when the entry entered, false when it was released as the worst.
     */
    bool push(K key, Slice&& value) noexcept requires owns_slices {
        const uint32_t handle = SliceHandle::detach(std::move(value));
        const uint64_t word = PriorityWords::pack(encode(key), handle);
        const uint64_t displaced = Queue::push(word);
        if (displaced != PriorityWords::EMPTY) {
            SliceHandle::adopt(PriorityWords::value_of(displaced));
        }
        return displaced != word;
    }
    /** ------------------------------------------------------------------------------------------- Pop
     * @brief Removes the best entry into key and value, leaving both untouched when none is
     * held.
     * @param key Receives the key.
     * @param value Receives the value.
     * @return True when an entry was removed.
     */
    bool pop(K& key, V& value) noexcept requires (!owns_slices) {
        const uint64_t word = Queue::pop();
        if (word == PriorityWords::EMPTY) return false;
        key = decode(PriorityWords::key_of(word));
        value = std::bit_cast<V>(PriorityWords::value_of(word));
        return true;
    }
    /** ------------------------------------------------------------------------------------------- Pop Slice
     * @brief Removes the best entry, handing its Slice reference to the caller.
     * @param key Receives the key.
     * @param value Receives the owning Slice.
     * @return True when an entry was removed.
     */
    bool pop(K& key, Slice& value) noexcept requires owns_slices {
        const uint64_t word = Queue::pop();
        if (word == PriorityWords::EMPTY) return false;
        key = decode(PriorityWords::key_of(word));
        value = SliceHandle::adopt(PriorityWords::value_of(word));
        return true;
    }
    /** ------------------------------------------------------------------------------------------- Best
     * @brief Reads the best entry without removing it.
     * @param key Receives the key.
     * @param value Receives the value.
     * @return True when an entry was found.
     */
    bool best(K& key, V& value) const noexcept requires (!owns_slices) {
        const uint64_t word = Queue::best();
        if (word == PriorityWords::EMPTY) return false;
        key = decode(PriorityWords::key_of(word));
        value = std::bit_cast<V>(PriorityWords::value_of(word));
        return true;
    }
    /** ------------------------------------------------------------------------------------------- Best Key
     * @brief Reads the key of the best entry without removing it.
     * @param key Receives the key.
     * @return True when an entry was found.
     */
    bool best(K& key) const noexcept {
        const uint64_t word = Queue::best();
        if (word == PriorityWords::EMPTY) return false;
        key = decode(PriorityWords::key_of(word));
        return true;
    }
    /** ------------------------------------------------------------------------------------------- Worst
     * @brief Reads the key the next eviction removes.
     * @param key Receives the key.
     * @return True when the container is full, false while room remains.
     */
    bool worst(K& key) const noexcept {
        const uint64_t word = Queue::worst();
        if (word == PriorityWords::EMPTY) return false;
        key = decode(PriorityWords::key_of(word));
        return true;
    }
    /** ------------------------------------------------------------------------------------------- Accepts
     * @brief Reports whether a push with this key would enter, counting a key equal to the worst
     * as rejected even though the natural order lets a smaller value win the tie.
     * @param key The key.
     * @return True while room remains or when the key beats the worst key.
     */
    bool accepts(K key) const noexcept {
        return Queue::accepts(PriorityWords::pack(encode(key), PriorityWords::NULL_VALUE - 1));
    }
    /** ------------------------------------------------------------------------------------------- Clear
     * @brief Removes every entry, releasing owned Slices.
     */
    void clear() noexcept {
        if constexpr (owns_slices) {
            for (uint64_t word = Queue::pop(); word != PriorityWords::EMPTY; word = Queue::pop()) {
                SliceHandle::adopt(PriorityWords::value_of(word));
            }
        } else {
            Queue::clear();
        }
    }
};
/// @brief Typed lock-free PrioritySlice.
template<typename K, typename V, int (*Order)(const K*, const K*) = nullptr>
using PrioritySliceT = PriorityT<PrioritySlice, K, V, Order>;
/// @brief Typed single-owner HeapSlice.
template<typename K, typename V, int (*Order)(const K*, const K*) = nullptr>
using HeapSliceT = PriorityT<HeapSlice, K, V, Order>;
template<typename Key>
class SliceCacheIterator;
template<typename Key = int64_t,
    SliceCacheIterator<Key> (*Evict)(SliceCacheIterator<Key>, SliceCacheIterator<Key>) noexcept
        = nullptr>
class SliceCache;
/** --------------------------------------------------------------------------------------------------------- Slice Cache Iterator
 * @class SliceCacheIterator
 * @brief Borrows immutable key and Slice pairs in most-to-least recently used order.
 * @tparam Key The cache key type.
 */
template<typename Key>
class SliceCacheIterator {
private:
    /** ------------------------------------------------------------------------------------------- Link
     * @brief Links entries in most-to-least recently used order.
     */
    struct Link {
        mutable const Link* previous = this;
        mutable const Link* next = this;
    };
    /** ------------------------------------------------------------------------------------------- Entry
     * @brief Holds one immutable key and its owned Slice in a stable hash node.
     */
    struct Entry : Link {
        const size_t hash;
        mutable std::pair<const Key, Slice> value;
        /** --------------------------------------------------------------------------------- Constructor
         * @brief Copies the key and takes ownership of the Slice.
         */
        Entry(const Key& key, Slice&& slice)
            : hash(std::hash<Key>{}(key)), value(key, std::move(slice)) {}
    };
    const Link* entry_ = nullptr;
    explicit SliceCacheIterator(const Link* entry) noexcept : entry_(entry) {}
    template<typename CacheKey,
        SliceCacheIterator<CacheKey> (*Select)(
            SliceCacheIterator<CacheKey>, SliceCacheIterator<CacheKey>
        ) noexcept>
    friend class SliceCache;
public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = std::pair<const Key, Slice>;
    using difference_type = std::ptrdiff_t;
    using pointer = const value_type*;
    using reference = const value_type&;
    SliceCacheIterator() noexcept = default;
    reference operator*() const noexcept {
        return static_cast<const Entry*>(entry_)->value;
    }
    pointer operator->() const noexcept { return &operator*(); }
    SliceCacheIterator& operator++() noexcept {
        entry_ = entry_->next;
        return *this;
    }
    SliceCacheIterator operator++(int) noexcept {
        SliceCacheIterator previous = *this;
        ++*this;
        return previous;
    }
    SliceCacheIterator& operator--() noexcept {
        entry_ = entry_->previous;
        return *this;
    }
    SliceCacheIterator operator--(int) noexcept {
        SliceCacheIterator previous = *this;
        --*this;
        return previous;
    }
    bool operator==(const SliceCacheIterator&) const noexcept = default;
};
/** --------------------------------------------------------------------------------------------------------- SliceCache
 * @class SliceCache
 * @brief Owns Slices within a byte budget with compile-time eviction for one mutating owner.
 * @tparam Key The copyable key type, supporting std::hash<Key> and equality.
 * @tparam Evict A noexcept selector returning an eligible iterator, or nullptr for LRU eviction.
 * @pre Evict returns within its nonempty [first, last) range without reentering the cache.
 */
template<typename Key,
    SliceCacheIterator<Key> (*Evict)(SliceCacheIterator<Key>, SliceCacheIterator<Key>) noexcept>
class SliceCache {
private:
    using Link = typename SliceCacheIterator<Key>::Link;
    using Entry = typename SliceCacheIterator<Key>::Entry;
    /** ------------------------------------------------------------------------------------------- Identity
     * @brief Locates an owned node without invoking user key operations during eviction.
     */
    struct Identity {
        const Entry* entry;
    };
    /** ------------------------------------------------------------------------------------------- Hash
     * @brief Hashes stored entries and lookup keys identically.
     */
    struct Hash {
        using is_transparent = void;
        size_t operator()(const Entry& entry) const noexcept { return entry.hash; }
        size_t operator()(Identity identity) const noexcept { return identity.entry->hash; }
        size_t operator()(const Key& key) const { return std::hash<Key>{}(key); }
    };
    /** ------------------------------------------------------------------------------------------- Equal
     * @brief Compares immutable keys without constructing temporary entries.
     */
    struct Equal {
        using is_transparent = void;
        bool operator()(const Entry& left, const Entry& right) const {
            return left.value.first == right.value.first;
        }
        bool operator()(const Entry& left, const Key& right) const {
            return left.value.first == right;
        }
        bool operator()(const Key& left, const Entry& right) const {
            return left == right.value.first;
        }
        bool operator()(const Entry& left, Identity right) const noexcept {
            return &left == right.entry;
        }
        bool operator()(Identity left, const Entry& right) const noexcept {
            return left.entry == &right;
        }
    };
    using Map = std::unordered_set<Entry, Hash, Equal>;
    Map map_;
    Link order_;
    size_t bytes_ = 0;
    size_t capacity_;
    /** ------------------------------------------------------------------------------------------- Unlink
     * @brief Removes a linked entry from its recency order.
     */
    static void unlink(const Link& entry) noexcept {
        entry.previous->next = entry.next;
        entry.next->previous = entry.previous;
    }
    /** ------------------------------------------------------------------------------------------- Prepend
     * @brief Places an unlinked entry at the most recently used end.
     */
    void prepend(const Link& entry) noexcept {
        entry.previous = &order_;
        entry.next = order_.next;
        order_.next->previous = &entry;
        order_.next = &entry;
    }
    /** ------------------------------------------------------------------------------------------- Promote
     * @brief Moves a linked entry to the most recently used end.
     */
    void promote(const Link& entry) noexcept {
        unlink(entry);
        prepend(entry);
    }
    /** ------------------------------------------------------------------------------------------- Remove
     * @brief Releases an indexed entry and removes its byte weight.
     */
    void remove(typename Map::const_iterator entry) {
        bytes_ -= entry->value.second.size_bytes();
        unlink(*entry);
        map_.erase(entry);
    }
    /** ------------------------------------------------------------------------------------------- Evict
     * @brief Releases the policy-selected entry from a nonempty eligible recency range.
     * @param first The most recently used eligible entry.
     */
    void evict(const Link* first) {
        const Link* victim;
        if constexpr (Evict == nullptr) {
            victim = order_.previous;
        } else {
            victim = Evict(const_iterator(first), end()).entry_;
        }
        remove(map_.find(Identity{static_cast<const Entry*>(victim)}));
    }
    /** ------------------------------------------------------------------------------------------- Take Order
     * @brief Reattaches transferred entries and empties the source order.
     */
    void take_order(SliceCache& other) noexcept {
        if (!map_.empty()) {
            order_.next = other.order_.next;
            order_.previous = other.order_.previous;
            order_.next->previous = &order_;
            order_.previous->next = &order_;
        }
        other.order_.next = other.order_.previous = &other.order_;
    }
public:
    using const_iterator = SliceCacheIterator<Key>;
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Creates an empty cache with a byte budget.
     * @param max_bytes The total Slice bytes kept before eviction begins.
     */
    explicit SliceCache(size_t max_bytes) : capacity_(max_bytes) {}
    /** ------------------------------------------------------------------------------------------- Move only */
    SliceCache(const SliceCache&) = delete;
    SliceCache& operator=(const SliceCache&) = delete;
    SliceCache(SliceCache&& other) noexcept
        : map_(std::move(other.map_)), bytes_(std::exchange(other.bytes_, 0)),
          capacity_(std::exchange(other.capacity_, 0)) {
        take_order(other);
    }
    SliceCache& operator=(SliceCache&& other) noexcept {
        if (this != &other) {
            clear();
            map_ = std::move(other.map_);
            bytes_ = std::exchange(other.bytes_, 0);
            capacity_ = std::exchange(other.capacity_, 0);
            take_order(other);
        }
        return *this;
    }
    /** ------------------------------------------------------------------------------------------- Set
     * @brief Stores and protects a Slice while evicting policy-selected entries to meet the budget.
     * @param key The key.
     * @param value The owned Slice, kept alone when larger than the budget.
     */
    void set(const Key& key, Slice&& value) {
        const size_t weight = value.size_bytes();
        auto found = map_.find(key);
        if (found == map_.end()) {
            found = map_.emplace(key, std::move(value)).first;
            prepend(*found);
        } else {
            bytes_ -= found->value.second.size_bytes();
            found->value.second = std::move(value);
            promote(*found);
        }
        const size_t remaining = capacity_ - std::min(capacity_, weight);
        while (map_.size() > 1 && (bytes_ > remaining || weight > capacity_)) {
            evict(order_.next->next);
        }
        bytes_ += weight;
    }
    /** ------------------------------------------------------------------------------------------- Get
     * @brief Borrows a key's Slice and marks it most recently used.
     * @param key The key.
     * @return The cached Slice, valid until it is evicted or erased, or nullptr when absent.
     */
    const Slice* get(const Key& key) {
        const auto found = map_.find(key);
        if (found == map_.end()) return nullptr;
        promote(*found);
        return &found->value.second;
    }
    /** ------------------------------------------------------------------------------------------- Peek
     * @brief Borrows a key's Slice without touching its recency.
     * @param key The key.
     * @return The cached Slice, or nullptr when absent.
     */
    const Slice* peek(const Key& key) const {
        const auto found = map_.find(key);
        return found == map_.end() ? nullptr : &found->value.second;
    }
    /** ------------------------------------------------------------------------------------------- View
     * @brief Returns an owning view of a key's Slice that outlives eviction, marking it most
     * recently used.
     * @param key The key.
     * @return A view sharing the Slice, or a null Slice when absent.
     */
    Slice view(const Key& key) {
        const Slice* found = get(key);
        return found == nullptr ? Slice() : found->slice();
    }
    /// @brief Whether a key is cached.
    bool exists(const Key& key) const { return map_.find(key) != map_.end(); }
    /** ------------------------------------------------------------------------------------------- Erase
     * @brief Drops a key and releases its Slice.
     * @param key The key.
     */
    void erase(const Key& key) {
        const auto found = map_.find(key);
        if (found != map_.end()) remove(found);
    }
    /// @brief The entry count.
    size_t size() const { return map_.size(); }
    /// @brief Whether no entry is cached.
    bool empty() const { return map_.empty(); }
    /// @brief The bytes the cached Slices weigh.
    size_t bytes() const { return bytes_; }
    /// @brief The byte budget.
    size_t capacity() const { return capacity_; }
    /** ------------------------------------------------------------------------------------------- Resize
     * @brief Changes the byte budget, evicting policy-selected entries without protection.
     * @param max_bytes The new budget.
     */
    void resize(size_t max_bytes) {
        capacity_ = max_bytes;
        while (bytes_ > capacity_) evict(order_.next);
    }
    /// @brief Releases every Slice.
    void clear() {
        map_.clear();
        order_.next = order_.previous = &order_;
        bytes_ = 0;
    }
    /// @brief Iterates from most to least recently used.
    const_iterator begin() const { return const_iterator(order_.next); }
    /// @brief The end of the iteration.
    const_iterator end() const { return const_iterator(&order_); }
};
/** --------------------------------------------------------------------------------------------------------- SliceChannel
 * @class SliceChannel
 * @brief Exchanges Slices across the network through listeners, one-shot sends, and replies.
 */
class SliceChannel {
public:
    enum class Protocol : uint8_t {
        TCP, UDP, RDMA, EncryptedTCP = 128, EncryptedUDP, EncryptedRDMA
    };
    SliceChannel() = delete;
    /** ------------------------------------------------------------------------------------------- Listen
     * @brief Starts receiving Slices on a port and protocol, invoking recv on the channel thread.
     */
    static void listen(uint16_t port, Protocol protocol, void (*recv)(Slice));
    /** ------------------------------------------------------------------------------------------- Close
     * @brief Closes the listener and pending exchanges for a port and protocol.
     */
    static void close(uint16_t port, Protocol protocol);
    /** ------------------------------------------------------------------------------------------- Send
     * @brief Sends a Slice, using an empty address inside recv to reply to its sender.
     * @param resp Receives the peer's reply or a null Slice on transport failure.
     */
    static void send(
        Slice slice, std::string address, uint16_t port, Protocol protocol,
        void (*resp)(Slice) = nullptr
    );
};
struct EncryptionKey;
/** --------------------------------------------------------------------------------------------------------- Is Big Endian
 * @brief Constexpr function to determine if the platform is big-endian.
 * @return True if the platform is big-endian, false otherwise.
 */
consteval bool is_big_endian() {
    return std::endian::native == std::endian::big;
}
/** --------------------------------------------------------------------------------------------------------- HashKey256Type
 * @enum HashKey256Type
 * @brief Enumeration of supported 256-bit hash key types for one-way hashing.
 */
enum class HashKey256Type : uint8_t {
    SHA256 = 0,            /// A SHA-256 hash
    CMAC256 = 1            /// A 256-bit CMAC using a secret key
};
/** --------------------------------------------------------------------------------------------------------- HashKey128Type
 * @enum HashKey128Type
 * @brief Enumeration of supported 128-bit hash key types for one-way hashing.
 */
enum class HashKey128Type : uint8_t {
    SHA256_TRUNCATED = 0,  /// A truncated SHA-256 hash (the first 128 bits of the full hash)
    HMAC128 = 1,           /// A 128-bit HMAC using a secret key
    CMAC128 = 2,           /// A 128-bit CMAC using a secret key
    NON_CRYPTO_128 = 3     /// A non-cryptographic 128-bit hash (e.g., CityHash, FarmHash)
};
/** --------------------------------------------------------------------------------------------------------- hash256_t
 * @struct hash256_t
 * @brief POD container for a 256-bit (32-byte) hash value.
 */
struct hash256_t {
    uint8_t bytes[32];
};
/** --------------------------------------------------------------------------------------------------------- HashKey256
 * @struct HashKey256
 * @brief POD container for a 256-bit (32-byte) hash key.
 * The POD structure is split up into multiple fields so we can quickly pull out a sub-hash
 * when we need to - no bit twiddling or shifting required.
 * 
 * NOTE: Here we have to make an exception to our "no memcpy" rule, because otherwise we end up
 * with endianness issues.
 */
template<HashKey256Type T = HashKey256Type::SHA256>
struct alignas(32) HashKey256 {
private:
    /// @brief The highest 192 bits of the hash key, stored as three 64-bit parts.
    uint64_t parts_[3];
    /// @brief The highest 64 bits of the hash key.
    uint32_t mid_;
    /// @brief The middle 32 bits of the hash key.
    uint16_t semi_mid_;
    /// @brief The next 8 bits of the hash key after the semi-middle 16 bits.
    uint8_t low_;
    /// @brief The lowest 8 bits of the hash key.
    uint8_t other_low_;
public:
    /** ------------------------------------------------------------------------------------------- HashKey256 Constructor
     * @brief Constructs a HashKey256 object.
     */
    HashKey256() = default;
    /** ------------------------------------------------------------------------------------------- HashKey256 Constructor with Data
     * @brief Constructs a HashKey256 object from raw data.
     * @param data Pointer to the input data.
     */
    HashKey256(void* data) {
        if constexpr (is_big_endian()) {
            // If we're on a big-endian platform, we need to reverse the byte order when copying
            uint8_t* bytes = reinterpret_cast<uint8_t*>(this);
            uint8_t* data_bytes = reinterpret_cast<uint8_t*>(data);
            for (size_t i = 0; i < 32; ++i) {
                bytes[i] = data_bytes[31 - i];
            }
        } else {
             std::memcpy(this, data, sizeof(HashKey256));
        }
    }
    /** ------------------------------------------------------------------------------------------- Default Copy/Move */
    HashKey256(const HashKey256&) = default;
    HashKey256& operator=(const HashKey256&) = default;
    HashKey256(HashKey256&&) = default;
    HashKey256& operator=(HashKey256&&) = default;
    /** ------------------------------------------------------------------------------------------- HashKey256 Constructor with hash256_t
     * @brief Constructs a HashKey256 object from a hash256_t object.
     * @param hash The input hash256_t object.
     */
    HashKey256(const hash256_t& hash) {
        std::memcpy(this, &hash, sizeof(HashKey256));
    }
    /** ------------------------------------------------------------------------------------------- Assignment Operator with hash256_t
     * @brief Assigns a hash256_t object to the HashKey256 object.
     * @param hash The input hash256_t object.
     * @return Reference to the updated HashKey256 object.
     */
    HashKey256& operator=(const hash256_t& hash) {
        if (this != &hash) {
            std::memcpy(this, &hash, sizeof(HashKey256));
        }
        return *this;
    }
    /** ------------------------------------------------------------------------------------------- Sub64
     * @brief Retrieves a 64-bit subpart of the hash.
     * @param index Index of the 64-bit part to retrieve.
     * @return The 64-bit subpart of the hash.
     */
    uint64_t sub64(size_t index = 0) const { return parts_[index]; }
    /** ------------------------------------------------------------------------------------------- Sub32
     * @brief Retrieves a 32-bit subpart of the hash.
     * @return The 32-bit subpart of the hash.
     */
    uint32_t sub32() const { return mid_; }
    /** ------------------------------------------------------------------------------------------- Sub16
     * @brief Retrieves a 16-bit subpart of the hash.
     * @return The 16-bit subpart of the hash.
     */
    uint16_t sub16() const { return semi_mid_; }
    /** ------------------------------------------------------------------------------------------- Sub8
     * @brief Retrieves an 8-bit subpart of the hash.
     * @return The 8-bit subpart of the hash.
     */
    uint8_t sub8() const { return low_; }
    /** ------------------------------------------------------------------------------------------- Other Sub8
     * @brief Retrieves another 8-bit subpart of the hash.
     * @return The other 8-bit subpart of the hash.
     */
    uint8_t other_sub8() const { return other_low_; }
    /** ------------------------------------------------------------------------------------------- To Bytes
     * @brief Converts the hash to a byte array.
     * @return A unique pointer to the byte array representing the hash.
     */
    std::unique_ptr<uint8_t[]> to_bytes() const {
        auto bytes = std::make_unique<uint8_t[]>(32);
        std::memcpy(bytes.get(), this, 32);
        return bytes;
    }
    /** ------------------------------------------------------------------------------------------- To Hex
     * @brief Converts the hash to a hexadecimal string.
     * @return A string representing the hash in hexadecimal format.
     */
    std::string to_hex() const {
        const auto bytes = to_bytes();
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (size_t i = 0; i < 32; ++i) {
            oss << std::setw(2) << static_cast<int>(bytes[i]);
        }
        return oss.str();
    }
    /** ------------------------------------------------------------------------------------------- Stream Output Operator
     * @brief Outputs the hash as a hexadecimal string to the given output stream.
     * @param os The output stream.
     * @param key The HashKey256 object.
     * @return Reference to the output stream.
     */
    friend std::ostream& operator<<(std::ostream& os, const HashKey256& key) {
        std::string hex_str = key.to_hex();
        os << hex_str;
        return os;
    }
    /** ------------------------------------------------------------------------------------------- Equality Operator
     * @brief Checks if two HashKey256 objects are equal.
     * @param other The other HashKey256 object.
     * @return True if the objects are equal, false otherwise.
     */
    bool operator==(const HashKey256& other) const {
        return std::memcmp(this, &other, sizeof(HashKey256)) == 0;
    }
    /** ------------------------------------------------------------------------------------------- Inequality Operator
     * @brief Checks if two HashKey256 objects are not equal.
     * @param other The other HashKey256 object.
     * @return True if the objects are not equal, false otherwise.
     */
    bool operator!=(const HashKey256& other) const {
        return !(*this == other);
    }
    /** ------------------------------------------------------------------------------------------- Less Than Operator
     * @brief Checks if this HashKey256 object is less than another.
     * @param other The other HashKey256 object.
     * @return True if this object is less than the other, false otherwise.
     */
    bool operator<(const HashKey256& other) const {
        return std::memcmp(this, &other, sizeof(HashKey256)) < 0;
    }
    /** ------------------------------------------------------------------------------------------- Greater Than Operator
     * @brief Checks if this HashKey256 object is greater than another.
     * @param other The other HashKey256 object.
     * @return True if this object is greater than the other, false otherwise.
     */
    bool operator>(const HashKey256& other) const {
        return std::memcmp(this, &other, sizeof(HashKey256)) > 0;
    }
    /** ------------------------------------------------------------------------------------------- Less Than or Equal Operator
     * @brief Checks if this HashKey256 object is less than or equal to another.
     * @param other The other HashKey256 object.
     * @return True if this object is less than or equal to the other, false otherwise.
     */
    bool operator<=(const HashKey256& other) const {
        return !(*this > other);
    }
    /** ------------------------------------------------------------------------------------------- Greater Than or Equal Operator
     * @brief Checks if this HashKey256 object is greater than or equal to another.
     * @param other The other HashKey256 object.
     * @return True if this object is greater than or equal to the other, false otherwise.
     */
    bool operator>=(const HashKey256& other) const {
        return !(*this < other);
    }
    /** ------------------------------------------------------------------------------------------- Conversion Operator to hash256_t
     * @brief Converts this HashKey256 object to a hash256_t.
     * @return The corresponding hash256_t object.
     */
    explicit operator hash256_t() const {
        hash256_t result;
        std::memcpy(&result, this, sizeof(HashKey256));
        return result;
    }
    /** ------------------------------------------------------------------------------------------- Type Getter
     * @brief Retrieves the type of this HashKey256 object.
     * @return The HashKey256Type of this object.
     */
    HashKey256Type type() const {
        if constexpr (T == HashKey256Type::SHA256) {
            return HashKey256Type::SHA256;
        }
        return HashKey256Type::CMAC256;
    }
    /** ------------------------------------------------------------------------------------------- Random HashKey256 Generator
     * @brief Generates a random HashKey256 object.
     * @return A randomly generated HashKey256 object.
     */
    static HashKey256 random() {
        HashKey256 key;
        if (RAND_bytes(
            reinterpret_cast<unsigned char*>(&key),
            sizeof(HashKey256)) != 1
        ) [[unlikely]] {
            THROW("Failed to generate random hash key");
        }
        return key;
    }
    /** ------------------------------------------------------------------------------------------- SHA-256 Hash Generator
     * @brief Computes the SHA-256 hash of the given data.
     * @param data Pointer to the input data.
     * @param len Length of the input data in bytes.
     * @return The 256-bit SHA-256 hash.
     */
    template<HashKey256Type U = T, typename = std::enable_if_t<U == HashKey256Type::SHA256>>
    static HashKey256 sha(const void* data, size_t len) {
        HashKey256 key;
        if (!SHA256(
            reinterpret_cast<const unsigned char*>(data),
            len,
            reinterpret_cast<unsigned char*>(&key))
        ) [[unlikely]] {
            THROW("Failed to compute SHA-256 hash");
        }
        return key;
    }
    /** ------------------------------------------------------------------------------------------- SHA-256 Hash Generator (Multiple Buffers)
     * @brief Computes the SHA-256 hash of multiple input buffers.
     * @param data Pointer to an array of input data pointers.
     * @param lens Pointer to an array of input data lengths.
     * @param count Number of input buffers.
     * @return The 256-bit SHA-256 hash.
     */
    template<HashKey256Type U = T, typename = std::enable_if_t<U == HashKey256Type::SHA256>>
    static HashKey256 sha(const void* const* data, const size_t* lens, size_t count) {
        HashKey256 key;
        for (size_t i = 0; i < count; ++i) {
            unsigned char** datas = reinterpret_cast<unsigned char**>(const_cast<void**>(data));
            if (!SHA256_Update(&key, datas[i], lens[i])) [[unlikely]] {
                THROW("Failed to update SHA-256 hash");
            }
        }
        if (!SHA256_Final(reinterpret_cast<unsigned char*>(&key), &key)) [[unlikely]] {
            THROW("Failed to finalize SHA-256 hash");
        }
        return key;
    }
    /** ------------------------------------------------------------------------------------------- mac (CMAC-256)
     * @brief Computes the CMAC-256 hash of the given data using the provided encryption key.
     * @param data Pointer to the input data.
     * @param len Length of the input data in bytes.
     * @param key Pointer to the encryption key.
     * @return The 256-bit CMAC hash.
     */
    template<HashKey256Type U = T, typename = std::enable_if_t<U == HashKey256Type::CMAC256>>
    static HashKey256 mac(const void* data, size_t len, const EncryptionKey* key);
};
static_assert(sizeof(HashKey256<>) == 32, "HashKey256 must be exactly 32 bytes");
using SHA256Hash = HashKey256<HashKey256Type::SHA256>;
using CMAC256Hash = HashKey256<HashKey256Type::CMAC256>;
/** --------------------------------------------------------------------------------------------------------- xxHash License
 * @brief Upstream license for the adapted xxh3_128 namespace immediately below.
 *
 * xxHash - Extremely Fast Hash algorithm
 * Header File
 * Copyright (C) 2012-2023 Yann Collet
 *
 * BSD 2-Clause License (https://www.opensource.org/licenses/bsd-license.php)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * You can contact the author at:
 *   - xxHash homepage: https://www.xxhash.com
 *   - xxHash source repository: https://github.com/Cyan4973/xxHash
 */
/** --------------------------------------------------------------------------------------------------------- xxh3_128
 * @namespace xxh3_128
 * @brief Inline implementation of the XXH3 128-bit hash, short/mid path only.
 * Source: Yann Collet, xxHash (BSD-2-Clause); transcribed to a header-only namespace
 * with the SIMD long-path stripped out - DHT keys never exceed 240 bytes so we
 * cover the canonical 0..240 envelope and refuse longer inputs.
 *  - Bit-identical to upstream `XXH3_128bits(data, len)` with seed=0 for len<=240
 *  - Uses `__uint128_t` for the 64x64->128 multiply (assumed available)
 *  - Zero dependencies beyond the standard library
 */
namespace xxh3_128 {
inline constexpr uint64_t PRIME64_1 = 0x9E3779B185EBCA87ULL;  ///< xxhash 64-bit prime 1
inline constexpr uint64_t PRIME64_2 = 0xC2B2AE3D27D4EB4FULL;  ///< xxhash 64-bit prime 2
inline constexpr uint64_t PRIME64_3 = 0x165667B19E3779F9ULL;  ///< xxhash 64-bit prime 3
inline constexpr uint64_t PRIME64_4 = 0x85EBCA77C2B2AE63ULL;  ///< xxhash 64-bit prime 4
inline constexpr uint32_t PRIME32_2 = 0x85EBCA77U;            ///< xxhash 32-bit prime 2
inline constexpr uint64_t PRIME_MX1 = 0x165667919E3779F9ULL;  ///< xxh3 mid-mix prime 1
inline constexpr uint64_t PRIME_MX2 = 0x9FB21C651E98DF25ULL;  ///< xxh3 mid-mix prime 2
/** --------------------------------------------------------------------------------------------------------- k_secret (192 B)
 * @brief XXH3 default 192-byte secret. Public-domain bytes from xxHash.
 */
inline constexpr uint8_t k_secret[192] = {
    0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c,
    0xde, 0xd4, 0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f,
    0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21,
    0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c,
    0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb, 0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3,
    0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8,
    0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d,
    0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31, 0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64,
    0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
    0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e,
    0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce,
    0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e,
};
/** --------------------------------------------------------------------------------------------------------- h128_t
 * @struct h128_t
 * @brief Represents a 128-bit hash value split into two 64-bit lanes.
 */
struct h128_t {
    uint64_t low64;   ///< low 64 bits of the digest
    uint64_t high64;  ///< high 64 bits of the digest
};
/** --------------------------------------------------------------------------------------------------------- read_le32
 * @brief Little-endian 32-bit load. Branchless on LE targets.
 * @param p Pointer to the 4-byte input data.
 * @return The 32-bit little-endian value.
 */
inline static uint32_t read_le32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    if constexpr (is_big_endian()) {
        v = __builtin_bswap32(v);
    }
    return v;
}
/** --------------------------------------------------------------------------------------------------------- read_le64
 * @brief Little-endian 64-bit load. Branchless on LE targets.
 * @param p Pointer to the 8-byte input data.
 * @return The 64-bit little-endian value.
 */
inline static uint64_t read_le64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    if constexpr (is_big_endian()) {
        v = __builtin_bswap64(v);
    }
    return v;
}
/** --------------------------------------------------------------------------------------------------------- mul128_fold64
 * @brief 64x64 -> 128-bit multiply, then xor-fold to 64 bits.
 * @param lhs The left-hand side 64-bit value.
 * @param rhs The right-hand side 64-bit value.
 * @return The 64-bit result of the xor-folded 128-bit product.
 */
inline static uint64_t mul128_fold64(uint64_t lhs, uint64_t rhs) {
    __uint128_t prod = static_cast<__uint128_t>(lhs) * static_cast<__uint128_t>(rhs);
    return static_cast<uint64_t>(prod) ^ static_cast<uint64_t>(prod >> 64);
}
/** --------------------------------------------------------------------------------------------------------- avalanche
 * @brief XXH3 fast avalanche stage (input partially mixed already).
 * @param h The 64-bit input value to be avalanche-mixed.
 */
inline static uint64_t avalanche(uint64_t h) {
    h = (h ^ (h >> 37)) * PRIME_MX1;
    h ^= h >> 32;
    return h;
}
/** --------------------------------------------------------------------------------------------------------- avalanche64
 * @brief XXH64 final avalanche; stronger than `avalanche` for unmixed input.
 * @param h The 64-bit input value to be avalanche-mixed.
 */
inline static uint64_t avalanche64(uint64_t h) {
    h ^= h >> 33;
    h *= PRIME64_2;
    h ^= h >> 29;
    h *= PRIME64_3;
    h ^= h >> 32;
    return h;
}
/** --------------------------------------------------------------------------------------------------------- mix16B
 * @brief Mix 16 input bytes against 16 secret bytes via xor-fold multiply.
 * @param input Pointer to the input data.
 * @param secret Pointer to the secret key used for hashing.
 * @param seed Seed value for the hash function.
 */
inline static uint64_t mix16B(const uint8_t* input, const uint8_t* secret, uint64_t seed) {
    uint64_t lo = read_le64(input);
    uint64_t hi = read_le64(input + 8);
    return mul128_fold64(
        lo ^ (read_le64(secret) + seed),
        hi ^ (read_le64(secret + 8) - seed)
    );
}
/** --------------------------------------------------------------------------------------------------------- mix32B
 * @brief Mix two 16-byte input runs against 32 secret bytes into the running acc.
 * @param acc The running 128-bit accumulator.
 * @param in1 Pointer to the first 16-byte input block.
 * @param in2 Pointer to the second 16-byte input block.
 * @param secret Pointer to the secret key used for hashing.
 * @param seed Seed value for the hash function.
 */
inline static h128_t mix32B(
    h128_t acc,
    const uint8_t* in1,
    const uint8_t* in2,
    const uint8_t* secret,
    uint64_t seed
) {
    acc.low64 += mix16B(in1, secret + 0, seed);
    acc.low64 ^= read_le64(in2) + read_le64(in2 + 8);
    acc.high64 += mix16B(in2, secret + 16, seed);
    acc.high64 ^= read_le64(in1) + read_le64(in1 + 8);
    return acc;
}
/** --------------------------------------------------------------------------------------------------------- len_1to3
 * @brief Length 1..3 path: pack three bytes into a 32-bit word and XXH64-avalanche.
 * @param input Pointer to the input data.
 * @param len Length of the input data in bytes.
 * @param secret Pointer to the secret key used for hashing.
 * @param seed Seed value for the hash function.
 */
inline static h128_t len_1to3(const uint8_t* input, size_t len, const uint8_t* secret, uint64_t seed) {
    uint8_t c1 = input[0];
    uint8_t c2 = input[len >> 1];
    uint8_t c3 = input[len - 1];
    uint32_t combinedl = (static_cast<uint32_t>(c1) << 16)
                        | (static_cast<uint32_t>(c2) << 24)
                        | (static_cast<uint32_t>(c3) << 0)
                        | (static_cast<uint32_t>(len) << 8);
    uint32_t combinedh = std::rotl<uint32_t>(__builtin_bswap32(combinedl), 13);
    uint64_t bitflipl = (static_cast<uint64_t>(read_le32(secret))
                        ^ static_cast<uint64_t>(read_le32(secret + 4))) + seed;
    uint64_t bitfliph = (static_cast<uint64_t>(read_le32(secret + 8))
                        ^ static_cast<uint64_t>(read_le32(secret + 12))) - seed;
    h128_t h128;
    h128.low64  = avalanche64(static_cast<uint64_t>(combinedl) ^ bitflipl);
    h128.high64 = avalanche64(static_cast<uint64_t>(combinedh) ^ bitfliph);
    return h128;
}
/** --------------------------------------------------------------------------------------------------------- len_4to8
 * @brief Length 4..8 path: pack first/last 4 bytes, multiply, double-avalanche.
 * @param input Pointer to the input data.
 * @param len Length of the input data in bytes.
 * @param secret Pointer to the secret key used for hashing.
 * @param seed Seed value for the hash function.
 */
inline static h128_t len_4to8(const uint8_t* input, size_t len, const uint8_t* secret, uint64_t seed) {
    seed ^= static_cast<uint64_t>(__builtin_bswap32(static_cast<uint32_t>(seed))) << 32;
    uint32_t input_lo = read_le32(input);
    uint32_t input_hi = read_le32(input + len - 4);
    uint64_t input_64 = static_cast<uint64_t>(input_lo)
                        + (static_cast<uint64_t>(input_hi) << 32);
    uint64_t bitflip = (read_le64(secret + 16) ^ read_le64(secret + 24)) + seed;
    uint64_t keyed = input_64 ^ bitflip;
    __uint128_t prod = static_cast<__uint128_t>(keyed)
                        * static_cast<__uint128_t>(PRIME64_1 + (len << 2));
    h128_t m128;
    m128.low64 = static_cast<uint64_t>(prod);
    m128.high64 = static_cast<uint64_t>(prod >> 64);
    m128.high64 += (m128.low64 << 1);
    m128.low64 ^= (m128.high64 >> 3);
    m128.low64 = m128.low64 ^ (m128.low64 >> 35);
    m128.low64 *= PRIME_MX2;
    m128.low64 = m128.low64 ^ (m128.low64 >> 28);
    m128.high64 = avalanche(m128.high64);
    return m128;
}
/** --------------------------------------------------------------------------------------------------------- len_9to16
 * @brief Length 9..16 path: 64-bit input lanes, length-mid-injection, twin avalanche.
 * @param input Pointer to the input data.
 * @param len Length of the input data in bytes.
 * @param secret Pointer to the secret key used for hashing.
 * @param seed Seed value for the hash function.
 */
inline static h128_t len_9to16(const uint8_t* input, size_t len, const uint8_t* secret, uint64_t seed) {
    uint64_t bitflipl = (read_le64(secret + 32) ^ read_le64(secret + 40)) - seed;
    uint64_t bitfliph = (read_le64(secret + 48) ^ read_le64(secret + 56)) + seed;
    uint64_t input_lo = read_le64(input);
    uint64_t input_hi = read_le64(input + len - 8);
    __uint128_t prod = static_cast<__uint128_t>(input_lo ^ input_hi ^ bitflipl)
                        * static_cast<__uint128_t>(PRIME64_1);
    h128_t m128;
    m128.low64  = static_cast<uint64_t>(prod);
    m128.high64 = static_cast<uint64_t>(prod >> 64);
    m128.low64 += static_cast<uint64_t>(len - 1) << 54;
    input_hi ^= bitfliph;
    m128.high64 += input_hi
                    + static_cast<uint64_t>(static_cast<uint32_t>(input_hi))
                    * static_cast<uint64_t>(PRIME32_2 - 1);
    m128.low64 ^= __builtin_bswap64(m128.high64);
    __uint128_t prod2 = static_cast<__uint128_t>(m128.low64)
                        * static_cast<__uint128_t>(PRIME64_2);
    h128_t h128;
    h128.low64  = static_cast<uint64_t>(prod2);
    h128.high64 = static_cast<uint64_t>(prod2 >> 64) + m128.high64 * PRIME64_2;
    h128.low64  = avalanche(h128.low64);
    h128.high64 = avalanche(h128.high64);
    return h128;
}
/** --------------------------------------------------------------------------------------------------------- len_0to16
 * @brief Length 0..16 dispatcher to the four sub-paths.
 * @param input Pointer to the input data.
 * @param len Length of the input data in bytes.
 * @param secret Pointer to the secret key used for hashing.
 * @param seed Seed value for the hash function.
 */
inline static h128_t len_0to16(const uint8_t* input, size_t len, const uint8_t* secret, uint64_t seed) {
    if (len > 8) {
        return len_9to16(input, len, secret, seed);
    }
    if (len >= 4) {
        return len_4to8(input, len, secret, seed);
    }
    if (len > 0) {
        return len_1to3(input, len, secret, seed);
    }
    h128_t h128;
    uint64_t bitflipl = read_le64(secret + 64) ^ read_le64(secret + 72);
    uint64_t bitfliph = read_le64(secret + 80) ^ read_le64(secret + 88);
    h128.low64  = avalanche64(seed ^ bitflipl);
    h128.high64 = avalanche64(seed ^ bitfliph);
    return h128;
}
/** --------------------------------------------------------------------------------------------------------- len_17to128
 * @brief Length 17..128 path: 1..4 16-byte runs mixed in pairs from both ends.
 * @param input Pointer to the input data.
 * @param len Length of the input data in bytes.
 * @param secret Pointer to the secret key used for hashing.
 * @param seed Seed value for the hash function.
 */
inline static h128_t len_17to128(const uint8_t* input, size_t len, const uint8_t* secret, uint64_t seed) {
    h128_t acc;
    acc.low64 = len * PRIME64_1;
    acc.high64 = 0;
    if (len > 32) {
        if (len > 64) {
            if (len > 96) {
                acc = mix32B(acc, input + 48, input + len - 64, secret + 96, seed);
            }
            acc = mix32B(acc, input + 32, input + len - 48, secret + 64, seed);
        }
        acc = mix32B(acc, input + 16, input + len - 32, secret + 32, seed);
    }
    acc = mix32B(acc, input, input + len - 16, secret, seed);
    h128_t h128;
    h128.low64  = acc.low64 + acc.high64;
    h128.high64 = (acc.low64 * PRIME64_1)
                + (acc.high64 * PRIME64_4)
                + ((len - seed) * PRIME64_2);
    h128.low64  = avalanche(h128.low64);
    h128.high64 = static_cast<uint64_t>(0) - avalanche(h128.high64);
    return h128;
}
/** --------------------------------------------------------------------------------------------------------- len_129to240
 * @brief Length 129..240 path: 5..7 32-byte runs with split avalanche around byte 160.
 * @param input Pointer to the input data.
 * @param len Length of the input data in bytes.
 * @param secret Pointer to the secret key used for hashing.
 * @param seed Seed value for the hash function.
 */
inline static h128_t len_129to240(const uint8_t* input, size_t len, const uint8_t* secret, uint64_t seed) {
    constexpr unsigned MIDSIZE_STARTOFFSET = 3;   ///< secret offset bias for second half
    constexpr unsigned MIDSIZE_LASTOFFSET  = 17;  ///< secret tail offset for last 32-byte mix
    constexpr unsigned SECRET_SIZE_MIN     = 136; ///< canonical XXH3_SECRET_SIZE_MIN
    h128_t acc;
    acc.low64 = len * PRIME64_1;
    acc.high64 = 0;
    for (unsigned i = 32; i < 160; i += 32) {
        acc = mix32B(acc, input + i - 32, input + i - 16, secret + i - 32, seed);
    }
    acc.low64  = avalanche(acc.low64);
    acc.high64 = avalanche(acc.high64);
    for (unsigned i = 160; i <= len; i += 32) {
        acc = mix32B(
            acc,
            input + i - 32,
            input + i - 16,
            secret + MIDSIZE_STARTOFFSET + i - 160,
            seed
        );
    }
    acc = mix32B(
        acc,
        input + len - 16,
        input + len - 32,
        secret + SECRET_SIZE_MIN - MIDSIZE_LASTOFFSET - 16,
        static_cast<uint64_t>(0) - seed
    );
    h128_t h128;
    h128.low64  = acc.low64 + acc.high64;
    h128.high64 = (acc.low64 * PRIME64_1)
                + (acc.high64 * PRIME64_4)
                + ((len - seed) * PRIME64_2);
    h128.low64  = avalanche(h128.low64);
    h128.high64 = static_cast<uint64_t>(0) - avalanche(h128.high64);
    return h128;
}
/** --------------------------------------------------------------------------------------------------------- hash
 * @brief Top-level dispatcher. Seed is fixed at zero (we don't expose seeding).
 * @param input Pointer to the bytes to hash.
 * @param len Number of bytes; must be <= 240 (DHT keys are bounded).
 * @return 128-bit XXH3 digest.
 */
inline static h128_t hash(const void* input, size_t len) {
    const uint8_t* in = static_cast<const uint8_t*>(input);
    if (len <= 16) {
        return len_0to16(in, len, k_secret, 0);
    }
    if (len <= 128) {
        return len_17to128(in, len, k_secret, 0);
    }
    if (len <= 240) {
        return len_129to240(in, len, k_secret, 0);
    }
    THROW("xxh3_128 long-path (>240 bytes) not implemented; DHT keys are bounded");
}
/** --------------------------------------------------------------------------------------------------------- hash
 * @brief Top-level dispatcher. Seed is fixed at zero (we don't expose seeding).
 * @param input Pointer to the bytes to hash.
 * @param len Number of bytes; must be <= 240 (DHT keys are bounded).
 * @return 128-bit XXH3 digest.
 */
inline static h128_t hash_id(int64_t input) {
    const uint8_t* in = reinterpret_cast<const uint8_t*>(&input);
    return len_0to16(in, 8, k_secret, 0);
}
} // namespace xxh3_128
/** --------------------------------------------------------------------------------------------------------- HashKey128
 * @struct HashKey128
 * @brief POD container for a 128-bit (16-byte) hash key.
 * The POD structure is split up into multiple fields so we can quickly pull out a sub-hash
 * when we need to - no bit twiddling or shifting required.
 * 
 * NOTE: Here we have to make an exception to our "no memcpy" rule, because otherwise we end up
 * with endianness issues.
 */
template<HashKey128Type T = HashKey128Type::CMAC128>
struct alignas(16) HashKey128 {
private:
    /// @brief The high 64 bits of the hash key.
    uint64_t high_ = 0;
    /// @brief The high 64 bits of the hash key.
    uint32_t mid_ = 0;
    /// @brief The middle 32 bits of the hash key.
    uint16_t semi_mid_ = 0;
    /// @brief The next 8 bits of the hash key, after the semi-middle 16 bits.
    uint8_t low_ = 0;
    /// @brief The lowest 8 bits of the hash key.
    uint8_t other_low_ = 0;
public:
    /** ------------------------------------------------------------------------------------------- Default constructor
     * @brief Constructs a default-initialized HashKey128 instance.
     */
    HashKey128() = default;
    /** ------------------------------------------------------------------------------------------- Constructor from raw data
     * @brief Constructs a HashKey128 instance from raw data.
     * @param data Pointer to the raw data to initialize the HashKey128 instance with.
     */
    HashKey128(void* data) {
        if constexpr (is_big_endian()) {
            // If we're on a big-endian platform, we need to reverse the byte order when copying
            uint8_t* bytes = reinterpret_cast<uint8_t*>(this);
            uint8_t* data_bytes = reinterpret_cast<uint8_t*>(data);
            for (size_t i = 0; i < 16; ++i) {
                bytes[i] = data_bytes[15 - i];
            }
        } else {
             std::memcpy(this, data, sizeof(HashKey128));
        }
    }
    /** ------------------------------------------------------------------------------------------- Default constructor and assignment operators */
    HashKey128(const HashKey128&) = default;
    HashKey128& operator=(const HashKey128&) = default;
    HashKey128(HashKey128&&) = default;
    HashKey128& operator=(HashKey128&&) = default;
    /** ------------------------------------------------------------------------------------------- HashKey128 from_hash constructor
     * @brief Constructs a HashKey128 instance from a hash128_t object.
     * @param hash The xxh3_128::h128_t object to initialize the HashKey128 instance with.
     */
    HashKey128(const xxh3_128::h128_t& hash) {
        std::memcpy(this, &hash, sizeof(HashKey128));
    }
    /** ------------------------------------------------------------------------------------------- HashKey128 from_hash method
     * @brief Assigns a xxh3_128::h128_t object to the HashKey128 instance.
     * @param hash The xxh3_128::h128_t object to assign to the HashKey128 instance.
     */
    HashKey128& operator=(const xxh3_128::h128_t& hash) {
        std::memcpy(this, &hash, sizeof(HashKey128));
        return *this;
    }
    /** ------------------------------------------------------------------------------------------- HashKey128 sub64 method
     * @brief Retrieves the high 64 bits of the hash key.
     * @return The high 64 bits of the hash key.
     */
    uint64_t sub64() const { return high_; }
    /** ------------------------------------------------------------------------------------------- HashKey128 sub32 method
     * @brief Retrieves the middle 32 bits of the hash key.
     * @return The middle 32 bits of the hash key.
     */
    uint32_t sub32() const { return mid_; }
    /** ------------------------------------------------------------------------------------------- HashKey128 sub16 method
     * @brief Retrieves the middle 16 bits of the hash key.
     * @return The middle 16 bits of the hash key.
     */
    uint16_t sub16() const { return semi_mid_; }
    /** ------------------------------------------------------------------------------------------- HashKey128 sub8 method
     * @brief Retrieves the next 8 bits of the hash key, after the semi-middle 16 bits.
     * @return The next 8 bits of the hash key.
     */
    uint8_t sub8() const { return low_; }
    /** ------------------------------------------------------------------------------------------- HashKey128 other_sub8 method
     * @brief Retrieves the lowest 8 bits of the hash key.
     * @return The lowest 8 bits of the hash key.
     */
    uint8_t other_sub8() const { return other_low_; }
    /** ------------------------------------------------------------------------------------------- HashKey128 to_bytes method
     * @brief Converts the HashKey128 instance to a byte array.
     * @return A unique pointer to the byte array representing the HashKey128 instance.
     */
    std::unique_ptr<uint8_t[]> to_bytes() const {
        auto bytes = std::make_unique<uint8_t[]>(16);
        std::memcpy(bytes.get(), this, 16);
        return bytes;
    }
    /** ------------------------------------------------------------------------------------------- HashKey128 to_hex method
     * @brief Converts the HashKey128 instance to a hexadecimal string representation.
     * @return The hexadecimal string representation of the HashKey128 instance.
     */
    std::string to_hex() const {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(this);
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (size_t i = 0; i < 16; ++i) {
            oss << std::setw(2) << static_cast<int>(bytes[i]);
        }
        return oss.str();
    }
    /** ------------------------------------------------------------------------------------------- HashKey128 stream insertion operator
     * @brief Stream insertion operator for HashKey128.
     * @param os The output stream.
     * @param key The HashKey128 instance to insert into the stream.
     * @return The output stream with the inserted HashKey128 in hexadecimal format.
     */
    friend std::ostream& operator<<(std::ostream& os, const HashKey128& key) {
        std::string hex_str = key.to_hex();
        os << hex_str;
        return os;
    }
    /** ------------------------------------------------------------------------------------------- HashKey128 == operator
     * @brief Equal-to comparison for HashKey128.
     * @param other The HashKey128 instance to compare with.
     * @return True if this instance is equal to the other, false otherwise.
     */
    bool operator==(const HashKey128& other) const {
        return std::memcmp(this, &other, sizeof(HashKey128)) == 0;
    }
    /** ------------------------------------------------------------------------------------------- HashKey128 != operator
     * @brief Not-equal-to comparison for HashKey128.
     * @param other The HashKey128 instance to compare with.
     * @return True if this instance is not equal to the other, false otherwise.
     */
    bool operator!=(const HashKey128& other) const {
        return !(*this == other);
    }
    /** ------------------------------------------------------------------------------------------- HashKey128 < operator
     * @brief Less-than comparison for HashKey128.
     * @param other The HashKey128 instance to compare with.
     * @return True if this instance is less than the other, false otherwise.
     */
    bool operator<(const HashKey128& other) const {
        return std::memcmp(this, &other, sizeof(HashKey128)) < 0;
    }
    /** ------------------------------------------------------------------------------------------- HashKey128 > operator
     * @brief Greater-than comparison for HashKey128.
     * @param other The HashKey128 instance to compare with.
     * @return True if this instance is greater than the other, false otherwise.
     */
    bool operator>(const HashKey128& other) const {
        return std::memcmp(this, &other, sizeof(HashKey128)) > 0;
    }
    /** ------------------------------------------------------------------------------------------- HashKey128 <= operator
     * @brief Less-than-or-equal-to comparison for HashKey128.
     * @param other The HashKey128 instance to compare with.
     * @return True if this instance is less than or equal to the other, false otherwise.
     */
    bool operator<=(const HashKey128& other) const {
        return !(*this > other);
    }
    /** ------------------------------------------------------------------------------------------- HashKey128 >= operator
     * @brief Greater-than-or-equal-to comparison for HashKey128.
     * @param other The HashKey128 instance to compare with.
     * @return True if this instance is greater than or equal to the other, false otherwise.
     */
    bool operator>=(const HashKey128& other) const {
        return !(*this < other);
    }
    /** ------------------------------------------------------------------------------------------- HashKey128 type accessor
     * @brief Retrieves the type of the HashKey128.
     * @return The HashKey128Type of the key.
     */
    HashKey128Type type() {
        if constexpr (T == HashKey128Type::SHA256_TRUNCATED) {
            return HashKey128Type::SHA256_TRUNCATED;
        } else if constexpr (T == HashKey128Type::HMAC128) {
            return HashKey128Type::HMAC128;
        }
        return HashKey128Type::CMAC128;
    }
    /** ------------------------------------------------------------------------------------------- Random HashKey128
     * @brief Generates a random HashKey128.
     * @return A randomly generated HashKey128.
     */
    static HashKey128 random() {
        HashKey128 key;
        if (RAND_bytes(
            reinterpret_cast<unsigned char*>(&key),
            sizeof(HashKey128)) != 1
        ) [[unlikely]] {
            THROW("Failed to generate random hash key");
        }
        return key;
    }
    /** ------------------------------------------------------------------------------------------- sha (truncated SHA-256)
     * @brief Computes the truncated SHA-256 hash for the given data.
     * @param data Pointer to the data to hash.
     * @param len Length of the data in bytes.
     * @return The truncated SHA-256 hash as a HashKey128.
     */
    template<HashKey128Type U = T,
        typename = std::enable_if_t<U == HashKey128Type::SHA256_TRUNCATED>>
    static HashKey128 sha(const void* data, size_t len) {
        HashKey256 full_hash = HashKey256<HashKey256Type::SHA256>::sha(data, len);
        HashKey128 truncated;
        std::memcpy(&truncated, &full_hash, sizeof(HashKey128));
        return truncated;
    }
    /** ------------------------------------------------------------------------------------------- sha (truncated SHA-256)
     * @brief Computes the truncated SHA-256 hash for the given data.
     * @param data Pointer to the data to hash.
     * @param len Length of the data in bytes.
     * @return The truncated SHA-256 hash as a HashKey128.
     */
    template<HashKey128Type U = T,
        typename = std::enable_if_t<U == HashKey128Type::SHA256_TRUNCATED>>
    static HashKey128 sha(const void* const* data, const size_t* lens, size_t count) {
        HashKey256 full_hash = HashKey256<HashKey256Type::SHA256>::sha(data, lens, count);
        HashKey128 truncated;
        std::memcpy(&truncated, &full_hash, sizeof(HashKey128));
        return truncated;   
    }
    /** ------------------------------------------------------------------------------------------- mac
     * @brief Computes the cryptographic MAC (Message Authentication Code) for the given data
     * using the provided key.
     * @param data Pointer to the data to hash.
     * @param len Length of the data in bytes.
     * @param key Pointer to the encryption key.
     * @return The computed MAC as a HashKey128.
     */
    static HashKey128 mac(const void* data, size_t len, const EncryptionKey* key);
    /** ------------------------------------------------------------------------------------------- Non-Crypto Hash
     * @brief A non-cryptographic hash function for cases where we want a fast,
     * non-secure hash. This is not suitable for any security-sensitive use cases.
     * @param data Pointer to the data to hash.
     * @param len Length of the data in bytes.
     */
    static HashKey128 non_crypto(const void* data, size_t len, const EncryptionKey*) {
        xxh3_128::h128_t hash = xxh3_128::hash(data, len);
        return std::bit_cast<HashKey128<>>(hash);
    }
    /// @brief Function pointer type for hash methods (secure or non-crypto)
    using HashMethodSignature = HashKey128(*)(const void*, size_t, const EncryptionKey*);
    /** ------------------------------------------------------------------------------------------- HashMethodContainer
     * @struct HashMethodContainer
     * @brief Container for a hash method (either secure or non-crypto).
     */
    struct HashMethodContainer {
        /// @brief The hash method function pointer.
        HashMethodSignature method;
    };
    /** ------------------------------------------------------------------------------------------- secure_hash_method
     * @brief Container for the secure (cryptographic) hash method.
     */
    static constexpr HashMethodContainer secure_hash_method = { mac };
    /** ------------------------------------------------------------------------------------------- non_secure_hash_method
     * @brief Container for the non-secure (non-cryptographic) hash method.
     */
    static constexpr HashMethodContainer non_secure_hash_method = { non_crypto };
    /** ------------------------------------------------------------------------------------------- get_hash_method
     * @brief Retrieves the current hash method container.
     * @return Reference to the current hash method container.
     */
    static HashMethodContainer& get_hash_method() {
        static HashMethodContainer hash_method = non_secure_hash_method;
        return hash_method;
    }
    /** ------------------------------------------------------------------------------------------- set_secure_hash_method
     * @brief Sets the hash method to use for hashing keys. If `use_secure` is true, the method
     * will be set to a secure hash (e.g., HMAC); if false, it will be set to a non-cryptographic
     * hash.
     * @param use_secure Whether to use a secure hash method.
     */
    static void set_secure_hash_method(bool use_secure) {
        if (use_secure) {
            get_hash_method() = secure_hash_method;
        } else {
            get_hash_method() = non_secure_hash_method;
        }
    }
    /** ------------------------------------------------------------------------------------------- hash_key
     * @brief Hashes the given value using the currently selected hash method (secure or
     * non-crypto).
     * @param value The value to hash.
     * @param key Optional encryption key for secure hashing; ignored for non-crypto
     * hashing.
     */
    static HashKey128 hash_key(int64_t value, EncryptionKey* key = nullptr) {
        return get_hash_method().method(reinterpret_cast<void*>(&value), sizeof(int64_t), key);
    }
    /** ------------------------------------------------------------------------------------------- hash_key overload
     * @brief Overload of `hash_key` that takes a `std::string_view`, for convenience.
     * @param str The string to hash.
     * @param key Optional encryption key for secure hashing; ignored for non-crypto
     * hashing.
     */
    static HashKey128 hash_key(std::string_view str, const EncryptionKey* key = nullptr) {
        return get_hash_method().method(
            reinterpret_cast<void*>(const_cast<char*>(str.data())),
            str.length(),
            key
        );
    }
    /** ------------------------------------------------------------------------------------------- hash_str
     * @brief Hashes a C-style string using the currently selected hash method (secure or
     * non-crypto).
     * @param cstr Pointer to the C-style string to hash.
     * @param len Length of the string in bytes (excluding null terminator).
     * @param key Optional encryption key for secure hashing; ignored for non-crypto
     * hashing.
     */
    static HashKey128 hash_str(const char* cstr, size_t len, const EncryptionKey* key = nullptr) {
        return get_hash_method().method(
            reinterpret_cast<void*>(const_cast<char*>(cstr)),
            len,
            key
        );
    }
};
static_assert(sizeof(HashKey128<>) == 16, "HashKey128 must be exactly 16 bytes");
using CMAC128Hash = HashKey128<HashKey128Type::CMAC128>;
using HMAC128Hash = HashKey128<HashKey128Type::HMAC128>;
using SHA128Hash = HashKey128<HashKey128Type::SHA256_TRUNCATED>;
using RecordKey = CMAC128Hash;
} // namespace buffetalligator
