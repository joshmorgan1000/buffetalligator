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
class Alligator; class Slice; class Memory;
class ChainBuffet; class SliceQueue; class SliceChannel;
struct SliceRecord; class PrioritySlice; class HeapSlice;
class AlignedHeapBuffer; class VulkanBuffer;
struct BuffetDescriptor; struct SliceTypeRecord; class IBuffet;
class SliceMap; class PotentialSlice; class SliceQueue;
template<typename T> class SliceT;
template<typename T> class SliceMapT;
template<typename T> class PotentialSliceT;
template<typename T> class SliceQueueT;
/// @brief Helper template that always evaluates to false, used for static assertions in templates.
template<typename> struct always_false : std::false_type {};
/** --------------------------------------------------------------------------------------------------------- Arguments Holder
 * @struct ArgumentsHolder
 * @brief Holds a collection of arguments for stuff.
 */
struct ArgumentsHolder {
    /// @brief Stores the arguments.
    std::vector<void*> args;
    /// @brief Stores the deleter functions for the arguments.
    std::vector<void(*)(void*)> deleters;
    /// @brief Function to convert the arguments to a tuple.
    void* (*to_tuple)(std::vector<void*>&&);
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs an ArgumentsHolder with the given arguments.
     * @tparam Args The types of the arguments.
     * @param a The arguments to store.
     */
    template<typename... Args>
    ArgumentsHolder(Args... a) {
        args.reserve(sizeof...(Args));
        (args.push_back(new std::remove_reference_t<Args>(std::move(a))), ...);
        deleters.reserve(sizeof...(Args));
        (deleters.push_back([](void* ptr) {
            if (ptr) delete static_cast<std::remove_reference_t<Args>*>(ptr);
        }), ...);
        to_tuple = [](std::vector<void*>&& args) -> void* {
            std::tuple<std::remove_reference_t<Args>...>* tuple_ptr =
                new std::tuple<std::remove_reference_t<Args>...>(
                    std::move(*static_cast<std::remove_reference_t<Args>*>(args[0]))...
                );
            return static_cast<void*>(tuple_ptr);
        };
    }
    /** ------------------------------------------------------------------------------------------- No Copy/Move */
    ArgumentsHolder(const ArgumentsHolder&) = delete;
    ArgumentsHolder& operator=(const ArgumentsHolder&) = delete;
    ArgumentsHolder(ArgumentsHolder&&) = delete;
    ArgumentsHolder& operator=(ArgumentsHolder&&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Destroys the ArgumentsHolder and calls the deleter functions for all
     * stored arguments.
     */
    ~ArgumentsHolder() { for (size_t i = 0; i < args.size(); ++i) deleters[i](args[i]); }
    /** ------------------------------------------------------------------------------------------- Drain to Tuple
     * @brief Converts the stored arguments to a tuple and returns it as a void pointer.
     * @return A void pointer to the tuple containing the stored arguments.
     */
    void* drain_to_tuple() { return to_tuple(std::move(args)); }
};
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
    const void* semantics = nullptr;  ///< The table Slices claimed here carry, null for the chain's own.
    uint32_t type = 0;  ///< The type word Slices claimed here carry, zero for the table's own.
    size_t index = 0;  ///< The placement's index in the registry.
    const BuffetDescriptor* base = nullptr;  ///< The placement whose memory this is: itself, or a type's base.
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
    IBuffetProxy& operator=(IBuffetProxy&& other) noexcept { std::swap(buffer_, other.buffer_); return *this; }
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
    static uint64_t device_address(void* ptr) { return static_cast<IBuffetProxy*>(ptr)->buffer_->device_address(); }
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
        list_[index]->index = index;
        list_[index]->base = list_[index];
        if (index == 0) default_descriptor().store(list_[0], std::memory_order_release);
        return list_[index];
    }
    /** --------------------------------------------------------------------------------------------------------- add_typed
     * @brief Registers a placement for a type: the base placement's hooks, claiming Slices that carry the
     * type's table and type word.
     * @param base The placement whose memory the type's claims come from.
     * @param semantics The type's table.
     * @param type The type word its claims carry.
     * @return A pointer to the type's placement descriptor.
     */
    const BuffetDescriptor* add_typed(const BuffetDescriptor& base, const void* semantics, uint32_t type);
    ~BuffetDescriptors() = default;
    friend class Slice;
public:
    BuffetDescriptors(const BuffetDescriptors&) = delete;
    BuffetDescriptors& operator=(const BuffetDescriptors&) = delete;
    BuffetDescriptors(BuffetDescriptors&&) = delete;
    BuffetDescriptors& operator=(BuffetDescriptors&&) = delete;
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
/** --------------------------------------------------------------------------------------------------------- AlignedHeapBuffer
 * @class AlignedHeapBuffer
 * @brief A heap-allocated buffer with 64-byte alignment.
 */
class alignas(16) AlignedHeapBuffer {
private:
    void* buffer_ = nullptr;
    size_t size_ = 0;
public:
    operator bool() const { return buffer_ != nullptr; }
    bool operator==(std::nullptr_t) const { return buffer_ == nullptr; }
    bool operator!=(std::nullptr_t) const { return buffer_ != nullptr; }
    AlignedHeapBuffer(size_t size = 0) : size_(((size + 63) & ~63)) {
        if (size_ == 0) return;
        if (size_ >= SLAB_FLOOR_BYTES) {
            const size_t available = available_host_bytes();
            if (available < size_) ALLIGATOR_THROW("AlignedHeapBuffer: " + std::to_string(size_)
                + " bytes requested with " + std::to_string(available) + " available on the host");
        }
        buffer_ = aligned_alloc(64, size_);
        if (buffer_ == nullptr) throw std::bad_alloc();
        std::memset(buffer_, 0, size_);
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
    static void* factory(size_t size) { return new AlignedHeapBuffer(size); }
    static size_t default_size() { return 256 * 1024 * 1024; }
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
/** --------------------------------------------------------------------------------------------------------- VulkanBuffer
 * @class VulkanBuffer
 * @brief The Vulkan handles behind one slab: buffer, memory, mapping, and device address.
 */
class VulkanBuffer {
private:
    vk::Buffer buffer_{};          ///< Vulkan buffer handle (the compute target).
    vk::DeviceMemory memory_{};    ///< Backing memory allocation.
    void* host_ = nullptr;         ///< Persistent CPU mapping.
    uint64_t address_ = 0;         ///< Device address (the shader-side pointer).
    size_t size_ = 0;              ///< Size in bytes.
    /** ------------------------------------------------------------------------------------------- release
     * @brief Unmaps and frees the Vulkan objects; shared by the destructor and move assignment.
     */
    void release();
    friend class VulkanContext;
public:
    VulkanBuffer() = default;
    /** ------------------------------------------------------------------------------------------- Allocating Constructor
     * @brief The five-call Vulkan allocation: create, get requirements, allocate, bind, map, plus
     * the device-address query and the zero-fill the slab contract requires.
     * @param size_bytes Buffer size in bytes.
     */
    VulkanBuffer(size_t size_bytes);
    /** ------------------------------------------------------------------------------------------- Allocating Constructor (typed)
     * @brief The same five-call allocation against an explicit memory type index.
     * @param size_bytes Buffer size in bytes.
     * @param memory_type_index The memory type to allocate from.
     */
    VulkanBuffer(size_t size_bytes, uint32_t memory_type_index);
    /** ------------------------------------------------------------------------------------------- Move-only ownership */
    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;
    VulkanBuffer(VulkanBuffer&& other) noexcept
    : buffer_(std::exchange(other.buffer_, nullptr))
    , memory_(std::exchange(other.memory_, nullptr))
    , host_(std::exchange(other.host_, nullptr))
    , address_(std::exchange(other.address_, 0))
    , size_(std::exchange(other.size_, 0)) {}
    VulkanBuffer& operator=(VulkanBuffer&& other) noexcept {
        std::swap(buffer_, other.buffer_); std::swap(memory_, other.memory_);
        std::swap(host_, other.host_); std::swap(address_, other.address_);
        std::swap(size_, other.size_); return *this;
    }
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Unmap and destroy the Vulkan objects. Defined in vulkan.cpp.
     */
    ~VulkanBuffer() { release(); }
    operator bool() const { return host_ != nullptr; }
    bool operator==(std::nullptr_t) const { return host_ == nullptr; }
    bool operator!=(std::nullptr_t) const { return host_ != nullptr; }
    /** ------------------------------------------------------------------------------------------- Buffet Hooks
     * @brief The static hooks BuffetDescriptors::descriptor_for binds for this type.
     */
    static void deleter_impl(void* ptr) { delete static_cast<VulkanBuffer*>(ptr); }
    static void (*deleter())(void*) { return deleter_impl; }
    static void* host_ptr(void* ptr) { return static_cast<VulkanBuffer*>(ptr)->host_; }
    static size_t size_of_impl(void* ptr) { return static_cast<VulkanBuffer*>(ptr)->size_; }
    static size_t size_of(void* ptr) { return size_of_impl(ptr); }
    static size_t (*size_of())(void*) { return size_of_impl; }
    static void* factory(size_t size) {
        if (size > (uint64_t{UINT32_MAX} << 6))
            ALLIGATOR_VULKAN_THROW("VulkanBuffer exceeds the 32-bit limit of 64-byte units");
        return new VulkanBuffer((size + 63) & ~size_t{63});
    }
    static size_t default_size() { return 256 * 1024 * 1024; }
    static size_t* type_idx() { static size_t idx = 1; return &idx; }
    static const char* type_name() { return "VulkanBuffer"; }
    static uint64_t device_address(void* ptr) {
        return static_cast<VulkanBuffer*>(ptr)->address_;
    }
    /** ------------------------------------------------------------------------------------------- address
     * @brief Retrieves the device address of the Vulkan buffer.
     * @return The device address.
     */
    uint64_t address() const { return address_; }
    /** ------------------------------------------------------------------------------------------- size
     * @brief Retrieves the size of the Vulkan buffer.
     * @return The size of the buffer in bytes.
     */
    size_t size() const { return size_; }
    /** ------------------------------------------------------------------------------------------- host
     * @brief Retrieves the host pointer of the Vulkan buffer.
     * @return The host pointer.
     */
    void* host() const { return host_; }
    /** ------------------------------------------------------------------------------------------- buffer
     * @brief The Vulkan buffer handle, for indirect dispatch commands recorded against it.
     * @return The buffer handle.
     */
    vk::Buffer buffer() const { return buffer_; }
    void* raw() { return host_; }
    const void* raw() const { return host_; }
};
static_assert(IsABuffetType<VulkanBuffer>, "VulkanBuffer must satisfy IsABuffetType");
/** --------------------------------------------------------------------------------------------------------- Create Slice Metadata
 * @struct CreateSliceMetadata
 * @brief Holds metadata required to create a slice backed by an external buffer type.
 */
struct CreateSliceMetadata {
    /// @brief The GPU device address of the buffer (if applicable - must be GPU coherent).
    uint64_t (*device_address_fn)(void*) = nullptr;
    /// @brief The size of the buffer.
    size_t size = 0;
    /// @brief The handle to the external buffer.
    void* buffer_handle;
    /// @brief The type information of the external buffer.
    const std::type_info* type_info;
};
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
class alignas(32) Slice {
public:
    /** ------------------------------------------------------------------------------------------- Default Placement
     * @brief Returns the default placement for slices, which is determined by the system's
     * capabilities.
     * @return The default `Placement` enum value for slices.
     */
    static const BuffetDescriptor* default_placement();
    /** ------------------------------------------------------------------------------------------- Default Constructor
     * @brief A null Slice: all zero bytes, a weak handle to nothing, so zeroed memory is null
     * Slices.
     */
    Slice() noexcept = default;
    /** ------------------------------------------------------------------------------------------- Constructor - Fresh Claim
     * @brief Claims a slice of pre-allocated memory in the buffet alligator's slab arena. The
     * slice is guaranteed to be zero-initialized.
     * @param size The size of the slice in bytes.
     * @param placement The placement to claim from.
     */
    Slice(size_t size, const BuffetDescriptor* placement = default_placement());
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
     * @param placement Optional placement descriptor for the new slice.
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
     * memory. The copy constructor and assignment operators are deleted to cut down on unintended
     * reference counting traffic which can be expensive in high-performance scenarios. Move
     * semantics are supported to allow efficient transfer of ownership of the underlying memory.
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
     * @param offset The offset in bytes from the start of the original slice to the start of the
     * new view.
     * @param length The length in bytes of the new view.
     * @return A new `Slice` object that is a view of the original slice.
     */
    Slice slice(size_t offset = 0, size_t length = SIZE_MAX) const;
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
    bool is_null() const noexcept;
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
    //--------------------------------------------------------------------------------------------- Property Accessors
    bool is_encrypted() const noexcept;
    bool is_compressed() const noexcept;
    bool is_text() const noexcept;
    bool is_numeric() const noexcept;
    bool is_unsigned() const noexcept;
    bool is_floating_point() const noexcept;
    size_t bitwidth() const noexcept;
    size_t size_bits() const noexcept;
    size_t size() const noexcept;
    size_t capacity() const noexcept;
    bool is_signed() const noexcept;
    bool is_integral() const noexcept;
    bool is_single_element() const noexcept;
    bool is_singular() const noexcept;
    bool is_list() const noexcept;
    bool is_chunked() const noexcept;
    bool is_json() const noexcept;
    size_t alignment() const noexcept;
    bool is_aligned() const noexcept;
    bool is_unaligned() const noexcept;
    bool is_page_aligned() const noexcept;
    bool is_borrowed() const noexcept;
    bool is_owned() const noexcept;
    bool is_shared() const noexcept;
    bool is_pooled() const noexcept;
    bool is_file_mapped() const noexcept;
    bool is_host_coherent() const noexcept;
    bool is_device_local() const noexcept;
    bool is_heap() const noexcept;
    bool is_slice_of_slices() const noexcept;
    bool is_slicemap() const noexcept;
    bool is_atomic() const noexcept;
    size_t size_bytes() const noexcept;
    const std::type_info& parent_type() const noexcept;
    /** ------------------------------------------------------------------------------------------- Semantics For
     * @brief Registers T as a type a Slice can be made over: `new T(args...)` makes its object,
     * the getters say where its bytes are and how many, and the last Slice over it deletes it.
     * @tparam T The type for which to provide semantics.
     * @tparam Args The types of the constructor arguments for T.
     * @param host_pointer The host pointer getter for a T.
     * @param size The byte size getter for a T.
     * @param share Whether Slices over one T share it by reference counting.
     * @return A void pointer to the semantics for type T.
     */
    template<typename T, typename... Args>
    static void* semantics_for(
        void* (*host_pointer)(void* object),
        size_t (*size)(void* object),
        bool share = true
    ) {
        return semantics_for(
            &typeid(T),
            reinterpret_cast<void*>(+[](ArgumentsHolder&& args) -> void* {
                std::tuple<std::remove_reference_t<Args>...>* arguments =
                    static_cast<std::tuple<std::remove_reference_t<Args>...>*>(args.drain_to_tuple());
                T* object = std::apply(
                    [](auto&&... a) { return new T(std::forward<decltype(a)>(a)...); },
                    std::move(*arguments)
                );
                delete arguments;
                return object;
            }),
            reinterpret_cast<void*>(size),
            reinterpret_cast<void*>(host_pointer),
            reinterpret_cast<void*>(+[](void* object) { delete static_cast<T*>(object); }),
            share
        );
    }
private:
    uint64_t address_ = 0;
    uint32_t size_ = 0;
    uint32_t num_elements_ = 0;
    void* parent_ = nullptr;
    uint32_t type_ = 0;
    int32_t count_ = 1;
    static const Slice* type_emulator(uint32_t, uint32_t);
    static void* semantics_for(const void*, void*, void*, void*, void*, bool);
    friend class Alligator; friend class PotentialSlice;
    friend class SliceMap; friend class SliceQueue;
    friend struct SliceStatics;
    template<typename T> friend class SliceT;
    template<typename T> friend class PotentialSliceT;
    template<typename T> friend class SliceMapT;
    template<typename T> friend class SliceQueueT;
};
static_assert(sizeof(Slice) == 32, "A Slice must be 32 bytes in size.");
static_assert(alignof(Slice) == 32, "A Slice is 32-byte aligned, with the device address first.");
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
class ConcurrentBitplane;
/** --------------------------------------------------------------------------------------------------------- Arena
 * @class Alligator
 * @brief The "Allocator" manages memory allocation and ownership for Slice claims.
 */
class Alligator {
private:
    static ChainBuffet* current_for(
        const BuffetDescriptor* desc,
        ChainBuffet** expected = nullptr,
        ChainBuffet* desired = nullptr
    );
    static void destroy(Slice& root);
    static void finalize();
    Alligator();
    ~Alligator();
    friend class Slice; friend class Memory;
    friend class ChainBuffet; friend class Kitchen;
    friend struct KitchenInitializer;
public:
    Alligator(const Alligator&) = delete;
    Alligator& operator=(const Alligator&) = delete;
    Alligator(Alligator&&) = delete;
    Alligator& operator=(Alligator&&) = delete;
    /** ------------------------------------------------------------------------------------------- Claim
     * @brief The one way to get a Slice: a 64-byte aligned cut from the placement's current link, or a
     * dedicated buffet for a novel or slab-sized request.
     * @param size The size in bytes, rounded up to 64 bytes; zero claims nothing.
     * @param novel Whether the Slice gets a dedicated buffet.
     * @param placement The placement to claim from.
     * @param type The SliceTypes record index the slot carries, zero for raw bytes.
     * @return The claimed Slice.
     */
    static Slice claim(
        uint32_t size,
        bool novel = false,
        const BuffetDescriptor* placement = Slice::default_placement()
    );
    /** ------------------------------------------------------------------------------------------- Buffet Of
     * @brief The buffet object behind a Slice, the pointer its placement's static hooks take.
     * @param slice The Slice.
     * @return The buffet object.
     */
    static void* buffet_of(const Slice& slice);
    /** ------------------------------------------------------------------------------------------- Instance
     * @brief Retrieves the singleton instance of the Alligator.
     * @return Reference to the Alligator instance.
     */
    static Alligator& inst();
    /** ------------------------------------------------------------------------------------------- For Type
     * @brief Retrieves the placement descriptor for the given type index.
     * @param type The type index.
     * @return The placement descriptor for the type.
     */
    static const BuffetDescriptor* for_type(uint32_t type);
};
/** --------------------------------------------------------------------------------------------------------- Claim Typed
 * @brief One T's worth of bytes from the default placement, in a claim whose table runs ~T ahead of the release.
 * @param novel_buffer Whether the claim gets a dedicated buffet.
 * @return The claim.
 */
template <typename T>
Slice SliceT<T>::claim_typed(bool novel_buffer) {
    static_assert(alignof(T) <= 64, "A typed claim is SIMD aligned; a T aligned past 64 bytes is not supported yet");
    /// T's table leads with the type word its claims carry.
    static const uint32_t type = *static_cast<const uint32_t*>(Slice::semantics_for(
        &typeid(T), nullptr, nullptr, nullptr,
        reinterpret_cast<void*>(+[](void* payload) { static_cast<T*>(payload)->~T(); }), true
    ));
    /// A T with a destructor needs its buffer to itself, so the last handle on it is the one that runs ~T.
    return Alligator::claim(sizeof(T), novel_buffer || !std::is_trivially_destructible_v<T>,
        Alligator::for_type(type));
}
} // namespace buffetalligator