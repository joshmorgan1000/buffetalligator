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
#include <source_location>
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
/** --------------------------------------------------------------------------------------------------------- Alligator Initializer
 * @brief Keeps lazy runtime owners alive through static Slice and Shader destruction.
 */
struct AlligatorInitializer {
    AlligatorInitializer();
    ~AlligatorInitializer();
};
static AlligatorInitializer alligator_initializer;
/** --------------------------------------------------------------------------------------------------------- AlligatorException
 * @class AlligatorException
 * @brief Exception class for Alligator-related errors.
 */
EXCEPTION_CLASS(Alligator)
#define ALLIGATOR_THROW(msg) throw AlligatorException(msg)
/** ------------------------------------------------------------------------------------- Forward Declarations */
class Alligator; class Slice; class Memory;
class SliceQueue; class SliceChannel;
class PrioritySlice;
class HeapSlice; struct SliceHandle;
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
    /** --------------------------------------------------------------------------------------------------------- set_allocation_tracking
     * @brief Turns allocation counters and location records on or off before the arena starts.
     * @param enabled Whether allocations and releases reach the memory tracker.
     */
    static void set_allocation_tracking(bool enabled);
    /** --------------------------------------------------------------------------------------------------------- allocation_tracking
     * @brief Reports whether allocations and releases reach the memory tracker.
     * @return True while tracking is on, which is the default.
     */
    static bool allocation_tracking();
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
    explicit SharedBuffet(
        size_t size,
        std::source_location location = std::source_location::current()
    );
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
/** --------------------------------------------------------------------------------------------------------- Slice Type Record
 * @struct SliceTypeRecord
 * @brief Lifecycle hooks a typed slot runs over its elements.
 */
struct SliceTypeRecord {
    void (*construct)(void* first, size_t count);  ///< Null when default construction is trivial.
    void (*destroy)(void* first, size_t count);  ///< Null when destruction is trivial.
    uint32_t size;  ///< Element size in bytes.
};
/** --------------------------------------------------------------------------------------------------------- Slice Types
 * @class SliceTypes
 * @brief Registers up to 255 element types under the type byte each slot carries.
 */
class SliceTypes {
private:
    static const SliceTypeRecord* records_[256];  ///< Index zero stays null for raw bytes.
    static std::atomic<uint32_t> count_;  ///< Highest assigned type index.
    friend class Slice;
    friend class ChainBuffet;
public:
    /** ------------------------------------------------------------------------------------------- Register Type
     * @brief Assigns the next type index to a record and throws past 255 types.
     * @param record The element type's lifecycle hooks, which must outlive the process.
     * @return The assigned type index.
     */
    static uint8_t register_type(const SliceTypeRecord* record);
};
/** --------------------------------------------------------------------------------------------------------- ChainBuffet
 * @class ChainBuffet
 * @brief Bump-claims Slices from the current buffer of a linked chain that always holds two
 * buffers ahead; a full buffer hands over to its already allocated successor without waiting.
 */
class alignas(64) ChainBuffet {
private:
    const BuffetDescriptor* descriptor_ = nullptr;  ///< Placement every buffer in this chain uses.
    uint64_t granules_ = 0;  ///< Buffer size in 64-byte granules.
    std::atomic<uint32_t> current_{UINT32_MAX};  ///< Buffer that claims bump.
    /** ------------------------------------------------------------------------------------------- Next
     * @brief Returns a buffer's successor, allocating it only when the link is empty, then makes
     * sure `ahead` more buffers follow it, so `next(buffer, 1)` keeps two buffers ahead.
     * @param buffer The buffer whose successor is wanted.
     * @param ahead Further successors to ensure past the returned one.
     * @return The successor's buffer index.
     */
    uint32_t next(uint32_t buffer, size_t ahead = 0);
    static void release_chains();
    friend class Alligator;
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Opens the chain's first buffer and the two buffers ahead of it.
     * @param descriptor The placement every buffer comes from.
     * @param size Buffer size in bytes, at most 4 GiB.
     */
    ChainBuffet(const BuffetDescriptor* descriptor, size_t size);
    /** ------------------------------------------------------------------------------------------- Constructor - Typed Buffet
     * @brief Opens a chain of T buffers.
     * @param size Buffer size in bytes, at most 4 GiB.
     */
    template<IsABuffetType T = AlignedHeapBuffer>
    explicit ChainBuffet(size_t size)
    : ChainBuffet(BuffetDescriptors::descriptor_for(static_cast<T*>(nullptr)), size) {}
    ChainBuffet(const ChainBuffet&) = delete;
    ChainBuffet& operator=(const ChainBuffet&) = delete;
    ChainBuffet(ChainBuffet&&) = delete;
    ChainBuffet& operator=(ChainBuffet&&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Drops the chain's holds; buffers with live Slices remain until those Slices release.
     */
    ~ChainBuffet();
    /** ------------------------------------------------------------------------------------------- Chain
     * @brief Claims from the process-wide chain of a placement, constructing typed elements.
     * @param placement The placement to claim from.
     * @param size Requested bytes.
     * @param novel_buffer Whether the Slice gets a dedicated buffer.
     * @param type Slot type index from SliceTypes, zero for raw bytes.
     * @param location The caller recorded by allocation location tracking.
     * @return The claimed Slice.
     */
    static Slice chain(
        const BuffetDescriptor* placement,
        size_t size,
        bool novel_buffer = false,
        uint8_t type = 0,
        std::source_location location = std::source_location::current()
    );
    /** ------------------------------------------------------------------------------------------- Claim
     * @brief Claims zeroed granules, or a dedicated buffer for novel or slab-sized requests.
     * @param size Requested bytes.
     * @param novel_buffer Whether the Slice gets a dedicated buffer.
     * @param location The caller recorded by allocation location tracking.
     * @return The claimed Slice.
     */
    Slice claim(
        size_t size,
        bool novel_buffer = false,
        std::source_location location = std::source_location::current()
    );
};
static_assert(sizeof(ChainBuffet) == 64, "ChainBuffet must be 64 bytes");
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
 * A Slice is a 4-byte slot id. Copies share the slot and its reference counter, much like
 * `std::shared_ptr`. A sub-slice of a different span gets its own slot that keeps the backing
 * buffer alive, so it can outlive the Slice it was cut from.
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
    explicit Slice(
        size_t size,
        const BuffetDescriptor* placement = default_placement(),
        std::source_location location = std::source_location::current()
    );
    /** ------------------------------------------------------------------------------------------- Constructor - Fresh Claim
     * @brief Constructs a slice of memory with the specified size. If not a novel buffer, then
     * it will be claimed from a pre-allocated slab in the buffet alligator's arena.
     * @param size The size of the slice in bytes.
     * @param novel_buffer If true, then the slice is allocated as a novel buffer instead of being
     * a claim of a pre-allocated slab. This is ideal for slices that are long-lived to help
     * reduce fragmentation in the arena.
     */
    Slice(
        size_t size,
        bool novel_buffer,
        const BuffetDescriptor* placement = default_placement(),
        std::source_location location = std::source_location::current()
    );
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
        const BuffetDescriptor* placement = default_placement(),
        std::source_location location = std::source_location::current()
    );
    /** ------------------------------------------------------------------------------------------- Copy/move semantics
     * @brief Copies share the slot id and add one to its 16-bit reference count; moves transfer
     * the id without touching the count.
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
     * @brief Returns a view of a byte range; the whole span is a copy of this Slice, and any other
     * span gets its own untyped slot that keeps the backing buffer alive.
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
     * @param location The caller recorded by allocation location tracking.
     */
    void resize(
        size_t new_size,
        bool preserve_data = true,
        bool novel_buffer = false,
        const BuffetDescriptor* placement = default_placement(),
        std::source_location location = std::source_location::current()
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
    /** ------------------------------------------------------------------------------------------- Discharge Unsafely
     * @brief Releases the underlying ID of the slice without marking the slots as unallocated,
     * which must be used carefully because it could lead to a memory leak if it is not re-adopted
     * by another slice object later (e.g. by using `Slice::from_id`).
     * @return The pool index of the discharged slice.
     */
    uint32_t discharge_unsafely();
    /** ------------------------------------------------------------------------------------------- From ID
     * @brief Creates a slice from the given previously discharged ID, that will safely release
     * the claim to memory when the `Slice` object is destroyed.
     * @param id The ID to create the slice from.
     * @return A slice corresponding to the given ID.
     */
    static Slice from_id(uint32_t id);
private:
    uint32_t id_ = UINT32_MAX;
    struct AdoptId {};
    explicit Slice(AdoptId, uint32_t id) : id_(id) {}
    friend class Alligator;
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
    requires (alignof(T) % 64 == 0)
class SliceT {
private:
    Slice slice_;  ///< The underlying Slice object that this SliceT wraps.
    /** ------------------------------------------------------------------------------------------- Construct Elements
     * @brief Value-constructs a freshly claimed element range before its slot is typed.
     * @param first The first element's host address.
     * @param count The number of elements to construct.
     */
    static void construct(void* first, size_t count) {
        if constexpr (std::is_default_constructible_v<T>) {
            std::uninitialized_value_construct_n(static_cast<T*>(first), count);
        }
    }
    /** ------------------------------------------------------------------------------------------- Destroy Elements
     * @brief Destroys a typed slot's elements when its last handle releases it.
     * @param first The first element's host address.
     * @param count The number of elements to destroy.
     */
    static void destroy(void* first, size_t count) {
        std::destroy_n(static_cast<T*>(first), count);
    }
    /** ------------------------------------------------------------------------------------------- Type Index
     * @brief Registers T's lifecycle record once, or returns zero without a default constructor.
     * @return The slot type index for typed claims.
     */
    static uint8_t type_index() {
        if constexpr (std::is_default_constructible_v<T>) {
            static constexpr SliceTypeRecord record{
                std::is_trivially_default_constructible_v<T> ? nullptr : &construct,
                std::is_trivially_destructible_v<T> ? nullptr : &destroy, sizeof(T)};
            static const uint8_t index = SliceTypes::register_type(&record);
            return index;
        }
        return 0;
    }
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs a SliceT with a specified size in bytes. The size must be a multiple of
     * the size of type T.
     * @param size The size of the slice in bytes.
     */
    SliceT(
        bool initialize = false,
        std::source_location location = std::source_location::current()
    );
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs a SliceT with a specified number of elements of type T.
     * @param count The number of elements of type T.
     */
    SliceT(size_t count, std::source_location location = std::source_location::current());
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
/** --------------------------------------------------------------------------------------------------------- SliceT Definitions
 * @brief Out-of-class definitions for SliceT's declared members.
 */
template <typename T>
    requires (alignof(T) % 64 == 0)
SliceT<T>::SliceT(bool initialize, std::source_location location) {
    if (!initialize) return;
    slice_ = ChainBuffet::chain(Slice::default_placement(), sizeof(T), false, type_index(),
        location);
}
/** --------------------------------------------------------------------------------------------------------- Constructor - From Count
 * @brief Constructs a SliceT with a specified number of elements of type T.
 * @param count The number of elements of type T.
 */
template <typename T>
    requires (alignof(T) % 64 == 0)
SliceT<T>::SliceT(size_t count, std::source_location location) {
    if (count > SIZE_MAX / sizeof(T)) ALLIGATOR_THROW("SliceT element count exceeds addressable memory");
    slice_ = ChainBuffet::chain(Slice::default_placement(), count * sizeof(T), false,
        type_index(), location);
}
/** --------------------------------------------------------------------------------------------------------- Constructor - From Slice
 * @brief Constructs a SliceT from an existing Slice.
 * @param slice The existing Slice to construct from.
 */
template <typename T>
    requires (alignof(T) % 64 == 0)
SliceT<T>::SliceT(Slice slice) : slice_(std::move(slice)) {
    if (slice_.size_bytes() < sizeof(T)) {
        ALLIGATOR_THROW("SliceT: slice size is smaller than sizeof(T)");
    }
}
/** --------------------------------------------------------------------------------------------------------- Copy Constructor
 * @brief Copy constructor for SliceT.
 * @param other The SliceT object to copy from.
 */
template <typename T>
    requires (alignof(T) % 64 == 0)
SliceT<T>::SliceT(const SliceT& other) : slice_(other.slice_) {}
/** --------------------------------------------------------------------------------------------------------- Operator= Copy
 * @brief Copy assignment operator for SliceT.
 * @param other The SliceT object to copy from.
 * @return Reference to the current SliceT object.
 */
template <typename T>
    requires (alignof(T) % 64 == 0)
SliceT<T>::SliceT(SliceT&& other) noexcept : slice_(std::move(other.slice_)) {}
/** --------------------------------------------------------------------------------------------------------- Operator= Copy
 * @brief Copy assignment operator for SliceT.
 * @param other The SliceT object to copy from.
 * @return Reference to the current SliceT object.
 */
template <typename T>
    requires (alignof(T) % 64 == 0)
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
    requires (alignof(T) % 64 == 0)
SliceT<T>& SliceT<T>::operator=(SliceT&& other) noexcept {
    if (this != &other) slice_ = std::move(other.slice_);
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- DummyAlignedStructure
 * @brief A dummy structure aligned to 64 bytes, used for testing alignment requirements.
 */
struct alignas(64) DummyAlignedStructure { uint8_t dummy[64]; };
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
static_assert(SliceType<SliceT<DummyAlignedStructure>>, "SliceT must satisfy the SliceType concept.");
static_assert(sizeof(SliceT<DummyAlignedStructure>) == sizeof(Slice), "SliceT must be the same size as Slice.");
class ConcurrentBitplane;
/** --------------------------------------------------------------------------------------------------------- Arena
 * @class Alligator
 * @brief Owns the arena's lifetime: slot segments, buffer records, and the directory shaders read.
 */
class Alligator {
private:
    Alligator();
    ~Alligator();
    friend struct AlligatorInitializer;
    friend struct RuntimeFinalizer;
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
    /** ------------------------------------------------------------------------------------------- Device Address
     * @brief Returns a live Slice's first device byte, or zero for a null or host-only Slice.
     * @param slice The Slice to resolve.
     * @return The device address.
     */
    static uint64_t device_address(const Slice& slice);
    /** ------------------------------------------------------------------------------------------- GPU Directory
     * @brief Returns the device address of the arena directory, or zero for CPU-only use.
     */
    static uint64_t gpu_directory_address();
};
} // namespace buffetalligator
