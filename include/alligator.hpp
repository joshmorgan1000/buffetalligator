#pragma once
/** --------------------------------------------------------------------------------------------------------- BuffetAlligator
 * @file buffetalligator.hpp
 * @brief Unified header for the BuffetAlligator.
 * (was supposed to be "buffer allocator" but voice-to-text got it wrong and it stuck)
 *                       _.---._
 *                  _.-'   o   `--..__
 *              _.-'                   `-._
 *          _.-'                  o     o  \
 *         /     /              .__________/
 *        /     /   `--.________/  /  /  /
 *       /     /        `-.____________.'
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
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <concepts>
#include <future>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
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
#include <iomanip>
#include <iostream>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <unordered_set>
#include <moodycamel/concurrentqueue.h>
#include <moodycamel/blockingconcurrentqueue.h>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- AlligatorException
 * @class AlligatorException
 * @brief Exception class for Alligator-related errors.
 */
EXCEPTION_CLASS(Alligator)
#define ALLIGATOR_THROW(msg) throw AlligatorException(msg)
/** ------------------------------------------------------------------------------------- Forward Declarations */
class Alligator; class Slice; class Memory;
class SliceQueue; class SliceChannel;
struct SliceEntry; class PrioritySlice;
class HeapSlice; struct SliceHandle; struct GPUBuf;
class AlignedHeapBuffer; class VulkanBuffer;
/** --------------------------------------------------------------------------------------------------------- IsABuffetType
 * @brief Concept to check if a type T satisfies the requirements of a Buffet.
 */
template<typename T>
concept IsABuffetType = requires(T t) {
    { T::deleter() } -> std::same_as<void(*)(void*)>;
    { T::host_ptr(std::declval<void*>(), std::declval<size_t>()) } -> std::same_as<void*>;
    { t.raw() } -> std::same_as<void*>;
    { T::size_of(std::declval<void*>()) } -> std::same_as<size_t>;
    { t.size() } -> std::same_as<size_t>;
    { T::factory(std::declval<size_t>()) } -> std::same_as<void*>;
    { T::type_idx() } -> std::same_as<size_t>;
    { T() == nullptr } -> std::same_as<bool>;
    { T::default_size() } -> std::same_as<size_t>;
    { T::type_name() } -> std::same_as<const char*>;
    { T::device_address(std::declval<void*>()) } -> std::same_as<uint64_t>;
} && (!std::is_copy_constructible_v<T> && !std::is_copy_assignable_v<T>)
&& (std::is_move_constructible_v<T> && std::is_move_assignable_v<T>)
&& std::is_convertible_v<T, bool>;
/** --------------------------------------------------------------------------------------------------------- BuffetDescriptor
 * @struct BuffetDescriptor
 * @brief Descriptor for a memory buffet, containing function pointers and metadata.
 */
struct BuffetDescriptor {
    const char* type_name;
    void (*deleter)(void*);
    void* (*host_ptr)(void*, size_t);
    size_t (*size_of)(void*);
    void* (*factory)(size_t);
    uint8_t type_idx;
    size_t default_size;
    uint64_t (*device_address)(void*);
};
/** --------------------------------------------------------------------------------------------------------- BuffetDescriptors
 * @struct BuffetDescriptors
 * @brief Provides access to all buffet descriptors.
 */
class BuffetDescriptors {
private:
    static std::atomic<size_t>& next_index();
    static std::array<const BuffetDescriptor*, 8>& list();
public:
    /** --------------------------------------------------------------------------------------------------------- descriptor_for
     * @brief Returns the BuffetDescriptor for a given buffet type T.
     * @tparam T The buffet type.
     * @param T* A pointer to the buffet type (used for type deduction).
     * @return A pointer to the static BuffetDescriptor for the type T.
     */
    template<IsABuffetType T>
    static const BuffetDescriptor* descriptor_for(T*) {
        static const BuffetDescriptor descriptor = {
            T::type_name(), &BuffetDescriptors::tracked_deleter<T>, T::host_ptr, T::size_of,
            &BuffetDescriptors::tracked_factory<T>, static_cast<uint8_t>(T::type_idx()), T::default_size(),
            T::device_address
        };
        return &descriptor;
    }
    /** --------------------------------------------------------------------------------------------------------- tracked_factory
     * @brief The factory every descriptor binds: allocates through T and reports the bytes to the tracker.
     * @tparam T The buffet type.
     * @param size The requested size in bytes.
     * @return The new buffet handle.
     */
    template<IsABuffetType T>
    static void* tracked_factory(size_t size) {
        void* buffet = T::factory(size);
        try {
            record_allocation(T::type_idx(), T::size_of(buffet), buffet);
        } catch (...) {
            T::deleter()(buffet);
            throw;
        }
        return buffet;
    }
    /** --------------------------------------------------------------------------------------------------------- tracked_deleter
     * @brief The deleter every descriptor binds: reports the bytes to the tracker, then releases through T.
     * @tparam T The buffet type.
     * @param buffet The buffet handle being released.
     */
    template<IsABuffetType T>
    static void tracked_deleter(void* buffet) {
        record_deallocation(T::type_idx(), T::size_of(buffet), buffet);
        T::deleter()(buffet);
    }
    /** --------------------------------------------------------------------------------------------------------- record_allocation
     * @brief Reports one completed buffet allocation to the memory tracker.
     * @param type The buffet type index.
     * @param bytes The allocation size in bytes.
     * @param buffet The new buffet handle.
     */
    static void record_allocation(size_t type, size_t bytes, const void* buffet);
    /** --------------------------------------------------------------------------------------------------------- record_deallocation
     * @brief Reports one buffet release to the memory tracker.
     * @param type The buffet type index.
     * @param bytes The allocation size in bytes.
     * @param buffet The buffet handle being released.
     */
    static void record_deallocation(size_t type, size_t bytes, const void* buffet);
    static const BuffetDescriptor* get(size_t type);
    static size_t register_descriptor(const BuffetDescriptor* descriptor);
    static size_t count();
    static const BuffetDescriptor*& default_placement();
};
/** --------------------------------------------------------------------------------------------------------- AlignedHeapBuffer
 * @class AlignedHeapBuffer
 * @brief A heap-allocated buffer with 64-byte alignment.
 */
class alignas(16) AlignedHeapBuffer {
private: void* buffer_ = nullptr; size_t size_ = 0;
public:
    operator bool() const { return buffer_ != nullptr; }
    bool operator==(std::nullptr_t) const { return buffer_ == nullptr; }
    bool operator!=(std::nullptr_t) const { return buffer_ != nullptr; }
    explicit AlignedHeapBuffer(size_t size = 0);
    AlignedHeapBuffer(const AlignedHeapBuffer&) = delete;
    AlignedHeapBuffer& operator=(const AlignedHeapBuffer&) = delete;
    AlignedHeapBuffer(AlignedHeapBuffer&& other) noexcept
    : buffer_(std::exchange(other.buffer_, nullptr)), size_(std::exchange(other.size_, 0)) {}
    AlignedHeapBuffer& operator=(AlignedHeapBuffer&& other) noexcept {
        std::swap(buffer_, other.buffer_); std::swap(size_, other.size_); return *this;
    }
    ~AlignedHeapBuffer() { if (buffer_) { free(buffer_); } size_ = 0; buffer_ = nullptr; }
    static void deleter_impl(void* ptr) { delete static_cast<AlignedHeapBuffer*>(ptr); }
    static void (*deleter())(void*) { return deleter_impl; }
    static void* host_ptr_impl(void* ptr, size_t offset) {
        return static_cast<uint8_t*>(static_cast<AlignedHeapBuffer*>(ptr)->buffer_) + offset;
    }
    static void* host_ptr(void* ptr, size_t offset) { return host_ptr_impl(ptr, offset); }
    static void* (*host_ptr())(void*, size_t) { return host_ptr_impl; }
    static size_t size_of_impl(void* ptr) { return static_cast<AlignedHeapBuffer*>(ptr)->size_; }
    static size_t size_of(void* ptr) { return size_of_impl(ptr); }
    static size_t (*size_of())(void*) { return size_of_impl; }
    static void* factory(size_t size) {
        return new AlignedHeapBuffer(size);
    }
    static size_t default_size() { return 64 * 1024 * 1024; }
    static size_t type_idx() { return 0; }
    static const char* type_name() { return "AlignedHeapBuffer"; }
    static uint64_t device_address(void*) { return 0; }
    size_t size() const { return size_; }
    void* raw() { return buffer_; }
    const void* raw() const { return buffer_; }
};
static_assert(IsABuffetType<AlignedHeapBuffer>, "AlignedHeapBuffer must satisfy IsABuffetType");
static_assert(sizeof(AlignedHeapBuffer) == 16, "AlignedHeapBuffer must be 16 bytes");
/** --------------------------------------------------------------------------------------------------------- SharedBuffet
 * @class SharedBuffet
 * @brief A reference-counted shared buffer wrapper for buffet types.
 */
class SharedBuffet {
private:
    /// @brief Token holding the shared buffer, its host pointer function, deleter, and reference count.
    std::tuple<void*, const BuffetDescriptor*, std::atomic<uint32_t>, uint32_t>* token_ = nullptr;
    void* buf() { return std::get<0>(*token_); }
    const void* buf() const { return std::get<0>(*token_); }
    const BuffetDescriptor* descriptor() const { return std::get<1>(*token_); }
    uint32_t& sizex64() const { static uint32_t zero = 0; return token_ ? std::get<3>(*token_) : zero; }
    std::atomic<uint32_t>& refs() { return std::get<2>(*token_); }
    void new_ref() { if (token_) refs().fetch_add(1, std::memory_order_acq_rel); }
    void free();
public:
    SharedBuffet() = default;
    explicit SharedBuffet(size_t size);
    template<IsABuffetType T = AlignedHeapBuffer>
    explicit SharedBuffet(T&& buffet) {
        const size_t bytes = buffet.size();
        if (bytes > (uint64_t{UINT32_MAX} << 6) || (bytes & 63) != 0) {
            ALLIGATOR_THROW("SharedBuffet requires a representable 64-byte-granule buffer");
        }
        const BuffetDescriptor* placement = BuffetDescriptors::descriptor_for(static_cast<T*>(nullptr));
        auto owned = std::make_unique<T>(std::move(buffet));
        token_ = new std::tuple<void*, const BuffetDescriptor*, std::atomic<uint32_t>, uint32_t>(
            owned.get(), placement, 1, static_cast<uint32_t>(bytes >> 6));
        try {
            BuffetDescriptors::record_allocation(placement->type_idx, bytes, owned.get());
        } catch (...) {
            delete token_;
            token_ = nullptr;
            throw;
        }
        owned.release();
    }
    SharedBuffet(const SharedBuffet& other)
    : token_(other.token_) { if (token_) refs().fetch_add(1, std::memory_order_acq_rel); }
    SharedBuffet& operator=(const SharedBuffet& other) {
        if (this != &other) { free(); token_ = other.token_; new_ref(); }
        return *this;
    }
    SharedBuffet(SharedBuffet&& other) noexcept
    : token_(other.token_) { other.token_ = nullptr; }
    SharedBuffet& operator=(SharedBuffet&& other) noexcept {
        if (this != &other) { free(); token_ = other.token_; other.token_ = nullptr; }
        return *this;
    }
    ~SharedBuffet() { free(); }
    operator bool() const { return token_ && buf() != nullptr; }
    bool operator==(std::nullptr_t) const { return !token_ || buf() == nullptr; }
    bool operator!=(std::nullptr_t) const { return token_ && buf() != nullptr; }
    void* buffer() { return token_ ? buf() : nullptr; }
    const void* buffer() const { return token_ ? buf() : nullptr; }
    void* raw(size_t offset = 0) {
        return token_ ? static_cast<char*>(descriptor()->host_ptr(buf(), offset)) : nullptr;
    }
    const void* raw(size_t offset = 0) const {
        return token_ ? static_cast<const char*>(
            descriptor()->host_ptr(const_cast<void*>(buf()), offset)
        ) : nullptr;
    }
    size_t size() const { return static_cast<size_t>(sizex64()) << 6; }
};
static_assert(sizeof(SharedBuffet) == 8, "SharedBuffet must be 8 bytes");
/** --------------------------------------------------------------------------------------------------------- ChainBuffet
 * @class ChainBuffet
 * @brief Abstract base class representing a bump pointer arena buffer that automatically pre-allocates
 * several memory blocks in advance to optimize allocation performance.
 */
class alignas(64) ChainBuffet {
public:
    class ChainBuffetToken {
    private:
        struct Control;
        Control* token_ = nullptr;
        void add_ref(); void free();
        ChainBuffetToken(const ChainBuffet* buffer = nullptr);
        friend class ChainBuffet;
        friend struct SliceEntry;
    public:
        ChainBuffetToken(const ChainBuffetToken& other);
        ChainBuffetToken& operator=(const ChainBuffetToken& other);
        ChainBuffetToken(ChainBuffetToken&& other) noexcept;
        ChainBuffetToken& operator=(ChainBuffetToken&& other) noexcept;
        ~ChainBuffetToken();
        const BuffetDescriptor* descriptor() const;
        bool is_novel() const;
        /** ------------------------------------------------------------------------------------------- Buffet
         * @brief The buffet handle this token keeps alive, as the descriptor's hooks expect it.
         * @return The buffet handle.
         */
        void* buffet() const;
        void* raw(size_t offset);
        const void* raw(size_t offset) const;
    };
private:
    void* buffer_ = nullptr;
    const BuffetDescriptor* descriptor = nullptr;
    ChainBuffetToken::Control* token_ = nullptr;
    std::atomic<size_t> bump_ptr_{0};
    std::atomic<ChainBuffet*> next_{nullptr};
    std::atomic<bool> owns_token_{false};
    ChainBuffet* next(bool allocate_next = true);
    static std::atomic<ChainBuffet*>& current_for(size_t idx);
    static void release_chains();
    ChainBuffet(const BuffetDescriptor* desc, size_t size, bool owned, bool novel);
    void free();
    friend class ChainBuffetToken;
    friend class Alligator;
public:
    ChainBuffet() = default; bool valid() const { return buffer_ != nullptr; }
    operator bool() const { return valid(); } ~ChainBuffet() { free(); }
    template<IsABuffetType T = AlignedHeapBuffer>
    explicit ChainBuffet(size_t size, bool allocate_next = false)
    : ChainBuffet(BuffetDescriptors::descriptor_for(static_cast<T*>(nullptr)), size, false, false) {
        if (allocate_next) (void)next(false);
    }
    ChainBuffet(const ChainBuffet&) = delete;
    ChainBuffet& operator=(const ChainBuffet&) = delete;
    ChainBuffet(ChainBuffet&&) = delete;
    ChainBuffet& operator=(ChainBuffet&&) = delete;
    static Slice chain(const BuffetDescriptor* desc, size_t size, bool novel_buffer = false);
    Slice claim(size_t size, bool novel_buffer = false);
};
static_assert(sizeof(ChainBuffet) == 64, "ChainBuffet must be 64 bytes");
/** --------------------------------------------------------------------------------------------------------- Pool Sizes
 * @brief Each `Slice` has a 32-bit identifier that represents a type and an index within a global buffer
 * pool.
 */
inline static constexpr size_t SLICE_SLOT_BITS = 19;
inline static constexpr size_t SLICE_REGION_BITS = 6;
inline static constexpr size_t POOL_BITS = SLICE_SLOT_BITS + SLICE_REGION_BITS;
inline static constexpr size_t POOL_SIZE = 1 << POOL_BITS;
inline static constexpr size_t REGION_SIZE = POOL_SIZE >> 6;
inline static constexpr size_t POOL_MASK = POOL_SIZE - 1;
inline static constexpr size_t pool_index(size_t id) { return id >> 9; }
inline static constexpr size_t POOL_TYPE_BITS = 3;
inline static constexpr size_t POOL_TYPE_SIZE = 1 << POOL_TYPE_BITS;
inline static constexpr size_t POOL_TYPE_MASK = POOL_TYPE_SIZE - 1;
inline static constexpr size_t pool_type(size_t id) { return id & POOL_TYPE_MASK; }
/** --------------------------------------------------------------------------------------------------------- GPUBuf
 * @struct GPUBuf
 * @brief Stores a slab base address with length and slab-relative offset in 64-byte granules.
 */
struct alignas(16) GPUBuf {
    uint64_t address = 0;
    uint32_t size = 0;
    uint32_t offset = 0;
    void set(uint64_t addr, uint32_t sz, uint32_t off) {
        address = addr; size = sz; offset = off;
    }
    void clear() { address = 0xFFFFFFFFFFFFFFFFull; size = 0; offset = 0; }
};
static_assert(sizeof(GPUBuf) == 16 && offsetof(GPUBuf, size) == 8
    && offsetof(GPUBuf, offset) == 12, "GPUBuf must match the GLSL GPUBufRef record.");
/** --------------------------------------------------------------------------------------------------------- Slice Entry
 * @struct SliceEntry
 * @brief Represents an entry in the slice table, containing a token, size, and offset.
 */
struct SliceEntry {
    alignas(ChainBuffet::ChainBuffetToken) std::byte data[sizeof(ChainBuffet::ChainBuffetToken)];
    std::atomic<uint32_t> owners{0};
    uint8_t region_id_ = 0;
    ChainBuffet::ChainBuffetToken* token();
    const ChainBuffet::ChainBuffetToken* token() const;
    void set_token(const ChainBuffet::ChainBuffetToken* token);
    uint8_t region() const;
    void set_region(uint8_t region);
    void set(const ChainBuffet::ChainBuffetToken* token, uint8_t region);
    void clear();
    size_t size() const;
    size_t offset() const;
    void* host_ptr();
    const void* host_ptr() const;
    GPUBuf* gpu_buf();
    const GPUBuf* gpu_buf() const;
    static SliceEntry* from_slice(const Slice& slice);
};
/** --------------------------------------------------------------------------------------------------------- Region
 * @struct Region
 * @brief Represents a region of slice entries.
 */
struct Region {
    inline static constexpr size_t NUM_ATOMICS = (POOL_SIZE >> 12);
    std::array<GPUBuf, (POOL_SIZE >> 6)> gpu_slots;
    std::array<SliceEntry, (POOL_SIZE >> 6)> slots;
    std::array<std::atomic<uint64_t>, NUM_ATOMICS> occupancy{};
    std::atomic<uint64_t> last_idx{0};
    std::atomic<uint64_t> claimed{0};
    std::atomic<uint64_t> freed{0};
    uint8_t region = 0;
    size_t slots_available() const;
    SliceEntry* claim(size_t skip_at_most);
    bool release(SliceEntry* entry);
    GPUBuf* gpu_table() { return gpu_slots.data(); }
    const GPUBuf* gpu_table() const { return gpu_slots.data(); }
    SliceEntry* slice_table() { return slots.data(); }
    const SliceEntry* slice_table() const { return slots.data(); }
};
/** --------------------------------------------------------------------------------------------------------- Slice Entry Size and Pool Overhead
 * @brief Defines the size of a SliceEntry and the total overhead for the pool.
 */
inline static constexpr size_t SLICE_ENTRY_SIZE = sizeof(SliceEntry);
inline static constexpr size_t ALLIGATOR_POOL_OVERHEAD = sizeof(Region) << SLICE_REGION_BITS;
/** --------------------------------------------------------------------------------------------------------- Host Memory Usage
 * @struct HostMemoryUsage
 * @brief Reports physical capacity, estimated available memory, and this process's resident bytes.
 */
struct HostMemoryUsage {
    uint64_t physical_bytes;
    uint64_t available_bytes;
    uint64_t resident_bytes;
};
/** --------------------------------------------------------------------------------------------------------- Comprehension Enforcer
 * @brief Gates non-hot-path methods behind proof the caller read the code. A gated method calls
 * CHECK_read_this(name), which passes only while that gate is armed on the calling thread. Arming is
 * set_read_this(name, value) with a value equal to the arithmetic in the gate's SETUP_read_this line;
 * the comparison is numeric, so any expression evaluating to the gate's number works, but no plaintext
 * literal exists anywhere to copy — grep the SETUP line and read it to learn the number. A wrong value
 * leaves the gate disarmed. unset_read_this(name) closes the armed scope. Gates are per-thread and
 * per-name and stay armed until unset, so an arm must never outlive the scope it was opened for.
 * @param name The gate name shared by the SETUP, set, unset and CHECK call sites.
 * @param val The secret arithmetic for the gate; never write it as a plain literal.
 * @param err_msg The message thrown at callers that skip the reading.
 */
#define SETUP_read_this(name, val, err_msg) struct name##_struct { \
inline static constexpr const char* error_message = err_msg; \
static bool& armed() { static thread_local bool armed_flag = false; return armed_flag; } \
static void set(const int v) { armed() = (v == (val)); } \
static void unset() { armed() = false; } \
static void enforce() { if (!armed()) { ALLIGATOR_THROW(error_message); } } };
/** --------------------------------------------------------------------------------------------------------- Check Requirement
 * @brief Throws unless the named gate is armed on this thread.
 * @param name The name of the gate to check.
 */
#define CHECK_read_this(name) name##_struct::enforce()
/** --------------------------------------------------------------------------------------------------------- Set Requirement
 * @brief Arms the named gate on this thread; a wrong value leaves the gate disarmed.
 * @param name The name of the gate to arm.
 * @param value Arithmetic that must evaluate to the gate's SETUP_read_this value.
 */
#define set_read_this(name, value) name##_struct::set(value)
/** --------------------------------------------------------------------------------------------------------- Unset Requirement
 * @brief Disarms the named gate, closing the armed scope.
 * @param name The name of the gate to unset.
 */
#define unset_read_this(name) name##_struct::unset()
/** --------------------------------------------------------------------------------------------------------- SliceType Concept
 * @brief Concept to check if a type conforms to the Slice interface. The full concept which includes the
 * conversion operator to other SliceType's is declared at the bottom of this file.
 */
template<typename T>
concept PrimitiveSliceType = (
    requires(T t) {
        { t.valid() } -> std::convertible_to<bool>;
        { t.slice(std::declval<size_t>(), std::declval<size_t>()) } -> std::convertible_to<Slice>;
        { t.slice() } -> std::convertible_to<Slice>;
        { t.raw() } -> std::convertible_to<void*>;
        { T(std::declval<size_t>()) } -> std::convertible_to<Slice>;
        { t.template data<uint8_t>() } -> std::convertible_to<uint8_t*>;
        { t.template data<const uint8_t>() } -> std::convertible_to<const uint8_t*>;
        { t.template get_as<uint8_t>() } -> std::convertible_to<const uint8_t&>;
        { t.template get_as<const uint8_t>() } -> std::convertible_to<const uint8_t&>;
        { t.template size<uint8_t>() } -> std::convertible_to<size_t>;
        { t.template size<const uint8_t>() } -> std::convertible_to<size_t>;
        { t.size_bytes() } -> std::convertible_to<size_t>;
        { t.is_null() } -> std::convertible_to<bool>;
        { t.root_slice() } -> std::convertible_to<Slice>;
        { std::as_const(t).root_slice() } -> std::convertible_to<Slice>;
    } && (
        requires(T t) {
            { t.placement() } -> std::same_as<const BuffetDescriptor*>;
        }
    ||
        requires(T t) {
            { t.placement() } -> std::same_as<nullptr_t>;
        }
    )
) || std::is_same_v<T, Slice>;
/** --------------------------------------------------------------------------------------------------------- Slice
 * @class Slice
 * @brief Represents a slice of memory in the buffet alligator.
 * 
 * All operations in the buffet alligator are performed on slices of memory, represented by the `Slice`
 * class.
 * 
 * Memory slices are claims from pre-allocated memory buffers that are managed by the buffet alligator's
 * slab arena. On systems that support unified memory, they are always sliced from host-coherent GPU
 * buffers when selected by the placement policy, and GPU work requires a compatible visible placement.
 * 
 * Slices can be sub-sliced to create smaller slices, and they all share the same reference counter and
 * underlying memory. Slices can behave much like `std::shared_ptr` by calling the `slice()` method with
 * no parameters passed, while copies publish distinct identifiers sharing the same backing allocation.
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
    /** ------------------------------------------------------------------------------------------- Default Placement
     * @brief Returns the default placement for slices, which is determined by the system's
     * capabilities.
     * @return The default `Placement` enum value for slices.
     */
    static const BuffetDescriptor* default_placement();
    /** ------------------------------------------------------------------------------------------- Constructor - Fresh Claim
     * @brief Claims a slice of pre-allocated memory in the buffet alligator's slab arena. The
     * slice is guaranteed to be zero-initialized.
     * @param size The size of the slice in bytes.
     */
    Slice() noexcept = default;
    explicit Slice(size_t size, const BuffetDescriptor* placement = default_placement());
    /** ------------------------------------------------------------------------------------------- Constructor - Fresh Claim
     * @brief Constructs a slice of memory with the specified size. If not a novel buffer, then
     * it will be claimed from a pre-allocated slab in the buffet alligator's arena.
     * @param size The size of the slice in bytes.
     * @param novel_buffer If true, then the slice is allocated as a novel buffer instead of being
     * a claim of a pre-allocated slab. This is ideal for slices that are long-lived to help
     * reduce fragmentation in the arena.
     */
    Slice(size_t size, bool novel_buffer, const BuffetDescriptor* placement = default_placement());
    /** ------------------------------------------------------------------------------------------- Constructor - Copy from External Memory
     * @brief Copies data from an external memory location into a new slice of memory in the
     * buffet alligator, ensuring the data is managed within the buffet alligator's memory system
     * and aligned properly for SIMD operations.
     * @param copy_from Pointer to the external memory to copy from.
     * @param size The size of the data to copy in bytes.
     * @param novel_buffer If true, then the slice is allocated as a novel buffer instead of being
     * a claim of a pre-allocated slab. This is ideal for slices that are long-lived to help
     * reduce fragmentation in the arena. Default is false.
     */
    Slice(
        const void* copy_from,
        size_t size,
        bool novel_buffer = false,
        const BuffetDescriptor* placement = default_placement()
    );
    /** ------------------------------------------------------------------------------------------- Copy/move semantics
     * @brief Copying a `Slice` does not actually copy the underlying memory, `Slice` objects act
     * much like `std::shared_ptr` in that they share the same reference counter and underlying
     * memory, while moves transfer the owned identifier without reference-counting traffic.
     */
    Slice(const Slice& other);
    Slice& operator=(const Slice& other);
    Slice(Slice&& other) noexcept;
    Slice& operator=(Slice&& other) noexcept;
    /** ------------------------------------------------------------------------------------------- Destructor */
    ~Slice() { free(); }
    /** ------------------------------------------------------------------------------------------- Placement
     * @brief Returns the memory placement type of the slice.
     * @return The `Placement` enum value representing the slice's memory placement.
     */
    const BuffetDescriptor* placement() const;
    /** ------------------------------------------------------------------------------------------- Raw accessors
     * @brief Use the buffet alligator's internal memory arena system to resolve the slice's
     * host-writable pointer to the underlying memory.
     */
    void* raw();
    /** ------------------------------------------------------------------------------------------- Raw accessors - const
     * @brief Use the buffet alligator's internal memory arena system to resolve the slice's
     * host-writable pointer to the underlying memory, but as a read-only pointer.
     */
    const void* raw() const;
    /** ------------------------------------------------------------------------------------------- Accessor - Typed
     * @brief Returns a pointer to the underlying data of the slice, cast to the specified type.
     * @tparam T The type to cast the underlying data to. Default is uint8_t.
     * @return A pointer to the underlying data cast to type T.
     */
    template<typename T = uint8_t>
    T* data() { return static_cast<T*>(raw()); }
    /** ------------------------------------------------------------------------------------------- Accessor - Typed - const
     * @brief Returns a const pointer to the underlying data of the slice, cast to the specified
     * type.
     * @tparam T The type to cast the underlying data to. Default is uint8_t.
     * @return A const pointer to the underlying data cast to type T.
     */
    template<typename T = uint8_t>
    const T* data() const { return static_cast<const T*>(raw()); }
    /** ------------------------------------------------------------------------------------------- Create new view
     * @brief Creates a new view of the slice, which is a sub-slice of the original slice. The new
     * view shares the same underlying memory and reference counter as the original slice. Using
     * the default parameters will create a new view that is essentially identical to the original
     * slice - a shared view that increments the reference counter and will keep the underlying
     * memory alive until all views are destroyed.
     * @param offset Requested byte offset, rounded down to a 64-byte boundary.
     * @param length Requested byte length, with the range end rounded up to a 64-byte boundary.
     * @return A new `Slice` object that is a view of the original slice.
     */
    Slice slice(size_t offset = 0, size_t length = SIZE_MAX) const;
    /** ------------------------------------------------------------------------------------------- Size in bytes
     * @brief Returns the represented 64-byte-granule length in bytes.
     * @return The size of the slice in bytes.
     */
    size_t size_bytes() const;
    /** ------------------------------------------------------------------------------------------- Size in elements
     * @brief Returns the size of the slice in elements of type T.
     * @tparam T The type of elements in the slice. Default is `uint8_t`.
     * @return The size of the slice in elements of type T.
     */
    template<typename T = uint8_t>
    size_t size() const {
        if constexpr (sizeof(T) == 8) {
            return size_bytes() >> 3;
        } else if constexpr (sizeof(T) == 4) {
            return size_bytes() >> 2;
        } else if constexpr (sizeof(T) == 2) {
            return size_bytes() >> 1;
        }
        return size_bytes() / sizeof(T);
    }
    /** ------------------------------------------------------------------------------------------- Resize
     * @brief Resizes the slice to a new size. If `preserve_data` is true, the existing data in
     * the slice will be preserved up to the minimum of the old and new sizes. If `preserve_data`
     * is false, the existing data will be discarded and the slice will be reallocated. This can
     * be called on a freed or null slice, in which case it will behave like a normal constructor
     * and allocate a new slice of the specified size.
     * @param new_size The new size of the slice in bytes.
     * @param preserve_data Whether to preserve existing data in the slice. Default is true.
     * @param novel_buffer Whether to allocate a novel buffer even if the slice is not null.
     * Default is false.
     * @param placement The memory placement strategy to use. Default is `default_placement()`.
     */
    void resize(
        size_t new_size,
        bool preserve_data = true,
        bool novel_buffer = false,
        const BuffetDescriptor* placement = default_placement()
    );
    /** ------------------------------------------------------------------------------------------- Check if slice is null
     * @brief Checks if the slice is null (i.e., has no underlying memory).
     * @return True if the slice is null, false otherwise.
     */
    bool is_null() const;
    /** ------------------------------------------------------------------------------------------- Check if slice is valid
     * @brief Checks if the slice is valid (i.e., has underlying memory).
     * @return True if the slice is valid, false otherwise.
     */
    bool valid() const;
    /** ------------------------------------------------------------------------------------------- Conversion to bool
     * @brief Allows the slice to be used in boolean contexts.
     * @return True if the slice is valid, false if it is null.
     */
    operator bool() const;
    /** ------------------------------------------------------------------------------------------- Free
     * @brief Frees the underlying memory of the slice. This is called automatically when the
     * slice is destroyed, but can be called manually to free the memory early. After calling this
     * method, the slice will be null.
     */
    void free();
    /** ------------------------------------------------------------------------------------------- Get as
     * @brief Returns a reference to the underlying data of the slice, cast to the specified type.
     * @tparam T The type to cast the underlying data to. Default is uint8_t.
     * @return A reference to the underlying data cast to type T.
     */
    template<typename T = uint8_t>
    T& get_as() { return *reinterpret_cast<T*>(data()); }
    /** ------------------------------------------------------------------------------------------- Get as (const)
     * @brief Returns a const reference to the underlying data of the slice, cast to the specified
     * type.
     * @tparam T The type to cast the underlying data to. Default is uint8_t.
     * @return A const reference to the underlying data cast to type T.
     */
    template<typename T = uint8_t>
    const T& get_as() const { return *reinterpret_cast<const T*>(data()); }
    /** ------------------------------------------------------------------------------------------- Novel Backing
     * @brief Reports whether this slice owns or views a dedicated novel buffer.
     * @return True when the backing allocation is a novel buffer.
     */
    bool is_novel() const noexcept;
    /** ------------------------------------------------------------------------------------------- Root slice
     * @brief Returns a reference to the root slice. This is useful when dealing with nested
     * slices or `SliceType` conceptual objects.
     * @return A reference to the root slice.
     */
    Slice& root_slice() { return *this; }
    /** ------------------------------------------------------------------------------------------- Root slice (const)
     * @brief Returns a const reference to the root slice. This is useful when dealing with nested
     * slices or `SliceType` conceptual objects.
     * @return A const reference to the root slice.
     */
    const Slice& root_slice() const { return *this; }
    /** ------------------------------------------------------------------------------------------- Adopt
     * @brief Adopts the given slice, becoming another view of the same underlying data.
     * @param other The slice to adopt.
     */
    void adopt(Slice other);
    /** ------------------------------------------------------------------------------------------- Pool Index
     * @brief Returns the complete encoded Slice identifier, with all bits set when null.
     * @return The Slice identifier.
     */
    uint32_t pool_index() const { return id_; }
    /** ------------------------------------------------------------------------------------------- Conversion to uint32_t
     * @brief Implicitly converts the slice to its pool index.
     * @return The pool index of the slice.
     */
    operator uint32_t() const { return pool_index(); }
    /** ------------------------------------------------------------------------------------------- Arrow Operator
     * @brief Provides access to the underlying data as a pointer of type T.
     * @tparam T The type to cast the raw pointer to.
     * @return A pointer to the underlying data cast to type T.
     */
    template<typename T>
    T* operator->() { return static_cast<T*>(raw()); }
    /** ------------------------------------------------------------------------------------------- Arrow Operator (const)
     * @brief Provides access to the underlying data as a pointer of type T for const slices.
     * @tparam T The type to cast the raw pointer to.
     * @return A const pointer to the underlying data cast to type T.
     */
    template<typename T>
    const T* operator->() const { return static_cast<const T*>(raw()); }
    /** ------------------------------------------------------------------------------------------- Dereference Operator
     * @brief Provides access to the underlying data as a reference of type T.
     * @tparam T The type to cast the raw pointer to.
     * @return A reference to the underlying data cast to type T.
     */
    template<typename T>
    T& operator*() { return *static_cast<T*>(raw()); }
    /** ------------------------------------------------------------------------------------------- Dereference Operator (const)
     * @brief Provides access to the underlying data as a reference of type T for const slices.
     * @tparam T The type to cast the raw pointer to.
     * @return A const reference to the underlying data cast to type T.
     */
    template<typename T>
    const T& operator*() const { return *static_cast<const T*>(raw()); }
    /** ------------------------------------------------------------------------------------------- ID
     * @brief Returns the ID of the slice.
     * @return The ID of the slice.
     */
    uint32_t id() const { return id_; }
private:
    uint32_t id_ = UINT32_MAX;
    struct AdoptId {};
    explicit Slice(AdoptId, uint32_t id) : id_(id) {}
    friend class Alligator;
    friend struct SliceEntry;
    friend class SliceQueue;
    friend struct SliceHandle;
    friend struct SliceNetworkAccess;
    friend class ChainBuffet;
    friend class ShaderState;
    friend struct ShaderOperation;
};
static_assert(sizeof(Slice) == 4, "Slice must be 4 bytes in size.");
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
    Slice slice_;  ///< The underlying Slice object that this SliceT wraps.
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs a SliceT with a specified size in bytes. The size must be a multiple of
     * the size of type T.
     * @param size The size of the slice in bytes.
     */
    SliceT(bool initialize = false);
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs a SliceT with a specified number of elements of type T.
     * @param count The number of elements of type T.
     */
    SliceT(size_t count);
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs a SliceT from a Slice. The Slice must have a size that is a multiple of
     * the size of type T.
     * @param slice The Slice to wrap.
     */
    explicit SliceT(Slice slice);
    /** ------------------------------------------------------------------------------------------- Copy/Move semantics */
    SliceT(const SliceT& other);
    SliceT(SliceT&& other) noexcept;
    SliceT& operator=(const SliceT& other);
    SliceT& operator=(SliceT&& other) noexcept;
    /** ------------------------------------------------------------------------------------------- Destructor */
    ~SliceT() { free(); }
    /** ------------------------------------------------------------------------------------------- BuffetDescriptor
     * @brief Returns the BuffetDescriptor of the slice.
     * @return The `BuffetDescriptor` pointer representing the slice's memory placement.
     */
    const BuffetDescriptor* placement() const;
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
    /** ------------------------------------------------------------------------------------------- Null check
     * @brief Checks if the slice is null.
     * @return True if the slice is null, false otherwise.
     */
    bool is_null() const { return slice_.is_null(); }
    /** ------------------------------------------------------------------------------------------- Boolean conversion
     * @brief Converts the slice to a boolean value indicating its validity.
     * @return True if the slice is valid, false otherwise.
     */
    operator bool() const { return slice_.valid(); }
    /** ------------------------------------------------------------------------------------------- Free
     * @brief Frees the resources associated with the slice.
     */
    void free() { slice_.free(); }
    /** ------------------------------------------------------------------------------------------- Validity check
     * @brief Checks if the slice is valid (i.e., not null).
     * @return True if the slice is valid, false otherwise.
     */
    bool valid() const { return !slice_.is_null(); }
    /** ------------------------------------------------------------------------------------------- Underlying slice
     * @brief Returns the underlying untyped Slice.
     * @return The underlying Slice represented by this typed slice.
     */
    Slice slice(size_t offset = 0, size_t length = SIZE_MAX) const {
        return slice_.slice(offset, length);
    }
    /** ------------------------------------------------------------------------------------------- Root slice
     * @brief Returns a reference to the underlying root Slice.
     * @return A reference to the underlying Slice.
     */
    Slice& root_slice() { return slice_.root_slice(); }
    /** ------------------------------------------------------------------------------------------- Root slice (const)
     * @brief Returns a const reference to the underlying root Slice.
     * @return A const reference to the underlying Slice.
     */
    const Slice& root_slice() const { return slice_.root_slice(); }
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
    /** ------------------------------------------------------------------------------------------- Conversion to uint32_t
     * @brief Implicitly converts the SliceT to its pool index.
     * @return The pool index of the slice.
     */
    uint32_t pool_index() const {
        return slice_.pool_index();
    }
    /** ------------------------------------------------------------------------------------------- Conversion to uint32_t
     * @brief Implicitly converts the SliceT to its pool index.
     * @return The pool index of the slice.
     */
    operator uint32_t() const {
        return pool_index();
    }
};
static_assert(sizeof(SliceT<uint8_t>) == sizeof(Slice), "SliceT must be the same size as Slice.");
/** --------------------------------------------------------------------------------------------------------- SliceT Definitions
 * @brief Out-of-class definitions for SliceT's declared members.
 */
template <typename T>
SliceT<T>::SliceT(bool initialize) {
    if (initialize) {
        slice_ = Slice(sizeof(T));
    }
}
/** --------------------------------------------------------------------------------------------------------- Constructor - From Count
 * @brief Constructs a SliceT with a specified number of elements of type T.
 * @param count The number of elements of type T.
 */
template <typename T>
SliceT<T>::SliceT(size_t count) {
    if (count > SIZE_MAX / sizeof(T)) ALLIGATOR_THROW("SliceT element count exceeds addressable memory");
    slice_ = Slice(count * sizeof(T));
}
/** --------------------------------------------------------------------------------------------------------- Constructor - From Slice
 * @brief Constructs a SliceT from an existing Slice.
 * @param slice The existing Slice to construct from.
 */
template <typename T>
SliceT<T>::SliceT(Slice slice) : slice_(std::move(slice)) {
    if (slice_.size_bytes() % sizeof(T) != 0) {
        ALLIGATOR_THROW("SliceT: byte size is not a multiple of sizeof(T)");
    }
}
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
/** --------------------------------------------------------------------------------------------------------- SliceType Concept
 * @brief Concept to check if a type conforms to the `Slice` conceptual interface, including the conversion
 * operator to other SliceType's.
 */
template<typename T>
concept SliceType = requires(T t) {
    { t.slice(std::declval<size_t>(), std::declval<size_t>()) } -> std::same_as<Slice>;
    { static_cast<Slice>(t) } -> std::same_as<Slice>;
} || PrimitiveSliceType<T> || std::is_same_v<T, Slice>;
static_assert(SliceType<Slice>, "Slice must satisfy the SliceType concept.");
static_assert(SliceType<SliceT<uint8_t>>, "SliceT must satisfy the SliceType concept.");

class ConcurrentBitplane;
/** --------------------------------------------------------------------------------------------------------- Arena
 * @class Alligator
 * @brief Owns the Slice id regions: the host entry tables and the GPUBuf tables shaders resolve ids through.
 */
class Alligator {
private:
    std::atomic<uint64_t> next_slice_{};
    uint32_t next_id(const BuffetDescriptor* placement);
    mutable std::array<std::atomic<Region*>, 64> regions{};
    struct SliceRegion {
        uint64_t device_address;
        void* handle;
        Region* host_ptr;
        const BuffetDescriptor* descriptor;
        SliceRegion(const BuffetDescriptor* desc, uint8_t index);
        SliceRegion(const SliceRegion&) = delete;
        SliceRegion& operator=(const SliceRegion&) = delete;
        SliceRegion(SliceRegion&&) = delete;
        SliceRegion& operator=(SliceRegion&&) = delete;
        ~SliceRegion();
    };
    mutable std::array<std::unique_ptr<SliceRegion>, 64> region_backing;
    const BuffetDescriptor* metadata_placement_ = nullptr;
    void* directory_backing_ = nullptr;
    uint64_t* directory_ = nullptr;
    uint64_t directory_address_ = 0;
    std::atomic<uint8_t> last_region{0};
    std::atomic<size_t> skip_at_most{4};
    SliceEntry& entry(const Slice& slice);
    const SliceEntry& entry(const Slice& slice) const;
    GPUBuf* gpubuf(const Slice& slice);
    const GPUBuf* gpubuf(const Slice& slice) const;
    void destroy(Slice& slice);
    Alligator();
    ~Alligator();
    friend class Slice; friend class Memory;
    friend class VulkanKernel; friend struct SliceEntry;
    friend struct AlligatorInitializer;
    friend class ChainBuffet; friend class Kitchen;
    friend class ChainBuffetToken;
    friend class ShaderState;
    friend struct ShaderOperation;
public:
    Alligator(const Alligator&) = delete;
    Alligator& operator=(const Alligator&) = delete;
    Alligator(Alligator&&) = delete;
    Alligator& operator=(Alligator&&) = delete;
    /** ------------------------------------------------------------------------------------------- Instance
     * @brief Returns the singleton instance of the Alligator.
     * @return The Alligator instance.
     */
    static Alligator& inst();
    /** ------------------------------------------------------------------------------------------- GPUBuf For
     * @brief The writable GPUBuf half for a live slice, nullptr when the slot is unoccupied.
     * @param slice The slice to resolve.
     * @return The slice's GPUBuf entry.
     */
    static GPUBuf* gpubuf_for(const Slice& slice);
    /** ------------------------------------------------------------------------------------------- GPU Directory
     * @brief Returns the stable device address of the region directory, or zero for CPU-only use.
     */
    static uint64_t gpu_directory_address();
    /** ------------------------------------------------------------------------------------------- GPU Table
     * @brief The shared GPUBuf table's host mapping, the same bytes shaders read at gpu_table_address().
     * @param region_id The region ID of the GPU table.
     * @return The table base.
     */
    static const GPUBuf* gpu_table(uint8_t region_id);
};
} // namespace buffetalligator
