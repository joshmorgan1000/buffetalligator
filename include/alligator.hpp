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
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <alligator/nutritionist.hpp>
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
class Alligator; class Chomp; class Memory;
struct Region; class ChompQueue; class ChompChannel;
struct ChompEntry; class PriorityChomp; class HeapChomp;
struct ChompHandle; struct GPUBuf; class AlignedHeapBuffer;
class VulkanBuffer; struct BuffetDescriptor; struct ChompTypeRecord;
class IBuffet; class SharedBuffet;
/** --------------------------------------------------------------------------------------------------------- IsABuffetType
 * @brief Concept to check if a type T satisfies the requirements of a Buffet.
 */
template<typename T>
concept IsABuffetType = requires(T t) {
    { T::deleter() } -> std::same_as<void(*)(void*)>;
    { T::host_ptr(std::declval<void*>()) } -> std::same_as<void*>;
    { t.raw() } -> std::same_as<void*>;
    { T::size_of(std::declval<void*>()) } -> std::same_as<size_t>;
    { t.size() } -> std::same_as<size_t>;
    { T::factory(std::declval<size_t>()) } -> std::same_as<void*>;
    { T::type_idx() } -> std::same_as<size_t*>;
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
    using TypeNameMethod = const char* (*)();
    TypeNameMethod type_name_fn;
    using DeleterMethod = void(*)(void*);
    DeleterMethod untracked_deleter;
    DeleterMethod tracked_deleter;
    DeleterMethod active_deleter;
    using HostPtrMethod = void* (*)(void*);
    HostPtrMethod host_ptr;
    using SizeOfMethod = size_t (*)(void*);
    SizeOfMethod size_of;
    using FactoryMethod = void*(*)(size_t); 
    FactoryMethod untracked_factory;
    FactoryMethod tracked_factory;
    FactoryMethod active_factory;
    size_t* (*type_idx_getter)();
    using DefaultSizeMethod = size_t (*)();
    DefaultSizeMethod default_size_method;
    using DeviceAddressMethod = uint64_t (*)(void*);
    DeviceAddressMethod device_address;
    void* current_chain = nullptr;  ///< The placement's current chain link, advanced as links fill.
};
/** --------------------------------------------------------------------------------------------------------- IBuffet
 * @class IBuffet
 * @brief Interface for custom buffer definition, if you prefer to use virtual methods instead of
 * descriptors.
 */
class IBuffet {
public:
    IBuffet() = default;
    virtual ~IBuffet() { if (is_allocated()) deallocate(); }
    virtual const char* get_type_name() const { return "Undefined"; }
    virtual void allocate(size_t size) { (void)size; }
    virtual bool is_allocated() const { return false; }
    virtual void deallocate() { (void)0; }
    virtual void* host_ptr() { return nullptr; }
    virtual const void* host_ptr() const { return nullptr; }
    virtual size_t size() const { return 0; }
    virtual size_t default_size() const { return 0; }
    virtual uint64_t device_address() const { return 0; }
    virtual IBuffet* copy_of() const { return nullptr; }
    using IFactoryMethod = IBuffet* (*)();
    virtual IFactoryMethod ifactory() const { return nullptr; }
};
/** --------------------------------------------------------------------------------------------------------- IsAnIBuffetType
 * @brief Concept to check if a type is a valid IBuffet derived type.
 */
template<typename T>
concept IsAnIBuffetType = std::is_base_of_v<IBuffet, T> && !std::is_same_v<IBuffet, T> && !IsABuffetType<T>;
/** --------------------------------------------------------------------------------------------------------- IBuffetProxy
 * @class IBuffetProxy
 * @tparam T The IBuffet derived class.
 * @brief Wraps an IBuffet implementation so that it satisfies IsABuffetType like any other buffet.
 */
template<IsAnIBuffetType T>
class IBuffetProxy {
private:
    IBuffet* buffer_ = nullptr;
public:
    IBuffetProxy(size_t size = 0) { if (size == 0) return; buffer_ = new T(); buffer_->allocate(size); }
    IBuffetProxy(const IBuffetProxy&) = delete;
    IBuffetProxy& operator=(const IBuffetProxy&) = delete;
    IBuffetProxy(IBuffetProxy&& other) noexcept : buffer_(std::exchange(other.buffer_, nullptr)) {}
    IBuffetProxy& operator=(IBuffetProxy&& other) noexcept {
        std::swap(buffer_, other.buffer_); return *this;
    }
    ~IBuffetProxy() { delete buffer_; }
    operator bool() const { return buffer_ != nullptr; }
    bool operator==(std::nullptr_t) const { return buffer_ == nullptr; }
    bool operator!=(std::nullptr_t) const { return buffer_ != nullptr; }
    static void deleter_impl(void* ptr) { delete static_cast<IBuffetProxy*>(ptr); }
    static void (*deleter())(void*) { return deleter_impl; }
    static void* host_ptr(void* ptr) { return static_cast<IBuffetProxy*>(ptr)->buffer_->host_ptr(); }
    static size_t size_of(void* ptr) { return static_cast<IBuffetProxy*>(ptr)->buffer_->size(); }
    static void* factory(size_t size) { return new IBuffetProxy(size); }
    static size_t default_size() { return T().default_size(); }
    static size_t* type_idx() { static size_t idx = 0; return &idx; }
    static const char* type_name() { return T().get_type_name(); }
    static uint64_t device_address(void* ptr) {
        return static_cast<IBuffetProxy*>(ptr)->buffer_->device_address();
    }
    size_t size() const { return buffer_->size(); }
    void* raw() { return buffer_->host_ptr(); }
    const void* raw() const { return buffer_->host_ptr(); }
};
/** --------------------------------------------------------------------------------------------------------- BuffetDescriptors
 * @struct BuffetDescriptors
 * @brief Provides access to all buffet descriptors.
 */
class BuffetDescriptors {
private:
    using Self = BuffetDescriptors; std::vector<BuffetDescriptor*> list_;
    std::shared_mutex mutex_; std::unordered_map<const std::type_info*, size_t> map_;
    BuffetDescriptors(); static Self& inst() { static Self* s = new Self; return *s; }
    static void record_allocation(size_t type, size_t bytes, const void* buffet);
    static void record_deallocation(size_t type, size_t bytes, const void* buffet);
    template<IsABuffetType T> static void* tracked_factory(size_t size) { void* buffet = T::factory(size);
        record_allocation(*T::type_idx(), T::size_of(buffet), buffet); return buffet; }
    template<IsABuffetType T> static void tracked_deleter(void* buffet) {
        record_deallocation(*T::type_idx(), T::size_of(buffet), buffet); T::deleter()(buffet); }
    static std::atomic<const BuffetDescriptor*>& default_descriptor() {
        static std::atomic<const BuffetDescriptor*> descriptor{nullptr}; return descriptor;
    }
    /** --------------------------------------------------------------------------------------------------------- add
     * @brief Registers T on this registry under the write lock, unless another thread already did; the
     * constructor seeds the heap through here, since `inst()` is still being built at that moment.
     * @tparam T The buffet type.
     * @return A pointer to the descriptor for the type T.
     */
    template<IsABuffetType T>
    const BuffetDescriptor* add() {
        std::unique_lock write_lock(mutex_);
        auto it = map_.find(&typeid(T));
        if (it != map_.end()) return list_[it->second];
        const size_t index = list_.size(); map_[&typeid(T)] = index;
        list_.emplace_back(new BuffetDescriptor{
            .type_name_fn = &T::type_name, .untracked_deleter = T::deleter(),
            .tracked_deleter = &tracked_deleter<T>,
            .active_deleter = allocation_tracking() ? &tracked_deleter<T> : T::deleter(),
            .host_ptr = &T::host_ptr, .size_of = &T::size_of, .untracked_factory = &T::factory,
            .tracked_factory = &tracked_factory<T>,
            .active_factory = allocation_tracking() ? &tracked_factory<T> : &T::factory,
            .type_idx_getter = &T::type_idx, .default_size_method = &T::default_size,
            .device_address = &T::device_address,
        });
        *list_[index]->type_idx_getter() = index;
        if (index == 0) default_descriptor().store(list_[0], std::memory_order_release);
        return list_[index];
    }
public:
    BuffetDescriptors(const BuffetDescriptors&) = delete;
    BuffetDescriptors& operator=(const BuffetDescriptors&) = delete;
    BuffetDescriptors(BuffetDescriptors&&) = delete;
    BuffetDescriptors& operator=(BuffetDescriptors&&) = delete;
    ~BuffetDescriptors() = default;
    /** --------------------------------------------------------------------------------------------------------- descriptor
     * @brief Returns the BuffetDescriptor for a given buffet type T. If the type has not yet been registered,
     * it will register the allocator type and then return the descriptor.
     * @tparam T The buffet type.
     * @return A pointer to the static BuffetDescriptor for the type T.
     */
    template<IsABuffetType T>
    static const BuffetDescriptor* descriptor() {
        /// Resolved once per type; after that this is a guard check and a load, no lock and no map.
        static const BuffetDescriptor* const resolved = inst().add<T>();
        return resolved;
    }
    /** --------------------------------------------------------------------------------------------------------- get
     * @brief Returns the BuffetDescriptor for a given type index.
     * @param type_idx The type index of the buffet.
     * @return A pointer to the BuffetDescriptor, or nullptr if the type index is invalid.
     */
    static const BuffetDescriptor* get(size_t type_idx) {
        std::shared_lock lock(inst().mutex_);
        if (type_idx >= inst().list_.size()) [[unlikely]] return nullptr;
        return inst().list_[type_idx];
    }
    /** --------------------------------------------------------------------------------------------------------- index_for
     * @brief Returns the type index for a given buffet type. If the type has not been registered, it will be
     * registered automatically.
     * @tparam T The buffet type.
     * @return The type index of the buffet.
     */
    template<IsABuffetType T> static size_t index_for() { return *descriptor<T>()->type_idx_getter(); }
    /** --------------------------------------------------------------------------------------------------------- register_descriptor
     * @brief Registers the descriptor for a given buffet type and optionally sets it as the default.
     * @tparam T The buffet type.
     * @param make_default If true, sets this type as the default.
     * @return The type index of the buffet.
     */
    template<IsABuffetType T> static size_t register_descriptor(bool make_default = false) {
        const BuffetDescriptor* desc = descriptor<T>();
        if (make_default) default_descriptor().store(desc, std::memory_order_release);
        return *desc->type_idx_getter();
    }
    /** --------------------------------------------------------------------------------------------------------- default_placement
     * @brief Returns the BuffetDescriptor for the default buffet type: the heap until a GPU probe moves it.
     * @return A pointer to the BuffetDescriptor of the default buffet type.
     */
    static const BuffetDescriptor* default_placement() {
        return default_descriptor().load(std::memory_order_acquire);
    }
    /** --------------------------------------------------------------------------------------------------------- allocation_tracking
     * @brief Returns whether allocation tracking is enabled.
     * @return True if allocation tracking is enabled, false otherwise.
     */
    static bool& allocation_tracking() { static bool tracking = true; return tracking; }
    /** --------------------------------------------------------------------------------------------------------- set_allocation_tracking
     * @brief Sets whether allocation tracking is enabled.
     * @param tracking True to enable allocation tracking, false to disable.
     */
    static void set_allocation_tracking(bool tracking) {
        if (allocation_tracking() == tracking) return;
        std::unique_lock lock(inst().mutex_); allocation_tracking() = tracking;
        for (auto* desc : inst().list_) {
            desc->active_deleter = tracking ? desc->tracked_deleter : desc->untracked_deleter;
            desc->active_factory = tracking ? desc->tracked_factory : desc->untracked_factory;
        }
    }
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
    AlignedHeapBuffer(size_t size = 0) : size_(((size + 63) & ~63)) {
        if (size_ == 0) return; buffer_ = aligned_alloc(64, size_); std::memset(buffer_, 0, size_);
    }
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
    static void* host_ptr(void* ptr) { return static_cast<AlignedHeapBuffer*>(ptr)->buffer_; }
    static const char* type_name() { return "AlignedHeapBuffer"; }
    static size_t size_of_impl(void* ptr) { return static_cast<AlignedHeapBuffer*>(ptr)->size_; }
    static size_t size_of(void* ptr) { return size_of_impl(ptr); }
    static size_t (*size_of())(void*) { return size_of_impl; }
    static void* factory(size_t size) {
        return new AlignedHeapBuffer(size);
    }
    static size_t default_size() { return 64 * 1024 * 1024; }
    static size_t* type_idx() { static size_t idx = 0; return &idx; }
    static uint64_t device_address(void* ptr) {
        return reinterpret_cast<uint64_t>(reinterpret_cast<AlignedHeapBuffer*>(ptr)->raw());
    }
    size_t size() const { return size_; }
    void* raw() { return buffer_; }
    const void* raw() const { return buffer_; }
};
static_assert(IsABuffetType<AlignedHeapBuffer>, "AlignedHeapBuffer must satisfy IsABuffetType");
static_assert(sizeof(AlignedHeapBuffer) == 16, "AlignedHeapBuffer must be 16 bytes");
/** --------------------------------------------------------------------------------------------------------- MultiAtomic
 * @class MultiAtomic
 * @brief Uses `thread_local` atomics for efficient reference counting in high-contention scenarios.
 */
class MultiAtomic {
private:
    int* refs_;
    static std::atomic<size_t>& counter();
    static size_t thread_id();
public:
    MultiAtomic();
    MultiAtomic(const MultiAtomic&) = delete;
    int refcount() const;
    MultiAtomic& operator=(const MultiAtomic&) = delete;
    static size_t num_atomics(); 
    MultiAtomic(MultiAtomic&&) = delete;
    MultiAtomic& operator=(MultiAtomic&&) = delete;
    ~MultiAtomic() { delete[] refs_; }
    void increment_refcount();
    int decrement_refcount();  
};
/** --------------------------------------------------------------------------------------------------------- SharedBuffet
 * @class SharedBuffet
 * @brief Wraps a buffet with reference counting for shared ownership.
 */
class SharedBuffet {
public:
    /** ------------------------------------------------------------------------------------------- Use the static emplacement methods */
    SharedBuffet(const SharedBuffet&) = delete;
    SharedBuffet& operator=(const SharedBuffet&) = delete;
    SharedBuffet(SharedBuffet&&) = delete;
    SharedBuffet& operator=(SharedBuffet&&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor */
    ~SharedBuffet();
    /** ------------------------------------------------------------------------------------------- Raw Accessor
     * @brief Retrieves the raw pointer to the ChainBuffet's buffer.
     * @return The raw pointer to the buffer, or nullptr if it doesn't exist.
     */
    void* raw();
    /** ------------------------------------------------------------------------------------------- Raw Accessor
     * @brief Retrieves the raw pointer to the ChainBuffet's buffer.
     * @return The raw pointer to the buffer, or nullptr if it doesn't exist.
     */
    const void* raw() const;
    /** ------------------------------------------------------------------------------------------- Size Accessor
     * @brief Retrieves the size of the ChainBuffet's buffer, if it exists.
     * @return The size of the buffer.
     */
    size_t size() const;
    /** ------------------------------------------------------------------------------------------- Descriptor Accessor
     * @brief The placement the ChainBuffet's buffer came from, null when the link is gone.
     * @return The descriptor.
     */
    const BuffetDescriptor* descriptor() const;
    /** ------------------------------------------------------------------------------------------- Static Emplacement Method
     * @brief Emplaces a SharedBuffet into the given target memory location. This should only be
     * used for initial buffers or novel buffers.
     * @param target The memory location to emplace the SharedBuffet into.
     * @param descriptor The buffet descriptor.
     * @param size The size of the buffer to allocate.
     * @param novel_buffer Indicates whether this is a novel buffer.
     */
    static void emplace_into(
        void* target,
        const BuffetDescriptor* descriptor,
        size_t size,
        bool novel_buffer
    ) {
        new (target) SharedBuffet(descriptor, size, novel_buffer);
    }
    /** ------------------------------------------------------------------------------------------- Copy Into Method
     * @brief Copies the current SharedBuffet into the given target memory location.
     * @param target The memory location to copy the SharedBuffet into.
     */
    void copy_into(void* target) {
        if (data_) data_->refs.increment_refcount();
        else ALLIGATOR_THROW("SharedBuffet: Cannot copy an empty SharedBuffet");
        SharedBuffet* sb = new (target) SharedBuffet();
        sb->data_ = data_;
    }
private:
    /** ------------------------------------------------------------------------------------------- Default Constructor
     * @brief Default constructor for SharedBuffet. Initializes an empty instance. Should only
     * be called by the copy_into method above.
     */
    SharedBuffet() = default;
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs a SharedBuffet with the given descriptor and size. Private to enforce
     * the use of static emplacement methods. Should only be used for the initial buffers in the
     * chain, or for novel buffers.
     * @param descriptor The buffet descriptor.
     * @param size The size of the buffet.
     * @param novel_buffer Indicates whether this is a novel buffer.
     */
    SharedBuffet(const BuffetDescriptor* descriptor, size_t size, bool novel_buffer);
    /** ------------------------------------------------------------------------------------------- Data Structure
     * @struct Data
     * @brief Holds the actual buffet pointer and the reference counter. Many slots may all point
     * to the same Data*, which itself does not actually live in any slots. The slots merely hold
     * the pointer reference to a shared Data instance.
     */
    struct Data {
        /// @brief Pointer to the ChainBuffet.
        void* chainbuffet;
        /// @brief Reference counter for the shared Data instance.
        MultiAtomic refs;
    };
    /// @brief Pointer to the shared Data instance.
    Data* data_ = nullptr;
    /// @brief Classes that may access the data member.
    friend class Alligator;
    friend class Chomp;
    friend struct ChompEntry;
};
/** --------------------------------------------------------------------------------------------------------- Chomp Entry
 * @struct ChompEntry
 * @brief We pack a ChompEntry into 10 bytes. These store either a pointer to the SharedBuffet, or a pointer
 * to another ChompEntry. We do the latter so that we don't have to have every single Chomp competing over
 * a single atomic.
 * THe exact way we do this is we create "anchor Chomps"
 */
struct ChompEntry {
    SharedBuffet* buffet = nullptr;  ///< The link's handle every slot on that link shares; null when free.
    size_t size() const { return buffet ? buffet->size() : 0; }
};
static_assert(sizeof(ChompEntry) == 8, "ChompEntry is one word of the atomics table");
/** --------------------------------------------------------------------------------------------------------- Pool Sizes
 * @brief Each `Chomp` has a 32-bit identifier. The region index describes which region the metadata lives
 * in, the claim index describes which index within that region the chomp's metadata occupies.
 * The chomp type is not used to locate the metadata, but rather it serves as a lookup for lifecycle
 * behaviors.
 */
inline static constexpr size_t CHOMP_TYPE_BITS = 6u;
inline static constexpr size_t REGION_BITS = 10u;
inline static constexpr size_t NOVEL_REGION_BITS = 8u;
inline static constexpr size_t CLAIM_BITS = 16u;
static_assert(CLAIM_BITS + REGION_BITS + CHOMP_TYPE_BITS == 32u, "Pool, region, and type bits must sum to 32");
static_assert(REGION_BITS > 0u, "Region bits must be greater than 0");
static_assert(NOVEL_REGION_BITS < REGION_BITS, "Novel region bits must be less than region bits");
/** --------------------------------------------------------------------------------------------------------- Chomp Type Bits
 * @brief The chomp type is stored in the least-significant bits of the chomp identifier. How many of those
 * bits are used is defined by the `CHOMP_TYPE_BITS` constant.
 */
inline static constexpr size_t NUMBER_OF_CHOMP_TYPES = 1u << CHOMP_TYPE_BITS;
inline static constexpr size_t CHOMP_TYPES_MASK = NUMBER_OF_CHOMP_TYPES - 1u;
/** --------------------------------------------------------------------------------------------------------- Get Chomp Type Index
 * @brief Extracts the chomp type index from a chomp identifier.
 * @param chomp_id The 32-bit chomp identifier.
 * @return The index of the chomp type.
 */
inline static constexpr size_t get_chomp_type_index(const uint32_t& chomp_id) {
    return chomp_id & CHOMP_TYPES_MASK;
}
/** --------------------------------------------------------------------------------------------------------- Get Chomp Type Record
 * @brief Retrieves the chomp type record for a given chomp identifier.
 * @param chomp_id The 32-bit chomp identifier.
 * @return A pointer to the chomp type record.
 */
static const ChompTypeRecord* get_chomp_type_record(uint32_t chomp_id);
/** --------------------------------------------------------------------------------------------------------- Combined Region Size
 * @brief The bits that are the next-least-significant after the chomp type bits represent the combination
 * of the transient chomps (short-lived, but fast, no allocation overhead) versus the novel claim regions
 * (dedicated chomps, individually allocated). What defines how many of the combined region bits are
 * allocated to novel claim regions is the `NOVEL_REGION_BITS` constant.
 */
inline static constexpr size_t COMBINED_REGION_SIZE = 1u << REGION_BITS;
inline static constexpr size_t COMBINED_REGION_MASK = COMBINED_REGION_SIZE - 1u;
inline static constexpr size_t NUMBER_OF_TRANSIENT_REGIONS =
    (((1u << (REGION_BITS - 1u)) - 1u) & ~((1u << NOVEL_REGION_BITS) - 1u)) | (1u << (REGION_BITS - 1u));
inline static constexpr size_t NOVEL_CLAIM_REGION_MASK =
    COMBINED_REGION_MASK & ~((1u << NOVEL_REGION_BITS) - 1u);
inline static constexpr size_t NUMBER_OF_NOVEL_CLAIM_REGIONS = (1u << REGION_BITS) - NUMBER_OF_TRANSIENT_REGIONS;
inline static constexpr size_t NOVEL_CLAIM_MASK = (1u << NOVEL_REGION_BITS) - 1u;
/** --------------------------------------------------------------------------------------------------------- Is Novel Claim
 * @brief Checks if a chomp identifier corresponds to a novel claim region.
 * @param chomp_id The 32-bit chomp identifier.
 * @return True if the chomp is a novel claim, false otherwise.
 */
inline static constexpr bool is_novel_chomp_id(uint32_t chomp_id) {
    return ((chomp_id >> CHOMP_TYPE_BITS) & NOVEL_CLAIM_REGION_MASK) == NOVEL_CLAIM_REGION_MASK;
}
/** --------------------------------------------------------------------------------------------------------- Is Transient Chomp
 * @brief Checks if a chomp identifier corresponds to a transient region.
 * @param chomp_id The 32-bit chomp identifier.
 * @return True if the chomp is transient, false otherwise.
 */
inline static constexpr bool is_transient_chomp_id(uint32_t chomp_id) {
    return !is_novel_chomp_id(chomp_id);
}
/** --------------------------------------------------------------------------------------------------------- Get Chomp Region Index
 * @brief Retrieves the region index for a given chomp identifier, its slot in the combined region
 * directory; novel claim regions occupy the top `NUMBER_OF_NOVEL_CLAIM_REGIONS` slots.
 * @param chomp_id The 32-bit chomp identifier.
 * @return The index of the region the chomp belongs to.
 */
inline static constexpr size_t get_chomp_region_index(uint32_t chomp_id) {
    return (chomp_id >> CHOMP_TYPE_BITS) & COMBINED_REGION_MASK;
}
/** --------------------------------------------------------------------------------------------------------- Claims
 * @brief Each region is made up of a collection of `ChompEntry`s, which are atomically claimed from that
 * region. Hence, these `ChompEntry`s are sometimes referred to as "claims" within that region.
 */
inline static constexpr size_t CLAIMS_PER_REGION = 1u << CLAIM_BITS;
inline static constexpr size_t CLAIM_MASK = CLAIMS_PER_REGION - 1u;
/** --------------------------------------------------------------------------------------------------------- Get Claimed Entry
 * @brief Returns an RValue pointer to the claimed entry corresponding to the given chomp identifier. If
 * the identifier does not point to a valid, claimed entry, a nullptr will be returned.
 * @param chomp_id The 32-bit chomp identifier.
 * @return A pointer to the claimed entry, or nullptr if the entry is not valid or not claimed.
 */
inline static constexpr size_t get_chomp_claim_index(uint32_t chomp_id) {
    return (chomp_id >> (CHOMP_TYPE_BITS + REGION_BITS)) & CLAIM_MASK;
}
/** --------------------------------------------------------------------------------------------------------- ChompTypeRecord
 * @struct ChompTypeRecord
 * @brief The lifecycle hooks a typed slot runs over its elements on the claim and on the last release.
 */
struct ChompTypeRecord {
    /// @brief The element type's lifecycle method signature for construction and destruction.
    using LifecycleMethodSignature = void(*)(uint32_t chomp_id, size_t count);
    /// @brief The element type's C++ type information.
    const std::type_info* const type;
    /// @brief The size of each element in the chomp.
    const size_t size_per_element;
    /// @brief Method reference for construction behavior for a list of elements in a chomp.
    const LifecycleMethodSignature construct;
    /// @brief Method reference for destruction behavior for a list of elements in a chomp.
    const LifecycleMethodSignature destroy;
};
/** --------------------------------------------------------------------------------------------------------- ChompTypes
 * @class ChompTypes
 * @brief The table of element types under the type byte each slot carries; index zero is raw bytes and
 * index 255 marks a ChainBuffet's own slot, the one holding its reference count.
 */
class ChompTypes {
private:
    const ChompTypeRecord* records_[NUMBER_OF_CHOMP_TYPES];
    static std::atomic<uint32_t>& count() { static std::atomic<uint32_t> c{1}; return c; }
    std::unordered_map<const std::type_info*, uint32_t> type_to_index_;
    std::shared_mutex type_to_index_mutex_;
    static void raw_bytes_construct(uint32_t chomp_id, size_t count);
    static void raw_bytes_destroy(uint32_t chomp_id, size_t count);
    using Sig = ChompTypeRecord::LifecycleMethodSignature;
    template<typename T> static Sig typed_construct(void (*construct_single)(void*));
    template<typename T> static Sig typed_destroy(void (*destroy_single)(void*));
    template<typename T> static Sig typed_construct_multi(void (*construct_multi)(void*, size_t));
    template<typename T> static Sig typed_destroy_multi(void (*destroy_multi)(void*, size_t));
    static uint32_t register_type(const ChompTypeRecord* const record);
    inline static constexpr ChompTypeRecord RAW_BYTES_RECORD{
        &typeid(uint8_t), sizeof(uint8_t), raw_bytes_construct, raw_bytes_destroy};
    static ChompTypes& inst(); ChompTypes() = default; friend class Alligator; friend class Chomp;
public:
    ChompTypes(const ChompTypes&) = delete; ChompTypes& operator=(const ChompTypes&) = delete;
    ChompTypes(ChompTypes&&) = delete; ChompTypes& operator=(ChompTypes&&) = delete;
    ~ChompTypes() = default;
    static const ChompTypeRecord* get(const uint32_t& id);
    static const ChompTypeRecord* get(const std::type_info* type);
    static const ChompTypeRecord* get(const std::type_info& type);
    template<typename T> static const ChompTypeRecord* get() { return get(&typeid(T)); }
    /** ------------------------------------------------------------------------------------------- Register Chomp Type
     * @brief Registers a new chomp type with optional customized construct and destruct
     * functions.
     * @tparam T The type to register.
     * @param construct Pointer to the construct function for a single instance of T.
     * @param destruct Pointer to the destruct function for a single instance of T.
     * @return The type index the registered record occupies, the type byte a slot carries.
     */
    template<typename T>
    static uint32_t register_type(
        void (*construct)(void*) = nullptr,
        void (*destruct)(void*) = nullptr
    ) {
        const ChompTypeRecord* record = new ChompTypeRecord{
            .type = &typeid(T),
            .size_per_element = sizeof(T),
            .construct = ChompTypes::typed_construct<T>(construct),
            .destroy = ChompTypes::typed_destroy<T>(destruct)
        };
        return register_type(record);
    }
    /** ------------------------------------------------------------------------------------------- Register Chomp Type (Multiple)
     * @brief Registers a new chomp type with optional customized construct and destruct functions
     * for vectorized instances.
     * @tparam T The type to register.
     * @param construct_multiple Pointer to the construct function for multiple instances of T.
     * @param destruct_multiple Pointer to the destruct function for multiple instances of T.
     * @return The type index the registered record occupies, the type byte a slot carries.
     */
    template<typename T>
    static uint32_t register_type(
        void (*construct_multiple)(void*, size_t),
        void (*destruct_multiple)(void*, size_t)
    ) {
        const ChompTypeRecord* record = new ChompTypeRecord{
            .type = &typeid(T),
            .size_per_element = sizeof(T),
            .construct = ChompTypes::typed_construct_multi<T>(construct_multiple),
            .destroy = ChompTypes::typed_destroy_multi<T>(destruct_multiple)
        };
        return register_type(record);
    }
};
/** --------------------------------------------------------------------------------------------------------- Get Chomp Type Record
 * @brief Retrieves the chomp type record for a given chomp identifier.
 * @param chomp_id The 32-bit chomp identifier.
 * @return A pointer to the chomp type record.
 */
inline const ChompTypeRecord* get_chomp_type_record(uint32_t chomp_id) {
    return ChompTypes::get(get_chomp_type_index(chomp_id));
}
/** --------------------------------------------------------------------------------------------------------- ChompID
 * @class ChompID
 * @brief Represents a unique identifier for a chomp, encoding its type, region, and claim indices.
 */
class ChompID {
private:
    explicit ChompID(uint32_t region_index, uint32_t claim_index, uint32_t type_index, bool is_novel);
    size_t claim_index() const { return get_chomp_claim_index(id_); } ChompEntry* entry();
    const ChompEntry* entry() const; const ChompTypeRecord* lifecycle() const
    { return get_chomp_type_record(id_); } GPUBuf* gpubuf(); const GPUBuf* gpubuf() const;
    uint32_t* owners(); const uint32_t* owners() const;
    uint32_t id_; friend class Alligator; friend class Chomp; friend struct Region;
public:
    ChompID() : id_(0xFFFFFFFFu) {}
    template<typename T> requires std::is_convertible_v<T, uint32_t> && (!std::is_same_v<T, ChompID>)
    explicit ChompID(T id) : id_(static_cast<uint32_t>(id)) {}
    template<typename T> requires std::is_convertible_v<T, uint32_t> && (!std::is_same_v<T, ChompID>)
    ChompID& operator=(T id) { id_ = static_cast<uint32_t>(id); return *this; }
    ChompID(const ChompID& other); ChompID& operator=(const ChompID& other);
    ChompID(ChompID&& other) : id_(other.id_) { other.id_ = 0xFFFFFFFFu; }
    ChompID& operator=(ChompID&& other) { id_ = other.id_; other.id_ = 0xFFFFFFFFu; return *this; }
    ~ChompID(); const uint32_t& id() const { return id_; } 
    operator const uint32_t&() const { return id_; }
    operator bool() const { return id_ != 0xFFFFFFFFu; }
    bool is_null() const { return id_ == 0xFFFFFFFFu; }
};
/** --------------------------------------------------------------------------------------------------------- GPUBuf
 * @struct GPUBuf
 * @brief Stores a chomp's slab base address, byte length, and slab-relative byte offset.
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
/** --------------------------------------------------------------------------------------------------------- ChompType Concept
 * @brief Concept to check if a type conforms to the Chomp interface. The full concept which includes the
 * conversion operator to other ChompType's is declared at the bottom of this file.
 */
template<typename T>
concept PrimitiveChompType = (
    requires(T t) {
        { t.valid() } -> std::convertible_to<bool>;
        { t.chomp(std::declval<size_t>(), std::declval<size_t>()) } -> std::convertible_to<Chomp>;
        { t.chomp() } -> std::convertible_to<Chomp>;
        { t.raw() } -> std::convertible_to<void*>;
        { T(std::declval<size_t>()) } -> std::convertible_to<Chomp>;
        { t.template data<uint8_t>() } -> std::convertible_to<uint8_t*>;
        { t.template data<const uint8_t>() } -> std::convertible_to<const uint8_t*>;
        { t.template get_as<uint8_t>() } -> std::convertible_to<const uint8_t&>;
        { t.template get_as<const uint8_t>() } -> std::convertible_to<const uint8_t&>;
        { t.template size<uint8_t>() } -> std::convertible_to<size_t>;
        { t.template size<const uint8_t>() } -> std::convertible_to<size_t>;
        { t.size_bytes() } -> std::convertible_to<size_t>;
        { t.is_null() } -> std::convertible_to<bool>;
        { t.root_chomp() } -> std::convertible_to<Chomp>;
        { std::as_const(t).root_chomp() } -> std::convertible_to<Chomp>;
    } && (
        requires(T t) {
            { t.placement() } -> std::same_as<const BuffetDescriptor*>;
        }
    ||
        requires(T t) {
            { t.placement() } -> std::same_as<nullptr_t>;
        }
    )
) || std::is_same_v<T, Chomp>;
/** --------------------------------------------------------------------------------------------------------- Chomp
 * @class Chomp
 * @brief Represents a chomp of memory in the buffet alligator.
 * 
 * All operations in the buffet alligator are performed on chomps of memory, represented by the `Chomp`
 * class.
 * 
 * Memory chomps are claims from pre-allocated memory buffers that are managed by the buffet alligator's
 * slab arena. On systems that support unified memory, they are always chompd from host-coherent GPU
 * buffers. For systems that do not support unified memory, transfers between host and device memory are
 * handled automatically by the buffet alligator.
 * 
 * Chomps can be sub-chompd to create smaller chomps, and they all share the same reference counter and
 * underlying memory. Chomps can behave much like `std::shared_ptr` by calling the `chomp()` method with
 * no parameters passed, essentially requesting a sub-chomp that is the full size of the original. The copy
 * constructor and assignment operators are deleted to cut down on unintended reference counting traffic
 * which can be expensive in high-performance scenarios.
 * 
 * In most cases, chomps are meant to be short-lived since they are references to memory that is part of a
 * larger slab in the arena's memory pool. Holding on to a small claim for a long time can lead to
 * fragmentation. For cases in which you need to hold on to a chomp for longer than a transient operation,
 * you should specify the `novel_buffer` parameter when constructing the slob to `true` so that the chomp
 * is created as its own individual buffer that is not part of a larger slab. This is a slower operation
 * since it requires the buffer to be allocated and freed individually, so keep that in mind.
 */
class Chomp {
public:
    /** ------------------------------------------------------------------------------------------- Default Placement
     * @brief Returns the default placement for chomps, which is determined by the system's
     * capabilities.
     * @return The default `Placement` enum value for chomps.
     */
    static const BuffetDescriptor* default_placement();
    /** ------------------------------------------------------------------------------------------- Constructor - Fresh Claim
     * @brief Claims a chomp of pre-allocated memory in the buffet alligator's slab arena. The
     * chomp is guaranteed to be zero-initialized.
     * @param size The size of the chomp in bytes.
     */
    Chomp(size_t size = 0, const BuffetDescriptor* placement = default_placement());
    /** ------------------------------------------------------------------------------------------- Constructor - Fresh Claim
     * @brief Constructs a chomp of memory with the specified size. If not a novel buffer, then
     * it will be claimed from a pre-allocated slab in the buffet alligator's arena.
     * @param size The size of the chomp in bytes.
     * @param novel_buffer If true, then the chomp is allocated as a novel buffer instead of being
     * a claim of a pre-allocated slab. This is ideal for chomps that are long-lived to help
     * reduce fragmentation in the arena.
     */
    Chomp(size_t size, bool novel_buffer, const BuffetDescriptor* placement = default_placement());
    /** ------------------------------------------------------------------------------------------- Constructor - Copy from External Memory
     * @brief Copies data from an external memory location into a new chomp of memory in the
     * buffet alligator, ensuring the data is managed within the buffet alligator's memory system
     * and aligned properly for SIMD operations.
     * @param copy_from Pointer to the external memory to copy from.
     * @param size The size of the data to copy in bytes.
     * @param novel_buffer If true, then the chomp is allocated as a novel buffer instead of being
     * a claim of a pre-allocated slab. This is ideal for chomps that are long-lived to help
     * reduce fragmentation in the arena. Default is false.
     */
    Chomp(
        const void* copy_from,
        size_t size,
        bool novel_buffer = false,
        const BuffetDescriptor* placement = default_placement()
    );
    /** ------------------------------------------------------------------------------------------- Copy/move semantics
     * @brief Copying a `Chomp` does not actually copy the underlying memory, `Chomp` objects act
     * much like `std::shared_ptr` in that they share the same reference counter and underlying
     * memory. The copy constructor and assignment operators are deleted to cut down on unintended
     * reference counting traffic which can be expensive in high-performance scenarios. Move
     * semantics are supported to allow efficient transfer of ownership of the underlying memory.
     */
    Chomp(const Chomp& other);
    Chomp& operator=(const Chomp& other);
    Chomp(Chomp&& other) noexcept;
    Chomp& operator=(Chomp&& other) noexcept;
    /** ------------------------------------------------------------------------------------------- Destructor */
    ~Chomp() { free(); }
    /** ------------------------------------------------------------------------------------------- Placement
     * @brief Returns the memory placement type of the chomp.
     * @return The `Placement` enum value representing the chomp's memory placement.
     */
    const BuffetDescriptor* placement() const;
    /** ------------------------------------------------------------------------------------------- Raw accessors
     * @brief Use the buffet alligator's internal memory arena system to resolve the chomp's
     * host-writable pointer to the underlying memory.
     */
    void* raw();
    /** ------------------------------------------------------------------------------------------- Raw accessors - const
     * @brief Use the buffet alligator's internal memory arena system to resolve the chomp's
     * host-writable pointer to the underlying memory, but as a read-only pointer.
     */
    const void* raw() const;
    /** ------------------------------------------------------------------------------------------- Accessor - Typed
     * @brief Returns a pointer to the underlying data of the chomp, cast to the specified type.
     * @tparam T The type to cast the underlying data to. Default is uint8_t.
     * @return A pointer to the underlying data cast to type T.
     */
    template<typename T = uint8_t>
    T* data() { return static_cast<T*>(raw()); }
    /** ------------------------------------------------------------------------------------------- Accessor - Typed - const
     * @brief Returns a const pointer to the underlying data of the chomp, cast to the specified
     * type.
     * @tparam T The type to cast the underlying data to. Default is uint8_t.
     * @return A const pointer to the underlying data cast to type T.
     */
    template<typename T = uint8_t>
    const T* data() const { return static_cast<const T*>(raw()); }
    /** ------------------------------------------------------------------------------------------- Create new view
     * @brief Creates a new view of the chomp, which is a sub-chomp of the original chomp. The new
     * view shares the same underlying memory and reference counter as the original chomp. Using
     * the default parameters will create a new view that is essentially identical to the original
     * chomp - a shared view that increments the reference counter and will keep the underlying
     * memory alive until all views are destroyed.
     * @param offset The offset in bytes from the start of the original chomp to the start of the
     * new view.
     * @param length The length in bytes of the new view.
     * @return A new `Chomp` object that is a view of the original chomp.
     */
    Chomp chomp(size_t offset = 0, size_t length = SIZE_MAX) const;
    /** ------------------------------------------------------------------------------------------- Create new view (consumer name)
     * @brief `chomp` under the name consumers call it by.
     * @param offset The offset in bytes from the start of the original chomp to the start of the
     * new view.
     * @param length The length in bytes of the new view.
     * @return A new `Chomp` object that is a view of the original chomp.
     */
    Chomp slice(size_t offset = 0, size_t length = SIZE_MAX) const { return chomp(offset, length); }
    /** ------------------------------------------------------------------------------------------- Size in bytes
     * @brief Returns the size of the chomp in bytes.
     * @return The size of the chomp in bytes.
     */
    size_t size_bytes() const;
    /** ------------------------------------------------------------------------------------------- Size in elements
     * @brief Returns the size of the chomp in elements of type T.
     * @tparam T The type of elements in the chomp. Default is `uint8_t`.
     * @return The size of the chomp in elements of type T.
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
     * @brief Resizes the chomp to a new size. If `preserve_data` is true, the existing data in
     * the chomp will be preserved up to the minimum of the old and new sizes. If `preserve_data`
     * is false, the existing data will be discarded and the chomp will be reallocated. This can
     * be called on a freed or null chomp, in which case it will behave like a normal constructor
     * and allocate a new chomp of the specified size.
     * @param new_size The new size of the chomp in bytes.
     * @param preserve_data Whether to preserve existing data in the chomp. Default is true.
     * @param novel_buffer Whether to allocate a novel buffer even if the chomp is not null.
     * Default is false.
     * @param placement The memory placement strategy to use. Default is `default_placement()`.
     */
    void resize(
        size_t new_size,
        bool preserve_data = true,
        bool novel_buffer = false,
        const BuffetDescriptor* placement = default_placement()
    );
    /** ------------------------------------------------------------------------------------------- Check if chomp is null
     * @brief Checks if the chomp is null (i.e., has no underlying memory).
     * @return True if the chomp is null, false otherwise.
     */
    bool is_null() const;
    /** ------------------------------------------------------------------------------------------- Check if chomp is valid
     * @brief Checks if the chomp is valid (i.e., has underlying memory).
     * @return True if the chomp is valid, false otherwise.
     */
    bool valid() const;
    /** ------------------------------------------------------------------------------------------- Conversion to bool
     * @brief Allows the chomp to be used in boolean contexts.
     * @return True if the chomp is valid, false if it is null.
     */
    operator bool() const;
    /** ------------------------------------------------------------------------------------------- Free
     * @brief Frees the underlying memory of the chomp. This is called automatically when the
     * chomp is destroyed, but can be called manually to free the memory early. After calling this
     * method, the chomp will be null.
     */
    void free();
    /** ------------------------------------------------------------------------------------------- Get as
     * @brief Returns a reference to the underlying data of the chomp, cast to the specified type.
     * @tparam T The type to cast the underlying data to. Default is uint8_t.
     * @return A reference to the underlying data cast to type T.
     */
    template<typename T = uint8_t>
    T& get_as() { return *reinterpret_cast<T*>(data()); }
    /** ------------------------------------------------------------------------------------------- Get as (const)
     * @brief Returns a const reference to the underlying data of the chomp, cast to the specified
     * type.
     * @tparam T The type to cast the underlying data to. Default is uint8_t.
     * @return A const reference to the underlying data cast to type T.
     */
    template<typename T = uint8_t>
    const T& get_as() const { return *reinterpret_cast<const T*>(data()); }
    /** ------------------------------------------------------------------------------------------- Novel Backing
     * @brief Reports whether this chomp owns or views a dedicated novel buffer.
     * @return True when the backing allocation is a novel buffer.
     */
    bool is_novel() const noexcept;
    /** ------------------------------------------------------------------------------------------- Root chomp
     * @brief Returns a reference to the root chomp. This is useful when dealing with nested
     * chomps or `ChompType` conceptual objects.
     * @return A reference to the root chomp.
     */
    Chomp& root_chomp() { return *this; }
    /** ------------------------------------------------------------------------------------------- Root chomp (const)
     * @brief Returns a const reference to the root chomp. This is useful when dealing with nested
     * chomps or `ChompType` conceptual objects.
     * @return A const reference to the root chomp.
     */
    const Chomp& root_chomp() const { return *this; }
    /** ------------------------------------------------------------------------------------------- Adopt
     * @brief Adopts the given chomp, becoming another view of the same underlying data.
     * @param other The chomp to adopt.
     */
    void adopt(Chomp other);
    /** ------------------------------------------------------------------------------------------- Pool Index
     * @brief The alligator pool slot this chomp occupies; the id bits are all ones when null.
     * @return The pool slot index.
     */
    uint32_t pool_index() const { return id_; }
    /** ------------------------------------------------------------------------------------------- Conversion to uint32_t
     * @brief Implicitly converts the chomp to its pool index.
     * @return The pool index of the chomp.
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
     * @brief Provides access to the underlying data as a pointer of type T for const chomps.
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
     * @brief Provides access to the underlying data as a reference of type T for const chomps.
     * @tparam T The type to cast the raw pointer to.
     * @return A const reference to the underlying data cast to type T.
     */
    template<typename T>
    const T& operator*() const { return *static_cast<const T*>(raw()); }
    /** ------------------------------------------------------------------------------------------- ID
     * @brief Returns the ID of the chomp.
     * @return The ID of the chomp.
     */
    uint32_t id() const { return id_; }
    /** ------------------------------------------------------------------------------------------- Discharge Unsafely
     * @brief Releases the underlying ID of the chomp without marking the slots as unallocated,
     * which must be used carefully because it could lead to a memory leak if it is not re-adopted
     * by another chomp object later (e.g. by using `Chomp::from_id`).
     * @return The pool index of the discharged chomp.
     */
    uint32_t discharge_unsafely();
    /** ------------------------------------------------------------------------------------------- From ID
     * @brief Creates a chomp from the given previously discharged ID, that will safely release
     * the claim to memory when the `Chomp` object is destroyed.
     * @param id The ID to create the chomp from.
     * @return A chomp corresponding to the given ID.
     */
    static Chomp from_id(uint32_t id);
private:
    uint32_t id_ = UINT32_MAX;
    friend class Alligator;
    friend struct ChompEntry;
    friend class ChompQueue;
    friend struct ChompHandle;
    friend struct ChompNetworkAccess;
};
static_assert(sizeof(Chomp) == 4, "Chomp must be 4 bytes in size.");
/** --------------------------------------------------------------------------------------------------------- PotentialChomp
 * @class PotentialChomp
 * @brief A chomp that gets filled lazily by the provided fulfillment method.
 */
class PotentialChomp {
private:
    struct Details;
    /// @brief Shared pointer to the internal details of the PotentialChomp, created in chomp.cpp where Details is complete.
    std::shared_ptr<Details> details_;
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Default constructor for PotentialChomp.
     */
    PotentialChomp(
        Chomp (*fulfillment_method)(void* context) = nullptr,
        void* context = nullptr,
        void (*context_deleter)(void* context) = nullptr
    );
    /** ------------------------------------------------------------------------------------------- Copy/move semantics */
    PotentialChomp(const PotentialChomp& other);
    PotentialChomp(PotentialChomp&& other) noexcept;
    PotentialChomp& operator=(const PotentialChomp& other);
    PotentialChomp& operator=(PotentialChomp&& other) noexcept;
    /** ------------------------------------------------------------------------------------------- Destructor */
    ~PotentialChomp();
    /** ------------------------------------------------------------------------------------------- Get Raw Pointer
     * @brief Returns the raw pointer to the underlying chomp's data, fulfilling the lazy
     * initialization if necessary.
     * @return The raw pointer to the underlying chomp's data.
     */
    void* raw();
    /** ------------------------------------------------------------------------------------------- Get Raw Pointer (const)
     * @brief Returns the raw pointer to the underlying chomp's data, fulfilling the lazy
     * initialization if necessary.
     * @return The raw pointer to the underlying chomp's data.
     */
    const void* raw() const;
    /** ------------------------------------------------------------------------------------------- Get Typed Data Pointer
     * @brief Returns a typed pointer to the underlying chomp's data, fulfilling the lazy
     * initialization if necessary.
     * @tparam T The type of the pointer to return.
     * @return A typed pointer to the underlying chomp's data.
     */
    template<typename T>
    T* data() { return static_cast<T*>(raw()); }
    /** ------------------------------------------------------------------------------------------- Get Typed Data Pointer (const)
     * @brief Returns a typed pointer to the underlying chomp's data, fulfilling the lazy
     * initialization if necessary.
     * @tparam T The type of the pointer to return.
     * @return A typed pointer to the underlying chomp's data.
     */
    template<typename T>
    const T* data() const { return static_cast<const T*>(raw()); }
    /** ------------------------------------------------------------------------------------------- Check Fulfillment
     * @brief Checks if the chomp has been fulfilled.
     * @return `true` if the chomp has been fulfilled, `false` otherwise.
     */
    bool is_fulfilled() const;
    /** ------------------------------------------------------------------------------------------- Fulfill Chomp
     * @brief Fulfills the chomp by invoking its fulfillment method if it has not been fulfilled
     * yet.
     */
    void fulfill();
    /** ------------------------------------------------------------------------------------------- Get Raw Chomp
     * @brief Returns a pointer to the underlying raw Chomp object, which may be nullptr if the
     * chomp has not been fulfilled yet.
     * @return A pointer to the underlying raw Chomp object.
     */
    Chomp* raw_chomp() const;
    /** ------------------------------------------------------------------------------------------- Get Subchomp
     * @brief Returns a subchomp of the current chomp, starting at the specified offset and with
     * the specified length. This will fulfill the current chomp if it has not been fulfilled yet.
     * @param offset The starting offset of the subchomp.
     * @param length The length of the subchomp.
     * @return A new Chomp representing the subchomp.
     */
    Chomp chomp(size_t offset = 0, size_t length = SIZE_MAX) const;
    /** ------------------------------------------------------------------------------------------- Get Typed Size
     * @brief Returns the size of the chomp in terms of the number of elements of type T.
     * @tparam T The type of elements.
     * @return The number of elements of type T in the chomp.
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
     * @brief Returns the size of the chomp in bytes.
     * @return The size of the chomp in bytes.
     */
    size_t size_bytes() const;
    /** ------------------------------------------------------------------------------------------- Free Chomp
     * @brief Frees the underlying memory of the chomp if it has been allocated.
     */
    void free();
    /** ------------------------------------------------------------------------------------------- Null Chomp
     * @brief Checks if the chomp is null.
     * @return True if the chomp is null, false otherwise.
     */
    bool is_null() const;
    /** ------------------------------------------------------------------------------------------- Valid Chomp
     * @brief Checks if the chomp is valid.
     * @return True if the chomp is valid, false otherwise.
     */
    bool valid() const;
    /** ------------------------------------------------------------------------------------------- Bool Conversion
     * @brief Converts the chomp to a boolean value, indicating whether it is valid.
     * @return True if the chomp is valid, false otherwise.
     */
    operator bool() const;
    /** ------------------------------------------------------------------------------------------- Adopt Chomp
     * @brief Adopts the given Chomp, becoming another view of the same underlying memory. This
     * will complete the fulfillment contract for the PotentialChomp, overriding any fufillment
     * methods, and clean up the context if a deleter is set for it.
     * @param chomp The Chomp to adopt.
     */
    void adopt(Chomp chomp);
};
/** --------------------------------------------------------------------------------------------------------- ChompT
 * @class ChompT
 * @brief A template class that wraps a Chomp and provides type-safe access to its underlying memory.
 * @tparam T The type of elements in the chomp.
 */
template <typename T>
    requires (alignof(T) % 64 == 0)
class ChompT {
private:
    /// @brief The underlying Chomp object that this ChompT wraps.
    Chomp chomp_;
    /// @brief Claims one T's worth of bytes in a slot carrying T's type, so the last release runs ~T.
    static Chomp claim_typed(bool novel_buffer);
public:
    /** ------------------------------------------------------------------------------------------- Constructor with Initialization
     * @brief Constructs a ChompT and optionally initializes it with the given arguments.
     * @param initialize If true, allocates and initializes the chomp.
     * @param novel_buffer If true, allocates a novel buffer for the chomp.
     * @param args Arguments to forward to the constructor of T.
     */
    template <typename ...Args>
        requires std::is_constructible_v<T, Args...>
    ChompT(bool initialize = false, bool novel_buffer = false, Args&&... args) {
        if (initialize) {
            chomp_ = claim_typed(novel_buffer);
            new (chomp_.raw()) T(std::forward<Args>(args)...);
        }
    }
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs a ChompT from a Chomp. The Chomp must have a size that is a multiple of
     * the size of type T.
     * @param chomp The Chomp to wrap.
     */
    explicit ChompT(Chomp&& chomp) : chomp_(std::move(chomp)) {}
    /** ------------------------------------------------------------------------------------------- Copy/Move semantics */
    ChompT(const ChompT& other);
    ChompT(ChompT&& other) noexcept;
    ChompT& operator=(const ChompT& other);
    ChompT& operator=(ChompT&& other) noexcept;
    /** ------------------------------------------------------------------------------------------- Destructor */
    ~ChompT() { free(); }
    /** ------------------------------------------------------------------------------------------- BuffetDescriptor
     * @brief Returns the BuffetDescriptor of the chomp.
     * @return The `BuffetDescriptor` pointer representing the chomp's memory placement.
     */
    const BuffetDescriptor* placement() const;
    /** ------------------------------------------------------------------------------------------- Size in elements
     * @brief Returns the size of the chomp in elements of type T.
     * @return The size of the chomp in elements of type T.
     */
    template<typename U = T>
    size_t size() const {
        return chomp_.size_bytes() / sizeof(U);
    }
    /** ------------------------------------------------------------------------------------------- Size in bytes
     * @brief Returns the size of the chomp in bytes.
     * @return The size of the chomp in bytes.
     */
    size_t size_bytes() const {
        return chomp_.size_bytes();
    }
    /** ------------------------------------------------------------------------------------------- Raw pointer access
     * @brief Returns a pointer to the underlying memory of the chomp.
     * @return A pointer to the underlying memory of the chomp.
     */
    void* raw() { return chomp_.raw(); }
    /** ------------------------------------------------------------------------------------------- Raw pointer access - const
     * @brief Returns a const pointer to the underlying memory of the chomp.
     * @return A const pointer to the underlying memory of the chomp.
     */
    const void* raw() const { return chomp_.raw(); }
    /** ------------------------------------------------------------------------------------------- Data access
     * @brief Returns a pointer to the underlying memory of the chomp, cast to type U*.
     * @return A pointer to the underlying memory of the chomp.
     */
    template<typename U = T>
    U* data() {
        return reinterpret_cast<U*>(chomp_.raw());
    }
    /** ------------------------------------------------------------------------------------------- Data access - const
     * @brief Returns a const pointer to the underlying memory of the chomp, cast to type U*.
     * @return A const pointer to the underlying memory of the chomp.
     */
    template<typename U = T>
    const U* data() const { return reinterpret_cast<const U*>(chomp_.raw()); }
    /** ------------------------------------------------------------------------------------------- Get as
     * @brief Returns a zero-copy view for view types or a reference to type U.
     * @return A view or reference to the underlying memory of the chomp.
     */
    template<typename U = T>
    decltype(auto) get_as() {
        if constexpr (std::is_same_v<T, U> && std::is_same_v<U, std::string_view>) {
            return std::string_view(
                reinterpret_cast<const char*>(chomp_.raw()), chomp_.size_bytes()
            );
        } else if constexpr (std::is_same_v<T, U> && requires {
            typename U::element_type;
            requires std::is_same_v<U, std::span<typename U::element_type, U::extent>>;
        }) {
            return U(
                reinterpret_cast<typename U::element_type*>(chomp_.raw()),
                chomp_.size_bytes() / sizeof(typename U::element_type)
            );
        } else {
            return *reinterpret_cast<U*>(chomp_.raw());
        }
    }
    /** ------------------------------------------------------------------------------------------- Get as - const
     * @brief Returns a read-only view for view types or a const reference to type U.
     * @return A read-only view or reference to the underlying memory of the chomp.
     */
    template<typename U = T>
    decltype(auto) get_as() const {
        if constexpr (std::is_same_v<T, U> && std::is_same_v<U, std::string_view>) {
            return std::string_view(
                reinterpret_cast<const char*>(chomp_.raw()), chomp_.size_bytes()
            );
        } else if constexpr (std::is_same_v<T, U> && requires {
            typename U::element_type;
            requires std::is_same_v<U, std::span<typename U::element_type, U::extent>>;
        }) {
            return std::span<const typename U::element_type, U::extent>(
                reinterpret_cast<const typename U::element_type*>(chomp_.raw()),
                chomp_.size_bytes() / sizeof(typename U::element_type)
            );
        } else {
            return *reinterpret_cast<const U*>(chomp_.raw());
        }
    }
    /** ------------------------------------------------------------------------------------------- Null check
     * @brief Checks if the chomp is null.
     * @return True if the chomp is null, false otherwise.
     */
    bool is_null() const { return chomp_.is_null(); }
    /** ------------------------------------------------------------------------------------------- Boolean conversion
     * @brief Converts the chomp to a boolean value indicating its validity.
     * @return True if the chomp is valid, false otherwise.
     */
    operator bool() const { return chomp_.valid(); }
    /** ------------------------------------------------------------------------------------------- Free
     * @brief Frees the resources associated with the chomp.
     */
    void free() { chomp_.free(); }
    /** ------------------------------------------------------------------------------------------- Validity check
     * @brief Checks if the chomp is valid (i.e., not null).
     * @return True if the chomp is valid, false otherwise.
     */
    bool valid() const { return !chomp_.is_null(); }
    /** ------------------------------------------------------------------------------------------- Underlying chomp
     * @brief Returns the underlying untyped Chomp.
     * @return The underlying Chomp represented by this typed chomp.
     */
    Chomp chomp(size_t offset = 0, size_t length = SIZE_MAX) const {
        return chomp_.chomp(offset, length);
    }
    /** ------------------------------------------------------------------------------------------- Root chomp
     * @brief Returns a reference to the underlying root Chomp.
     * @return A reference to the underlying Chomp.
     */
    Chomp& root_chomp() { return chomp_.root_chomp(); }
    /** ------------------------------------------------------------------------------------------- Root chomp (const)
     * @brief Returns a const reference to the underlying root Chomp.
     * @return A const reference to the underlying Chomp.
     */
    const Chomp& root_chomp() const { return chomp_.root_chomp(); }
    /** ------------------------------------------------------------------------------------------- Raw pointer access
     * @brief Returns a pointer to the underlying memory of the chomp, cast to type T*.
     * @return A pointer to the underlying memory of the chomp.
     */
    T& operator*() {
        return *reinterpret_cast<T*>(chomp_.raw());
    }
    /** ------------------------------------------------------------------------------------------- Raw pointer access - const
     * @brief Returns a const pointer to the underlying memory of the chomp, cast to const T*.
     * @return A const pointer to the underlying memory of the chomp.
     */
    const T& operator*() const {
        return *reinterpret_cast<const T*>(chomp_.raw());
    }
    /** ------------------------------------------------------------------------------------------- Pointer access
     * @brief Returns a pointer to the underlying memory of the chomp, cast to type T*.
     * @return A pointer to the underlying memory of the chomp.
     */
    T* operator->() {
        return reinterpret_cast<T*>(chomp_.raw());
    }
    /** ------------------------------------------------------------------------------------------- Pointer access - const
     * @brief Returns a const pointer to the underlying memory of the chomp, cast to const T*.
     * @return A const pointer to the underlying memory of the chomp.
     */
    const T* operator->() const {
        return reinterpret_cast<const T*>(chomp_.raw());
    }
    /** ------------------------------------------------------------------------------------------- Conversion operator
     * @brief Converts the ChompT to a Chomp. This allows for implicit conversion to the
     * underlying Chomp type.
     */
    operator Chomp() const {
        return chomp_;
    }
    /** ------------------------------------------------------------------------------------------- Length of the chomp
     * @brief Returns the length of the chomp in terms of the number of elements of type T.
     * @return The length of the chomp.
     */
    size_t length() const {
        if constexpr (std::is_same_v<T, std::string>) {
            return chomp_.size_bytes();
        } else if constexpr (requires {
            typename T::value_type;
            requires std::is_same_v<std::vector<typename T::value_type>, T>;
        }) {
            return chomp_.size_bytes() / sizeof(typename T::value_type);
        }
        return chomp_.size_bytes() / sizeof(T);
    }
    /** ------------------------------------------------------------------------------------------- Conversion to uint32_t
     * @brief Implicitly converts the ChompT to its pool index.
     * @return The pool index of the chomp.
     */
    uint32_t pool_index() const {
        return chomp_.pool_index();
    }
    /** ------------------------------------------------------------------------------------------- Conversion to uint32_t
     * @brief Implicitly converts the ChompT to its pool index.
     * @return The pool index of the chomp.
     */
    operator uint32_t() const {
        return pool_index();
    }
};
/** --------------------------------------------------------------------------------------------------------- Copy Constructor
 * @brief Copy constructor for ChompT.
 * @param other The ChompT object to copy from.
 */
template <typename T>
    requires (alignof(T) % 64 == 0)
ChompT<T>::ChompT(const ChompT& other) : chomp_(other.chomp_) {}
/** --------------------------------------------------------------------------------------------------------- Operator= Copy
 * @brief Copy assignment operator for ChompT.
 * @param other The ChompT object to copy from.
 * @return Reference to the current ChompT object.
 */
template <typename T>
    requires (alignof(T) % 64 == 0)
ChompT<T>::ChompT(ChompT&& other) noexcept : chomp_(std::move(other.chomp_)) {}
/** --------------------------------------------------------------------------------------------------------- Operator= Copy
 * @brief Copy assignment operator for ChompT.
 * @param other The ChompT object to copy from.
 * @return Reference to the current ChompT object.
 */
template <typename T>
    requires (alignof(T) % 64 == 0)
ChompT<T>& ChompT<T>::operator=(const ChompT& other) {
    if (this != &other) chomp_ = other.chomp_;
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Operator= Move
 * @brief Move assignment operator for ChompT.
 * @param other The ChompT object to move from.
 * @return Reference to the current ChompT object.
 */
template <typename T>
    requires (alignof(T) % 64 == 0)
ChompT<T>& ChompT<T>::operator=(ChompT&& other) noexcept {
    if (this != &other) chomp_ = std::move(other.chomp_);
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- ChompType Concept
 * @brief Concept to check if a type conforms to the `Chomp` conceptual interface, including the conversion
 * operator to other ChompType's.
 */
template<typename T>
concept ChompType = requires(T t) {
    { t.chomp(std::declval<size_t>(), std::declval<size_t>()) } -> std::same_as<Chomp>;
    { static_cast<Chomp>(t) } -> std::same_as<Chomp>;
} || PrimitiveChompType<T> || std::is_same_v<T, Chomp>;
static_assert(ChompType<Chomp>, "Chomp must satisfy the ChompType concept.");
class ConcurrentBitplane;
/** --------------------------------------------------------------------------------------------------------- Arena
 * @class Alligator
 * @brief The "Allocator" manages memory allocation and ownership for Chomp claims.
 */
class Alligator {
private:
    class ChainBuffet {
    private:
        void* buffer_;
        const BuffetDescriptor* desc;
        SharedBuffet* parent_ = nullptr;
        std::atomic<uint32_t> bump_ptr_{0};
        std::atomic<ChainBuffet*> next_{nullptr};
        std::atomic<ChainBuffet*> prev_{nullptr};
        ChainBuffet* next(bool allocate_next = false);
        ChainBuffet(
            SharedBuffet* parent,
            const BuffetDescriptor* desc,
            size_t size,
            bool allocate_next = false
        );
        void free();
        friend class Alligator; friend struct ChompEntry;
        friend class SharedBuffet;
    public:
        ChainBuffet() = default; bool valid() const { return buffer_ != nullptr; }
        operator bool() const { return valid(); } ~ChainBuffet() { free(); }
        ChainBuffet(const ChainBuffet&) = delete;
        ChainBuffet& operator=(const ChainBuffet&) = delete;
        ChainBuffet(ChainBuffet&&) = delete;
        ChainBuffet& operator=(ChainBuffet&&) = delete;
        ChompEntry* claim_into(ChompEntry* slot, GPUBuf* record, size_t size);
    };
    static void* current_for(const BuffetDescriptor* desc, void** expected = nullptr, void* desired = nullptr);
    struct Region {
        std::array<GPUBuf, CLAIMS_PER_REGION> gpu_slots;
        std::array<ChompEntry, CLAIMS_PER_REGION> chomp_slots;
        std::array<uint32_t, CLAIMS_PER_REGION> owner_slots;  ///< Chomps on each slot; the last out clears it.
        std::atomic<uint32_t> next_index{0};
        MultiAtomic refcounts;
        GPUBuf* gpu_table() { return gpu_slots.data(); }
        const GPUBuf* gpu_table() const { return gpu_slots.data(); }
    };
    static size_t& default_region_allocator();
    struct ChompRegion {
        uint64_t device_address = 0;
        struct HostDetails {
            Region* region;
            void* handle;
            const BuffetDescriptor* desc;
        };
        HostDetails* host_details = nullptr;
        Region* region();
        const Region* region() const;
        Region* materialize();
        ChompRegion();
        ChompRegion(const ChompRegion&) = delete;
        ChompRegion& operator=(const ChompRegion&) = delete;
        ChompRegion(ChompRegion&&) = delete;
        ChompRegion& operator=(ChompRegion&&) = delete;
        ~ChompRegion();
    };
    uint64_t region_backing_device_address_ = 0;
    void* region_backing_raw_ = nullptr;
    void* region_backing_host_ = nullptr;
    std::array<ChompRegion, COMBINED_REGION_SIZE>* region_backing();
    mutable std::atomic<uint32_t> transient_idx{0};  ///< Region above CLAIM_BITS, claim below.
    mutable std::atomic<uint32_t> novel_idx{0};  ///< Region above CLAIM_BITS, claim below.
    ChompEntry* entry(Chomp& chomp);
    const ChompEntry* entry(const Chomp& chomp) const;
    GPUBuf* gpubuf(const Chomp& chomp);
    const GPUBuf* gpubuf(const Chomp& chomp) const;
    uint64_t gpu_directory_address() const;
    void destroy(Chomp& chomp);
    static SharedBuffet* buffer_of(const Chomp& chomp);
    static void retain(const Chomp& chomp);
    static void retire_handle(void* handle);
    static void finalize();
    Alligator();
    ~Alligator();
    friend class Chomp; friend class Memory;
    friend class VulkanKernel; friend class ChompID;
    friend class Kitchen; friend class ChompTypes;
    friend class SharedBuffet; friend struct ChompEntry;
    friend struct KitchenInitializer;
public:
    Alligator(const Alligator&) = delete;
    Alligator& operator=(const Alligator&) = delete;
    Alligator(Alligator&&) = delete;
    Alligator& operator=(Alligator&&) = delete;
    /** ------------------------------------------------------------------------------------------- Claim
     * @brief The one way to get a Chomp: whole 64-byte granules cut from the placement's current link,
     * or a dedicated buffet for a novel or slab-sized request, in a fresh slot carrying the type.
     * @param size The size in bytes, rounded up to 64-byte granules; zero claims nothing.
     * @param novel Whether the Chomp gets a dedicated buffet.
     * @param placement The placement to claim from.
     * @param type The ChompTypes record index the slot carries, zero for raw bytes.
     * @return The claimed Chomp.
     */
    static Chomp claim(
        size_t size,
        bool novel = false,
        const BuffetDescriptor* placement = Chomp::default_placement(),
        uint32_t type = 0
    );
    /** ------------------------------------------------------------------------------------------- Buffet Of
     * @brief The buffet object behind a Chomp, the pointer its placement's static hooks take.
     * @param chomp The Chomp.
     * @return The buffet object.
     */
    static void* buffet_of(const Chomp& chomp);
    /** ------------------------------------------------------------------------------------------- Instance
     * @brief Retrieves the singleton instance of the Alligator.
     * @return Reference to the Alligator instance.
     */
    static Alligator& inst();
};
/** --------------------------------------------------------------------------------------------------------- Typed Lifecycle Hooks
 * @brief Each registered T keeps its single-element hooks in one static pair; the record's hooks walk a
 * slot's elements through them, or through T's own constructor and destructor when none were given.
 */
template<typename T> struct ChompTypeHooks {
    inline static void (*construct_single)(void*) = nullptr;
    inline static void (*destroy_single)(void*) = nullptr;
    inline static void (*construct_multi)(void*, size_t) = nullptr;
    inline static void (*destroy_multi)(void*, size_t) = nullptr;
    static void construct(uint32_t chomp_id, size_t count) {
        Chomp chomp = Chomp::from_id(chomp_id);
        T* elements = chomp.data<T>();
        if (construct_multi) construct_multi(elements, count);
        else for (size_t index = 0; index < count; ++index) {
            if (construct_single) construct_single(elements + index); else new (elements + index) T();
        }
        chomp.discharge_unsafely();
    }
    static void destroy(uint32_t chomp_id, size_t count) {
        Chomp chomp = Chomp::from_id(chomp_id);
        T* elements = chomp.data<T>();
        if (destroy_multi) destroy_multi(elements, count);
        else for (size_t index = 0; index < count; ++index) {
            if (destroy_single) destroy_single(elements + index); else elements[index].~T();
        }
        chomp.discharge_unsafely();
    }
};
template<typename T> ChompTypes::Sig ChompTypes::typed_construct(void (*construct_single)(void*)) {
    ChompTypeHooks<T>::construct_single = construct_single;
    return &ChompTypeHooks<T>::construct;
}
template<typename T> ChompTypes::Sig ChompTypes::typed_destroy(void (*destroy_single)(void*)) {
    ChompTypeHooks<T>::destroy_single = destroy_single;
    return &ChompTypeHooks<T>::destroy;
}
template<typename T> ChompTypes::Sig ChompTypes::typed_construct_multi(void (*construct_multi)(void*, size_t)) {
    ChompTypeHooks<T>::construct_multi = construct_multi;
    return &ChompTypeHooks<T>::construct;
}
template<typename T> ChompTypes::Sig ChompTypes::typed_destroy_multi(void (*destroy_multi)(void*, size_t)) {
    ChompTypeHooks<T>::destroy_multi = destroy_multi;
    return &ChompTypeHooks<T>::destroy;
}
/** --------------------------------------------------------------------------------------------------------- Typed Claim
 * @brief One T claimed in a slot carrying T's registered type index.
 */
template <typename T>
    requires (alignof(T) % 64 == 0)
Chomp ChompT<T>::claim_typed(bool novel_buffer) {
    return Alligator::claim(sizeof(T), novel_buffer, Chomp::default_placement(), ChompTypes::register_type<T>());
}
} // namespace buffetalligator