#pragma once
/** --------------------------------------------------------------------------------------------------------- BuffetAlligator
 * @file buffetalligator.hpp
 * @brief Unified header for the BuffetAlligator.
 * (was supposed to be "buffer allocator" but voice-to-text got it wrong and it stuck)
 *                       _.---._
 *                  _.-'   o   `--..__ 
 *              _.-'                   `-._      ,-------------.
 *          _.-'                  o     o  \    |   ITS A       |
 *         /     /              .__________/    |    MEMORY     |
 *        /     /   `--.________/  /  /  /    <.   SMORGASBORG! |
 *       /     /        `-.____________.'        \_____________/
 *      /  ^  /              /
 *     /  ^  /      .-------'
 *    /  ^  /      /      __
 *   /  ^  /       |     /  `--.
 *  /     /        |    /       ( @ )  _/\/\_
 * /     /    .----`---/____   (_____) {&}{&}__
 * /     /    /              '\________________/'
 *|    /     \____.---------  \____________/'
 *|   /           /       /    \
 * \  \          /       /     |
 *  \  `-.______|       /____.-'
 *   `-.._______/______/____)
 * @author Josh Morgan <https://github.com/joshmorgan1000/buffetalligator>
 * Released under the Apache License 2.0.
 */
#include <logging.hpp>
#include <vulkan/vulkan.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <concepts>
#include <future>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>
#include <cassert>
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <alligator/nutritionist.hpp>
#include <moodycamel/concurrentqueue.h>
#include <moodycamel/blockingconcurrentqueue.h>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Exception Classes */
EXCEPTION_CLASS(AlligatorType)
#define ALLIGATOR_TYPE_THROW(msg) throw AlligatorTypeException(msg)
EXCEPTION_CLASS(AlligatorVulkan)
#define ALLIGATOR_VULKAN_THROW(msg) throw AlligatorVulkanException(msg)
EXCEPTION_CLASS(AlligatorHeap)
#define ALLIGATOR_HEAP_THROW(msg) throw AlligatorHeapException(msg)
EXCEPTION_CLASS(Alligator)
#define ALLIGATOR_THROW(msg) throw AlligatorException(msg)
/** ------------------------------------------------------------------------------------- Forward Declarations */
class Alligator; class Slice;
class SliceQueue; class SliceChannel;
struct SliceRecord; class PrioritySlice;
class AlignedHeapBuffer; class VulkanBuffer;
class IBuffet; class SliceMap; class Memory;
class PotentialSlice; class SliceQueue;
struct SliceRec;
void ez_vulkan_touch();
template<typename T> class SliceT;
template<typename T> class SliceMapT;
template<typename T> class PotentialSliceT;
template<typename T> class SliceQueueT;
/// @brief Helper template that always evaluates to false, used for static assertions in templates.
template<typename> struct always_false : std::false_type {};
/** --------------------------------------------------------------------------------------------------------- IsAnIBuffetType
 * @brief Concept to check if a type is a valid IBuffet derived type.
 */
template<typename T>
concept IsAnIBuffetType = std::is_base_of_v<IBuffet, T> && !std::is_same_v<IBuffet, T>;
/** --------------------------------------------------------------------------------------------------------- Host Memory Usage
 * @struct HostMemoryUsage
 * @brief Reports physical capacity, estimated available memory, and this process's resident bytes.
 */
struct HostMemoryUsage {
    uint64_t physical_bytes;
    uint64_t available_bytes;
    uint64_t resident_bytes;
};
/** --------------------------------------------------------------------------------------------------------- Host Memory Usage Query
 * @brief Queries the host for its physical bytes, its available bytes, and this process's resident bytes.
 * @return The host memory usage.
 */
HostMemoryUsage host_memory_usage();
/** --------------------------------------------------------------------------------------------------------- Available Host Bytes
 * @brief Queries the host, in one call, for the bytes it can hand out without reclaiming.
 * @return The available bytes.
 */
size_t available_host_bytes();
/// @brief The smallest buffer a chain links; a buffer this large is checked against the host before it is touched.
inline constexpr size_t SLAB_FLOOR_BYTES = 64ull << 20;
/** --------------------------------------------------------------------------------------------------------- Slice
 * @class Slice
 * @brief A slice of memory in the buffet alligator: 32 bytes by value, the device address and size a shader
 * reads first, then the parent it counts on, its type word and its count.
 * 
 * All operations in the buffet alligator are performed on slices of memory, represented by the `Slice`
 * class. A Slice is the record a shader reads, so a Slice or a list of them goes to the GPU as it is.
 * 
 * Memory slices are claims from pre-allocated memory buffers that are managed by the buffet alligator's
 * slab arena. On systems that support unified memory, they are always sliced from host-coherent GPU
 * buffers. For systems that do not support unified memory, transfers between host and device memory are
 * handled automatically by the buffet alligator.
 * 
 * Slices can be sub-sliced to create smaller slices, and they all share the same reference counter and
 * underlying memory. Slices can behave much like `std::shared_ptr` by calling the `slice()` method with
 * no parameters passed, essentially requesting a sub-slice that is the full size of the original. The copy
 * constructor and assignment operators are deleted to cut down on unintended reference counting traffic
 * which can be expensive in high-performance scenarios.
 * 
 * In most cases, slices are meant to be short-lived since they are references to memory that is part of a
 * larger slab in the arena's memory pool. Holding on to a small claim for a long time can lead to
 * fragmentation. For cases in which you need to hold on to a slice for longer than a transient operation,
 * you should specify the `novel_buffer` parameter when constructing the slob to `true` so that the slice
 * is created as its own individual buffer that is not part of a larger slab. This is a slower operation
 * since it requires the buffer to be allocated and freed individually, so keep that in mind.
 */
class Slice {
private:
    /// @brief The ID of the slice in the buffer's pool.
    uint32_t id_ = 0xFFFFFFFFu;
    /// @brief Constructor used internally by the buffet.
    Slice(uint64_t,void*,uint32_t,uint32_t);
    /// @brief Friend class declaration for IBuffet.
    friend class IBuffet;
public:
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Default constructor for a Slice, creating an invalid slice.
     */
    Slice() = default;
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Constructs a slice of the given size. If `novel` is true, the slice is created as
     * its own individual buffer.
     * @param size The size of the slice to create.
     * @param novel Whether to create the slice as its own individual buffer.
     */
    Slice(size_t size, bool novel = false);
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Copy constructor for a Slice.
     * @param other The slice to copy.
     */
    Slice(const Slice& other);
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Copy assignment operator for a Slice.
     * @param other The slice to copy.
     * @return Reference to the assigned slice.
     */
    Slice& operator=(const Slice& other);
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Move constructor for a Slice.
     * @param other The slice to move.
     */
    Slice(Slice&& other);
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Move assignment operator for a Slice.
     * @param other The slice to move.
     * @return Reference to the assigned slice.
     */
    Slice& operator=(Slice&& other);
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Destructor for a Slice.
     */
    ~Slice();
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Returns a pointer to the raw data of the slice.
     * @return Pointer to the raw data.
     */
    void* raw();
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Returns a const pointer to the raw data of the slice.
     * @return Const pointer to the raw data.
     */
    const void* raw() const;
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Returns a pointer to the data of the slice cast to the specified type.
     * @tparam T The type to cast the data pointer to.
     * @return Pointer to the data of the specified type.
     */
    template<typename T> T* data() { return static_cast<T*>(raw()); }
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Returns a const pointer to the data of the slice cast to the specified type.
     * @tparam T The type to cast the data pointer to.
     * @return Const pointer to the data of the specified type.
     */
    template<typename T> const T* data() const { return static_cast<const T*>(raw()); }
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Returns the size of the slice in bytes.
     * @return Size of the slice in bytes.
     */
    size_t size_bytes() const;
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Returns the size of the slice in terms of the specified type.
     * @tparam T The type to calculate the size for.
     * @return Size of the slice in terms of the specified type.
     */
    template<typename T> size_t size() const { return size_bytes() / sizeof(T); }
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Returns a sub-slice of the current slice.
     * @param offset The starting offset of the sub-slice.
     * @param length The length of the sub-slice.
     * @return The sub-slice.
     */
    Slice slice(size_t offset = 0, size_t length = SIZE_MAX) const;
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Returns the buffer associated with the slice.
     * @return Shared pointer to the buffer.
     */
    std::shared_ptr<IBuffet> buffer() const;
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Returns the address associated with the slice.
     * @return Reference to the address.
     */
    uint64_t& address();
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Returns the address associated with the slice.
     * @return Const reference to the address.
     */
    const uint64_t& address() const;
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Returns the underlying SliceRec of the slice.
     * @return Reference to the SliceRec.
     */
    SliceRec& rec();
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Returns the underlying SliceRec of the slice.
     * @return Const reference to the SliceRec.
     */
    const SliceRec& rec() const;
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Constructs a Slice from a SliceRec.
     * @param rec The SliceRec to construct the Slice from.
     */
    Slice(const SliceRec& rec);
    /** ------------------------------------------------------------------------------------------- Free
     * @brief Frees the underlying memory of the slice.
     */
    void free();
    /// @brief Whether the handle names no claim.
    bool is_null() const { return id_ == 0xFFFFFFFFu; }
    /// @brief Whether the handle names a claim.
    bool valid() const { return id_ != 0xFFFFFFFFu; }
    operator bool() const { return id_ != 0xFFFFFFFFu; }
    /// @brief Whether the claim has a buffer to itself.
    bool is_novel() const;
    /// @brief The claim's bytes rounded up to its 64-byte units.
    size_t capacity() const { return (size_bytes() + 63) & ~size_t{63}; }
    /// @brief The bytes as a T.
    template<typename T> T& get_as() { return *static_cast<T*>(raw()); }
    /// @brief The bytes as a const T.
    template<typename T> const T& get_as() const { return *static_cast<const T*>(raw()); }
};
/** --------------------------------------------------------------------------------------------------------- SliceRec
 * @struct SliceRec
 * @brief A record representing a slice of a buffer, including its raw pointer, associated buffer,
 * ID, and size. Provides mechanisms for claiming and releasing slices.
 */
struct SliceRec {
    /// @brief Raw pointer to the slice data.
    void* raw_ = nullptr;
    /// @brief Shared pointer to the buffer associated with the slice.
    std::shared_ptr<IBuffet> buffer_;
    /// @brief ID of the slice.
    uint32_t id_ = 0;
    /// @brief Size of the slice.
    uint32_t size_ = 0;
    /** ------------------------------------------------------------------------------------------- SliceRec
     * @brief Attempts to claim the slice for exclusive use.
     * @return True if the slice was successfully claimed, false otherwise.
     */
    bool try_claim() {
        if (std::atomic_ref<uint32_t>(id_).load() & 0x80000000) return false;
        return (std::atomic_ref<uint32_t>(id_).fetch_or(0x80000000) & 0x80000000) == 0;
    }
    /** ------------------------------------------------------------------------------------------- SliceRec
     * @brief Releases the slice, making it available for others.
     */
    void release() {
        std::atomic_ref<uint32_t>(id_).store(0, std::memory_order_release);
        size_ = 0; raw_ = nullptr; buffer_.reset();
    }
    /// @brief Default constructor for SliceRec. Initializes members to default values.
    SliceRec() = default;
    /** ------------------------------------------------------------------------------------------- SliceRec
     * @brief Constructs a SliceRec with the specified raw pointer, buffer, ID, and size.
     * @param raw Raw pointer to the slice data.
     * @param buffer Shared pointer to the buffer associated with the slice.
     * @param id ID of the slice.
     * @param size Size of the slice.
     */
    SliceRec(void* raw, std::shared_ptr<IBuffet> buffer, uint32_t id, uint32_t size)
    : raw_(raw), buffer_(buffer), id_(id | 0x80000000u), size_(size) {}
    /** ------------------------------------------------------------------------------------------- SliceRec
     * @brief Copy constructor for SliceRec.
     * @param other The SliceRec to copy from.
     */
    SliceRec(const SliceRec& other) : raw_(other.raw_)
    , buffer_(other.buffer_), id_(other.id_), size_(other.size_) {}
    /** ------------------------------------------------------------------------------------------- SliceRec
     * @brief Copy assignment operator for SliceRec.
     * @param other The SliceRec to copy from.
     * @return Reference to the assigned SliceRec.
     */
    SliceRec& operator=(const SliceRec& other) {
        release();
        raw_ = other.raw_; buffer_ = other.buffer_;
        id_ = other.id_; size_ = other.size_; return *this;
    }
    /** ------------------------------------------------------------------------------------------- SliceRec
     * @brief Move constructor for SliceRec.
     * @param other The SliceRec to move from.
     */
    SliceRec(SliceRec&& other) noexcept : raw_(other.raw_), buffer_(std::move(other.buffer_))
    , id_(other.id_ | 0x80000000u), size_(other.size_) {
        other.raw_ = nullptr; other.id_ = 0; other.size_ = 0;
    }
    /** ------------------------------------------------------------------------------------------- SliceRec
     * @brief Move assignment operator for SliceRec.
     * @param other The SliceRec to move from.
     * @return Reference to the assigned SliceRec.
     */
    SliceRec& operator=(SliceRec&& other) noexcept {
        release();
        raw_ = other.raw_; buffer_ = std::move(other.buffer_); id_ = other.id_ | 0x80000000u;
        size_ = other.size_; other.raw_ = nullptr; other.id_ = 0; other.size_ = 0; return *this;
    }
    /** ------------------------------------------------------------------------------------------- SliceRec
     * @brief Destructor for SliceRec.
     */
    ~SliceRec() { release(); }
    /** ------------------------------------------------------------------------------------------- SliceRec
     * @brief Creates a slice of the current SliceRec.
     * @param offset Offset into the current slice.
     * @param length Length of the new slice.
     * @return A new SliceRec representing the slice.
     */
    SliceRec slice(size_t offset, size_t length) const {
        SliceRec rec_copy = *this;
        rec_copy.id_ = 0x80000000u | ((rec_copy.id_ & 0x7FFFFFFFu) + offset);
        rec_copy.raw_ = static_cast<char*>(rec_copy.raw_) + offset;
        rec_copy.size_ = length == SIZE_MAX ? size_ - offset : length;
        return rec_copy;
    }
};
/** --------------------------------------------------------------------------------------------------------- IBuffet
 * @class IBuffet
 * @brief Interface for custom buffer definition, if you prefer to use virtual methods instead of
 * descriptors.
 */
class IBuffet : public std::enable_shared_from_this<IBuffet> {
public:
    /** ------------------------------------------------------------------------------------------- FastAllocator
     * @struct FastAllocator
     * @brief A fast allocator for Vulkan buffers, managing a pool of slice records and providing
     * efficient allocation and reclamation of buffer slices.
     */
    struct FastAllocator {
        /// @brief Array of addresses for the buffer slices.
        uint64_t* addresses = nullptr;
        /// @brief Address table for the buffer slices.
        uint64_t address_table;
        /// @brief Atomic counter for picking the next thread ID.
        std::atomic<size_t> pick{0};
        /** --------------------------------------------------------------------------------------- t_id
         * @brief Get the thread-local ID for the current thread.
         * @return The thread-local ID.
         */
        size_t t_id() {
            static thread_local size_t id = pick.fetch_add(1); return id;
        }
        /// @brief Queue of fresh buffer instances.
        moodycamel::ConcurrentQueue<std::shared_ptr<IBuffet>> fresh;
        /// @brief Array of current buffer instances.
        std::array<std::shared_ptr<IBuffet>, 2> current;
        /// @brief Mutexes for protecting access to the current buffer instances.
        std::array<std::mutex, 2> current_mutexes;
        /// @brief Atomic counter for thread IDs.
        std::atomic<size_t> tid{0};
        /// @brief Pool of slice records.
        std::array<SliceRec, 0x100000> pool;
        /// @brief Atomic counter for the next available index in the pool.
        std::atomic<size_t> next_idx{0};
        /// @brief Flag indicating if the allocator is stopping.
        bool stopping = false;
        /** --------------------------------------------------------------------------------------- next_available
         * @brief Get the next available slice index for the given buffer.
         * @param buffer The buffer to allocate from.
         * @param offset The offset within the buffer.
         * @param size The size of the slice.
         * @return The index of the next available slice.
         */
        uint32_t next_available(
            std::shared_ptr<IBuffet> buffer,
            uint32_t offset = 0,
            uint32_t size = 0xFFFFFFFFu
        ) {
            if (offset == 0 && size == 0xFFFFFFFFu) {
                offset = 0;
                size = buffer->size();
            }
            while (true) {
                const size_t idx = next_idx.fetch_add(1) & 0xFFFFF;
                if (pool[idx].try_claim()) {
                    pool[idx].buffer_ = buffer;
                    pool[idx].size_ = size;
                    pool[idx].id_ = offset | 0x80000000u;
                    return idx;
                }
            }
        }
        /** --------------------------------------------------------------------------------------- Next Available Slice
         * @brief Get the next available slice record from the pool.
         * @return Pointer to the next available SliceRec.
         */
        SliceRec* next_available() {
            while (true) {
                const size_t idx = next_idx.fetch_add(1) & 0xFFFFF;
                if (pool[idx].try_claim()) return &pool[idx];
            }
        }
        /// @brief The last record is the null handle's: claimed forever, no bytes, no buffer.
        FastAllocator() { pool[0xFFFFF].id_ = 0x80000000u; }
        ~FastAllocator() { stopping = true; }
        FastAllocator(const FastAllocator&) = delete;
        FastAllocator& operator=(const FastAllocator&) = delete;
        FastAllocator(FastAllocator&&) = delete;
        FastAllocator& operator=(FastAllocator&&) = delete;
    };
    /** --------------------------------------------------------------------------------------- fast_allocator
     * @brief Get the singleton instance of the FastAllocator.
     * @return Reference to the FastAllocator instance.
     */
    static FastAllocator& fast_allocator() {
        static FastAllocator instance;
        return instance;
    }
    /// @brief ID of the slice, initialized to an invalid value.
    uint32_t id_ = 0xFFFFFFFFu;
    /// @brief The Vulkan buffer handle, null for a heap buffer.
    void* buffer_ = nullptr;
    /// @brief The Vulkan memory handle, null for a heap buffer.
    void* memory_ = nullptr;
    /// @brief Pointer to the mapped memory, if any.
    void* mapped_ = nullptr;
    /// @brief Device address of the memory.
    uint64_t address_ = 0;
    /// @brief Size of the buffer.
    uint32_t size_ = 0;
    /// @brief Atomic bump pointer for allocation within the slice.
    std::atomic<uint32_t> bump_{0};
    /// @brief Shared pointer to self, used for lifetime management.
    std::shared_ptr<IBuffet> self_;
    /// @brief Flag indicating if the buffer is novel.
    bool novel_ = false;
    /// @brief Runs over the payload before a dedicated buffer frees it; a typed claim's destructor.
    void (*finalize_)(void*) = nullptr;
    /// @brief Buffers alive right now, pools and dedicated alike, for leak checks.
    inline static std::atomic<uint32_t> live_{0};
    /** ------------------------------------------------------------------------------------------- Thread-local ID
     * @brief Get the thread-local ID for the current thread.
     * @return Thread-local ID.
     */
    static size_t threadid() {
        static thread_local size_t thid =
            fast_allocator().tid.fetch_add(1, std::memory_order_relaxed)
                % fast_allocator().current.size();
        return thid;
    }
    friend struct SliceRec;
    friend class Slice;
public:
    virtual ~IBuffet();
    using SharedPointerConstructorMethod = std::shared_ptr<IBuffet> (*)(size_t, bool);
    virtual void* host_ptr() { return mapped_; }
    virtual const void* host_ptr() const { return mapped_; }
    virtual uint64_t device_address() const { return address_; }
    virtual size_t size() const { return size_; }
    static uint64_t device_table() { return fast_allocator().address_table; }
    static uint32_t live() { return live_.load(std::memory_order_acquire); }
    static uint64_t* addresses() { return fast_allocator().addresses; }
    static uint64_t& addr(uint32_t index) { return fast_allocator().addresses[index]; }
    IBuffet(size_t size, bool novel = false);
    IBuffet(const IBuffet&) = delete;
    IBuffet& operator=(const IBuffet&) = delete;
    IBuffet(IBuffet&&) = delete;
    IBuffet& operator=(IBuffet&&) = delete;
    void* mapped() const { return mapped_; }
    uint64_t address() const { return address_; }
    static Slice embue_id(std::shared_ptr<IBuffet> buffer) {
        ensure_table();
        FastAllocator& allocator = fast_allocator();
        const uint32_t idx = allocator.next_available(buffer, 0, buffer->size());
        allocator.pool[idx].raw_ = buffer->mapped_;
        allocator.addresses[idx] = buffer->address_;
        return Slice(buffer->address_, buffer->mapped_, idx, buffer->size());
    }
    static Slice claim(uint32_t size, bool novel = false) {
        std::shared_ptr<IBuffet> current = IBuffet::current();
        if (size == 0) return Slice{};
        FastAllocator& allocator = fast_allocator();
        if (novel || size > 64u * 1024u * 1024u) {
            std::shared_ptr<IBuffet> dedicated =
                std::make_shared<IBuffet>(size, true);
            const uint32_t idx = allocator.next_available(dedicated, 0, size);
            dedicated->id_ = idx;
            allocator.pool[idx].raw_ = dedicated->mapped_;
            allocator.addresses[idx] = dedicated->address_;
            return Slice(dedicated->address_, dedicated->mapped_, idx, size);
        }
        const uint32_t units = (size + 63u) >> 6;
        const uint32_t start = current->bump_.fetch_add(units, std::memory_order_relaxed);
        if (start + units > (current->size_ >> 6)) {
            std::shared_ptr<IBuffet> next;
            {
                std::lock_guard<std::mutex> lock(allocator.current_mutexes[threadid()]);
                if (current == allocator.current[threadid()]) {
                    if (!allocator.fresh.try_dequeue(next)) {
                        next = pooled();
                        allocator.fresh.enqueue(pooled());
                    }
                    allocator.current[threadid()] = next;
                } else {
                    next = allocator.current[threadid()];
                }
            }
            return next->claim(size);
        }
        const uint32_t offset = start << 6;
        const uint32_t idx = allocator.next_available(current, offset, size);
        if (current->id_ == 0xFFFFFFFFu) current->id_ = idx;
        allocator.pool[idx].raw_ = static_cast<char*>(current->mapped_) + offset;
        allocator.addresses[idx] = current->address_ + offset;
        return Slice(current->address_ + offset, allocator.pool[idx].raw_, idx, size);
    }
    static SliceRec* next_available() { return fast_allocator().next_available(); }
    static SliceRec* ref(uint32_t idx) { return &fast_allocator().pool[idx & 0xFFFFFu]; }
    /** ------------------------------------------------------------------------------------------- Ensure Table
     * @brief Builds the device address table once, after the context has had its chance to freeze.
     */
    static void ensure_table() {
        static std::once_flag built;
        std::call_once(built, &IBuffet::build_table);
    }
    /// @brief The one table: a buffer of the backend the context froze on, never freed.
    static void build_table() {
        ez_vulkan_touch();
        IBuffet* table = new IBuffet(sizeof(uint64_t) * 0x100000);
        fast_allocator().addresses = static_cast<uint64_t*>(table->mapped_);
        fast_allocator().address_table = table->address_;
    }
    static void recycle(IBuffet* buffer) {
        FastAllocator& allocator = fast_allocator();
        if (!allocator.stopping && allocator.fresh.size_approx() < 6) {
            std::memset(buffer->mapped_, 0, buffer->size_);
            buffer->bump_.store(0, std::memory_order_relaxed);
            buffer->id_ = 0xFFFFFFFFu;
            allocator.fresh.enqueue(std::shared_ptr<IBuffet>(buffer, &recycle));
        } else {
            delete buffer;
        }
    }
    static std::shared_ptr<IBuffet> pooled() {
        return std::shared_ptr<IBuffet>(new IBuffet(64u * 1024u * 1024u), &recycle);
    }
    static void shutdown() {
        FastAllocator& allocator = fast_allocator();
        allocator.stopping = true;
        for (std::shared_ptr<IBuffet>& buffer : allocator.current) buffer.reset();
        std::shared_ptr<IBuffet> spare;
        while (allocator.fresh.try_dequeue(spare)) spare.reset();
    }
    static uint32_t copy_rec(const SliceRec& rec, vk::DeviceAddress address) {
        FastAllocator& allocator = fast_allocator();
        SliceRec* fresh = allocator.next_available();
        *fresh = rec;
        const uint32_t idx = static_cast<uint32_t>(fresh - allocator.pool.data());
        allocator.addresses[idx] = address;
        return idx;
    }
    static std::shared_ptr<IBuffet> current() {
        if (fast_allocator().current[threadid()] == nullptr) {
            std::lock_guard<std::mutex> lock(fast_allocator().current_mutexes[threadid()]);
            if (fast_allocator().current[threadid()] == nullptr) {
                ensure_table();
                std::shared_ptr<IBuffet> v;
                if (!fast_allocator().fresh.try_dequeue(v)) {
                    fast_allocator().current[threadid()] = pooled();
                    fast_allocator().fresh.enqueue(pooled());
                } else {
                    fast_allocator().current[threadid()] = std::move(v);
                }
            }
        }
        return fast_allocator().current[threadid()];
    }
    static void* pool() { return &fast_allocator().pool; }  
};
/** --------------------------------------------------------------------------------------------------------- AlignedHeapBuffer
 * @class AlignedHeapBuffer
 * @brief A heap-allocated buffer with 64-byte alignment.
 */
class AlignedHeapBuffer : public IBuffet {
private:
    /// @brief Pointer to the aligned heap buffer.
    void* buffer_ = nullptr;
    /// @brief The size of the aligned heap buffer.
    size_t size_ = 0;
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Creates an aligned heap buffer with the specified size and optional novelty flag.
     * @param size The size of the buffer.
     * @param novel Whether the buffer is novel.
     */
    AlignedHeapBuffer(size_t size = 0, bool novel = false)
    : IBuffet(size, novel), size_(((size + 63) & ~63)) {
        if (size_ == 0) return;
        buffer_ = aligned_alloc(64, size_);
        if (buffer_ == nullptr) {
            std::string error_msg = "AlignedHeapBuffer: failed to allocate " + std::to_string(size_) +
                " bytes";
            ALLIGATOR_THROW(error_msg);
        }
        std::memset(buffer_, 0, size_);
    }
    ~AlignedHeapBuffer() {
        if (buffer_) std::free(buffer_);
    }
    AlignedHeapBuffer(const AlignedHeapBuffer&) = delete;
    AlignedHeapBuffer& operator=(const AlignedHeapBuffer&) = delete;
    AlignedHeapBuffer(AlignedHeapBuffer&&) = delete;
    AlignedHeapBuffer& operator=(AlignedHeapBuffer&&) = delete;

};
inline static vk::DeviceAddress* addresses() { return IBuffet::addresses(); }
inline static SliceRec* next_available() { return IBuffet::next_available(); }
inline static std::shared_ptr<IBuffet> current() { return IBuffet::current(); }
inline static Slice tiny_claim(uint32_t size, bool novel = false) {
    return IBuffet::claim(size, novel);
}
/** --------------------------------------------------------------------------------------------------------- PotentialSlice
 * @class PotentialSlice
 * @brief A slice that gets filled lazily by the provided fulfillment method.
 */
class PotentialSlice {
private:
    struct Details;
    /// @brief Shared pointer to the internal details of the PotentialSlice, created in slice.cpp where Details is complete.
    std::shared_ptr<Details> details_;
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Default constructor for PotentialSlice.
     */
    PotentialSlice(
        Slice (*fulfillment_method)(void* context) = nullptr,
        void* context = nullptr,
        void (*context_deleter)(void* context) = nullptr
    );
    /** ------------------------------------------------------------------------------------------- Copy/move semantics */
    PotentialSlice(const PotentialSlice& other);
    PotentialSlice(PotentialSlice&& other) noexcept;
    PotentialSlice& operator=(const PotentialSlice& other);
    PotentialSlice& operator=(PotentialSlice&& other) noexcept;
    /** ------------------------------------------------------------------------------------------- Destructor */
    ~PotentialSlice();
    /** ------------------------------------------------------------------------------------------- Get Raw Pointer
     * @brief Returns the raw pointer to the underlying slice's data, fulfilling the lazy
     * initialization if necessary.
     * @return The raw pointer to the underlying slice's data.
     */
    void* raw();
    /** ------------------------------------------------------------------------------------------- Get Raw Pointer (const)
     * @brief Returns the raw pointer to the underlying slice's data, fulfilling the lazy
     * initialization if necessary.
     * @return The raw pointer to the underlying slice's data.
     */
    const void* raw() const;
    /** ------------------------------------------------------------------------------------------- Get Typed Data Pointer
     * @brief Returns a typed pointer to the underlying slice's data, fulfilling the lazy
     * initialization if necessary.
     * @tparam T The type of the pointer to return.
     * @return A typed pointer to the underlying slice's data.
     */
    template<typename T>
    T* data() { return static_cast<T*>(raw()); }
    /** ------------------------------------------------------------------------------------------- Get Typed Data Pointer (const)
     * @brief Returns a typed pointer to the underlying slice's data, fulfilling the lazy
     * initialization if necessary.
     * @tparam T The type of the pointer to return.
     * @return A typed pointer to the underlying slice's data.
     */
    template<typename T>
    const T* data() const { return static_cast<const T*>(raw()); }
    /** ------------------------------------------------------------------------------------------- Check Fulfillment
     * @brief Checks if the slice has been fulfilled.
     * @return `true` if the slice has been fulfilled, `false` otherwise.
     */
    bool is_fulfilled() const;
    /** ------------------------------------------------------------------------------------------- Fulfill Slice
     * @brief Fulfills the slice by invoking its fulfillment method if it has not been fulfilled
     * yet.
     */
    void fulfill();
    /** ------------------------------------------------------------------------------------------- Get Raw Slice
     * @brief Returns a pointer to the underlying raw Slice object, which may be nullptr if the
     * slice has not been fulfilled yet.
     * @return A pointer to the underlying raw Slice object.
     */
    Slice* raw_slice() const;
    /** ------------------------------------------------------------------------------------------- Get Subslice
     * @brief Returns a subslice of the current slice, starting at the specified offset and with
     * the specified length. This will fulfill the current slice if it has not been fulfilled yet.
     * @param offset The starting offset of the subslice.
     * @param length The length of the subslice.
     * @return A new Slice representing the subslice.
     */
    Slice slice(size_t offset = 0, size_t length = SIZE_MAX) const;
    /** ------------------------------------------------------------------------------------------- Get Typed Size
     * @brief Returns the size of the slice in terms of the number of elements of type T.
     * @tparam T The type of elements.
     * @return The number of elements of type T in the slice.
     */
    template<typename T>
    size_t size() const {
        if constexpr (sizeof(T) == 8)
            return size_bytes() >> 3;
        else if constexpr (sizeof(T) == 4)
            return size_bytes() >> 2;
        else if constexpr (sizeof(T) == 2)
            return size_bytes() >> 1;
        else if constexpr (sizeof(T) == 1)
            return size_bytes();
        else
            return size_bytes() / sizeof(T);
    }
    /** ------------------------------------------------------------------------------------------- Get Size in Bytes
     * @brief Returns the size of the slice in bytes.
     * @return The size of the slice in bytes.
     */
    size_t size_bytes() const;
    /** ------------------------------------------------------------------------------------------- Free Slice
     * @brief Frees the underlying memory of the slice if it has been allocated.
     */
    void free();
    /** ------------------------------------------------------------------------------------------- Null Slice
     * @brief Checks if the slice is null.
     * @return True if the slice is null, false otherwise.
     */
    bool is_null() const;
    /** ------------------------------------------------------------------------------------------- Valid Slice
     * @brief Checks if the slice is valid.
     * @return True if the slice is valid, false otherwise.
     */
    bool valid() const;
    /** ------------------------------------------------------------------------------------------- Bool Conversion
     * @brief Converts the slice to a boolean value, indicating whether it is valid.
     * @return True if the slice is valid, false otherwise.
     */
    operator bool() const;
    /** ------------------------------------------------------------------------------------------- Adopt Slice
     * @brief Adopts the given Slice, becoming another view of the same underlying memory. This
     * will complete the fulfillment contract for the PotentialSlice, overriding any fufillment
     * methods, and clean up the context if a deleter is set for it.
     * @param slice The Slice to adopt.
     */
    void adopt(Slice slice);
};
/** --------------------------------------------------------------------------------------------------------- SliceT
 * @class SliceT
 * @brief A template class that wraps a Slice and provides type-safe access to its underlying memory.
 * @tparam T The type of elements in the slice.
 */
template <typename T>
class SliceT {
private:
    /// @brief The underlying Slice object that this SliceT wraps.
    Slice slice_;
    /// @brief Claims one T's worth of bytes in a slot carrying T's type, so the last release runs ~T.
    static Slice claim_typed(bool novel_buffer);
    /// @brief Runs ~T over a dedicated buffer's payload as the buffer frees.
    static void finalize(void* payload) { static_cast<T*>(payload)->~T(); }
public:
    /** ------------------------------------------------------------------------------------------- Constructor with Initialization
     * @brief Constructs a SliceT and optionally initializes it with the given arguments.
     * @param initialize If true, allocates and initializes the slice.
     * @param novel_buffer If true, allocates a novel buffer for the slice.
     * @param args Arguments to forward to the constructor of T.
     */
    template <typename ...Args>
        requires std::is_constructible_v<T, Args...>
    SliceT(bool initialize = false, bool novel_buffer = false, Args&&... args) {
        if (initialize) {
            slice_ = claim_typed(novel_buffer);
            new (slice_.raw()) T(std::forward<Args>(args)...);
        }
    }
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs a SliceT from a Slice. The Slice must have a size that is a multiple of
     * the size of type T.
     * @param slice The Slice to wrap.
     */
    explicit SliceT(Slice&& slice) : slice_(std::move(slice)) {}
    /** ------------------------------------------------------------------------------------------- Copy/Move semantics */
    SliceT(const SliceT& other);
    SliceT(SliceT&& other) noexcept;
    SliceT& operator=(const SliceT& other);
    SliceT& operator=(SliceT&& other) noexcept;
    /** ------------------------------------------------------------------------------------------- Destructor */
    ~SliceT() { free(); }
    /** ------------------------------------------------------------------------------------------- Size in elements
     * @brief Returns the size of the slice in elements of type T.
     * @return The size of the slice in elements of type T.
     */
    template<typename U = T>
    size_t size() const {
        return slice_.size_bytes() / sizeof(U);
    }
    /** ------------------------------------------------------------------------------------------- Size in bytes
     * @brief Returns the size of the slice in bytes.
     * @return The size of the slice in bytes.
     */
    size_t size_bytes() const {
        return slice_.size_bytes();
    }
    /** ------------------------------------------------------------------------------------------- Raw pointer access
     * @brief Returns a pointer to the underlying memory of the slice.
     * @return A pointer to the underlying memory of the slice.
     */
    void* raw() { return slice_.raw(); }
    /** ------------------------------------------------------------------------------------------- Raw pointer access - const
     * @brief Returns a const pointer to the underlying memory of the slice.
     * @return A const pointer to the underlying memory of the slice.
     */
    const void* raw() const { return slice_.raw(); }
    /** ------------------------------------------------------------------------------------------- Data access
     * @brief Returns a pointer to the underlying memory of the slice, cast to type U*.
     * @return A pointer to the underlying memory of the slice.
     */
    template<typename U = T>
    U* data() {
        return reinterpret_cast<U*>(slice_.raw());
    }
    /** ------------------------------------------------------------------------------------------- Data access - const
     * @brief Returns a const pointer to the underlying memory of the slice, cast to type U*.
     * @return A const pointer to the underlying memory of the slice.
     */
    template<typename U = T>
    const U* data() const { return reinterpret_cast<const U*>(slice_.raw()); }
    /** ------------------------------------------------------------------------------------------- Get as
     * @brief Returns a zero-copy view for view types or a reference to type U.
     * @return A view or reference to the underlying memory of the slice.
     */
    template<typename U = T>
    decltype(auto) get_as() {
        if constexpr (std::is_same_v<T, U> && std::is_same_v<U, std::string_view>) {
            return std::string_view(
                reinterpret_cast<const char*>(slice_.raw()), slice_.size_bytes()
            );
        } else if constexpr (std::is_same_v<T, U> && requires {
            typename U::element_type;
            requires std::is_same_v<U, std::span<typename U::element_type, U::extent>>;
        }) {
            return U(
                reinterpret_cast<typename U::element_type*>(slice_.raw()),
                slice_.size_bytes() / sizeof(typename U::element_type)
            );
        } else {
            return *reinterpret_cast<U*>(slice_.raw());
        }
    }
    /** ------------------------------------------------------------------------------------------- Get as - const
     * @brief Returns a read-only view for view types or a const reference to type U.
     * @return A read-only view or reference to the underlying memory of the slice.
     */
    template<typename U = T>
    decltype(auto) get_as() const {
        if constexpr (std::is_same_v<T, U> && std::is_same_v<U, std::string_view>) {
            return std::string_view(
                reinterpret_cast<const char*>(slice_.raw()), slice_.size_bytes()
            );
        } else if constexpr (std::is_same_v<T, U> && requires {
            typename U::element_type;
            requires std::is_same_v<U, std::span<typename U::element_type, U::extent>>;
        }) {
            return std::span<const typename U::element_type, U::extent>(
                reinterpret_cast<const typename U::element_type*>(slice_.raw()),
                slice_.size_bytes() / sizeof(typename U::element_type)
            );
        } else {
            return *reinterpret_cast<const U*>(slice_.raw());
        }
    }
    /** ------------------------------------------------------------------------------------------- Underlying slice
     * @brief Returns the underlying untyped Slice.
     * @return The underlying Slice represented by this typed slice.
     */
    Slice slice(size_t offset = 0, size_t length = SIZE_MAX) const {
        return slice_.slice(offset, length);
    }
    /** ------------------------------------------------------------------------------------------- Raw pointer access
     * @brief Returns a pointer to the underlying memory of the slice, cast to type T*.
     * @return A pointer to the underlying memory of the slice.
     */
    T& operator*() {
        return *reinterpret_cast<T*>(slice_.raw());
    }
    /** ------------------------------------------------------------------------------------------- Raw pointer access - const
     * @brief Returns a const pointer to the underlying memory of the slice, cast to const T*.
     * @return A const pointer to the underlying memory of the slice.
     */
    const T& operator*() const {
        return *reinterpret_cast<const T*>(slice_.raw());
    }
    /** ------------------------------------------------------------------------------------------- Pointer access
     * @brief Returns a pointer to the underlying memory of the slice, cast to type T*.
     * @return A pointer to the underlying memory of the slice.
     */
    T* operator->() {
        return reinterpret_cast<T*>(slice_.raw());
    }
    /** ------------------------------------------------------------------------------------------- Pointer access - const
     * @brief Returns a const pointer to the underlying memory of the slice, cast to const T*.
     * @return A const pointer to the underlying memory of the slice.
     */
    const T* operator->() const {
        return reinterpret_cast<const T*>(slice_.raw());
    }
    /** ------------------------------------------------------------------------------------------- Conversion operator
     * @brief Converts the SliceT to a Slice. This allows for implicit conversion to the
     * underlying Slice type.
     */
    operator Slice() const {
        return slice_;
    }
    /** ------------------------------------------------------------------------------------------- Length of the slice
     * @brief Returns the length of the slice in terms of the number of elements of type T.
     * @return The length of the slice.
     */
    size_t length() const {
        if constexpr (std::is_same_v<T, std::string>) {
            return slice_.size_bytes();
        } else if constexpr (requires {
            typename T::value_type;
            requires std::is_same_v<std::vector<typename T::value_type>, T>;
        }) {
            return slice_.size_bytes() / sizeof(typename T::value_type);
        }
        return slice_.size_bytes() / sizeof(T);
    }
    /** ------------------------------------------------------------------------------------------- Free the slice
     * @brief Frees the underlying memory of the slice.
     */
    void free() { slice_.free(); }
    /// @brief Whether the typed handle names no claim.
    bool is_null() const { return slice_.is_null(); }
    /// @brief Whether the typed handle names a claim.
    bool valid() const { return slice_.valid(); }
    operator bool() const { return slice_.valid(); }
    /// @brief The underlying Slice.
    Slice& root_slice() { return slice_; }
    /// @brief The underlying Slice.
    const Slice& root_slice() const { return slice_; }
};
/** --------------------------------------------------------------------------------------------------------- Copy Constructor
 * @brief Copy constructor for SliceT.
 * @param other The SliceT object to copy from.
 */
template <typename T>
SliceT<T>::SliceT(const SliceT& other) : slice_(other.slice_) {}
/** --------------------------------------------------------------------------------------------------------- Operator= Copy
 * @brief Copy assignment operator for SliceT.
 * @param other The SliceT object to copy from.
 * @return Reference to the current SliceT object.
 */
template <typename T>
SliceT<T>::SliceT(SliceT&& other) noexcept : slice_(std::move(other.slice_)) {}
/** --------------------------------------------------------------------------------------------------------- Operator= Copy
 * @brief Copy assignment operator for SliceT.
 * @param other The SliceT object to copy from.
 * @return Reference to the current SliceT object.
 */
template <typename T>
SliceT<T>& SliceT<T>::operator=(const SliceT& other) {
    if (this != &other) slice_ = other.slice_;
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Operator= Move
 * @brief Move assignment operator for SliceT.
 * @param other The SliceT object to move from.
 * @return Reference to the current SliceT object.
 */
template <typename T>
SliceT<T>& SliceT<T>::operator=(SliceT&& other) noexcept {
    if (this != &other) slice_ = std::move(other.slice_);
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Claim Typed
 * @brief One T's worth of bytes; a T with a destructor gets a dedicated buffer so the last handle's release runs ~T.
 * @param novel_buffer Whether the claim gets a dedicated buffer.
 * @return The claim.
 */
template <typename T>
Slice SliceT<T>::claim_typed(bool novel_buffer) {
    static_assert(alignof(T) <= 64, "A typed claim is SIMD aligned; a T aligned past 64 bytes is not supported yet");
    if constexpr (std::is_trivially_destructible_v<T>) return IBuffet::claim(sizeof(T), novel_buffer);
    Slice claim = IBuffet::claim(sizeof(T), true);
    claim.rec().buffer_->finalize_ = &SliceT<T>::finalize;
    return claim;
}
} // namespace buffetalligator