#pragma once
/** --------------------------------------------------------------------------------------------------------- Slice
 * @file buffetalligator.hpp
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
#include <concepts>
#include <limits>
#include <memory>
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

namespace buffetalligator {
class Alligator; class Buffet; class BuffetMenu; class Slice; class SliceFriend; class Slice; class Memory;
EXCEPTION_CLASS(Alligator)
#define ALLIGATOR_THROW(msg) throw AlligatorException(msg)
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
        Buffet* buffet = nullptr;
        /// @brief The special context associated with this allocation.
        void* context = nullptr;
    };
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
        if (default_slab_size < 256 * 1024 * 1024) default_slab_size = 256 * 1024 * 1024;
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
        if (set_as_default || default_placement() == nullptr) {
            default_placement() = placement.get();
        }
        inst.placement_indices_.emplace(name, type);
        inst.placements_.emplace_back(std::move(placement));
        inst.notify_change_listeners();
        return type;
    }
    /** ------------------------------------------------------------------------------------------- Get
     * @brief Returns the registered Placemat for an identifier.
     * @param type The placement identifier.
     * @return The registered placement factory.
     */
    static Placemat* get(uint16_t type) {
        return instance().placements_.at(type).get();
    }
    /** ------------------------------------------------------------------------------------------- Count
     * @brief Returns the number of Placemat types registered so far.
     * @return The registered placement count.
     */
    static size_t count() {
        return instance().placements_.size();
    }
    /** ------------------------------------------------------------------------------------------- Default Placement
     * @brief Returns the default Placemat instance.
     * @return The default Placemat pointer.
     */
    static const Placemat*& default_placement() {
        static const Placemat* default_placement = nullptr;
        return default_placement;
    }
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
    std::vector<std::unique_ptr<Placemat>> placements_;
    std::unordered_map<std::string, size_t> placement_indices_;
    std::vector<std::pair<void*, void (*)(void*)>> change_listeners_;
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
        { t.placement() } -> std::same_as<const Placemat*>;
    }
) || std::is_same_v<T, Slice>;
/** --------------------------------------------------------------------------------------------------------- Slice
 * @class Slice
 * @brief Represents a slice of memory in Nebula.
 * 
 * All operations in Nebula are performed on slices of memory, represented by the `Slice` class.
 * 
 * Memory slices are claims from pre-allocated memory buffers that are managed by Nebula's slab arena. On
 * systems that support unified memory, they are always sliced from host-coherent GPU buffers. For systems
 * that do not support unified memory, transfers between host and device memory are handled automatically
 * by Nebula.
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
     * @brief Claims a slice of pre-allocated memory in Nebula's slab arena. The slice is
     * guaranteed to be zero-initialized.
     * @param size The size of the slice in bytes.
     */
    Slice(size_t size, const Placemat* placement = default_placement());
    /** ------------------------------------------------------------------------------------------- Constructor - Fresh Claim
     * @brief Claims a slice of pre-allocated memory in Nebula's slab arena, with the option to
     * specify whether the slice should be part of a larger slab or a novel buffer. The slice is
     * guaranteed to be zero-initialized.
     * @param size The size of the slice in bytes.
     * @param novel_buffer If true, then the slice is allocated as a novel buffer instead of being
     * a claim of a pre-allocated slab. This is ideal for slices that are long-lived to help
     * reduce fragmentation in the arena.
     */
    Slice(size_t size, bool novel_buffer, const Placemat* placement = default_placement());
    /** ------------------------------------------------------------------------------------------- Constructor - Copy from External Memory
     * @brief Copies data from an external memory location into a new slice of memory in Nebula.
     * This can be used to deep-copy a slice, or load data from an external source into Nebula's
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
    /** ------------------------------------------------------------------------------------------- Constructor - From Slice
     * @brief Constructs a `Slice` from an existing object
     * @param other The existing `Slice` to construct from.
     */
    template<typename T>
        requires (!std::is_same_v<T, Slice>)
        && (!std::is_arithmetic_v<T>)
        && (std::is_standard_layout_v<T>
        || std::is_convertible_v<T, Slice>)
    explicit Slice(T other, const Placemat* placement = default_placement());
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
     * @brief Use Nebula's internal memory arena system to resolve the slice's host-writable
     * pointer to the underlying memory.
     */
    void* raw();
    /** ------------------------------------------------------------------------------------------- Raw accessors - const
     * @brief Use Nebula's internal memory arena system to resolve the slice's host-writable
     * pointer to the underlying memory, but as a read-only pointer.
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
    /** ------------------------------------------------------------------------------------------- Size in bytes
     * @brief Returns the size of the slice in bytes.
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
        const Placemat* placement = default_placement()
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
    /** ------------------------------------------------------------------------------------------- Equality operator for PrimitiveSliceType
     * @brief Compares the current Slice with another Slice of a PrimitiveSliceType.
     * @tparam T The PrimitiveSliceType to compare with.
     * @param other The other Slice to compare with.
     * @return True if the slices are equal, false otherwise.
     */
    template<typename T>
    bool operator==(const T& other) const {
        if constexpr (std::is_convertible_v<T, Slice>) {
            return static_cast<Slice>(other).encoded_ == encoded_;
        } else {
            return get_as<T>() == other;
        }
    }
private:
    uint32_t encoded_ = 0;
    friend class SliceFriend;
};
static_assert(sizeof(Slice) == 4, "Slice must be 4 bytes in size.");
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
     * @brief Constructs a SliceT from a Slice. The Slice must have a size that is a multiple of
     * the size of type T.
     * @param slice The Slice to wrap.
     */
    explicit SliceT(Slice slice);
    /** ------------------------------------------------------------------------------------------- Conversion from other primitive slice types
     * @tparam U The type of the other primitive slice type.
     * @param other The other primitive slice to convert from.
     */
    template<typename U>
        requires (!std::is_same_v<U, SliceT<T>>)
        && (!std::is_arithmetic_v<U>)
        && (PrimitiveSliceType<std::remove_reference_t<U>>
        || std::is_standard_layout_v<std::remove_reference_t<U>>
        || std::is_convertible_v<U, Slice>)
    SliceT(U other) {
        if constexpr (PrimitiveSliceType<std::remove_reference_t<U>>) {
            *this = SliceT(static_cast<Slice>(other));
        } else if constexpr (std::is_convertible_v<U, Slice>) {
            *this = SliceT(static_cast<Slice>(other));
        } else if constexpr (std::is_standard_layout_v<std::remove_reference_t<U>>) {
            *this = SliceT(sizeof(U));
            std::memcpy(raw(), &other, sizeof(U));
        } else {
            static_assert(false, "Unsupported type for SliceT conversion.");
        }
    }
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
     * @brief Returns a reference to the underlying memory of the slice, cast to type U&.
     * @return A reference to the underlying memory of the slice.
     */
    template<typename U = T>
    U& get_as() {
        if constexpr (std::is_same_v<T, U>) { 
            if constexpr (std::is_same_v<T, std::string_view>) {
                return std::string_view(
                    reinterpret_cast<const char*>(slice_.raw()), slice_.size_bytes()
                );
            } else if constexpr (requires { typename T::value_type; }
                && std::is_same_v<T, std::span<typename T::value_type>>) {
                return std::span<typename T::value_type>(
                    reinterpret_cast<typename T::value_type*>(slice_.raw()),
                    slice_.size_bytes() / sizeof(typename T::value_type)
                );
            }
        }
        return *reinterpret_cast<U*>(slice_.raw());
    }
    /** ------------------------------------------------------------------------------------------- Get as - const
     * @brief Returns a const reference to the underlying memory of the slice, cast to type U&.
     * @return A const reference to the underlying memory of the slice.
     */
    template<typename U = T>
    const U& get_as() const {
        if constexpr (std::is_same_v<T, U>) { 
            if constexpr (std::is_same_v<T, std::string_view>) {
                return std::string_view(
                    reinterpret_cast<const char*>(slice_.raw()), slice_.size_bytes()
                );
            } else if constexpr (requires { typename T::value_type; }
                && std::is_same_v<T, std::span<typename T::value_type>>) {
                return std::span<typename T::value_type>(
                    reinterpret_cast<const typename T::value_type*>(slice_.raw()),
                    slice_.size_bytes() / sizeof(typename T::value_type)
                );
            }
        }
        return *reinterpret_cast<const U*>(slice_.raw());
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
        } else if constexpr(std::is_same_v<std::vector<typename T::value_type>, T>) {
            return slice_.size_bytes() / sizeof(typename T::value_type);
        }
        return slice_.size_bytes() / sizeof(T);
    }
};
static_assert(sizeof(SliceT<uint8_t>) == sizeof(Slice), "SliceT must be the same size as Slice.");
/** ------------------------------------------------------------------------------------------- Constructor - From Slice with Placemat
 * @brief Constructs a `Slice` from an existing `Slice`, with the option to specify the
 * placement.
 * @param other The existing `Slice` to construct from.
 * @param placement The placement for the new slice.
 */
template<typename T>
    requires (!std::is_same_v<T, Slice>)
    && (!std::is_arithmetic_v<T>)
    && (std::is_standard_layout_v<T>
    || std::is_convertible_v<T, Slice>)
inline Slice::Slice(T other, const Placemat* placement) {
    if constexpr (PrimitiveSliceType<std::remove_reference_t<T>>) {
        if (placement == other.placement()) {
            *this = Slice(static_cast<Slice>(other));
        }
        *this = Slice(other.size_bytes(), placement);
        std::memcpy(data(), other.data(), other.size_bytes());
    } else if constexpr (std::is_standard_layout_v<T>) {
        *this = Slice(sizeof(T), placement);
        std::memcpy(data(), &other, sizeof(T));
    } else {
        static_assert(false, "Unsupported type for Slice constructor with placement.");
    }
}
/** --------------------------------------------------------------------------------------------------------- SliceT Definitions
 * @brief Out-of-class definitions for SliceT's declared members.
 */
template <typename T>
SliceT<T>::SliceT(bool initialize) {
    if (initialize) {
        slice_ = Slice(sizeof(T));
    }
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
    TypeInfoContainer* type_info_ = nullptr;
    /// @brief Pointer to the stored value (either std::atomic<T> or T*)
    void* ptr_ = nullptr;
    /// @brief Atomic pointer used for borrow/return_borrowed operations
    std::unique_ptr<std::atomic<void*>> atomic_ = nullptr;
    /// @brief Backoff threshold for borrow operations
    const uint64_t throw_ =
            backoff_threshold_for(std::chrono::nanoseconds(std::chrono::seconds(60)));
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
        requires (AllowableAtomicType<T> && !NativeAtomicType<T> && !std::is_pointer_v<T>)
    AtomicContainer(T&& value) {
        type_info_ = TypeInfoContainer::create<T>();
        auto* a = new T(std::forward<T>(value));
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
        requires (NativeAtomicType<T> && !std::is_pointer_v<T>)
    AtomicContainer(T&& value) {
        type_info_ = TypeInfoContainer::create<T>();
        auto* a = new std::atomic<T>(std::forward<T>(value));
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
        requires (AllowableAtomicType<T> && std::is_pointer_v<T>)
    AtomicContainer(T&& value) {
        type_info_ = TypeInfoContainer::create<T>();
        auto* a = new std::atomic<T>(std::forward<T>(value));
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
        type_info_ = TypeInfoContainer::create<T>();
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
    T* borrow() {
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
        if (ptr_ != nullptr && type_info_ != nullptr && type_info_->deleter_method_ != nullptr
            && type_info_->is_pointer_ == false) {
            type_info_->deleter_method_(ptr_);
        }
        if (type_info_ != nullptr) {
            delete type_info_;
        }
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
            // Since this is a const method, we don't have to borrow it, we can just load directly
            T* value_ptr = static_cast<T*>(atomic_->load(order));
            if (value_ptr == nullptr) [[unlikely]] {
                ATOMIC_THROW("AtomicContainer::load(): Attempt to load from an empty container");
            }
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
            a->store(std::forward<T>(value), order);
        } else {
            T* value_ptr = borrow<T>();
            *value_ptr = std::move(value);
            return_borrowed(value_ptr);
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
            T* value_ptr = borrow<T>();
            *value_ptr = value;
            return_borrowed(value_ptr);
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
    T exchange(T&& value, std::memory_order order = std::memory_order_seq_cst) {
        if (typeid(T) != type_info_->type_info_) [[unlikely]] {
            std::string errormsg = "AtomicContainer::exchange(): Type"
                " mismatch when exchanging value in AtomicContainer."
                " Requested: " + std::string(typeid(T).name()) + ", Actual: " +
                    std::string(type_info_->type_info_.name());
            ATOMIC_THROW(errormsg);
        }
        if constexpr (NativeAtomicType<T> || std::is_pointer_v<T>) {
            auto* a = static_cast<std::atomic<T>*>(atomic_->load(std::memory_order_acquire));
            if (a == nullptr) {
                ATOMIC_THROW("AtomicContainer::exchange(): Attempt to exchange on an empty container");
            }
            return a->exchange(std::forward<T>(value), order);
        } else {
            T* value_ptr = borrow<T>();
            T old_value = std::move(*value_ptr);
            *value_ptr = std::move(value);
            return_borrowed(value_ptr);
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
            T* value_ptr = borrow<T>();
            bool result = false;
            if (*value_ptr == expected) {
                *value_ptr = std::move(desired);
                result = true;
            } else {
                expected = *value_ptr;
                result = false;
            }
            return_borrowed(value_ptr);
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
            T* value_ptr = borrow<T>();
            T old_value = *value_ptr;
            *value_ptr = *value_ptr + arg;
            return_borrowed(value_ptr);
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
            T* value_ptr = borrow<T>();
            T old_value = *value_ptr;
            *value_ptr = *value_ptr - arg;
            return_borrowed(value_ptr);
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
            T* value_ptr = borrow<T>();
            T old_value = *value_ptr;
            *value_ptr = *value_ptr & arg;
            return_borrowed(value_ptr);
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
            T* value_ptr = borrow<T>();
            T old_value = *value_ptr;
            *value_ptr = *value_ptr | arg;
            return_borrowed(value_ptr);
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
            T* value_ptr = borrow<T>();
            T old_value = *value_ptr;
            *value_ptr = *value_ptr ^ arg;
            return_borrowed(value_ptr);
            return old_value;
        }
    }
    /** --------------------------------------------------------------------------------- Fetch Modify
     * @brief Atomically modify the value in place using a provided function.
     * The function should take a T&& and return a T.
     * @param func The function to apply for modification.
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
        (void)order;
        if (typeid(T) != type_info_->type_info_) [[unlikely]] {
            std::string errormsg = "AtomicContainer::fetch_modify(): Type"
                " mismatch in AtomicContainer."
                " Requested: " + std::string(typeid(T).name()) + ", Actual: " +
                    std::string(type_info_->type_info_.name());
            ATOMIC_THROW(errormsg);
        }
        T* value_ptr = borrow<T>();
        T old_value = *value_ptr;
        *value_ptr = func(std::move(*value_ptr), args...);
        return_borrowed(value_ptr);
        return old_value;
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
        gate_->lock();
        if (registry_->find(key) != registry_->end()) {
            gate_->unlock();
            THROW("AtomicRegistry::create(): Key already exists: " + key);
        }
        std::unique_ptr<AtomicContainer> container =
                std::make_unique<AtomicContainer>(std::forward<T>(initial_value));
        AtomicContainer* ptr = container.get();
        registry_->emplace(key, std::move(container));
        gate_->unlock();
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
        global_gate().lock();
        if (registry->find(key) != registry->end()) {
            global_gate().unlock();
            THROW("AtomicRegistry::create_global(): Key already exists: " + key);
        }
        std::unique_ptr<AtomicContainer> container =
                std::make_unique<AtomicContainer>(std::forward<T>(initial_value));
        AtomicContainer* ptr = container.get();
        registry->emplace(key, std::move(container));
        global_gate().unlock();
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
        gate_->lock();
        auto it = registry_->find(key);
        if (it != registry_->end()) {
            AtomicContainer* result = it->second.get();
            gate_->unlock();
            return result;
        }
        std::unique_ptr<AtomicContainer> container =
                std::make_unique<AtomicContainer>(static_cast<std::decay_t<T>>(value));
        AtomicContainer* ptr = container.get();
        registry_->emplace(key, std::move(container));
        gate_->unlock();
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
        global_gate().lock();
        auto it = registry->find(key);
        if (it != registry->end()) {
            AtomicContainer* result = it->second.get();
            global_gate().unlock();
            return result;
        }
        std::unique_ptr<AtomicContainer> container =
                std::make_unique<AtomicContainer>(static_cast<std::decay_t<T>>(value));
        AtomicContainer* ptr = container.get();
        registry->emplace(key, std::move(container));
        global_gate().unlock();
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
        uint64_t count = counter_->fetch_add<uint64_t>(1, std::memory_order_acq_rel) + 1;
        if (count >= wait_for_) {
            counter_->store<uint64_t>(0, std::memory_order_release);
            notify_all();
        } else {
            int spin_count = 0;
            while (spin_count < 1000) {
                uint64_t current = counter_->load<uint64_t>(std::memory_order_acquire);
                if (current == 0) {
                    return;
                }
                std::this_thread::yield();
                spin_count++;
            }
            counter_->wait<uint64_t>(0, std::memory_order_acquire);
        }
    }
    /** --------------------------------------------------------------------------------- Notify All
     * @brief Notifies all threads waiting at the barrier. This can be used to
     * release waiting threads in response to an external signal, rather than
     * waiting for the count to be reached.
     */
    void notify_all() {
        counter_->notify_all();
    }
    /** --------------------------------------------------------------------------------- Signal
     * @brief A convenience method to signal the barrier from an external source.
     * This is just an alias for notify_all() to make the intent clearer when
     * signaling from outside the barrier itself.
     */
    void signal() {
        counter_->fetch_add<uint64_t>(1, std::memory_order_acq_rel);
         if (counter_->load<uint64_t>(std::memory_order_acquire) >= wait_for_) {
            counter_->store<uint64_t>(0, std::memory_order_release);
            notify_all();
        }
    }
    /** --------------------------------------------------------------------------------- Reset
     * @brief Resets the barrier to its initial state. This can be used to reuse
     * the same barrier for multiple rounds of synchronization.
     */
    void reset() {
        counter_->store<uint64_t>(0, std::memory_order_release);
        notify_all();
    }
};
} // namespace buffetalligator
