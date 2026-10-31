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
class PotentialSlice; class SliceQueue; class SliceRec;
struct OnZero; struct Entry;
void ez_vulkan_touch();
template<typename T> class SliceT;
template<typename T> class SliceMapT;
template<typename T> class PotentialSliceT;
template<typename T> class SliceQueueT;
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
/// @brief The smallest buffer a chain links; a buffer this large is checked against the host
/// before it is touched.
inline constexpr size_t SLAB_FLOOR_BYTES = 64ull << 20;
/** --------------------------------------------------------------------------------------------------------- Slice
 * @class Slice
 * @brief A slice of memory in the buffet alligator: 32 bytes by value, the device address and size a
 * shader reads first, then the parent it counts on, its type word and its count.
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
     * @brief Copies `size` bytes from a view into a fresh claim.
     * @param data The bytes to copy from.
     * @param size How many bytes.
     * @param novel Whether the claim gets a dedicated buffer.
     */
    Slice(const void* data, size_t size, bool novel = false);
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
    template<typename T = uint8_t>
    T* data() { return static_cast<T*>(raw()); }
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Returns a const pointer to the data of the slice cast to the specified type.
     * @tparam T The type to cast the data pointer to.
     * @return Const pointer to the data of the specified type.
     */
    template<typename T = uint8_t> const
    T* data() const { return static_cast<const T*>(raw()); }
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Returns a reference to the data of the slice cast to the specified type.
     * @tparam T The type to cast the data pointer to.
     * @return Reference to the data of the specified type.
     */
    template<typename T = uint8_t>
    T& get_as() { return *data<T>(); }
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Returns a const reference to the data of the slice cast to the specified type.
     * @tparam T The type to cast the data pointer to.
     * @return Const reference to the data of the specified type.
     */
    template<typename T = uint8_t>
    const T& get_as() const { return *data<T>(); }
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Returns a reference to the data of the slice cast to the specified type.
     * @tparam T The type to cast the data pointer to.
     * @return Reference to the data of the specified type.
     */
    template<typename T = uint8_t>
    T& operator*() { return *data<T>(); }
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Returns a const reference to the data of the slice cast to the specified type.
     * @tparam T The type to cast the data pointer to.
     * @return Const reference to the data of the specified type.
     */
    template<typename T = uint8_t>
    const T& operator*() const { return *data<T>(); }
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Returns a pointer to the data of the slice cast to the specified type.
     * @tparam T The type to cast the data pointer to.
     * @return Pointer to the data of the specified type.
     */
    template<typename T = uint8_t>
    T* operator->() { return data<T>(); }
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Returns a const pointer to the data of the slice cast to the specified type.
     * @tparam T The type to cast the data pointer to.
     * @return Const pointer to the data of the specified type.
     */
    template<typename T = uint8_t>
    const T* operator->() const { return data<T>(); }
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
    template<typename T = uint8_t>
    size_t size() const { return size_bytes() / sizeof(T); }
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
    /** ------------------------------------------------------------------------------------------- Resize
     * @brief Resizes the claim: a preserving shrink stays in place, anything else is a fresh
     * claim, with the old bytes copied over when preserved; a null Slice resizes into a plain
     * claim.
     * @param new_size The new size in bytes; zero frees.
     * @param preserve_data Whether the existing bytes carry over.
     * @param novel_buffer Whether a fresh claim gets a dedicated buffer.
     */
    void resize(size_t new_size, bool preserve_data = true, bool novel_buffer = false);
    /** ------------------------------------------------------------------------------------------- Is Null
     * @brief Checks if the slice is null (i.e., does not name a claim).
     * @return True if the slice is null, false otherwise.
     */
    bool is_null() const { return id_ == 0xFFFFFFFFu; }
    /** ------------------------------------------------------------------------------------------- Valid
     * @brief Checks if the slice names a claim.
     * @return True if the slice is valid, false otherwise.
     */
    bool valid() const { return id_ != 0xFFFFFFFFu; }
    /** ------------------------------------------------------------------------------------------- Operator Bool
     * @brief Checks if the slice is valid in a boolean context.
     * @return True if the slice is valid, false otherwise.
     */
    operator bool() const { return id_ != 0xFFFFFFFFu; }
    /** ------------------------------------------------------------------------------------------- Pool Index
     * @brief Retrieves the pool index of the slice.
     * @return The pool index.
     */
    uint32_t pool_index() const { return id_ & 0xFFFFFu; }
    /** ------------------------------------------------------------------------------------------- Capacity
     * @brief Retrieves the capacity of the slice in bytes, rounded up to 64-byte units.
     * @return The capacity in bytes.
     */
    size_t capacity() const { return (size_bytes() + 63) & ~size_t{63}; }
private:
    uint32_t id_ = 0xFFFFFFFFu;
    Slice(uint64_t,void*,uint32_t,uint32_t);
    friend class IBuffet;
};
/** --------------------------------------------------------------------------------------------------------- Packed Size
 * @class PackedSize
 * @brief Represents a packed size with different unit types (single, vector, matrix, page) and provides
 * mechanisms for claiming and releasing a bit for slot ownership.
 */
class PackedSize {
private:
    uint32_t size_ = 0;
    PackedSize& operator=(PackedSize&& other);
public:
    PackedSize() = default;
    PackedSize(size_t size);
    PackedSize& operator=(size_t size);
    PackedSize(const PackedSize&) = delete;
    PackedSize& operator=(const PackedSize&) = delete;
    PackedSize(PackedSize&&) = delete;
    ~PackedSize();
    operator size_t() const;
    size_t unit() const;
    size_t size() const;
    size_t capacity() const;
    bool try_claim();
    void release();
};
/** --------------------------------------------------------------------------------------------------------- Packed Offset
 * @class PackedOffset
 * @brief Represents a packed offset with different unit types and provides access to the offset value.
 */
class PackedOffset {
private:
    uint32_t offset_ = 0;
    PackedOffset& operator=(PackedOffset&& other);
public:
    PackedOffset(size_t offset = 0);
    PackedOffset(const PackedOffset&) = delete;
    PackedOffset& operator=(const PackedOffset&) = delete;
    PackedOffset(PackedOffset&&) = delete;
    ~PackedOffset() = default;
    operator size_t() const;
    size_t unit() const;
    size_t offset() const;
};
/** --------------------------------------------------------------------------------------------------------- SliceRec
 * @struct SliceRec
 * @brief A record representing a slice of a buffer, including its raw pointer, associated buffer,
 * ID, and size. Provides mechanisms for claiming and releasing slices.
 */
class alignas(32) SliceRec {
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs a SliceRec with the given raw pointer, buffer, ID, and size.
     * @param raw Raw pointer to the slice data.
     * @param buffer Shared pointer to the associated buffer.
     * @param size Size of the slice.
     */
    SliceRec(void* raw, std::shared_ptr<IBuffet> buffer, uint32_t size)
    : raw_(raw), buffer_(buffer), size_(size) {}
    /** ------------------------------------------------------------------------------------------- SliceRec
     * @brief Creates a sub-slice of the current SliceRec.
     * @param offset Offset into the current slice.
     * @param length Length of the new slice.
     * @return A new SliceRec representing the slice.
     */
    SliceRec* sub_slice(size_t offset, size_t length) const;
    //--------------------------------------------------------------------------------------------- Accessors, etc
    bool try_claim(); void release(); SliceRec() = default; ~SliceRec();
    SliceRec(const SliceRec& other) = delete;
    SliceRec& operator=(const SliceRec& other) = delete;
    SliceRec(SliceRec&& other) noexcept = delete;
    SliceRec& operator=(SliceRec&& other) noexcept = delete;
    size_t size() const;
    void set_size(size_t size);
    size_t capacity() const;
    std::shared_ptr<IBuffet> buffer() const;
    void set_buffer(std::shared_ptr<IBuffet> buffer);
    void* raw();
    const void* raw() const;
    void set_raw(void* raw);
    size_t id() const;
    void set(std::shared_ptr<IBuffet> buffer, void* raw, size_t size);
    void set(const SliceRec& other);
    uint64_t& address();
    const uint64_t& address() const;
private:
    void* raw_ = nullptr;
    std::shared_ptr<IBuffet> buffer_;
    PackedSize size_;
    friend class IBuffet;
    friend class Slice;
};
/** --------------------------------------------------------------------------------------------------------- IBuffet
 * @class IBuffet
 * @brief Interface for custom buffer definition, if you prefer to use virtual methods instead of
 * descriptors.
 */
class IBuffet : public std::enable_shared_from_this<IBuffet> {
public:
    struct Allocator;
    virtual ~IBuffet();
    IBuffet(size_t size = 0);
    virtual void* host_ptr() = 0;
    virtual const void* host_ptr() const = 0;
    virtual void allocate();
    virtual uint64_t device_address() const;
    virtual size_t size() const = 0;
    virtual void set_size(size_t size) = 0;
    virtual void free() {}
    static uint64_t device_table();
    static uint64_t* addresses();
    static uint64_t& addr(uint32_t index);
    using DefaultBufferFactoryFn = std::shared_ptr<IBuffet> (*)();
    /** ------------------------------------------------------------------------------------------- Set Default Factory
     * @brief Registers the factory every chain buffer comes from and builds the address table with it.
     * @param factory The factory, a static method of the backend's buffer type.
     */
    static void set_default_factory(DefaultBufferFactoryFn factory);
    IBuffet(const IBuffet&) = delete;
    IBuffet& operator=(const IBuffet&) = delete;
    IBuffet(IBuffet&&) = delete;
    IBuffet& operator=(IBuffet&&) = delete;
    void* mapped() const;
    uint64_t address() const;
    /** ------------------------------------------------------------------------------------------- Claim
     * @brief Claims a slice of the buffer.
     * @param size The size of the slice.
     * @param novel Whether to allocate a novel buffer.
     * @return The claimed slice.
     */
    static Slice claim(uint32_t size, bool novel = false);
    static SliceRec* next_available();
    static SliceRec* ref(uint32_t idx);
    static std::shared_ptr<IBuffet> current();
    static void* pool();
    /** ------------------------------------------------------------------------------------------- FastAllocator
     * @struct FastAllocator
     * @brief While the Allocator is an embedded structure, this is actually a static singleton
     * It manages buffer slices and provides efficient allocation and reclamation mechanisms.
     */
    struct Allocator {
        /// @brief Array of addresses for the buffer slices.
        uint64_t* addresses = nullptr;
        /// @brief Address table for the buffer slices.
        uint64_t address_table;
        /// @brief The buffer the address table lives in, made by the default factory.
        std::shared_ptr<IBuffet> table;
        /// @brief Atomic counter for picking the next thread ID.
        std::atomic<size_t> pick{0};
        /// @brief Queue of fresh buffer instances.
        moodycamel::ConcurrentQueue<std::shared_ptr<IBuffet>> fresh;
        /// @brief Array of current buffer instances.
        std::array<std::shared_ptr<IBuffet>, 2> current;
        /// @brief Atomic counter for thread IDs.
        std::atomic<size_t> tid{0};
        /// @brief Pool of slice records.
        std::array<SliceRec, 0x100000> pool;
        /// @brief Atomic counter for the next available index in the pool.
        std::atomic<size_t> next_idx{0};
        /// @brief Set by shutdown, so recycle deletes buffers instead of returning them to fresh.
        bool stopping = false;
        /// @brief The thread ID
        size_t t_id();
        uint32_t next_available(
            std::shared_ptr<IBuffet> buffer,
            size_t offset = 0,
            size_t size = 0xFFFFFFFFu
        );
        SliceRec* next_available();
        Allocator() = default;
        ~Allocator() = default;
        Allocator(const Allocator&) = delete;
        Allocator& operator=(const Allocator&) = delete;
        Allocator(Allocator&&) = delete;
        Allocator& operator=(Allocator&&) = delete;
    };
    static Allocator& allocator();
protected:
    PackedSize size_;
    std::atomic<uint32_t> bump_{0};
    static size_t threadid();
    static DefaultBufferFactoryFn& default_factory();
    static void ensure_table();
    static void build_table();
    static void recycle(IBuffet* buffer);
    static void shutdown();
    static uint32_t copy_rec(const SliceRec& rec, vk::DeviceAddress address);
    static Slice embue_id(std::shared_ptr<IBuffet> buffer);
    friend class SliceRec;
    friend class Slice;
};
/** --------------------------------------------------------------------------------------------------------- AlignedHeapBuffer
 * @class AlignedHeapBuffer
 * @brief A heap-allocated buffer with 64-byte alignment.
 */
class AlignedHeapBuffer : public IBuffet {
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Creates an aligned heap buffer with the specified size and optional novelty flag.
     * @param size The size of the buffer.
     * @param novel Whether the buffer is novel.
     */
    AlignedHeapBuffer(size_t size = 0) : IBuffet(size) {}
    AlignedHeapBuffer(const AlignedHeapBuffer& other) = delete;
    AlignedHeapBuffer& operator=(const AlignedHeapBuffer& other) = delete;
    AlignedHeapBuffer(AlignedHeapBuffer&& other) = delete;
    AlignedHeapBuffer& operator=(AlignedHeapBuffer&& other) = delete;
    ~AlignedHeapBuffer();
    void allocate() override;
    void free() override;
    size_t size() const override;
    void set_size(size_t size) override;
    void* host_ptr() override;
    const void* host_ptr() const override;
private:
    /// @brief The aligned heap allocation, null until allocate.
    void* host_ptr_ = nullptr;
};
inline static vk::DeviceAddress* addresses() { return IBuffet::addresses(); }
inline static SliceRec* next_available() { return IBuffet::next_available(); }
inline static std::shared_ptr<IBuffet> current() { return IBuffet::current(); }
inline static Slice claim_slice(uint32_t size, bool novel = false) { return IBuffet::claim(size, novel); }
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
     * @param fulfillment_method The method to fulfill the slice.
     * @param context The context for the fulfillment method.
     * @param context_deleter The deleter for the context.
     */
    PotentialSlice(
        Slice (*fulfillment_method)(void* context) = nullptr,
        void* context = nullptr,
        void (*context_deleter)(void* context) = nullptr
    );
    /** ------------------------------------------------------------------------------------------- Constructor with shared context
     * @brief Constructor for PotentialSlice with a shared context.
     * @param fulfillment_method The method to fulfill the slice.
     * @param context The shared context for the fulfillment method.
     */
    PotentialSlice(
        Slice (*fulfillment_method)(std::shared_ptr<void> context),
        std::shared_ptr<void> context = nullptr
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
    /** ------------------------------------------------------------------------------------------- Slice as
     * @brief Returns a reference to the data of the slice cast to the specified type.
     * @tparam T The type to cast the data pointer to.
     * @return Reference to the data of the specified type.
     */
    template<typename T = uint8_t>
    T& get_as() { return *data<T>(); }
    /** ------------------------------------------------------------------------------------------- Slice as (const)
     * @brief Returns a const reference to the data of the slice cast to the specified type.
     * @tparam T The type to cast the data pointer to.
     * @return Const reference to the data of the specified type.
     */
    template<typename T = uint8_t>
    const T& get_as() const { return *data<T>(); }
    /** ------------------------------------------------------------------------------------------- Arrow Operator
     * @brief Provides access to the underlying data pointer using the arrow operator.
     * @tparam T The type of the pointer to return.
     * @return A typed pointer to the underlying slice's data.
     */
    template<typename T = uint8_t>
    T* operator->() { return data<T>(); }
    /** ------------------------------------------------------------------------------------------- Arrow Operator (const)
     * @brief Provides access to the underlying data pointer using the arrow operator.
     * @tparam T The type of the pointer to return.
     * @return A typed pointer to the underlying slice's data.
     */
    template<typename T = uint8_t>
    const T* operator->() const { return data<T>(); }
    /** ------------------------------------------------------------------------------------------- Dereference Operator
     * @brief Provides access to the underlying data using the dereference operator.
     * @tparam T The type of the reference to return.
     * @return A reference to the underlying data of the specified type.
     */
    template<typename T = uint8_t>
    T& operator*() { return as<T>(); }
    /** ------------------------------------------------------------------------------------------- Dereference Operator (const)
     * @brief Provides access to the underlying data using the dereference operator.
     * @tparam T The type of the reference to return.
     * @return A const reference to the underlying data of the specified type.
     */
    template<typename T = uint8_t>
    const T& operator*() const { return as<T>(); }
    /** ------------------------------------------------------------------------------------------- Check Fulfillment
     * @brief Checks if the slice has been fulfilled.
     * @return `true` if the slice has been fulfilled, `false` otherwise.
     */
    bool is_fulfilled() const;
    /** ------------------------------------------------------------------------------------------- Fulfill Slice
     * @brief Schedules a fulfillment of the slice if it has not been fulfilled yet asynchronously.
     * This can be called ahead of time to pre-fetch the slice's data if we know we're going to
     * need it, for example if looping through a set of these, call the `fulfill()` method on all
     * all of them first, then process them as needed.
     */
    void fulfill() const;
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
public:
    /** ------------------------------------------------------------------------------------------- Constructor with Initialization
     * @brief Constructs a SliceT and optionally initializes it with the given arguments.
     * @param initialize If true, allocates and initializes the slice.
     * @param count The number of elements of type T to allocate.
     * @param novel_buffer If true, allocates a novel buffer for the slice.
     * @param args Arguments to forward to the constructor of T.
     */
    template <typename ...Args>
        requires std::is_constructible_v<T, Args...>
    SliceT(
        bool initialize = false,
        size_t count = 1,
        bool novel_buffer = false,
        Args&&... args
    ) {
        if (initialize) {
            slice_.resize(count * sizeof(T), false, novel_buffer);
            for (size_t i = 0; i < count; ++i) {
                new (static_cast<T*>(slice_.raw()) + i) T(std::forward<Args>(args)...);
            }
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
    /** ------------------------------------------------------------------------------------------- Initialize
     * @brief Initializes the slice with the given number of elements and constructor arguments.
     * @tparam Args The types of the constructor arguments for T.
     * @param count The number of elements of type T to allocate.
     * @param novel_buffer If true, allocates a novel buffer for the slice.
     * @param args Arguments to forward to the constructor of T.
     */
    template<typename... Args>
    void initialize(size_t count = 1, bool novel_buffer = false, Args&&... args) {
        slice_.resize(count * sizeof(T), false, novel_buffer);
        for (size_t i = 0; i < count; ++i) {
            new (static_cast<T*>(slice_.raw()) + i) T(std::forward<Args>(args)...);
        }
    }
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
     * @tparam U The type to cast the underlying memory to.
     * @return A pointer to the underlying memory of the slice.
     */
    template<typename U = T>
    U* data() {
        return reinterpret_cast<U*>(slice_.raw());
    }
    /** ------------------------------------------------------------------------------------------- Data access - const
     * @brief Returns a const pointer to the underlying memory of the slice, cast to type U*.
     * @tparam U The type to cast the underlying memory to.
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
    /** ------------------------------------------------------------------------------------------- Free the slice
     * @brief Frees the underlying memory of the slice.
     */
    void free(bool destroy = false) {
        if (destroy) {
            for (size_t i = 0; i < size(); ++i) {
                reinterpret_cast<T*>(slice_.raw())[i].~T();
            }
        }
        slice_.free();
    }
    /// @brief Whether the typed handle names no claim.
    bool is_null() const { return slice_.is_null(); }
    /// @brief Whether the typed handle names a claim.
    bool valid() const { return slice_.valid(); }
    operator bool() const { return slice_.valid(); }
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
} // namespace buffetalligator