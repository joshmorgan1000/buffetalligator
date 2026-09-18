#pragma once
/** --------------------------------------------------------------------------------------------------------- Slice
 * @file alligator.hpp
 * @brief Unified header for the BuffetAlligator.
 * (was supposed to be "buffer allocator" but voice-to-text got it wrong and it stuck)
 */
#include <logging.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <concepts>
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
#include <iomanip>
#include <iostream>
#include <openssl/rand.h>
#include <openssl/sha.h>
#if defined(BUFFETALLIGATOR_HAS_VULKAN)
#include <vulkan/vulkan.h>
#endif
#if defined(BUFFETALLIGATOR_HAS_CUDA)
#include <cuda.h>
#endif

namespace buffetalligator {
class Alligator; class Buffet; class BuffetMenu; class Slice;
class SliceFriend; class Memory; class SliceQueue; class SliceChannel;
EXCEPTION_CLASS(Alligator)
#define ALLIGATOR_THROW(msg) throw AlligatorException(msg)
/** --------------------------------------------------------------------------------------------------------- Host Memory Usage
 * @struct HostMemoryUsage
 * @brief Reports physical capacity, estimated available memory, and this process's resident bytes.
 */
struct HostMemoryUsage {
    uint64_t physical_bytes;
    uint64_t available_bytes;
    uint64_t resident_bytes;
};
/** --------------------------------------------------------------------------------------------------------- Placemat
 * @class Placemat
 * @brief A substrate for memory allocation within the BuffetAlligator framework.
 */
class Placemat {
public:
    /** ------------------------------------------------------------------------------------------- Handle
     * @struct Handle
     * @brief Opaque handle representing an allocation within a Placemat.
     */
    struct Handle {
        /// @brief The buffet instance associated with this allocation.
        void* substrate_handle = nullptr;
        /// @brief The special context associated with this allocation.
        void* context = nullptr;
    };
    /** ------------------------------------------------------------------------------------------- Get for Slice
     * @brief Returns the handle associated with a given Slice.
     * @param slice The slice for which to retrieve the handle.
     * @return The handle associated with the slice, or nullptr if not found.
     */
    static Handle* get_for(const Slice* slice);
    /** ------------------------------------------------------------------------------------------- Type
     * @brief Returns the registry identifier assigned to this placement.
     * @return The placement registry identifier.
     */
    uint16_t type() const { return type_; }
    /** ------------------------------------------------------------------------------------------- Name
     * @brief Returns the name of the placement.
     * @return The placement name.
     */
    const char* name() const { return name_; }
    /** ------------------------------------------------------------------------------------------- Accessors */
    void* alligator() { return reinterpret_cast<void*>(alligator_); }
    void* deallocate() { return reinterpret_cast<void*>(deallocate_); }
    void* get_host_ptr() { return reinterpret_cast<void*>(get_host_ptr_); }
    void* get_context() { return reinterpret_cast<void*>(get_context_); }
    /** ------------------------------------------------------------------------------------------- No copy/move */
    Placemat(const Placemat&) = delete;
    Placemat& operator=(const Placemat&) = delete;
    Placemat(Placemat&&) = delete;
    Placemat& operator=(Placemat&&) = delete;
    ~Placemat() = default;
private:
    const char* name_ = nullptr;
    Handle* (*alligator_)(size_t size, void* context);
    void (*deallocate_)(Handle* handle, void* context);
    void* (*get_host_ptr_)(Handle* handle);
    void* (*get_context_)();
    uint16_t type_ = 0;
    uint16_t bump_alignment_ = 64;
    uint32_t default_slab_size_ = 0;
    Placemat() = default;
    friend class BuffetMenu;
    friend class Buffet;
    friend class Alligator;
    friend class Memory;
};
/** --------------------------------------------------------------------------------------------------------- BuffetTypeRegistry
 * @class BuffetTypeRegistry
 * @brief Startup registry assigning one stable type to each Placemat instance.
 */
class BuffetMenu {
public:
    /** ------------------------------------------------------------------------------------------- Memory Usage
     * @brief Queries host memory statistics on macOS and Linux outside the allocation path.
     * @return Physical capacity, estimated available memory, and process residency in bytes.
     */
    static HostMemoryUsage memory_usage();
    /** ------------------------------------------------------------------------------------------- Register Type
     * @brief Registers one process-lifetime Placemat before the first Slice is created.
     * @param placement_factory The factory instance to register.
     * @return The stable placement identifier.
     */
    static uint16_t register_type(
        const char* name,
        size_t default_slab_size,
        size_t bump_alignment,
        Placemat::Handle* (*alligator)(size_t size, void* context),
        void (*deallocate)(Placemat::Handle* handle, void* context),
        void* (*get_host_ptr)(Placemat::Handle* handle),
        void* (*get_context)(),
        bool set_as_default = false
    ) {
        if (!instance().builtins_ready_.load(std::memory_order_acquire)) {
            ensure_builtins_slow();
        }
        return register_type_unchecked(
            name, default_slab_size, bump_alignment, alligator, deallocate, get_host_ptr,
            get_context, set_as_default
        );
    }
    /** ------------------------------------------------------------------------------------------- Get
     * @brief Returns the registered Placemat for an identifier.
     * @param type The placement identifier.
     * @return The registered placement factory.
     */
    static Placemat* get(uint16_t type) {
        if (!instance().builtins_ready_.load(std::memory_order_acquire)) {
            ensure_builtins_slow();
        }
        return instance().placements_.at(type).get();
    }
    /** ------------------------------------------------------------------------------------------- Get by Name
     * @brief Returns the registered Placemat for a given name.
     * @param name The name of the placement.
     * @return The registered placement factory, or nullptr if not found.
     */
    static Placemat* get(const std::string& name) {
        if (!instance().builtins_ready_.load(std::memory_order_acquire)) {
            ensure_builtins_slow();
        }
        auto it = instance().placement_indices_.find(name);
        if (it != instance().placement_indices_.end()) {
            return instance().placements_.at(it->second).get();
        }
        return nullptr;
    }
    /** ------------------------------------------------------------------------------------------- Count
     * @brief Returns the number of Placemat types registered so far.
     * @return The registered placement count.
     */
    static size_t count() {
        if (!instance().builtins_ready_.load(std::memory_order_acquire)) {
            ensure_builtins_slow();
        }
        return instance().placements_.size();
    }
    /** ------------------------------------------------------------------------------------------- Default Placement
     * @brief Returns the default Placemat instance, registering the built-ins first if needed.
     * @return The default Placemat pointer.
     */
    static const Placemat*& default_placement() {
        if (!instance().builtins_ready_.load(std::memory_order_acquire)) {
            ensure_builtins_slow();
        }
        return default_placement_slot();
    }
    /** ------------------------------------------------------------------------------------------- Shutdown
     * @brief Stops channel reactors at application quiescence before the process exits.
     */
    static void shutdown();
    /** ------------------------------------------------------------------------------------------- Register Change Listener
     * @brief Registers a change listener that will be called when certain events occur.
     * @param context The context pointer to be passed to the callback.
     * @param callback The callback function to be invoked.
     */
    static void register_change_listener(
        void* context,
        void (*callback)(void*)
    ) {
        instance().change_listeners_.emplace_back(context, callback);
    }
    /** ------------------------------------------------------------------------------------------- No copy/move */
    BuffetMenu(const BuffetMenu&) = delete;
    BuffetMenu& operator=(const BuffetMenu&) = delete;
    BuffetMenu(BuffetMenu&&) = delete;
    BuffetMenu& operator=(BuffetMenu&&) = delete;
    ~BuffetMenu() = default;
private:
    /// @brief Vector of unique pointers to all registered Placemat instances.
    std::vector<std::unique_ptr<Placemat>> placements_;
    /// @brief Mapping from placement names to their corresponding indices in the placements_ vector.
    std::unordered_map<std::string, size_t> placement_indices_;
    /// @brief List of registered change listeners along with their context pointers.
    std::vector<std::pair<void*, void (*)(void*)>> change_listeners_;
    /// @brief Set while the built-in placements are being registered.
    std::atomic<bool> builtins_claimed_{false};
    /// @brief Set once the built-in placements hold their stable identifiers.
    std::atomic<bool> builtins_ready_{false};
    /** ------------------------------------------------------------------------------------------- Ensure Builtins Slow
     * @brief Slow path that registers the built-in placements; defined in the library.
     */
    static void ensure_builtins_slow();
    /** ------------------------------------------------------------------------------------------- Default Placement Slot
     * @brief The storage behind default_placement(), reachable without the built-in check.
     * @return The default Placemat pointer slot.
     */
    static const Placemat*& default_placement_slot() {
        static const Placemat* default_placement = nullptr;
        return default_placement;
    }
    /** ------------------------------------------------------------------------------------------- Register Type Unchecked
     * @brief Registers one Placemat without the built-in check; the slow path uses this to
     * give the built-ins their stable identifiers.
     * @return The stable placement identifier.
     */
    static uint16_t register_type_unchecked(
        const char* name,
        size_t default_slab_size,
        size_t bump_alignment,
        Placemat::Handle* (*alligator)(size_t size, void* context),
        void (*deallocate)(Placemat::Handle* handle, void* context),
        void* (*get_host_ptr)(Placemat::Handle* handle),
        void* (*get_context)(),
        bool set_as_default
    ) {
        if (default_slab_size < 64 * 1024 * 1024) default_slab_size = 64 * 1024 * 1024;
        auto& inst = instance();
        uint16_t type = static_cast<uint16_t>(inst.placements_.size());
        auto placement = std::unique_ptr<Placemat>(new Placemat());
        placement->name_ = name;
        placement->alligator_ = alligator;
        placement->default_slab_size_ = default_slab_size >> 12;
        placement->bump_alignment_ = bump_alignment;
        placement->deallocate_ = deallocate;
        placement->get_host_ptr_ = get_host_ptr;
        placement->get_context_ = get_context;
        placement->type_ = type;
        if (set_as_default || default_placement_slot() == nullptr) {
            default_placement_slot() = placement.get();
        }
        inst.placement_indices_.emplace(name, type);
        inst.placements_.emplace_back(std::move(placement));
        inst.notify_change_listeners();
        return type;
    }
    /** ------------------------------------------------------------------------------------------- Notify Change Listeners
     * @brief Notifies all registered change listeners by invoking their callbacks with the
     * provided context.
     */
    void notify_change_listeners() {
        for (auto& listener : change_listeners_) {
            listener.second(listener.first);
        }
    }
    BuffetMenu() = default;
    static BuffetMenu& instance() {
        static BuffetMenu instance;
        return instance;
    }
    friend class Memory;
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
            { t.placement() } -> std::same_as<const Placemat*>;
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
 * All operations in the buffet alligator are performed on slices of memory, represented by the `Slice` class.
 * 
 * Memory slices are claims from pre-allocated memory buffers that are managed by the buffet alligator's slab arena. On
 * systems that support unified memory, they are always sliced from host-coherent GPU buffers. For systems
 * that do not support unified memory, transfers between host and device memory are handled automatically
 * by the buffet alligator.
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
    /** ------------------------------------------------------------------------------------------- Default Placement
     * @brief Returns the default placement for slices, which is determined by the system's
     * capabilities.
     * @return The default `Placement` enum value for slices.
     */
    static const Placemat* default_placement();
    /** ------------------------------------------------------------------------------------------- Constructor - Default
     * @brief Constructs a null slice with no underlying memory with an unspecified placement.
     */
    Slice() = default;
    /** ------------------------------------------------------------------------------------------- Constructor - Fresh Claim
     * @brief Claims a slice of pre-allocated memory in the buffet alligator's slab arena. The slice is
     * guaranteed to be zero-initialized.
     * @param size The size of the slice in bytes.
     */
    Slice(size_t size, const Placemat* placement = default_placement());
    /** ------------------------------------------------------------------------------------------- Constructor - Fresh Claim
     * @brief Claims a slice of pre-allocated memory in the buffet alligator's slab arena, with the option to
     * specify whether the slice should be part of a larger slab or a novel buffer. The slice is
     * guaranteed to be zero-initialized.
     * @param size The size of the slice in bytes.
     * @param novel_buffer If true, then the slice is allocated as a novel buffer instead of being
     * a claim of a pre-allocated slab. This is ideal for slices that are long-lived to help
     * reduce fragmentation in the arena.
     */
    Slice(size_t size, bool novel_buffer, const Placemat* placement = default_placement());
    /** ------------------------------------------------------------------------------------------- Constructor - Copy from External Memory
     * @brief Copies data from an external memory location into a new slice of memory in the buffet alligator.
     * This can be used to deep-copy a slice, or load data from an external source into the buffet alligator's
     * memory management system.
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
        const Placemat* placement = default_placement()
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
    const Placemat* placement() const;
    /** ------------------------------------------------------------------------------------------- Raw accessors
     * @brief Use the buffet alligator's internal memory arena system to resolve the slice's host-writable
     * pointer to the underlying memory.
     */
    void* raw() { return cached_; }
    /** ------------------------------------------------------------------------------------------- Raw accessors - const
     * @brief Use the buffet alligator's internal memory arena system to resolve the slice's host-writable
     * pointer to the underlying memory, but as a read-only pointer.
     */
    const void* raw() const { return cached_; }
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
    /** ------------------------------------------------------------------------------------------- Size in bytes
     * @brief Returns the size of the slice in bytes.
     * @return The size of the slice in bytes.
     */
    size_t size_bytes() const {
        return meta_ == SIZE_MAX ? 0 : (meta_ >> 17) & SIZE_MAX;
    }
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
        const Placemat* placement = default_placement()
    );
    /** ------------------------------------------------------------------------------------------- Check if slice is null
     * @brief Checks if the slice is null (i.e., has no underlying memory).
     * @return True if the slice is null, false otherwise.
     */
    bool is_null() const { return meta_ == UINT64_MAX; }
    /** ------------------------------------------------------------------------------------------- Check if slice is valid
     * @brief Checks if the slice is valid (i.e., has underlying memory).
     * @return True if the slice is valid, false otherwise.
     */
    bool valid() const { return !is_null(); }
    /** ------------------------------------------------------------------------------------------- Conversion to bool
     * @brief Allows the slice to be used in boolean contexts.
     * @return True if the slice is valid, false if it is null.
     */
    operator bool() const { return !is_null(); }
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
private:
    /// @brief All-ones marks the null slice; otherwise [0..16] arena ID, [17..63] byte-exact size.
    uint64_t meta_ = UINT64_MAX;
    /// @brief Cached host pointer to the slice's first byte.
    void* cached_ = nullptr;
    friend class Buffet;
    friend class Placemat;
    friend class SliceQueue;
    friend struct SliceNetworkAccess;
};
static_assert(sizeof(Slice) == 16, "Slice must be 16 bytes in size.");
/** --------------------------------------------------------------------------------------------------------- PotentialSlice
 * @class PotentialSlice
 * @brief A slice that gets filled lazily by the provided fulfillment method.
 */
class PotentialSlice {
private:
    struct Details;
    /// @brief Shared pointer to the internal details of the PotentialSlice.
    std::shared_ptr<Details> details_ = std::make_shared<Details>();
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
    /** ------------------------------------------------------------------------------------------- Placemat
     * @brief Returns the memory placement type of the slice.
     * @return The `Placemat` enum value representing the slice's memory placement.
     */
    const Placemat* placement() const;
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
EXCEPTION_CLASS(Atomic)
#define ATOMIC_THROW(msg) throw AtomicException(msg)
/** --------------------------------------------------------------------------------------------------------- Concepts
 * @brief This class uses type erasure and pointers in order to store
 * any type T inside an atomic container. Some types have native atomic
 * support for specific operations (like fetch_add etc) so we track those
 * in order to take advantage of them where possible.
 */
template<typename T>
concept NativeAtomicType = std::is_same_v<std::remove_cvref_t<T>, bool> ||
    std::is_same_v<std::remove_cvref_t<T>, int8_t> ||
    std::is_same_v<std::remove_cvref_t<T>, uint8_t> ||
    std::is_same_v<std::remove_cvref_t<T>, int16_t> ||
    std::is_same_v<std::remove_cvref_t<T>, uint16_t> ||
    std::is_same_v<std::remove_cvref_t<T>, int32_t> ||
    std::is_same_v<std::remove_cvref_t<T>, uint32_t> ||
    std::is_same_v<std::remove_cvref_t<T>, int64_t> ||
    std::is_same_v<std::remove_cvref_t<T>, uint64_t> ||
    std::is_same_v<std::remove_cvref_t<T>, float> ||
    std::is_same_v<std::remove_cvref_t<T>, double> ||
    std::is_pointer_v<std::remove_cvref_t<T>>;
/** --------------------------------------------------------------------------------------------------------- CanDoAddition
 * @brief A concept that checks if a type T supports addition operations.
 */
template<typename T>
concept CanDoAddition = requires(T a, T b) { { a + b } -> std::convertible_to<T>; };
/** --------------------------------------------------------------------------------------------------------- CanDoSubtraction
 * @brief A concept that checks if a type T supports subtraction operations.
 */
template<typename T>
concept CanDoSubtraction = requires(T a, T b) { { a - b } -> std::convertible_to<T>; };
/** --------------------------------------------------------------------------------------------------------- AllowableAtomicType
 * @brief We don't allow for void pointer types, because we have to store these
 * as a void pointer internally, which could leave dangling pointers too easily
 * if we were to allow them
 */
template<typename T>
concept AllowableAtomicType = (!(std::is_pointer_v<T>
    && std::is_same_v<std::remove_cvref_t<T>, void>) &&
    !std::is_same_v<std::remove_cvref_t<T>, void>);
/** --------------------------------------------------------------------------------------------------------- CanDoBitOperations
 * @brief Combined bitwise-support detector unique to this header.
 */
template<typename T>
concept CanDoBitOperations = requires(T a, T b) { { a & b } -> std::convertible_to<T>;
    { a | b } -> std::convertible_to<T>; { a ^ b } -> std::convertible_to<T>; };
/** --------------------------------------------------------------------------------------------------------- Spin backoff calculation
 * @brief Calculate backoff threshold based on a time budget.
 * @param budget The time budget for backoff in nanoseconds.
 * @return The calculated backoff threshold as an integer.
 */
inline static int64_t backoff_threshold_for(std::chrono::nanoseconds budget) noexcept {
    constexpr long double r = 1.0006933869703014L;
    long double factor = (r - 1.0L) / r;
    long double T = factor * static_cast<long double>(budget.count());
    return static_cast<int64_t>(std::ceil(T));
}
/** --------------------------------------------------------------------------------------------------------- AtomicMutex
 * @class AtomicMutex
 * 
 * @brief Simple atomic mutex implementation for thread-safe access
 * to shared resources without using std::mutex.
 * In most cases this will outperform std::mutex, but it is a spinlock
 * so it may not be suitable for all use-cases.
 */
class AtomicMutex {
private:
    /// @brief Even/odd counter for readers/writers
    std::unique_ptr<std::atomic<int64_t>> even_odd_ =
        std::make_unique<std::atomic<int64_t>>(0);
    /// @brief Number of writers waiting for the lock
    std::unique_ptr<std::atomic<int64_t>> writers_waiting_ =
        std::make_unique<std::atomic<int64_t>>(0);
public:
    /// @brief Default constructor.
    AtomicMutex() = default;
    /// @brief Copy constructor.
    AtomicMutex(const AtomicMutex& other) = delete;
    /// @brief Copy assignment operator. 
    AtomicMutex& operator=(const AtomicMutex& other) = delete;
    /// @brief Move constructor.
    AtomicMutex(AtomicMutex&& other) noexcept = default;
    /// @brief Move assignment operator.
    AtomicMutex& operator=(AtomicMutex&& other) noexcept = default;
    /// @brief Destructor. No special cleanup needed due to use of smart pointers.
    ~AtomicMutex() = default;
    /** --------------------------------------------------------------------------------- Exclusive Lock
     * @brief Locks the mutex for exclusive write access.
     * This will block until the mutex is acquired. No other readers or writers
     * will be able to access the resource until this lock() is granted and then
     * subsequently unlocked.
     */
    void lock() {
        int64_t val = even_odd_->fetch_or(1, std::memory_order_acquire);
        // Spin until we have exclusive access (no readers or writers) (val == 0)
        const uint64_t backoff_threshold = backoff_threshold_for(std::chrono::nanoseconds(1000000)); // 1ms
        const uint64_t giveup_threshold = backoff_threshold * 20000; // 20s
        uint64_t backoff_counter = 0;
        writers_waiting_->fetch_add(1, std::memory_order_release);
        while (val & 1) {
            // Progressive backoff
            if (backoff_counter++ < backoff_threshold) {
                val = even_odd_->load(std::memory_order_acquire);
            } else {
                std::this_thread::yield();
                val = even_odd_->load(std::memory_order_acquire);
                if (backoff_counter > giveup_threshold) [[unlikely]] {
                    ATOMIC_THROW("AtomicMutex::lock(): Timeout waiting for exclusive lock");
                }
            }
            if (!(val & 1)) {
                val = even_odd_->fetch_or(1, std::memory_order_acquire);
            }
        }
        while (even_odd_->load(std::memory_order_acquire) != 1) {
            // Wait for readers to drain
            if (backoff_counter++ < backoff_threshold) {
                // Just spin
            } else {
                std::this_thread::yield();
                if (backoff_counter > giveup_threshold) [[unlikely]] {
                    ATOMIC_THROW("AtomicMutex::lock(): Timeout waiting for readers to drain");
                }
            }
        }
        writers_waiting_->fetch_sub(1, std::memory_order_release);
    }
    /** --------------------------------------------------------------------------------- Unlock
     * @brief Unlocks the mutex.
     */
    void unlock() {
        even_odd_->fetch_add(-1, std::memory_order_release);
    }
    /** --------------------------------------------------------------------------------- Is Locked
     * @brief Checks if the mutex is currently locked for writing.
     * @return True if the mutex is locked for writing, false otherwise.
     */
    bool is_locked() const {
        return ((even_odd_->load(std::memory_order_acquire) & 1) != 0)
                || (writers_waiting_->load(std::memory_order_acquire) > 0);
    }
    /** --------------------------------------------------------------------------------- Shared Lock
     * @brief Locks the mutex for shared read access.
     * This will block until the mutex is acquired. Other readers may also
     * access the resource while this lock is held, but writers will be blocked.
     */
    void lock_shared() {
        const uint64_t backoff_threshold = backoff_threshold_for(std::chrono::nanoseconds(1000000));
        const uint64_t giveup_threshold = backoff_threshold * 20000;
        uint64_t backoff_counter = 0;
        while (true) {
            // Wait for writers to clear before even trying
            while (writers_waiting_->load(std::memory_order_acquire) > 0) {
                if (backoff_counter++ < backoff_threshold) { /* spin */ }
                else {
                    std::this_thread::yield();
                }
            }
            int64_t val = even_odd_->fetch_add(2, std::memory_order_acquire);
            if (!(val & 1)) {
                return;  // Success — no writer held the lock
            }
            // Writer snuck in, undo and retry
            even_odd_->fetch_sub(2, std::memory_order_release);
            if (backoff_counter++ < backoff_threshold) { /* spin */ }
            else {
                std::this_thread::yield();
                if (backoff_counter > giveup_threshold) [[unlikely]] {
                    ATOMIC_THROW("AtomicMutex::lock_shared(): Timeout waiting for shared lock");
                }
            }
        }
    }
    /** --------------------------------------------------------------------------------- Shared Unlock
     * @brief Unlocks the mutex from shared read access.
     */
    void unlock_shared() {
        even_odd_->fetch_sub(2, std::memory_order_release);
    }
};
// Method signature for deleter functions so they can be stored statically
using DeleterMethod = void(*)(void*);
using NotifyOneMethod = void(*)(void*);
using NotifyAllMethod = void(*)(void*);
class AtomicRegistry;
/** --------------------------------------------------------------------------------------------------------- TypeInfoContainer
 * @struct TypeInfoContainer
 * 
 * @brief Container for type information and capabilities of a specific type.
 * Used mostly by AtomicContainer to manage type-erased storage.
 */
struct TypeInfoContainer {
    const std::type_info& type_info_;               // Type information
    bool is_native_atomic_ = false;                 // Whether type has native atomic support
    bool can_do_addition_ = false;                  // Whether type supports addition
    bool can_do_subtraction_ = false;               // Whether type supports subtraction
    bool can_do_bit_operations_ = false;            // Whether type supports bit operations
    bool is_pointer_ = false;                       // Whether type is a pointer
    DeleterMethod deleter_method_ = nullptr;        // Method to delete owned storage
    NotifyOneMethod notify_one_method_ = nullptr;   // Method to notify one waiter
    NotifyAllMethod notify_all_method_ = nullptr;   // Method to notify all waiters
    /** --------------------------------------------------------------------------------- Create
     * @brief Create a TypeInfoContainer for the specified type U.
     * @tparam U The type to create the container for.
     * @return Pointer to the created TypeInfoContainer.
     */
    template<typename U>
    static TypeInfoContainer* create() {
        auto* container = new TypeInfoContainer(typeid(U));
        if constexpr (NativeAtomicType<U>) {
            container->is_native_atomic_ = true;
        }
        if constexpr (CanDoAddition<U>) {
            container->can_do_addition_ = true;
        }
        if constexpr (CanDoSubtraction<U>) {
            container->can_do_subtraction_ = true;
        }
        if constexpr (CanDoBitOperations<U>) {
            container->can_do_bit_operations_ = true;
        }
        if constexpr (std::is_pointer_v<U>) {
            container->is_pointer_ = true;
        }
        // Always install a deleter for the owned storage object.
        // For native/pointer types, we will allocate std::atomic<U>.
        // For emulated types, we will allocate U.
        container->deleter_method_ = [](void* ptr) {
            if constexpr (NativeAtomicType<U> || std::is_pointer_v<U>) {
                delete static_cast<std::atomic<U>*>(ptr);
            } else {
                delete static_cast<U*>(ptr);
            }
        };
        // For native atomic types and pointer types, set up notify methods
        if constexpr (NativeAtomicType<U> || std::is_pointer_v<U>) {
            container->notify_one_method_ = [](void* ptr) {
                auto* atomic_ptr = static_cast<std::atomic<U>*>(ptr);
                atomic_ptr->notify_one();
            };
            container->notify_all_method_ = [](void* ptr) {
                auto* atomic_ptr = static_cast<std::atomic<U>*>(ptr);
                atomic_ptr->notify_all();
            };
        }
        return container;
    }
private:
    /** --------------------------------------------------------------------------------- Constructor
     * @brief Private constructor to enforce use of create() method.
     * @param ti The type information for the type.
     */
    TypeInfoContainer(const std::type_info& ti) : type_info_(ti) {}
};
/** --------------------------------------------------------------------------------------------------------- Fetch Modify Signature
 * @brief Method signature for fetch_modify methods
 * that modify the underlying value in place.
 */
template<typename T, typename... Args>
using FetchModifyMethodSignature = T(*)(T&&,Args...);
/** --------------------------------------------------------------------------------------------------------- AtomicContainer
 * @class AtomicContainer
 * 
 * @brief A type-erased container for concrete types as well as pointers to types that uses type
 * erasure so that we can store a collection of owned atomic values of different types in a single
 * container. For types that are not able to be turned into native atomics, locks are used to
 * emulate atomic behavior. We use an AtomicMutex for locking, which uses atomic operations and
 * progressive backoff to minimize contention.
 * 
 * We've also added a `fetch_modify` method that works much like fetch_add etc, but allows for
 * in-place modification of non-atomic types by "borrowing" a pointer to the underlying value,
 * modifying it, and then returning it. This allows for atomic modification of complex types
 * that don't have native atomic support.
 */
class AtomicContainer {
private:
    /// @brief Type information for the stored type T
    std::unique_ptr<TypeInfoContainer> type_info_;
    /// @brief Pointer to the stored value (either std::atomic<T> or T*)
    void* ptr_ = nullptr;
    /// @brief Atomic pointer used for borrow/return_borrowed operations
    std::unique_ptr<std::atomic<void*>> atomic_ = nullptr;
    /// @brief Backoff threshold for borrow operations
    const uint64_t throw_ =
            backoff_threshold_for(std::chrono::nanoseconds(std::chrono::seconds(60)));
    /** ------------------------------------------------------------------------------------------- Return Borrowed
     * @brief Publishes borrowed storage when an operation completes or throws.
     */
    struct ReturnBorrowed {
        std::atomic<void*>* slot;
        /** --------------------------------------------------------------------------------- Release
         * @brief Releases the value to the next acquiring borrower.
         */
        template<typename Value>
        void operator()(Value* value) const noexcept {
            slot->store(value, std::memory_order_release);
        }
    };
    //-------------- Allow AtomicRegistry to access private members ----------------//
    friend class AtomicRegistry;
public:
    /** --------------------------------------------------------------------------------- Constructor
     * @brief Construct an AtomicContainer with an initial value.
     * This is the ONLY way to create an AtomicContainer - must have a value.
     * AtomicRegistry creates these and hands out raw pointers.
     * @param value The initial value to store in the atomic container.
     */
    template<typename T>
        requires (AllowableAtomicType<T> && !NativeAtomicType<T>
            && !std::is_pointer_v<std::remove_cvref_t<T>>)
    AtomicContainer(T&& value) {
        using Value = std::remove_cvref_t<T>;
        type_info_.reset(TypeInfoContainer::create<Value>());
        auto* a = new Value(std::forward<T>(value));
        ptr_ = static_cast<void*>(a);
        atomic_ = std::make_unique<std::atomic<void*>>(static_cast<void*>(ptr_));
    }
    /** --------------------------------------------------------------------------------- Constructor for Native Atomics
     * @brief Construct an AtomicContainer with an initial value for types that
     * have native atomic support. This constructor is optimized to directly create
     * a std::atomic<T> for types that support it.
     * @param value The initial value to store in the atomic container.
     */
    template<typename T>
        requires (NativeAtomicType<T> && !std::is_pointer_v<std::remove_cvref_t<T>>)
    AtomicContainer(T&& value) {
        using Value = std::remove_cvref_t<T>;
        type_info_.reset(TypeInfoContainer::create<Value>());
        auto* a = new std::atomic<Value>(std::forward<T>(value));
        ptr_ = static_cast<void*>(a);
        atomic_ = std::make_unique<std::atomic<void*>>(static_cast<void*>(ptr_));
    }
    /** --------------------------------------------------------------------------------- Constructor for Pointer Types
     * @brief Construct an AtomicContainer with an initial value for pointer types.
     * This constructor is optimized to directly create a std::atomic<T>
     * where T is a pointer type.
     * @param value The initial value to store in the atomic container.
     */
    template<typename T>
        requires (AllowableAtomicType<T> && std::is_pointer_v<std::remove_cvref_t<T>>)
    AtomicContainer(T&& value) {
        using Value = std::remove_cvref_t<T>;
        type_info_.reset(TypeInfoContainer::create<Value>());
        auto* a = new std::atomic<Value>(std::forward<T>(value));
        ptr_ = static_cast<void*>(a);
        atomic_ = std::make_unique<std::atomic<void*>>(static_cast<void*>(ptr_));
    }
    /** --------------------------------------------------------------------------------- Constructor from Unique Ptr
     * @brief Construct an AtomicContainer from a unique pointer to the value.
     * This is used for pointer types and complex types that need to be heap allocated.
     * @param value_ptr Unique pointer to the initial value to store in the atomic container.
     */
    template<typename T>
        requires (AllowableAtomicType<T> && !std::is_pointer_v<T>)
    AtomicContainer(std::unique_ptr<T>&& value_ptr) {
        type_info_.reset(TypeInfoContainer::create<T>());
        if constexpr (NativeAtomicType<T>) {
            auto* a = new std::atomic<T>(*(value_ptr.get()));
            ptr_ = static_cast<void*>(a);
            value_ptr.reset();
        } else {
            ptr_ = value_ptr.release();
        }
        atomic_ = std::make_unique<std::atomic<void*>>(static_cast<void*>(ptr_));
    }
    /** --------------------------------------------------------------------------------- Borrow
     * @brief "Borrows" a reference pointer to the underlying value T from the
     * atomic container.
     * This operation will block until the value can be borrowed.
     * The borrowed value must be returned using return_borrowed() when done.
     * These are used by the "fetch_modify" methods that allow in-place atomic
     * modifications.
     * @return A reference to the underlying value T.
     */
    template<typename T>
        requires AllowableAtomicType<T>
    T* borrow() const {
        if (typeid(T) != type_info_->type_info_) [[unlikely]] {
            std::string errormsg ="AtomicContainer::borrow(): Type"
                " mismatch when borrowing from AtomicContainer."
                " Requested: " + std::string(typeid(T).name()) + ", Actual: " +
                    std::string(type_info_->type_info_.name());
            ATOMIC_THROW(errormsg);
        }
        int64_t loop_counter = 1000;
        while (true) {
            void* expected = atomic_->load(std::memory_order_acquire);
            if (expected == nullptr) {
                // Currently borrowed by someone else; back off
                if (--loop_counter < 0) {
                    std::this_thread::yield();
                    if (loop_counter < -1000) {
                        uint64_t wait_cnt = std::pow(2, (-loop_counter) / 1000);
                        std::this_thread::sleep_for(std::chrono::nanoseconds(wait_cnt));
                        if (wait_cnt > throw_) {
                            ATOMIC_THROW("AtomicContainer::borrow(): Timeout waiting to borrow"
                                " value from AtomicContainer. This is likely due to a deadlock or"
                                " livelock condition.");
                        }
                    }
                }
                continue;
            }
            // Try to swap expected with nullptr to mark as borrowed
            if (atomic_->compare_exchange_weak(
                expected, nullptr, std::memory_order_acquire, std::memory_order_relaxed)
            ) {
                // Success: expected holds the pointer we borrowed
                return static_cast<T*>(expected);
            }
            // CAS failed spuriously or contention; back off and retry
            if (--loop_counter < 0) {
                std::this_thread::yield();
                if (loop_counter < -1000) {
                    uint64_t wait_cnt = std::pow(2, (-loop_counter) / 1000);
                    std::this_thread::sleep_for(std::chrono::nanoseconds(wait_cnt));
                    if (wait_cnt > throw_) {
                        ATOMIC_THROW("AtomicContainer::borrow(): Timeout waiting to borrow"
                            " value from AtomicContainer. This is likely due to a deadlock or"
                            " livelock condition.");
                    }
                }
            }
        }
    }
    /** --------------------------------------------------------------------------------- Return Borrowed
     * @brief Returns a previously borrowed value back to the atomic container,
     * allowing other operations to proceed.
     * @param value_ptr The pointer to return.
     */
    template<typename T>
        requires AllowableAtomicType<T>
    void return_borrowed(T* value_ptr) {
        if (typeid(T) != type_info_->type_info_) [[unlikely]] {
            std::string errormsg = "AtomicContainer::return_borrowed(): Type"
                " mismatch when returning borrowed value to AtomicContainer."
                " Requested: " + std::string(typeid(T).name()) + ", Actual: " +
                    std::string(type_info_->type_info_.name());
            ATOMIC_THROW(errormsg);
        }
        void* expected = nullptr;
        if (!atomic_->compare_exchange_strong(expected, static_cast<void*>(value_ptr),
            std::memory_order_release, std::memory_order_relaxed)) [[unlikely]] {
            ATOMIC_THROW("AtomicContainer::return_borrowed(): Attempt to return"
                " borrowed value to AtomicContainer when it was not borrowed.");
        }
    }
    //-------------- Disable Copy and Move ----------------//
    AtomicContainer() = delete;
    AtomicContainer(const AtomicContainer& other) = delete;
    AtomicContainer& operator=(const AtomicContainer& other) = delete;
    AtomicContainer(AtomicContainer&& other) = delete;
    AtomicContainer& operator=(AtomicContainer&& other) = delete;
    /* --------------------------------------------------------------------------------- Destructor
     * @brief Destructor - cleans up owned storage.
     */
    ~AtomicContainer() {
        type_info_->deleter_method_(ptr_);
    }
    /** --------------------------------------------------------------------------------- Direct Pointer Access
     * @brief Get a direct pointer to the underlying object of type T.
     * This is intended for containers constructed with T (not T*). The
     * AtomicContainer owns the lifetime of the pointed-to object and will
     * delete it when destroyed.
     * @param order The memory order to use for the load operation (default is acquire).
     * @return A pointer to the underlying object of type T.
     */
    template<typename T>
        requires (AllowableAtomicType<T> && !std::is_pointer_v<T>)
    T* ptr(std::memory_order order = std::memory_order_acquire) const {
        if (typeid(T) != type_info_->type_info_) [[unlikely]] {
            std::string errormsg = "AtomicContainer::ptr(): Type"
                " mismatch when accessing pointer from AtomicContainer."
                " Requested: " + std::string(typeid(T).name()) + ", Actual: " +
                    std::string(type_info_->type_info_.name());
            ATOMIC_THROW(errormsg);
        }
        return static_cast<T*>(atomic_->load(order));
    }
    /** --------------------------------------------------------------------------------- Load
     * @brief Atomically load the underlying value T from the atomic container.
     * If the container is empty (no underlying atomic), this will throw an exception.
     * @param order The memory order to use for the load operation
     * (default is sequentially consistent).
     * @return The value stored in the atomic container.
     */
    template<typename T>
        requires (AllowableAtomicType<T> && !std::is_pointer_v<T>)
    const T load(std::memory_order order = std::memory_order_seq_cst) const {
        if (typeid(T) != type_info_->type_info_) [[unlikely]] {
            std::string errormsg ="AtomicContainer::load(): Type"
                " mismatch when loading from AtomicContainer."
                " Requested: " + std::string(typeid(T).name()) + ", Actual: " +
                    std::string(type_info_->type_info_.name());
            ATOMIC_THROW(errormsg);
        }
        if constexpr (NativeAtomicType<T>) {
            auto* a = static_cast<std::atomic<T>*>(atomic_->load(order));
            if (a == nullptr) [[unlikely]] {
                ATOMIC_THROW("AtomicContainer::load(): Attempt to load from an empty container");
            }
            return a->load(order);
        } else {
            std::unique_ptr<T, ReturnBorrowed> value_ptr(borrow<T>(), {atomic_.get()});
            return *value_ptr;
        }
    }
    /** ---------------------------------------------------------------------------------- Load Pointer
     * @brief Atomically load the underlying pointer T from the atomic container,
     * for pointer types. If the container is empty (no underlying atomic), this will
     * throw an exception.
     * @param order The memory order to use for the load operation (default is
     * sequentially consistent).
     * @return The pointer stored in the atomic container.
     */
    template<typename T>
        requires std::is_pointer_v<T>
    T load(std::memory_order order = std::memory_order_seq_cst) const {
        if (typeid(T) != type_info_->type_info_) [[unlikely]] {
            std::string errormsg ="AtomicContainer::load(): Type"
                " mismatch when loading from AtomicContainer."
                " Requested: " + std::string(typeid(T).name()) + ", Actual: " +
                    std::string(type_info_->type_info_.name());
            ATOMIC_THROW(errormsg);
        }
        auto* a = static_cast<std::atomic<T>*>(atomic_->load(order));
        if (a == nullptr) [[unlikely]] {
            ATOMIC_THROW("AtomicContainer::load(): Attempt to load from an empty container");
        }
        return a->load(order);
    }
    /** --------------------------------------------------------------------------------- Store
     * @brief Atomically store a new value into the atomic container.
     * @param value The new value to store.
     * @param order The memory order to use for the store operation
     * (default is sequentially consistent).
     */
    template<typename T>
        requires AllowableAtomicType<T>
    void store(T&& value, std::memory_order order = std::memory_order_seq_cst) {
        using Value = std::remove_cvref_t<T>;
        if (typeid(T) != type_info_->type_info_) [[unlikely]] {
            std::string errormsg = "AtomicContainer::store(): Type"
                " mismatch when storing to AtomicContainer."
                " Requested: " + std::string(typeid(T).name()) + ", Actual: " +
                    std::string(type_info_->type_info_.name());
            ATOMIC_THROW(errormsg);
        }
        if constexpr (NativeAtomicType<Value>) {
            auto* a = static_cast<std::atomic<Value>*>(atomic_->load(std::memory_order_acquire));
            if (a == nullptr) [[unlikely]] {
                ATOMIC_THROW("AtomicContainer::store(): Attempt to store to an empty container");
            }
            a->store(std::forward<T>(value), order);
        } else {
            std::unique_ptr<Value, ReturnBorrowed> value_ptr(borrow<Value>(), {atomic_.get()});
            *value_ptr = std::forward<T>(value);
        }
    }
    /** --------------------------------------------------------------------------------- Store (const ref)
     * @brief Atomically store a new value into the atomic container
     * (const reference version).
     * @param value The new value to store.
     * @param order The memory order to use for the store operation
     * (default is sequentially consistent).
     */
    template<typename T>
        requires AllowableAtomicType<T>
    void store(const T& value, std::memory_order order = std::memory_order_seq_cst) {
        if (typeid(T) != type_info_->type_info_) [[unlikely]] {
            std::string errormsg = "AtomicContainer::store(): Type"
                " mismatch when storing to AtomicContainer."
                " Requested: " + std::string(typeid(T).name()) + ", Actual: " +
                    std::string(type_info_->type_info_.name());
            ATOMIC_THROW(errormsg);
        }
        if constexpr (NativeAtomicType<T> || std::is_pointer_v<T>) {
            auto* a = static_cast<std::atomic<T>*>(atomic_->load(std::memory_order_acquire));
            if (a == nullptr) [[unlikely]] {
                ATOMIC_THROW("AtomicContainer::store(): Attempt to store to an empty container");
            }
            a->store(value, order);
        } else {
            std::unique_ptr<T, ReturnBorrowed> value_ptr(borrow<T>(), {atomic_.get()});
            *value_ptr = value;
        }
    }
    /** --------------------------------------------------------------------------------- Exchange
     * @brief Atomically exchange the value with a new value, returning the old value.
     * @param value The new value to set.
     * @param order The memory order to use for the exchange operation
     * (default is sequentially consistent).
     * @return The old value that was replaced.
     */
    template<typename T>
        requires AllowableAtomicType<T>
    std::remove_cvref_t<T> exchange(T&& value, std::memory_order order = std::memory_order_seq_cst) {
        using Value = std::remove_cvref_t<T>;
        if (typeid(T) != type_info_->type_info_) [[unlikely]] {
            std::string errormsg = "AtomicContainer::exchange(): Type"
                " mismatch when exchanging value in AtomicContainer."
                " Requested: " + std::string(typeid(T).name()) + ", Actual: " +
                    std::string(type_info_->type_info_.name());
            ATOMIC_THROW(errormsg);
        }
        if constexpr (NativeAtomicType<Value>) {
            auto* a = static_cast<std::atomic<Value>*>(atomic_->load(std::memory_order_acquire));
            if (a == nullptr) {
                ATOMIC_THROW("AtomicContainer::exchange(): Attempt to exchange on an empty container");
            }
            return a->exchange(std::forward<T>(value), order);
        } else {
            std::unique_ptr<Value, ReturnBorrowed> value_ptr(borrow<Value>(), {atomic_.get()});
            Value old_value = std::move(*value_ptr);
            *value_ptr = std::forward<T>(value);
            return old_value;
        }
    }
    /** --------------------------------------------------------------------------------- Compare and Exchange Strong
     * @brief Atomically compare and exchange: if current value equals
     * expected, replace with desired.
     * @param expected Reference to the expected value
     * (updated with actual value on failure).
     * @param desired The new value to store if expected matches.
     * @param success_order Memory order for successful exchange.
     * @param failure_order Memory order for failed exchange.
     * @return true if exchange succeeded, false otherwise.
     */
    template<typename T>
        requires AllowableAtomicType<T>
    bool compare_exchange_strong(
        T& expected,
        T desired,
        std::memory_order success_order,
        std::memory_order failure_order
    ) {
        if (typeid(T) != type_info_->type_info_) [[unlikely]] {
            std::string errormsg = "AtomicContainer::compare_exchange_strong(): Type"
                " mismatch when comparing/exchanging in AtomicContainer."
                " Requested: " + std::string(typeid(T).name()) + ", Actual: " +
                    std::string(type_info_->type_info_.name());
            ATOMIC_THROW(errormsg);
        }
        if constexpr (NativeAtomicType<T> || std::is_pointer_v<T>) {
            auto* a = static_cast<std::atomic<T>*>(atomic_->load(std::memory_order_acquire));
            if (a == nullptr) [[unlikely]] {
                ATOMIC_THROW("AtomicContainer::compare_exchange_strong(): Attempt on empty container");
            }
            return a->compare_exchange_strong(expected, desired, success_order, failure_order);
        } else {
            std::unique_ptr<T, ReturnBorrowed> value_ptr(borrow<T>(), {atomic_.get()});
            bool result = false;
            if (*value_ptr == expected) {
                *value_ptr = std::move(desired);
                result = true;
            } else {
                expected = *value_ptr;
                result = false;
            }
            return result;
        }
    }
    /** --------------------------------------------------------------------------------- Compare and Exchange Weak
     * @brief Atomically compare and exchange (weak version): may spuriously fail.
     * @param expected Reference to the expected value
     * (updated with actual value on failure).
     * @param desired The new value to store if expected matches.
     * @param success_order Memory order for successful exchange.
     * @param failure_order Memory order for failed exchange.
     * @return true if exchange succeeded, false otherwise (or spurious failure).
     */
    template<typename T>
        requires AllowableAtomicType<T>
    bool compare_exchange_weak(
        T& expected,
        T desired,
        std::memory_order success_order,
        std::memory_order failure_order
    ) {
        if (typeid(T) != type_info_->type_info_) [[unlikely]] {
            std::string errormsg = "AtomicContainer::compare_exchange_weak(): Type"
                " mismatch when comparing/exchanging in AtomicContainer."
                " Requested: " + std::string(typeid(T).name()) + ", Actual: " +
                    std::string(type_info_->type_info_.name());
            ATOMIC_THROW(errormsg);
        }
        if constexpr (NativeAtomicType<T> || std::is_pointer_v<T>) {
            auto* a = static_cast<std::atomic<T>*>(atomic_->load(std::memory_order_acquire));
            if (a == nullptr) [[unlikely]] {
                ATOMIC_THROW("AtomicContainer::compare_exchange_weak(): Attempt on empty container");
            }
            return a->compare_exchange_weak(expected, desired, success_order, failure_order);
        } else {
            // For non-native types, weak version behaves same as strong
            return compare_exchange_strong(expected, desired, success_order, failure_order);
        }
    }
    /** --------------------------------------------------------------------------------- Fetch Add
     * @brief Atomically add a value and return the previous value.
     * @param arg The value to add.
     * @param order The memory order to use (default is sequentially consistent).
     * @return The value before addition.
     */
    template<typename T>
        requires AllowableAtomicType<T> && CanDoAddition<T>
    T fetch_add(T arg, std::memory_order order = std::memory_order_seq_cst) {
        if (typeid(T) != type_info_->type_info_) [[unlikely]] {
            std::string errormsg = "AtomicContainer::fetch_add(): Type"
                " mismatch in AtomicContainer."
                " Requested: " + std::string(typeid(T).name()) + ", Actual: " +
                    std::string(type_info_->type_info_.name());
            ATOMIC_THROW(errormsg);
        }
        if constexpr (NativeAtomicType<T> && (std::is_integral_v<T> || std::is_floating_point_v<T>)) {
            auto* a = static_cast<std::atomic<T>*>(atomic_->load(std::memory_order_acquire));
            if (a == nullptr) [[unlikely]] {
                ATOMIC_THROW("AtomicContainer::fetch_add(): Attempt on empty container");
            }
            return a->fetch_add(arg, order);
        } else {
            std::unique_ptr<T, ReturnBorrowed> value_ptr(borrow<T>(), {atomic_.get()});
            T old_value = *value_ptr;
            *value_ptr = *value_ptr + arg;
            return old_value;
        }
    }
    /** --------------------------------------------------------------------------------- Fetch Sub
     * @brief Atomically subtract a value and return the previous value.
     * @param arg The value to subtract.
     * @param order The memory order to use (default is sequentially consistent).
     * @return The value before subtraction.
     */
    template<typename T>
        requires AllowableAtomicType<T> && CanDoSubtraction<T>
    T fetch_sub(T arg, std::memory_order order = std::memory_order_seq_cst) {
        if (typeid(T) != type_info_->type_info_) [[unlikely]] {
            std::string errormsg = "AtomicContainer::fetch_sub(): Type"
                " mismatch in AtomicContainer."
                " Requested: " + std::string(typeid(T).name()) + ", Actual: " +
                    std::string(type_info_->type_info_.name());
            ATOMIC_THROW(errormsg);
        }
        if constexpr (NativeAtomicType<T> && (std::is_integral_v<T> || std::is_floating_point_v<T>)) {
            auto* a = static_cast<std::atomic<T>*>(atomic_->load(std::memory_order_acquire));
            if (a == nullptr) [[unlikely]] {
                ATOMIC_THROW("AtomicContainer::fetch_sub(): Attempt on empty container");
            }
            return a->fetch_sub(arg, order);
        } else {
            std::unique_ptr<T, ReturnBorrowed> value_ptr(borrow<T>(), {atomic_.get()});
            T old_value = *value_ptr;
            *value_ptr = *value_ptr - arg;
            return old_value;
        }
    }
    /** --------------------------------------------------------------------------------- Fetch And
     * @brief Atomically perform bitwise AND and return the previous value.
     * @param arg The value to AND with.
     * @param order The memory order to use (default is sequentially consistent).
     * @return The value before the operation.
     */
    template<typename T>
        requires AllowableAtomicType<T> && CanDoBitOperations<T>
    T fetch_and(T arg, std::memory_order order = std::memory_order_seq_cst) {
        if (typeid(T) != type_info_->type_info_) [[unlikely]] {
            std::string errormsg = "AtomicContainer::fetch_and(): Type"
                " mismatch in AtomicContainer."
                " Requested: " + std::string(typeid(T).name()) + ", Actual: " +
                    std::string(type_info_->type_info_.name());
            ATOMIC_THROW(errormsg);
        }
        if constexpr (NativeAtomicType<T> && std::is_integral_v<T>) {
            auto* a = static_cast<std::atomic<T>*>(atomic_->load(std::memory_order_acquire));
            if (a == nullptr) [[unlikely]] {
                ATOMIC_THROW("AtomicContainer::fetch_and(): Attempt on empty container");
            }
            return a->fetch_and(arg, order);
        } else {
            std::unique_ptr<T, ReturnBorrowed> value_ptr(borrow<T>(), {atomic_.get()});
            T old_value = *value_ptr;
            *value_ptr = *value_ptr & arg;
            return old_value;
        }
    }
    /** --------------------------------------------------------------------------------- Fetch Or
     * @brief Atomically perform bitwise OR and return the previous value.
     * @param arg The value to OR with.
     * @param order The memory order to use (default is sequentially consistent).
     * @return The value before the operation.
     */
    template<typename T>
        requires AllowableAtomicType<T> && CanDoBitOperations<T>
    T fetch_or(T arg, std::memory_order order = std::memory_order_seq_cst) {
        if (typeid(T) != type_info_->type_info_) [[unlikely]] {
            std::string errormsg = "AtomicContainer::fetch_or(): Type"
                " mismatch in AtomicContainer."
                " Requested: " + std::string(typeid(T).name()) + ", Actual: " +
                    std::string(type_info_->type_info_.name());
            ATOMIC_THROW(errormsg);
        }
        if constexpr (NativeAtomicType<T> && std::is_integral_v<T>) {
            auto* a = static_cast<std::atomic<T>*>(atomic_->load(std::memory_order_acquire));
            if (a == nullptr) [[unlikely]] {
                ATOMIC_THROW("AtomicContainer::fetch_or(): Attempt on empty container");
            }
            return a->fetch_or(arg, order);
        } else {
            std::unique_ptr<T, ReturnBorrowed> value_ptr(borrow<T>(), {atomic_.get()});
            T old_value = *value_ptr;
            *value_ptr = *value_ptr | arg;
            return old_value;
        }
    }
    /** --------------------------------------------------------------------------------- Fetch Xor
     * @brief Atomically perform bitwise XOR and return the previous value.
     * @param arg The value to XOR with.
     * @param order The memory order to use (default is sequentially consistent).
     * @return The value before the operation.
     */
    template<typename T>
        requires AllowableAtomicType<T> && CanDoBitOperations<T>
    T fetch_xor(T arg, std::memory_order order = std::memory_order_seq_cst) {
        if (typeid(T) != type_info_->type_info_) [[unlikely]] {
            std::string errormsg = "AtomicContainer::fetch_xor(): Type"
                " mismatch in AtomicContainer."
                " Requested: " + std::string(typeid(T).name()) + ", Actual: " +
                    std::string(type_info_->type_info_.name());
            ATOMIC_THROW(errormsg);
        }
        if constexpr (NativeAtomicType<T> && std::is_integral_v<T>) {
            auto* a = static_cast<std::atomic<T>*>(atomic_->load(std::memory_order_acquire));
            if (a == nullptr) [[unlikely]] {
                ATOMIC_THROW("AtomicContainer::fetch_xor(): Attempt on empty container");
            }
            return a->fetch_xor(arg, order);
        } else {
            std::unique_ptr<T, ReturnBorrowed> value_ptr(borrow<T>(), {atomic_.get()});
            T old_value = *value_ptr;
            *value_ptr = *value_ptr ^ arg;
            return old_value;
        }
    }
    /** --------------------------------------------------------------------------------- Fetch Modify
     * @brief Atomically modifies the value through a function accepting T&& and returning T.
     * @param func Modification function, which may be retried for contended native atomics.
     * @param order The memory order to use.
     * @param args Additional arguments to pass to the function.
     * @return The old value before modification.
     */
    template<typename T, typename... Args>
        requires AllowableAtomicType<T>
    T fetch_modify(
        FetchModifyMethodSignature<T, Args...> func,
        std::memory_order order,
        Args... args
    ) {
        if (typeid(T) != type_info_->type_info_) [[unlikely]] {
            std::string errormsg = "AtomicContainer::fetch_modify(): Type"
                " mismatch in AtomicContainer."
                " Requested: " + std::string(typeid(T).name()) + ", Actual: " +
                    std::string(type_info_->type_info_.name());
            ATOMIC_THROW(errormsg);
        }
        if constexpr (NativeAtomicType<T>) {
            auto* value = static_cast<std::atomic<T>*>(ptr_);
            T expected = value->load(std::memory_order_relaxed);
            do {
                T current = expected;
                T desired = func(std::move(current), args...);
                if (value->compare_exchange_weak(expected, desired, order,
                    std::memory_order_relaxed)) return expected;
            } while (true);
        } else {
            std::unique_ptr<T, ReturnBorrowed> value_ptr(borrow<T>(), {atomic_.get()});
            T old_value = *value_ptr;
            *value_ptr = func(std::move(*value_ptr), args...);
            return old_value;
        }
    }
    /** --------------------------------------------------------------------------------- Wait
     * @brief Blocks until the atomic value is not equal to the old value.
     * Works for both native atomic types and pointer types.
     * @param old The value to wait for change from.
     * @param order The memory order to use (default is sequentially consistent).
     */
    template<typename T>
        requires AllowableAtomicType<T>
    void wait(T old, std::memory_order order = std::memory_order_seq_cst) const {
        if (typeid(T) != type_info_->type_info_) [[unlikely]] {
            std::string errormsg = "AtomicContainer::wait(): Type"
                " mismatch in AtomicContainer."
                " Requested: " + std::string(typeid(T).name()) + ", Actual: " +
                    std::string(type_info_->type_info_.name());
            ATOMIC_THROW(errormsg);
        }
        if constexpr (NativeAtomicType<T> || std::is_pointer_v<T>) {
            // For native atomics and pointers, use actual atomic wait
            auto* a = static_cast<std::atomic<T>*>(atomic_->load(std::memory_order_acquire));
            if (a == nullptr) [[unlikely]] {
                ATOMIC_THROW("AtomicContainer::wait(): Attempt on empty container");
            }
            a->wait(old, order);
        } else {
            // For non-native atomics, busy-wait with yield
            while (true) {
                T current = load<T>(order);
                if (current != old) break;
                std::this_thread::yield();
            }
        }
    }
    /** --------------------------------------------------------------------------------- Notify One
     * @brief Notifies one thread waiting on this atomic.
     * Uses stored type information to call the appropriate atomic notify_one().
     */
    void notify_one() {
        if (type_info_ == nullptr || atomic_ == nullptr) [[unlikely]] {
            ATOMIC_THROW("AtomicContainer::notify_one(): Cannot notify on empty container");
        }
        if (type_info_->notify_one_method_ != nullptr) {
            void* value_ptr = atomic_->load(std::memory_order_acquire);
            if (value_ptr != nullptr) [[likely]] {
                type_info_->notify_one_method_(value_ptr);
            }
        }
    }
    /** --------------------------------------------------------------------------------- Notify All
     * @brief Notifies all threads waiting on this atomic.
     * Uses stored type information to call the appropriate atomic notify_all().
     */
    void notify_all() {
        if (type_info_ == nullptr || atomic_ == nullptr) [[unlikely]] {
            ATOMIC_THROW("AtomicContainer::notify_all(): Cannot notify on empty container");
        }
        if (type_info_->notify_all_method_ != nullptr) {
            void* value_ptr = atomic_->load(std::memory_order_acquire);
            if (value_ptr != nullptr) [[likely]] {
                type_info_->notify_all_method_(value_ptr);
            }
        }
    }
    /** --------------------------------------------------------------------------------- Type Info
     * @brief Get the type_info of the stored type.
     * @return Reference to the type_info of the stored type.
     */
    const std::type_info& type() const {
        if (type_info_ == nullptr) [[unlikely]] {
            ATOMIC_THROW("AtomicContainer::type(): Cannot get type of empty container");
        }
        return type_info_->type_info_;
    }
    /** --------------------------------------------------------------------------------- Is Type
     * @brief Check if the stored type matches the type of the provided example.
     * @param example An example value of the type to check against.
     * @return true if the types match, false otherwise.
     */
    template<typename T>
    bool is_type(const T&) const {
        return (typeid(T) == type_info_->type_info_);
    }
    /** --------------------------------------------------------------------------------- Is Type (no arg)
     * @brief Templated version to check if the stored type matches T.
     * @return true if the types match, false otherwise.
     */
    template<typename T>
    bool is_type() const {
        return (typeid(T) == type_info_->type_info_);
    }
    /** --------------------------------------------------------------------------------- Is Pointer
     * @brief Check if the stored type is a pointer type.
     * @return true if pointer type, false otherwise.
     */
    bool is_pointer() const noexcept {
        return type_info_ != nullptr && type_info_->is_pointer_;
    }
    /** --------------------------------------------------------------------------------- Is Native Atomic
     * @brief Check if the stored type is a native atomic type.
     * @return true if native atomic, false otherwise.
     */
    bool is_native_atomic() const noexcept {
        return type_info_ != nullptr && type_info_->is_native_atomic_;
    }
    /** --------------------------------------------------------------------------------- To JSON
     * @brief Serialize the atomic container to JSON.
     * For native atomic types, includes the current value.
     * For pointer types, indicates it's a pointer.
     * For complex types, indicates it's complex.
     * @return A JSON representation of the atomic container.
     */
    std::string to_json() const {
        std::string s = "{";
        s += "\"type\":\"" + threadsafe_logger::json_escape(type_info_->type_info_.name()) + "\"";
        if (is_pointer()) {
            s += ",\"value\":\"pointer type\"";
        } else if (is_native_atomic()) {
            if (type_info_->type_info_ == typeid(int)) {
                s += ",\"value\":" + std::to_string(load<int>());
            } else if (type_info_->type_info_ == typeid(long)) {
                s += ",\"value\":" + std::to_string(load<long>());
            } else if (type_info_->type_info_ == typeid(float)) {
                s += ",\"value\":" + std::to_string(load<float>());
            } else if (type_info_->type_info_ == typeid(double)) {
                s += ",\"value\":" + std::to_string(load<double>());
            } else if (type_info_->type_info_ == typeid(bool)) {
                s += std::string(",\"value\":") + (load<bool>() ? "true" : "false");
            } else if (type_info_->type_info_ == typeid(uint64_t)) {
                s += ",\"value\":" + std::to_string(load<uint64_t>());
            } else if (type_info_->type_info_ == typeid(uint32_t)) {
                s += ",\"value\":" + std::to_string(load<uint32_t>());
            } else if (type_info_->type_info_ == typeid(int64_t)) {
                s += ",\"value\":" + std::to_string(load<int64_t>());
            } else if (type_info_->type_info_ == typeid(int32_t)) {
                s += ",\"value\":" + std::to_string(load<int32_t>());
            } else {
                s += ",\"value\":\"unsupported native atomic type\"";
            }
        } else {
            s += ",\"value\":\"complex type\"";
        }
        s += "}";
        return s;
    }
    /** --------------------------------------------------------------------------------- From JSON
     * @brief Deserialize an AtomicContainer from JSON.
     * Supports only native atomic types for deserialization.
     * @param j The JSON object to deserialize from.
     * @return A unique pointer to the created AtomicContainer.
     */
    template<typename JsonElement>
    static std::unique_ptr<AtomicContainer> from_json(JsonElement j) {
        std::string_view type_sv;
        if (j["type"].get_string().get(type_sv)) {
            ATOMIC_THROW("AtomicContainer::from_json(): JSON does not contain 'type' field");
        }
        std::string type_name(type_sv);
        if (type_name == typeid(int).name()) {
            int64_t value = 0;
            (void)j["value"].get_int64().get(value);
            int v = static_cast<int>(value);
            return std::make_unique<AtomicContainer>(std::move(v));
        } else if (type_name == typeid(long).name()) {
            int64_t value = 0;
            (void)j["value"].get_int64().get(value);
            long v = static_cast<long>(value);
            return std::make_unique<AtomicContainer>(std::move(v));
        } else if (type_name == typeid(float).name()) {
            double value = 0.0;
            (void)j["value"].get_double().get(value);
            float v = static_cast<float>(value);
            return std::make_unique<AtomicContainer>(std::move(v));
        } else if (type_name == typeid(double).name()) {
            double value = 0.0;
            (void)j["value"].get_double().get(value);
            return std::make_unique<AtomicContainer>(std::move(value));
        } else if (type_name == typeid(bool).name()) {
            bool value = false;
            (void)j["value"].get_bool().get(value);
            return std::make_unique<AtomicContainer>(std::move(value));
        } else if (type_name == typeid(uint64_t).name()) {
            uint64_t value = 0;
            (void)j["value"].get_uint64().get(value);
            return std::make_unique<AtomicContainer>(std::move(value));
        } else if (type_name == typeid(uint32_t).name()) {
            uint64_t value = 0;
            (void)j["value"].get_uint64().get(value);
            uint32_t v = static_cast<uint32_t>(value);
            return std::make_unique<AtomicContainer>(std::move(v));
        } else if (type_name == typeid(int64_t).name()) {
            int64_t value = 0;
            (void)j["value"].get_int64().get(value);
            return std::make_unique<AtomicContainer>(std::move(value));
        } else if (type_name == typeid(int32_t).name()) {
            int64_t value = 0;
            (void)j["value"].get_int64().get(value);
            int32_t v = static_cast<int32_t>(value);
            return std::make_unique<AtomicContainer>(std::move(v));
        }
        return nullptr;
    }
};
/** --------------------------------------------------------------------------------------------------------- Atomic Registry
 * @class AtomicRegistry
 * 
 * @brief A thread-safe registry for storing and managing AtomicContainer instances
 * identified by string keys.
 * This allows us to establish a clear chain of ownership for values that need to be thread-safe.
 */
class AtomicRegistry {
private:
    /// @brief The registry mapping string keys to AtomicContainer instances
    std::unique_ptr<std::unordered_map<std::string, std::unique_ptr<AtomicContainer>>> registry_;
    /// @brief Mutex to protect access to the registry
    std::unique_ptr<AtomicMutex> gate_;
    /** --------------------------------------------------------------------------------- Global Registry
     * @brief Returns a pointer to the global registry of AtomicContainer instances.
     * This is a static method that provides access to a shared registry across all
     * instances of AtomicRegistry.
     * @return Pointer to the global registry map.
     */
    static std::unordered_map<std::string, std::unique_ptr<AtomicContainer>>* global_registry() {
        static std::unordered_map<std::string, std::unique_ptr<AtomicContainer>> registry;
        return &registry;
    }
    /** --------------------------------------------------------------------------------- Global Gate
     * @brief Returns a reference to the global mutex used to protect access to the
     * global registry. This ensures thread-safe operations on the global registry.
     * @return Reference to the global AtomicMutex.
     */
    static AtomicMutex& global_gate() {
        static AtomicMutex gate;
        return gate;
    }
public:
    /** --------------------------------------------------------------------------------- Constructor
     * @brief Constructs an empty AtomicRegistry.
     */
    AtomicRegistry() {
        registry_ = std::make_unique<std::unordered_map<std::string, std::unique_ptr<AtomicContainer>>>();
        gate_ = std::make_unique<AtomicMutex>();
    }
    //----------------------------------------------------------------------------------- Move Only Semantics
    AtomicRegistry(AtomicRegistry& other) = delete;
    AtomicRegistry& operator=(const AtomicRegistry& other) = delete;
    AtomicRegistry(AtomicRegistry&& other) noexcept = default;
    AtomicRegistry& operator=(AtomicRegistry&& other) noexcept = default;
    ~AtomicRegistry() = default;
    /** --------------------------------------------------------------------------------- Create
     * @brief Creates a new atomic value of the specified type, stores
     * it in the registry, and then returns an AtomicContainer that can
     * be used to access it.
     * @param key The key to associate with the atomic value in the registry.
     * @param initial_value The initial value to store in the atomic container.
     * @returns Pointer to the AtomicContainer in the registry.
     */
    template<typename T>
        requires std::is_move_constructible_v<T>
    AtomicContainer* create(const std::string& key, T&& initial_value) {
        std::lock_guard<AtomicMutex> lock(*gate_);
        if (registry_->find(key) != registry_->end()) {
            THROW("AtomicRegistry::create(): Key already exists: " + key);
        }
        std::unique_ptr<AtomicContainer> container =
                std::make_unique<AtomicContainer>(std::forward<T>(initial_value));
        AtomicContainer* ptr = container.get();
        registry_->emplace(key, std::move(container));
        return ptr;
    }
    /** --------------------------------------------------------------------------------- Create Global
     * @brief Creates a new atomic value of the specified type in the global registry,
     * stores it, and then returns an AtomicContainer that can be used to access it.
     * This is a static method that allows for shared access across all instances of
     * AtomicRegistry.
     * @param key The key to associate with the atomic value in the global registry.
     * @param initial_value The initial value to store in the atomic container.
     * @returns Pointer to the AtomicContainer in the global registry.
     */
    template<typename T>
        requires std::is_move_constructible_v<T>
    static AtomicContainer* create_global(const std::string& key, T&& initial_value) {
        auto* registry = global_registry();
        std::lock_guard<AtomicMutex> lock(global_gate());
        if (registry->find(key) != registry->end()) {
            THROW("AtomicRegistry::create_global(): Key already exists: " + key);
        }
        std::unique_ptr<AtomicContainer> container =
                std::make_unique<AtomicContainer>(std::forward<T>(initial_value));
        AtomicContainer* ptr = container.get();
        registry->emplace(key, std::move(container));
        return ptr;
    }
    /** --------------------------------------------------------------------------------- Get
     * @brief Retrieves the AtomicContainer associated with the specified key.
     * If the key does not exist, throws an exception.
     * @param key The key of the atomic value to retrieve.
     * @returns Pointer to the AtomicContainer.
     */
    AtomicContainer* get(const std::string& key) {
        gate_->lock_shared();
        auto it = registry_->find(key);
        if (it == registry_->end()) {
            gate_->unlock_shared();
            return nullptr;
        }
        AtomicContainer* result = it->second.get();
        gate_->unlock_shared();
        return result;
    }
    /** --------------------------------------------------------------------------------- Get Global
     * @brief Retrieves the AtomicContainer associated with the specified key from the
     * global registry. If the key does not exist, returns nullptr.
     * @param key The key of the atomic value to retrieve from the global registry.
     * @returns Pointer to the AtomicContainer if found, nullptr otherwise.
     */
    static AtomicContainer* get_global(const std::string& key) {
        auto* registry = global_registry();
        global_gate().lock_shared();
        auto it = registry->find(key);
        if (it == registry->end()) {
            global_gate().unlock_shared();
            return nullptr;
        }
        AtomicContainer* result = it->second.get();
        global_gate().unlock_shared();
        return result;
    }
    /** --------------------------------------------------------------------------------- Get or Create
     * @brief Retrieves the AtomicContainer associated with the specified key,
     * or creates one with the provided initial value if it does not exist.
     */
    template<typename T>
    AtomicContainer* get_or_create(const std::string& key, T&& value) {
        std::lock_guard<AtomicMutex> lock(*gate_);
        auto it = registry_->find(key);
        if (it != registry_->end()) {
            AtomicContainer* result = it->second.get();
            return result;
        }
        std::unique_ptr<AtomicContainer> container =
                std::make_unique<AtomicContainer>(std::forward<T>(value));
        AtomicContainer* ptr = container.get();
        registry_->emplace(key, std::move(container));
        return ptr;
    }
    /** ------------------------------------------------------------------------------------------- Get or Create Global
     * @brief Retrieves the AtomicContainer associated with the specified key from the global
     * registry, or creates one with the provided initial value if it does not exist. This is a
     * static method that allows for shared access across all instances of AtomicRegistry.
     * @param key The key of the atomic value to retrieve or create in the global registry.
     * @param value The initial value to store in the atomic container if it does not exist.
     * @returns Pointer to the AtomicContainer in the global registry.
     */
    template<typename T>
    static AtomicContainer* get_or_create_global(const std::string& key, T&& value) {
        auto* registry = global_registry();
        std::lock_guard<AtomicMutex> lock(global_gate());
        auto it = registry->find(key);
        if (it != registry->end()) {
            AtomicContainer* result = it->second.get();
            return result;
        }
        std::unique_ptr<AtomicContainer> container =
                std::make_unique<AtomicContainer>(std::forward<T>(value));
        AtomicContainer* ptr = container.get();
        registry->emplace(key, std::move(container));
        return ptr;
    }
    /** --------------------------------------------------------------------------------- Remove
     * @brief Removes the AtomicContainer associated with the
     * specified key from the registry.
     * @param key The key of the atomic value to remove
     * @returns true if the key was found and removed, false
     * if it was not found.
     */
    std::unique_ptr<AtomicContainer> remove(const std::string& key) {
        gate_->lock();
        auto it = registry_->find(key);
        if (it == registry_->end()) {
            gate_->unlock();
            return nullptr;
        }
        std::unique_ptr<AtomicContainer> result = std::move(it->second);
        registry_->erase(it);
        gate_->unlock();
        return result;
    }
    /** ------------------------------------------------------------------------------------------- Remove Global
     * @brief Removes the AtomicContainer associated with the specified key from the global
     * registry. This is a static method that allows for shared access across all instances of
     * AtomicRegistry.
     * @param key The key of the atomic value to remove from the global registry.
     * @returns A unique pointer to the removed AtomicContainer if found, nullptr otherwise.
     */
    static std::unique_ptr<AtomicContainer> remove_global(const std::string& key) {
        auto* registry = global_registry();
        global_gate().lock();
        auto it = registry->find(key);
        if (it == registry->end()) {
            global_gate().unlock();
            return nullptr;
        }
        std::unique_ptr<AtomicContainer> result = std::move(it->second);
        registry->erase(it);
        global_gate().unlock();
        return result;
    }
    /** --------------------------------------------------------------------------------- Contains
     * @brief Checks if the registry contains an entry for the specified key.
     * @param key The key to check for
     * @returns true if the key exists in the registry, false otherwise
     */
    bool contains(const std::string& key) {
        gate_->lock_shared();
        bool found = (registry_->find(key) != registry_->end());
        gate_->unlock_shared();
        return found;
    }
    /** ------------------------------------------------------------------------------------------- Contains Global
     * @brief Checks if the global registry contains an entry for the specified key. This is a
     * static method that allows for shared access across all instances of AtomicRegistry.
     * @param key The key to check for in the global registry.
     * @returns true if the key exists in the global registry, false otherwise.
     */
    static bool global_contains(const std::string& key) {
        auto* registry = global_registry();
        global_gate().lock_shared();
        bool found = (registry->find(key) != registry->end());
        global_gate().unlock_shared();
        return found;
    }
    /** --------------------------------------------------------------------------------- Clear
     * @brief Clears all entries from the registry.
     * This will destroy all AtomicContainer instances and free their memory.
     */
    void clear() {
        gate_->lock();
        registry_->clear();
        gate_->unlock();
    }
    /** ------------------------------------------------------------------------------------------- Clear Global
     * @brief Clears all entries from the global registry. This is a static method that allows for
     * shared access across all instances of AtomicRegistry. It will destroy all AtomicContainer
     * instances in the global registry and free their memory.
     */
    static void global_clear() {
        auto* registry = global_registry();
        global_gate().lock();
        registry->clear();
        global_gate().unlock();
    }
    /** --------------------------------------------------------------------------------- Size
     * @brief Returns the number of entries in the registry.
     */
    size_t size() const {
        gate_->lock_shared();
        const size_t s = registry_->size();
        gate_->unlock_shared();
        return s;
    }
    /** ------------------------------------------------------------------------------------------- Size Global
     * @brief Returns the number of entries in the global registry. This is a static method that
     * allows for shared access across all instances of AtomicRegistry.
     * @return The number of entries in the global registry.
     */
    static size_t global_size() {
        auto* registry = global_registry();
        global_gate().lock_shared();
        const size_t s = registry->size();
        global_gate().unlock_shared();
        return s;
    }
    /** --------------------------------------------------------------------------------- Empty
     * @brief Checks if the registry is empty.
     */
    bool empty() const {
        gate_->lock();
        const bool e = registry_->empty();
        gate_->unlock();
        return e;
    }
    /** ------------------------------------------------------------------------------------------- Global Empty
     * @brief Checks if the global registry is empty. This is a static method that allows for
     * shared access across all instances of AtomicRegistry.
     * @return true if the global registry is empty, false otherwise.
     */
    static bool global_empty() {
        auto* registry = global_registry();
        global_gate().lock_shared();
        const bool e = registry->empty();
        global_gate().unlock_shared();
        return e;
    }
    /** --------------------------------------------------------------------------------- Operator []
     * @brief Index operator to access AtomicContainer by key.
     * Throws an exception if the key does not exist.
     */
    AtomicContainer* operator[](const std::string& key) {
        gate_->lock_shared();
        auto it = registry_->find(key);
        if (it == registry_->end()) {
            gate_->unlock_shared();
            ATOMIC_THROW("AtomicRegistry::operator[]: Key not found: " + key);
        }
        AtomicContainer* result = it->second.get();
        gate_->unlock_shared();
        return result;
    }
    /** --------------------------------------------------------------------------------- Operator [] Const
     * @brief Const index operator to access AtomicContainer by key.
     * Throws an exception if the key does not exist.
     */
    const AtomicContainer* operator[](const std::string& key) const {
        gate_->lock_shared();
        auto it = registry_->find(key);
        if (it == registry_->end()) {
            gate_->unlock_shared();
            ATOMIC_THROW("AtomicRegistry::operator[]: Key not found: " + key);
        }
        const AtomicContainer* result = it->second.get();
        gate_->unlock_shared();
        return result;
    }
    /** --------------------------------------------------------------------------------- Keys
     * @brief Returns a vector of all keys currently stored in the registry.
     */
    std::vector<std::string> keys() const {
        gate_->lock_shared();
        std::vector<std::string> result;
        result.reserve(registry_->size());
        for (const auto& pair : *registry_) {
            result.push_back(pair.first);
        }
        gate_->unlock_shared();
        return result;
    }
    /** ------------------------------------------------------------------------------------------- Global Keys
     * @brief Returns a vector of all keys currently stored in the global registry.
     * @return A vector of strings representing the keys in the global registry.
     */
    static std::vector<std::string> global_keys() {
        auto* registry = global_registry();
        global_gate().lock_shared();
        std::vector<std::string> result;
        result.reserve(registry->size());
        for (const auto& pair : *registry) {
            result.push_back(pair.first);
        }
        global_gate().unlock_shared();
        return result;
    }
    /** --------------------------------------------------------------------------------- Select
     * @brief Select multiple entries from the registry by a list of keys.
     * Returns a map of key to AtomicContainer for all keys that were found.
     * @tparam ...Args A parameter pack of string-convertible types
     * @param ...keys The keys to look up
     * @return A map of key to AtomicContainer for all found keys
     */
    template<typename... Args>
        requires (std::convertible_to<Args, std::string> && ...)
    std::unordered_map<std::string, AtomicContainer*> select(Args&&... keys) {
        gate_->lock_shared();
        std::unordered_map<std::string, AtomicContainer*> result;
        auto process_key = [this, &result](const std::string& key) {
            auto it = registry_->find(key);
            if (it != registry_->end()) {
                result.emplace(it->first, it->second.get());
            }
        };
        (process_key(std::forward<Args>(keys)), ...);
        gate_->unlock_shared();
        return result;
    }
    /** ------------------------------------------------------------------------------------------- Select Global
     * @brief Select multiple entries from the global registry by a list of keys. Returns a map of
     * key to AtomicContainer for all keys that were found.
     * @tparam ...Args A parameter pack of string-convertible types
     * @param ...keys The keys to look up in the global registry
     * @return A map of key to AtomicContainer for all found keys in the global registry
     */
    template<typename... Args>
        requires (std::convertible_to<Args, std::string> && ...)
    static std::unordered_map<std::string, AtomicContainer*> select_global(Args&&... keys) {
        auto* registry = global_registry();
        global_gate().lock_shared();
        std::unordered_map<std::string, AtomicContainer*> result;
        auto process_key = [registry, &result](const std::string& key) {
            auto it = registry->find(key);
            if (it != registry->end()) {
                result.emplace(it->first, it->second.get());
            }
        };
        (process_key(std::forward<Args>(keys)), ...);
        global_gate().unlock_shared();
        return result;
    }
    /** --------------------------------------------------------------------------------- Select (vector)
     * @brief Select multiple entries from the registry by a vector of keys.
     * Returns a map of key to AtomicContainer for all keys that were found.
     * @param keys The vector of keys to look up
     * @return A map of key to AtomicContainer for all found keys
     */
    std::unordered_map<std::string, AtomicContainer*> select(
        const std::vector<std::string>& keys
    ) {
        gate_->lock_shared();
        std::unordered_map<std::string, AtomicContainer*> result;
        for (const auto& key : keys) {
            auto it = registry_->find(key);
            if (it != registry_->end()) {
                result.emplace(it->first, it->second.get());
            }
        }
        gate_->unlock_shared();
        return result;
    }
    /** ------------------------------------------------------------------------------------------- Select Global (vector)
     * @brief Select multiple entries from the global registry by a vector of keys. Returns a map
     * of key to AtomicContainer for all keys that were found.
     * @param keys The vector of keys to look up in the global registry
     * @return A map of key to AtomicContainer for all found keys in the global registry
     */
    static std::unordered_map<std::string, AtomicContainer*> select_global(
        const std::vector<std::string>& keys
    ) {
        auto* registry = global_registry();
        global_gate().lock_shared();
        std::unordered_map<std::string, AtomicContainer*> result;
        for (const auto& key : keys) {
            auto it = registry->find(key);
            if (it != registry->end()) {
                result.emplace(it->first, it->second.get());
            }
        }
        global_gate().unlock_shared();
        return result;
    }
    /** --------------------------------------------------------------------------------- List
     * @brief Returns a list of all AtomicContainer pointers in the registry
     * along with their associated keys.
     * @return A map of key to AtomicContainer for all entries in the registry
     */
    std::unordered_map<std::string, AtomicContainer*> list() {
        gate_->lock_shared();
        std::unordered_map<std::string, AtomicContainer*> result;
        for (const auto& pair : *registry_) {
            result.emplace(pair.first, pair.second.get());
        }
        gate_->unlock_shared();
        return result;
    }
    /** ------------------------------------------------------------------------------------------- List Global
     * @brief Returns a list of all AtomicContainer pointers in the global registry along with
     * their associated keys. This is a static method that allows for shared access across all
     * instances of AtomicRegistry.
     * @return A map of key to AtomicContainer for all entries in the global registry
     */
    static std::unordered_map<std::string, AtomicContainer*> list_global() {
        auto* registry = global_registry();
        global_gate().lock_shared();
        std::unordered_map<std::string, AtomicContainer*> result;
        for (const auto& pair : *registry) {
            result.emplace(pair.first, pair.second.get());
        }
        global_gate().unlock_shared();
        return result;
    }
};
/** --------------------------------------------------------------------------------------------------------- Atomic Barrier
 * @class AtomicBarrier
 * @brief A synchronization primitive that allows multiple threads to wait until a
 * certain number of them have reached a common barrier point. This is useful for
 * coordinating not just threads, but remote signals as well
 */
class AtomicBarrier {
private:
    /// @brief Atomic counter to track the number of threads that have reached the barrier
    std::unique_ptr<AtomicContainer> counter_ =
                std::make_unique<AtomicContainer>(static_cast<uint64_t>(0));
    /// @brief Completed phase observed by waiters before entering the next phase.
    std::atomic<uint64_t> generation_{0};
    /// @brief The number of threads that must reach the barrier before they can proceed
    uint64_t wait_for_;
    /** --------------------------------------------------------------------------------- Constructor
     * @brief Private constructor to enforce unique pointer factory method usage.
     */
    AtomicBarrier() = default;
public:
    //----------------------------------------------------------------------------------- Deleted copy/move
    AtomicBarrier(const AtomicBarrier&) = delete;
    AtomicBarrier& operator=(const AtomicBarrier&) = delete;
    AtomicBarrier(AtomicBarrier&&) = delete;
    AtomicBarrier& operator=(AtomicBarrier&&) = delete;
    /** --------------------------------------------------------------------------------- Factory Method
     * @brief Factory method to create an AtomicBarrier with the specified number
     * of threads or signals to wait for.
     * @param wait_for The number of threads or signals that must reach the barrier
     * before they can proceed
     * @return A unique pointer to the created AtomicBarrier
     */
    static std::unique_ptr<AtomicBarrier> create(uint64_t wait_for) {
        AtomicBarrier* barrier = new AtomicBarrier();
        barrier->wait_for_ = wait_for;
        return std::unique_ptr<AtomicBarrier>(barrier);
    }
    /** --------------------------------------------------------------------------------- Wait
     * @brief Waits at the barrier until the specified number of threads or signals
     * have reached it. Once the count is reached, all waiting threads are released.
     */
    void wait() {
        const uint64_t generation = generation_.load(std::memory_order_acquire);
        uint64_t count = counter_->fetch_add<uint64_t>(1, std::memory_order_acq_rel) + 1;
        if (count == wait_for_) {
            notify_all();
        } else {
            generation_.wait(generation, std::memory_order_acquire);
        }
    }
    /** --------------------------------------------------------------------------------- Notify All
     * @brief Notifies all threads waiting at the barrier. This can be used to
     * release waiting threads in response to an external signal, rather than
     * waiting for the count to be reached.
     */
    void notify_all() {
        counter_->store<uint64_t>(0, std::memory_order_relaxed);
        generation_.fetch_add(1, std::memory_order_release);
        generation_.notify_all();
    }
    /** --------------------------------------------------------------------------------- Signal
     * @brief Records one external arrival and releases the phase when its count is reached.
     */
    void signal() {
        const uint64_t count = counter_->fetch_add<uint64_t>(1, std::memory_order_acq_rel) + 1;
        if (count == wait_for_) {
            notify_all();
        }
    }
    /** --------------------------------------------------------------------------------- Reset
     * @brief Resets the barrier to its initial state. This can be used to reuse
     * the same barrier for multiple rounds of synchronization.
     */
    void reset() {
        notify_all();
    }
};
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
    const Placemat* placement() const {
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
/** --------------------------------------------------------------------------------------------------------- Device Memory Usage
 * @struct DeviceMemoryUsage
 * @brief Reports native device measurements with unavailable quantities left empty.
 */
struct DeviceMemoryUsage {
    std::optional<uint64_t> capacity_bytes;
    std::optional<uint64_t> available_bytes;
    std::optional<uint64_t> process_bytes;
    std::optional<uint64_t> budget_bytes;
};
/** --------------------------------------------------------------------------------------------------------- Mmap Allocator
 * @class MmapAllocator
 * @brief Registers disk-backed scratch slabs whose temporary files live until slab reclamation.
 */
class MmapAllocator {
public:
    /** ------------------------------------------------------------------------------------------- Register Type
     * @brief Registers one process-lifetime scratch placement before creating any Slices.
     * @param directory Directory on the filesystem that will back the scratch slabs.
     * @return The registered mmap placement.
     */
    static const Placemat* register_type(const std::string& directory);
    /** ------------------------------------------------------------------------------------------- Enable Spillover
     * @brief Enables automatic heap-to-mmap backing selection before the first Slice is created.
     * @param reserve_bytes Available host RAM to preserve after each new heap allocation.
     * @param resume_bytes Higher available-RAM threshold required to resume heap allocation.
     */
    static void enable_spillover(uint64_t reserve_bytes, uint64_t resume_bytes);
    /** ------------------------------------------------------------------------------------------- Flush
     * @brief Synchronizes the pages covered by a Slice belonging to this placement.
     * @param slice The mmap-backed Slice to synchronize.
     */
    static void flush(const Slice& slice);
    /** ------------------------------------------------------------------------------------------- File Descriptor
     * @brief Returns the borrowed backing-file descriptor for an mmap-backed Slice's slab.
     */
    static int file_descriptor(const Slice& slice);
    /** ------------------------------------------------------------------------------------------- File Offset
     * @brief Returns an mmap-backed Slice's byte offset within its slab's backing file.
     */
    static uint64_t file_offset(const Slice& slice);
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
#if defined(BUFFETALLIGATOR_HAS_METAL)
/** --------------------------------------------------------------------------------------------------------- Metal Allocator
 * @class MetalAllocator
 * @brief Registers shared Metal buffers on a caller-supplied MTLDevice.
 */
class MetalAllocator {
public:
    /** ------------------------------------------------------------------------------------------- Register Type
     * @brief Registers one process-lifetime placement using a bridged id<MTLDevice>.
     */
    static const Placemat* register_type(void* device);
    /** ------------------------------------------------------------------------------------------- Buffer
     * @brief Returns the borrowed id<MTLBuffer> backing a Metal Slice's slab.
     */
    static void* buffer(const Slice& slice);
    /** ------------------------------------------------------------------------------------------- Buffer Offset
     * @brief Returns the byte offset of a Metal Slice within its MTLBuffer.
     */
    static uint64_t buffer_offset(const Slice& slice);
    /** ------------------------------------------------------------------------------------------- Memory Usage
     * @brief Reports currentAllocatedSize and recommendedMaxWorkingSetSize for the device.
     */
    static DeviceMemoryUsage memory_usage();
};
#endif
#if defined(BUFFETALLIGATOR_HAS_VULKAN)
/** --------------------------------------------------------------------------------------------------------- Vulkan Allocator
 * @class VulkanAllocator
 * @brief Allocates mapped coherent slabs on one caller-owned process-lifetime Vulkan device.
 */
class VulkanAllocator {
public:
    /** ------------------------------------------------------------------------------------------- Register Type
     * @brief Registers a Vulkan 1.1-or-newer device after resolving its coherent buffer memory type.
     */
    static const Placemat* register_type(VkPhysicalDevice physical_device, VkDevice device);
    /** ------------------------------------------------------------------------------------------- Buffer
     * @brief Returns the borrowed storage and transfer buffer backing a Vulkan Slice's slab.
     */
    static VkBuffer buffer(const Slice& slice);
    /** ------------------------------------------------------------------------------------------- Buffer Offset
     * @brief Returns a Vulkan Slice's byte offset within its slab buffer.
     */
    static VkDeviceSize buffer_offset(const Slice& slice);
    /** ------------------------------------------------------------------------------------------- Memory Usage
     * @brief Reports the selected heap's capacity and optional EXT_memory_budget measurements.
     */
    static DeviceMemoryUsage memory_usage();
};
#endif
#if defined(BUFFETALLIGATOR_HAS_CUDA)
/** --------------------------------------------------------------------------------------------------------- CUDA Memory Kind
 * @brief Selects managed memory or explicitly mapped pinned host memory.
 */
enum class CudaMemoryKind { managed, mapped_host };
/** --------------------------------------------------------------------------------------------------------- CUDA Allocator
 * @class CudaAllocator
 * @brief Allocates host-accessible slabs using a caller-owned process-lifetime CUDA context.
 */
class CudaAllocator {
public:
    /** ------------------------------------------------------------------------------------------- Register Type
     * @brief Registers one CUDA placement after probing the context's device capabilities.
     */
    static const Placemat* register_type(
        CUcontext context, CudaMemoryKind kind = CudaMemoryKind::managed
    );
    /** ------------------------------------------------------------------------------------------- Device Address
     * @brief Returns the CUDA address of a Slice belonging to this placement.
     */
    static CUdeviceptr device_address(const Slice& slice);
    /** ------------------------------------------------------------------------------------------- Memory Usage
     * @brief Reports device capacity and free bytes from cuMemGetInfo.
     */
    static DeviceMemoryUsage memory_usage();
};
#endif
} // namespace buffetalligator
