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
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <concepts>
#include <limits>
#include <memory>
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

namespace buffetalligator {
class Alligator; class Buffet; class BuffetMenu; class Slice; class Memory;
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
    uint32_t core_type_ = 0;
    Placemat() = default;
    friend class BuffetMenu;
    friend class Buffet;
    friend class Alligator;
    friend class Memory;
};
/** --------------------------------------------------------------------------------------------------------- Placement Description
 * @brief Describes a zeroed host-accessible allocator with optional resource limits and caching.
 */
struct PlacementDescription {
    const char* name = nullptr; ///< Process-lifetime placement name.
    size_t slab_bytes = 0; ///< Requested slab size or zero for runtime geometry.
    size_t base_alignment = 64; ///< Guaranteed base alignment of every allocation.
    size_t budget_bytes = 0; ///< Capacity ceiling or zero for derived capacity.
    size_t novel_cache_bytes = 0; ///< Maximum zeroed novel-cache capacity.
    bool set_as_default = false; ///< Selects this placement as the default.
    Placemat::Handle* (*allocate)(size_t, void*) = nullptr; ///< Returns zeroed memory in a new handle.
    void (*deallocate)(Placemat::Handle*, void*) = nullptr; ///< Frees substrate storage before framework handle deletion.
    void* (*get_host_ptr)(Placemat::Handle*) = nullptr; ///< Resolves the writable host address.
    void* (*get_context)() = nullptr; ///< Returns allocation context.
    uint64_t (*query_available)(void*) = nullptr; ///< Reports custom placement capacity.
    void (*zero)(Placemat::Handle*, uint64_t, uint64_t, void*) = nullptr; ///< Rezeros a retired range.
};
/** --------------------------------------------------------------------------------------------------------- BuffetTypeRegistry
 * @class BuffetTypeRegistry
 * @brief Startup registry assigning one stable type to each Placemat instance.
 */
class BuffetMenu {
public:
    /** ------------------------------------------------------------------------------------------- Register Description
     * @brief Registers a placement with explicit resource policy and optional zeroing callbacks.
     */
    static uint16_t register_type(const PlacementDescription& description);

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
    static const Placemat*& default_placement();
    /** ------------------------------------------------------------------------------------------- Shutdown
     * @brief Stops allocation and releases idle resources after application threads quiesce.
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
    );
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
        static BuffetMenu* instance = new BuffetMenu();
        return *instance;
    }
    friend class Memory;
};
/** --------------------------------------------------------------------------------------------------------- Memory
 * @brief Reports arena resources and controls budgets and idle capacity.
 */
class Memory {
public:
    enum class Pressure { None, Warn, Critical };
    Memory() = delete;
    /** ------------------------------------------------------------------------------------------- total_allocations
     * @brief Returns all slab and novel bytes obtained from placements.
     */
    static size_t total_allocations();
    /** ------------------------------------------------------------------------------------------- total_freed
     * @brief Returns all slab and novel bytes returned to placements.
     */
    static size_t total_freed();
    /** ------------------------------------------------------------------------------------------- placement_allocations
     * @brief Returns all bytes obtained from one placement.
     */
    static size_t placement_allocations(const Placemat& placement);
    /** ------------------------------------------------------------------------------------------- placement_freed
     * @brief Returns all bytes returned to one placement.
     */
    static size_t placement_freed(const Placemat& placement);
    /** ------------------------------------------------------------------------------------------- placement_usage
     * @brief Returns bytes currently owned by one placement.
     */
    static size_t placement_usage(const Placemat& placement);
    /** ------------------------------------------------------------------------------------------- placement_reserved
     * @brief Returns free-list and runway capacity.
     */
    static size_t placement_reserved(const Placemat& placement);
    /** ------------------------------------------------------------------------------------------- placement_live
     * @brief Returns placement usage excluding free-list and runway reserves.
     */
    static size_t placement_live(const Placemat& placement);
    /** ------------------------------------------------------------------------------------------- placement_budget
     * @brief Returns the placement capacity budget.
     */
    static size_t placement_budget(const Placemat& placement);
    /** ------------------------------------------------------------------------------------------- placement_available
     * @brief Returns budget headroom excluding live ownership.
     */
    static size_t placement_available(const Placemat& placement);
    /** ------------------------------------------------------------------------------------------- set_placement_budget
     * @brief Sets a placement capacity ceiling.
     */
    static void set_placement_budget(const Placemat& placement, size_t bytes);
    /** ------------------------------------------------------------------------------------------- placement_novel_cached
     * @brief Returns published zeroed novel-cache bytes.
     */
    static size_t placement_novel_cached(const Placemat& placement);
    /** ------------------------------------------------------------------------------------------- placement_runway_target
     * @brief Returns the current prepared slab target.
     */
    static size_t placement_runway_target(const Placemat& placement);
    /** ------------------------------------------------------------------------------------------- placement_slab_size
     * @brief Returns the resolved slab capacity.
     */
    static size_t placement_slab_size(const Placemat& placement);
    /** ------------------------------------------------------------------------------------------- trim
     * @brief Synchronously releases idle placement reserves and novel caches.
     */
    static void trim(const Placemat& placement);
    /** ------------------------------------------------------------------------------------------- trim_all
     * @brief Synchronously releases idle capacity across all placements.
     */
    static void trim_all();
    /** ------------------------------------------------------------------------------------------- system_physical
     * @brief Returns physical memory capacity.
     */
    static size_t system_physical();
    /** ------------------------------------------------------------------------------------------- system_available
     * @brief Returns available operating-system memory.
     */
    static size_t system_available();
    /** ------------------------------------------------------------------------------------------- system_limit
     * @brief Returns the effective process memory ceiling.
     */
    static size_t system_limit();
    /** ------------------------------------------------------------------------------------------- system_headroom
     * @brief Returns available memory constrained by process headroom.
     */
    static size_t system_headroom();
    /** ------------------------------------------------------------------------------------------- page_size
     * @brief Returns the native OS page size.
     */
    static size_t page_size();
    /** ------------------------------------------------------------------------------------------- large_page_size
     * @brief Returns a usable reported large-page size or zero.
     */
    static size_t large_page_size();
    /** ------------------------------------------------------------------------------------------- hardware_threads
     * @brief Returns hardware threads available to the process.
     */
    static unsigned hardware_threads();
    /** ------------------------------------------------------------------------------------------- pressure
     * @brief Returns the operating-system memory pressure level.
     */
    static Pressure pressure();
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
     * @brief Copies data from an external memory location into a new slice of memory in the
     * buffet alligator.
     * @param copy_from Pointer to the external memory to copy from.
     * @param size The size of the data to copy in bytes.
     * @param novel_buffer If true, then the slice is allocated as a novel buffer instead of being
     * a claim of a pre-allocated slab. This is ideal for slices that are long-lived to help
     * reduce fragmentation in the arena. Default is false.
     * @param placement The memory placement type for the new slice.
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
    /** ------------------------------------------------------------------------------------------- Novel Backing
     * @brief Reports dedicated novel backing for this Slice or subview, returning false for null.
     */
    bool is_novel() const noexcept;
    /** ------------------------------------------------------------------------------------------- Raw accessors
     * @brief Use the buffet alligator's internal memory arena system to resolve the slice's
     * host-writable pointer to the underlying memory.
     * @return A pointer to the underlying memory.
     */
    void* raw() { return cached_; }
    /** ------------------------------------------------------------------------------------------- Raw accessors - const
     * @brief Use the buffet alligator's internal memory arena system to resolve the slice's
     * host-writable pointer to the underlying memory, but as a read-only pointer.
     * @return A const pointer to the underlying memory.
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
        return meta_ == SIZE_MAX ? 0 : (meta_ >> ALLIGATOR_SLOT_BITS) & SIZE_MAX;
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
private:
    /// @brief All-ones marks the null slice; otherwise [0..16] arena ID, [17..63] byte-exact size.
    uint64_t meta_ = UINT64_MAX;
    /// @brief Cached host pointer to the slice's first byte.
    void* cached_ = nullptr;
    friend struct SliceLayout;
    friend class SliceQueue;
    friend class Buffet;
    friend class Placemat;
};
static_assert(sizeof(Slice) == 16, "Slice must be 16 bytes in size.");
/** --------------------------------------------------------------------------------------------------------- SliceQueue
 * @class SliceQueue
 * @brief Moves owned Slices through fixed worker pools with TLS blocks and semaphore wakeups.
 */
class SliceQueue {
private:
    void* queue_; ///< Private C queue state.
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
    U& get_as() { return *reinterpret_cast<U*>(slice_.raw()); }
    /** ------------------------------------------------------------------------------------------- Get as - const
     * @brief Returns a const reference to the underlying memory of the slice, cast to type U&.
     * @return A const reference to the underlying memory of the slice.
     */
    template<typename U = T>
    const U& get_as() const { return *reinterpret_cast<const U*>(slice_.raw()); }
    /** ------------------------------------------------------------------------------------------- View
     * @brief Returns a string or span view by value over the underlying bytes.
     */
    template<typename U = T>
    U view() const {
        if constexpr (std::is_same_v<U, std::string_view>) {
            return U(static_cast<const char*>(slice_.raw()), slice_.size_bytes());
        } else {
            static_assert(requires { typename U::element_type; U::extent; });
            return U(reinterpret_cast<typename U::element_type*>(const_cast<void*>(slice_.raw())),
                slice_.size_bytes() / sizeof(typename U::element_type));
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
        } else if constexpr (requires { typename T::value_type; }) {
            if constexpr (std::is_same_v<std::vector<typename T::value_type>, T>) {
                return slice_.size_bytes() / sizeof(typename T::value_type);
            }
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
/** -------------------------------------------------------------------------------------------------------------------- SliceMap
 * @class SliceMap
 * @brief Fixed-capacity result channel of (ID, Slice) rows with per-slot publication and hazard-pointer-safe row
 * reclamation.
 */
class SliceMap {
public:
    inline static constexpr int kHazardMaxThreads    = 256;  ///< Legacy constant retained for source compatibility.
    inline static constexpr int kHazardPtrsPerThread = 2;    ///< Hazard slots per thread row
    inline static constexpr size_t kRetireBatch      = 32;   ///< Retired nodes scanned per reclamation pass
    struct HazardSlot { std::atomic<void*> ptr{nullptr}; };
    struct HazardRegistration {
        unsigned row = 0;
        ~HazardRegistration() {
            if (row != 0) hazard_release_row(row);
        }
    };
    struct Row {
        int64_t id;
        Slice payload;
        Row(const int64_t& row_id, Slice&& claim)
        : id(row_id), payload(std::move(claim)) {}
        /** --------------------------------------------------------------------------------------- Allocate
         * @brief Claims row storage through the TLS arena with an adjacent owning Slice.
         */
        static void* operator new(size_t bytes);
        /** --------------------------------------------------------------------------------------- Placement Allocate
         * @brief Preserves placement construction into caller-owned row storage.
         */
        static void* operator new(size_t, void* storage) noexcept { return storage; }
        /** --------------------------------------------------------------------------------------- Deallocate
         * @brief Releases the adjacent owner after the row payload has been destroyed.
         */
        static void operator delete(void* pointer) noexcept;
        /** --------------------------------------------------------------------------------------- Placement Deallocate
         * @brief Leaves caller-owned placement storage intact if construction throws.
         */
        static void operator delete(void*, void*) noexcept {}
    };
    struct OrphanBatch {
        static constexpr size_t kCapacity = 32;
        void* nodes[kCapacity];
        size_t count = 0;
        OrphanBatch* next = nullptr;
    };
    struct RetiredList {
        std::vector<void*> nodes;
        ~RetiredList() {
            for (void* node : nodes) {
                if (!try_reclaim(node)) orphan_push(node);
            }
        }
        bool try_reclaim(void* p) {
            if (hazard_is_protected(p)) return false;
            destroy_row(p);
            return true;
        }
        void scan_and_reclaim() {
            std::vector<void*> survivors;
            survivors.reserve(nodes.size());
            for (void* node : nodes) {
                if (!try_reclaim(node)) survivors.push_back(node);
            }
            nodes = std::move(survivors);
            OrphanBatch* batch = orphan_top().exchange(nullptr, std::memory_order_acq_rel);
            while (batch != nullptr) {
                OrphanBatch* next = batch->next;
                for (size_t i = 0; i < batch->count; ++i) {
                    if (!try_reclaim(batch->nodes[i])) nodes.push_back(batch->nodes[i]);
                }
                operator delete(batch);
                batch = next;
            }
        }
        void retire(void* p) {
            nodes.push_back(p);
            if (nodes.size() >= kRetireBatch) scan_and_reclaim();
        }
    };
    /** ------------------------------------------------------------------------------------------- Constructor with Capacity
     * @brief Claims slots for exactly the rows the request will produce; there is no resize.
     * @param capacity Number of rows this set will carry.
     */
    explicit SliceMap(size_t capacity)
    : index_(index_capacity(capacity) * sizeof(size_t))
    , ids_(capacity * sizeof(int64_t))
    , raws_(capacity * sizeof(void*))
    , slots_(capacity * sizeof(Row*))
    , expected_(capacity)
    , capacity_(capacity)
    , index_mask_(index_capacity(capacity) - 1) {
        /// The sentinel is all-ones, so one byte fill marks every slot unpublished.
        if (capacity_) std::memset(ids(), 0xFF, capacity_ * sizeof(int64_t));
    }
    /** ------------------------------------------------------------------------------------------- Move Only Semantics
     * @brief The slots hold live row claims, so the set moves rather than copies.
     */
    SliceMap(const SliceMap&) = delete;
    SliceMap& operator=(const SliceMap&) = delete;
    SliceMap(SliceMap&& other) noexcept
    : index_(std::move(other.index_))
    , ids_(std::move(other.ids_))
    , raws_(std::move(other.raws_))
    , slots_(std::move(other.slots_))
    , claimed_(other.claimed_.load(std::memory_order_acquire))
    , published_(other.published_.load(std::memory_order_acquire))
    , expected_(other.expected_.load(std::memory_order_acquire))
    , wait_threshold_(other.wait_threshold_.load(std::memory_order_acquire))
    , wait_sem_(0)
    , on_publish_(other.on_publish_)
    , on_publish_context_(other.on_publish_context_)
    , capacity_(other.capacity_)
    , index_mask_(other.index_mask_) {
        other.capacity_ = 0;
        other.expected_.store(0, std::memory_order_release);
    }
    SliceMap& operator=(SliceMap&& other) noexcept {
        if (this != &other) {
            this->~SliceMap();
            new (this) SliceMap(std::move(other));
        }
        return *this;
    }
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Frees every row still in its slot; retired rows are owned by the reclamation lists.
     */
    ~SliceMap() {
        if (slots_) {
            for (size_t slot = 0; slot < capacity_; ++slot) {
                destroy_row(slot_row(slot).load(std::memory_order_relaxed));
            }
        }
    }
    /** ------------------------------------------------------------------------------------------- Add Slice
     * @brief Claims and publishes one row in a single call.
     * @param id The identifier for the row.
     * @param slice The row's payload claim, moved in.
     */
    template<typename ID>
        requires std::is_convertible_v<ID, int64_t>
    void add_slice(ID id, Slice slice) {
        publish_row(claim(), static_cast<int64_t>(id), std::move(slice));
    }
    /** ------------------------------------------------------------------------------------------- Merge
     * @brief Drains another set's landed rows into this one at node-transfer speed; runs after
     * both gathers have landed, never concurrently with publishes or readers.
     * @param other The set to drain into this one.
     * @return This set, holding both sets' rows.
     */
    SliceMap& merge(SliceMap& other) {
        const size_t count = other.capacity_;
        const size_t first = claim(other.size());
        size_t taken = 0;
        for (size_t slot = 0; slot < count; ++slot) {
            Row* row = other.slot_row(slot).exchange(nullptr, std::memory_order_relaxed);
            if (row == nullptr) continue;
            raws()[first + taken] = row->payload.data<void>();
            slot_row(first + taken).store(row, std::memory_order_relaxed);
            std::atomic_ref<int64_t>(ids()[first + taken]).store(row->id, std::memory_order_release);
            index_row(first + taken, row->id);
            ++taken;
        }
        if (count) {
            std::memset(other.ids(), 0xFF, count * sizeof(int64_t));
            std::memset(other.raws(), 0, count * sizeof(void*));
            std::memset(other.index_.raw(), 0, other.index_.size_bytes());
        }
        other.published_.store(0, std::memory_order_release);
        other.claimed_.store(0, std::memory_order_release);
        notify_if_waiting(published_.fetch_add(taken, std::memory_order_release) + taken);
        return *this;
    }
    /** ------------------------------------------------------------------------------------------- Addition Operator
     * @brief Combine two sets by draining the other into this one.
     * @param other The other set to drain.
     * @return A reference to the updated set.
     */
    SliceMap& operator+(SliceMap& other) {
        return merge(other);
    }
    /** ------------------------------------------------------------------------------------------- Find
     * @brief Locates a published row by ID; a miss means the row has not landed (yet). The hit is
     * hazard-validated, so the returned slot stays safe to read under any reclamation schedule.
     * @param id The ID to look for.
     * @return The row's slot, or -1 when it has not been published.
     */
    template<typename ID>
        requires std::is_convertible_v<ID, int64_t>
    int64_t find(const ID& id) const {
        return find_internal(static_cast<int64_t>(id));
    }
    /** ------------------------------------------------------------------------------------------- Get Slice by ID
     * @brief Copies a published row's payload out by ID, sharing the underlying claim; safe under
     * any reclamation schedule because the copy-out happens inside the hazard window.
     * @param id The identifier of the row to retrieve.
     * @return The payload claim, or a null slice when the row has not been published.
     */
    template<typename ID>
        requires std::is_convertible_v<ID, int64_t>
    Slice get_slice(ID id) {
        return get_slice_internal(static_cast<int64_t>(id));
    }
    /** ------------------------------------------------------------------------------------------- Data Access
     * @brief The kernel-facing pointer list: one payload data pointer per slot, in slot order,
     * maintained at publish time so extraction is free. Several of our SIMD distance methods
     * accept these lists of raw data pointers as parameters.
     * @return Pointer list with `capacity()` entries; unpublished slots read as nullptr.
     */
    template<typename P = void>
    P** data() {
        return reinterpret_cast<P**>(raws());
    }
    /** ------------------------------------------------------------------------------------------- Access Payload as Specific Type
     * @brief Access the payload at the given slot as a specific type.
     * @tparam T The payload type.
     * @param index The slot of the row to access.
     * @return A reference to the payload viewed as type T.
     */
    template<typename T>
    T& as(size_t index) {
        return *reinterpret_cast<T*>(raws()[index]);
    }
    /** ------------------------------------------------------------------------------------------- Slice At
     * @brief Shares the payload claim at a slot; call after `wait`/`wait_until_full` or under a
     * drain gate per the slot-access protocol.
     * @param index The slot of the row.
     * @return The payload claim.
     */
    Slice slice_at(size_t index) const {
        return slot_row(index).load(std::memory_order_acquire)->payload.slice();
    }
    /** ------------------------------------------------------------------------------------------- Access ID as Specific Type
     * @brief Access the ID at the given slot in the collection's native ID type.
     * @param index The slot of the row to access.
     * @return The ID; the sentinel value when the slot has not been published.
     */
    template<typename ID>
        requires std::is_convertible_v<int64_t, ID>
    ID id(size_t index) const {
        return static_cast<ID>(std::atomic_ref<int64_t>(const_cast<int64_t&>(ids()[index])).load(std::memory_order_acquire));
    }
    /** ------------------------------------------------------------------------------------------- Published Check
     * @brief Whether a slot's row has landed.
     * @param slot The slot to check.
     * @return True once the slot's ID has been release-stored.
     */
    bool published(const size_t& slot) const {
        return std::atomic_ref<int64_t>(const_cast<int64_t&>(ids()[slot]))
            .load(std::memory_order_acquire) != SENTINEL;
    }
    /** ------------------------------------------------------------------------------------------- Size Access
     * @brief The number of rows that have landed so far.
     * @return The published row count.
     */
    size_t size() const {
        return published_.load(std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Capacity
     * @brief Number of slots in the set.
     * @return The capacity.
     */
    size_t capacity() const noexcept { return capacity_; }
    /** ------------------------------------------------------------------------------------------- Expect
     * @brief Arms the landed-count the channel is expected to fulfill; `wait` and
     * `wait_threshold` watch this value, and `settled`-style callers compare `size()` against it.
     * @param count The expected landed count.
     */
    void expect(const size_t& count) {
        expected_.store(count, std::memory_order_release);
    }
    /** ------------------------------------------------------------------------------------------- Wait Threshold
     * @brief The armed expectation; `size()` equaling this value means every requested row landed.
     * @return The expected landed count.
     */
    size_t wait_threshold() const {
        return expected_.load(std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Wait
     * @brief Parks until the armed expectation has landed; the expectation defaults to capacity.
     */
    void wait() {
        wait_until_full(expected_.load(std::memory_order_acquire));
    }
    /** ------------------------------------------------------------------------------------------- Wait Until Full
     * @brief Parks the calling thread until at least `count` rows have landed. Producers signal
     * when their landed count crosses the armed threshold for an instant wake; the bounded poll
     * is the coherence backstop that no interleaving can strand.
     * @param count The number of rows to wait for.
     */
    void wait_until_full(size_t count) {
        if (size() >= count) return;
        wait_threshold_.store(count, std::memory_order_release);
        while (size() < count) {
            if (wait_sem_.try_acquire_for(std::chrono::microseconds(1000))) {
                wake_pending_.clear(std::memory_order_relaxed);
            }
        }
        wait_threshold_.store(SIZE_MAX, std::memory_order_release);
    }
    /** ------------------------------------------------------------------------------------------- Reset
     * @brief Retires every landed row and rewinds the channel for reuse; not concurrent with
     * producers, and readers of retired rows survive through their hazard claims.
     */
    void reset() {
        for (size_t bucket = 0; capacity_ && bucket <= index_mask_; ++bucket) {
            index_bucket(bucket).store(0, std::memory_order_relaxed);
        }
        for (size_t slot = 0; slot < capacity_; ++slot) {
            std::atomic_ref<int64_t>(ids()[slot]).store(SENTINEL, std::memory_order_release);
            Row* row = slot_row(slot).exchange(nullptr, std::memory_order_seq_cst);
            if (row != nullptr) [[likely]] {
                thread_retired().retire(row);
            }
        }
        if (capacity_) std::memset(raws(), 0, capacity_ * sizeof(void*));
        claimed_.store(0, std::memory_order_release);
        published_.store(0, std::memory_order_release);
        wait_threshold_.store(SIZE_MAX, std::memory_order_release);
        thread_retired().scan_and_reclaim();
    }
    /** ------------------------------------------------------------------------------------------- Publish Hook Signature
     * @brief Static hook fired on the publishing thread once a row's ID is visible and before the
     * row counts toward `size()` - so any waiter woken by the row sees the hook's effects.
     */
    using PublishHook = void (*)(void*, void*, const size_t&);
    /** ------------------------------------------------------------------------------------------- On Publish
     * @brief Registers the per-row publish hook; call once before the channel enters service.
     * @param hook The static hook to fire per landed row.
     * @param context Caller-defined context handed back to the hook.
     */
    void on_publish(PublishHook hook, void* context) {
        on_publish_context_ = context;
        on_publish_ = hook;
    }
    /** ------------------------------------------------------------------------------------------- Gc
     * @brief Periodic reclamation helper: frees retired rows no thread currently hazard-claims.
     */
    static void gc() { thread_retired().scan_and_reclaim(); }
private:
    /// @brief Unpublished-slot marker; all-ones is never a valid ID in either category.
    static constexpr int64_t SENTINEL = -1;
    /// @brief Separates the two active hazard slots of each thread by a full cache line.
    static constexpr size_t kHazardStride = 128 / sizeof(HazardSlot);
    /// @brief Open-addressed slot and fingerprint index with zero marking an empty bucket.
    Slice index_;
    /// @brief One ID per slot, sentinel-filled until its row is published; the publication flag.
    Slice ids_;
    /// @brief One raw payload pointer per slot, the kernel-facing view maintained at publish time.
    Slice raws_;
    /// @brief One row node pointer per slot, null until published and after reset.
    Slice slots_;
    /// @brief Producer slot claims; claiming and publishing are separate steps.
    alignas(128) std::atomic<size_t> claimed_{0};
    /// @brief Rows landed; the counter watched by size and wait.
    alignas(128) std::atomic<size_t> published_{0};
    /// @brief The landed count the channel is expected to fulfill; `wait` and `wait_threshold` watch.
    std::atomic<size_t> expected_{0};
    /// @brief Armed landed-count threshold a parked waiter is watching; SIZE_MAX means none.
    alignas(128) std::atomic<size_t> wait_threshold_{SIZE_MAX};
    /// @brief Coalesces posted or in-flight wakes into one binary-semaphore credit.
    std::atomic_flag wake_pending_ = ATOMIC_FLAG_INIT;
    /// @brief The parked waiter's semaphore, with at most one outstanding credit.
    std::binary_semaphore wait_sem_{0};
    /// @brief The per-row publish hook, or null when none is registered.
    PublishHook on_publish_ = nullptr;
    /// @brief Caller-defined context handed to the publish hook.
    void* on_publish_context_ = nullptr;
    /// @brief Number of slots.
    size_t capacity_ = 0;
    /// @brief Power-of-two bucket count minus one.
    size_t index_mask_ = 0;
    /// @brief Internal type-erased accessor for slices based on ID category and ID.
    int64_t find_internal(int64_t id) const;
    int64_t find_internal(uint32_t id) const;
    Slice get_slice_internal(int64_t id);
    Slice get_slice_internal(uint32_t id);
    /** ------------------------------------------------------------------------------------------- Index Capacity
     * @brief Reserves at least two buckets per row without integer overflow.
     */
    static size_t index_capacity(size_t capacity) {
        if (capacity > SIZE_MAX / (4 * sizeof(size_t))) ALLIGATOR_THROW("SliceMap capacity is too large");
        return std::bit_ceil(std::max(capacity, size_t(1))) * 2;
    }
    /** ------------------------------------------------------------------------------------------- Index Bucket
     * @brief Exposes a stable bucket through atomic accesses during publication and reset.
     */
    std::atomic_ref<size_t> index_bucket(size_t bucket) const {
        return std::atomic_ref<size_t>(const_cast<size_t&>(index_.data<size_t>()[bucket]));
    }
    /** ------------------------------------------------------------------------------------------- Hash ID
     * @brief Mixes every ID bit for both the bucket position and its fingerprint.
     */
    size_t hash_id(int64_t id) const {
        uint64_t hash = static_cast<uint64_t>(id);
        hash = (hash ^ (hash >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        hash = (hash ^ (hash >> 27)) * UINT64_C(0x94d049bb133111eb);
        return static_cast<size_t>(hash ^ (hash >> 31));
    }
    /** ------------------------------------------------------------------------------------------- Index Row
     * @brief Publishes each key's earliest slot into one independently claimed hash bucket.
     */
    void index_row(size_t slot, int64_t id) {
        const size_t hash = hash_id(id);
        const size_t fingerprint = hash & ~index_mask_;
        const size_t desired = fingerprint | (slot + 1);
        size_t bucket = hash & index_mask_;
        size_t entry = 0;
        for (;;) {
            if (index_bucket(bucket).compare_exchange_strong(entry, desired,
                std::memory_order_release, std::memory_order_relaxed)) return;
            if ((entry & ~index_mask_) == fingerprint) {
                entry = index_bucket(bucket).load(std::memory_order_acquire);
                const size_t previous = (entry & index_mask_) - 1;
                if (std::atomic_ref<int64_t>(ids()[previous]).load(std::memory_order_relaxed) == id) {
                    if (previous <= slot) return;
                    continue;
                }
            }
            bucket = (bucket + 1) & index_mask_;
            entry = 0;
        }
    }
    /** ------------------------------------------------------------------------------------------- Lookup Slot
     * @brief Screens one probe cluster for the key's earliest published slot.
     */
    int64_t lookup_slot(int64_t id, size_t first) const;
    /** ------------------------------------------------------------------------------------------- Typed bases */
    int64_t* ids() { return ids_.template data<int64_t>(); }
    const int64_t* ids() const { return ids_.template data<int64_t>(); }
    void** raws() { return raws_.template data<void*>(); }
    Row** row_base() { return reinterpret_cast<Row**>(slots_.template data<void*>()); }
    /** ------------------------------------------------------------------------------------------- Row Base Access
     * @brief Provides access to the base row pointers.
     * @return A pointer to the array of row pointers.
     */
    const Row* const* row_base() const {
        return reinterpret_cast<const Row* const*>(slots_.template data<void*>());
    }
    /** ------------------------------------------------------------------------------------------- Slot Row Access
     * @brief Provides atomic access to the row at the specified slot.
     * @param index The slot index of the row to access.
     * @return An atomic reference to the row at the specified slot.
     */
    std::atomic_ref<Row*> slot_row(size_t index) {
        return std::atomic_ref<Row*>(row_base()[index]);
    }
    /** ------------------------------------------------------------------------------------------- Slot Row Access
     * @brief Provides atomic access to the row at the specified slot.
     * @param index The slot index of the row to access.
     * @return An atomic reference to the row at the specified slot.
     */
    std::atomic_ref<Row*> slot_row(size_t index) const {
        return std::atomic_ref<Row*>(const_cast<Row*&>(row_base()[index]));
    }
    /** ------------------------------------------------------------------------------------------- Claim
     * @brief Claims the next `n` slots for the producer; slots publish independently.
     * @param n Number of slots to claim.
     * @return The first claimed slot.
     */
    size_t claim(const size_t& n = 1) {
        return claimed_.fetch_add(n, std::memory_order_relaxed);
    }
    /** ------------------------------------------------------------------------------------------- Publish
     * @brief Fills a claimed slot and publishes it; the release store of the ID is what makes
     * the payload bytes visible to readers.
     * @param slot The claimed slot.
     * @param id The row's ID.
     * @param slice The row's payload, moved in.
     */
    template<typename ID>
        requires std::is_convertible_v<ID, int64_t>
    void publish(const size_t& slot, const ID& id, Slice&& slice) {
        publish_row(slot, static_cast<int64_t>(id), std::move(slice));
    }
    /** ------------------------------------------------------------------------------------------- Publish Row
     * @brief Lands one row: the release store of the ID makes the payload bytes visible to
     * readers, the hook runs before the row counts, and the waiter channel wakes last.
     * @param slot The claimed slot.
     * @param id The row's ID.
     * @param slice The row's payload, moved in.
     */
    void publish_row(const size_t& slot, const int64_t& id, Slice&& slice) {
        Row* row = new Row(id, std::move(slice));
        raws()[slot] = row->payload.data<void>();
        slot_row(slot).store(row, std::memory_order_release);
        std::atomic_ref<int64_t>(ids()[slot]).store(id, std::memory_order_release);
        index_row(slot, id);
        if (on_publish_ != nullptr) [[unlikely]] {
            on_publish_(on_publish_context_, static_cast<void*>(this), slot);
        }
        notify_if_waiting(published_.fetch_add(1, std::memory_order_release) + 1);
    }
    /** ------------------------------------------------------------------------------------------- Notify If Waiting
     * @brief Wakes a parked waiter once the landed count crosses its armed threshold.
     */
    void notify_if_waiting(size_t landed) {
        size_t threshold = wait_threshold_.load(std::memory_order_relaxed);
        if (landed >= threshold && wait_threshold_.compare_exchange_strong(threshold, SIZE_MAX,
            std::memory_order_relaxed, std::memory_order_relaxed)) [[unlikely]] {
            if (!wake_pending_.test_and_set(std::memory_order_relaxed)) wait_sem_.release();
        }
    }
    /** ------------------------------------------------------------------------------------------- Verify Slot
     * @brief Hazard-validates that slot's row is live and still carries the needle; leaves the
     * slot's hazard claim held on success so the caller can read the row, clears it on failure.
     * @param slot The candidate slot.
     * @param needle The ID the caller is matching.
     * @return True when the hazard claim is held and the row matches.
     */
    bool verify_slot(size_t slot, int64_t needle) const {
        Row* row = slot_row(slot).load(std::memory_order_seq_cst);
        hazard_protect(0, row);
        if (verify_held(slot, needle, row)) return true;
        hazard_clear(0);
        return false;
    }
    /** ------------------------------------------------------------------------------------------- Verify Held
     * @brief Re-validates an already-held hazard claim: the row pointer is stable, non-null, and
     * still carries the needle - the ABA guard that makes the read safe to complete.
     * @param slot The candidate slot.
     * @param needle The ID the caller is matching.
     * @param row The hazard-held row candidate.
     * @return True when the held claim is valid for the needle.
     */
    bool verify_held(size_t slot, int64_t needle, const Row* row) const {
        return row != nullptr
            && slot_row(slot).load(std::memory_order_seq_cst) == row
            && std::atomic_ref<int64_t>(const_cast<int64_t&>(ids()[slot]))
                .load(std::memory_order_acquire) == needle
            && row->id == needle;
    }
    /** ------------------------------------------------------------------------------------------- Hazard Storage
     * @struct HazardStorage
     * @brief Owns process-lifetime hazard rows and their exclusive registration bits.
     */
    struct HazardStorage {
        size_t rows; ///< Runtime row capacity.
        Slice pointers; ///< Hazard pointer storage.
        Slice owners; ///< Registration bits.
        /** --------------------------------------------------------------------------------- Constructor
         * @brief Allocates four hazard rows per available hardware thread.
         */
        HazardStorage()
        : rows(4 * Memory::hardware_threads())
        , pointers(rows * kHazardStride * sizeof(HazardSlot))
        , owners(rows * sizeof(std::atomic<bool>)) {
            for (size_t index = 0; index < rows * kHazardStride; ++index) {
                new (pointers.data<HazardSlot>() + index) HazardSlot();
            }
            for (size_t index = 0; index < rows; ++index) {
                new (owners.data<std::atomic<bool>>() + index) std::atomic<bool>(false);
            }
        }
    };
    /** ------------------------------------------------------------------------------------------- Hazard Storage
     * @brief Returns hazard storage intentionally retained for the process lifetime.
     * @note The returned storage is intended to live for the entire process lifetime.
     */
    static HazardStorage& hazard_storage() {
        static HazardStorage* storage = new HazardStorage();
        return *storage;
    }
    /** ------------------------------------------------------------------------------------------- Hazard Pool
     * @brief Returns the dynamically sized hazard pointer array.
     * @return A pointer to the first hazard slot in the pool.
     */
    static HazardSlot* hazard_pool() { return hazard_storage().pointers.data<HazardSlot>(); }
    /** ------------------------------------------------------------------------------------------- Claim Hazard Row
     * @brief Exclusively registers a bounded row or reports pool exhaustion.
     * @return The row index of the claimed hazard row.
     */
    static unsigned hazard_claim_row() {
        auto& storage = hazard_storage();
        auto* owners = storage.owners.data<std::atomic<bool>>();
        for (unsigned row = 1; row < storage.rows; ++row) {
            bool expected = false;
            if (owners[row].compare_exchange_strong(expected, true, std::memory_order_acquire,
                std::memory_order_relaxed)) return row;
        }
        ALLIGATOR_THROW("SliceMap: hazard pool exhausted");
    }
    /** ------------------------------------------------------------------------------------------- Release Hazard Row
     * @brief Clears a thread's hazards before making its row available for reuse.
     * @param row The row index of the hazard pointers to release.
     */
    static void hazard_release_row(unsigned row) {
        HazardSlot* pool = hazard_pool();
        for (int slot = 0; slot < kHazardPtrsPerThread; ++slot) {
            pool[row * kHazardStride + slot].ptr.store(nullptr, std::memory_order_seq_cst);
        }
        hazard_storage().owners.data<std::atomic<bool>>()[row].store(false, std::memory_order_release);
    }
    /** ------------------------------------------------------------------------------------------- Hazard Registration
     * @brief Returns the hazard registration for the current thread.
     * @return A reference to the current thread's hazard registration.
     */
    static HazardRegistration& hazard_registration() {
        static thread_local HazardRegistration registration;
        return registration;
    }
    /** ------------------------------------------------------------------------------------------- Hazard Mine
     * @brief Returns the row index of the current thread's hazard registration, claiming a row if
     * necessary.
     * @return The row index of the current thread's hazard registration.
     */
    static unsigned hazard_mine() {
        HazardRegistration& registration = hazard_registration();
        if (registration.row == 0) [[unlikely]] {
            registration.row = hazard_claim_row();
        }
        return registration.row;
    }
    /** ------------------------------------------------------------------------------------------- Hazard Protect
     * @brief Sets the hazard pointer at the specified slot for the current thread.
     * @param slot The slot index of the hazard pointer to set.
     * @param p Pointer to the node to protect.
     */
    static void hazard_protect(int slot, void* p) {
        hazard_pool()[hazard_mine() * kHazardStride + slot].ptr.store(p, std::memory_order_seq_cst);
    }
    /** ------------------------------------------------------------------------------------------- Hazard Clear
     * @brief Clears the hazard pointer at the specified slot for the current thread.
     * @param slot The slot index of the hazard pointer to clear.
     */
    static void hazard_clear(int slot) {
        hazard_pool()[hazard_mine() * kHazardStride + slot].ptr.store(nullptr, std::memory_order_seq_cst);
    }
    /** ------------------------------------------------------------------------------------------- Hazard Is Protected
     * @brief Checks if a pointer is currently protected by any hazard pointer.
     * @param p Pointer to the node to check.
     * @return True if the pointer is protected, false otherwise.
     */
    static bool hazard_is_protected(const void* p) {
        const HazardSlot* pool = hazard_pool();
        for (size_t row = 1; row < hazard_storage().rows; ++row) {
            for (int slot = 0; slot < kHazardPtrsPerThread; ++slot) {
                if (pool[row * kHazardStride + slot].ptr.load(std::memory_order_seq_cst) == p) return true;
            }
        }
        return false;
    }
    /** ------------------------------------------------------------------------------------------- Orphan Top
     * @brief Returns the top of the orphan stack.
     * @return A reference to the atomic pointer representing the top of the orphan stack.
     */
    static std::atomic<OrphanBatch*>& orphan_top() {
        static std::atomic<OrphanBatch*> top{nullptr};
        return top;
    }
    /** ------------------------------------------------------------------------------------------- Orphan Push
     * @brief Pushes a node onto the orphan stack for later reclamation.
     * @param p Pointer to the node to be orphaned.
     */
    static void orphan_push(void* p) {
        OrphanBatch* batch = new OrphanBatch();
        batch->nodes[batch->count++] = p;
        batch->next = orphan_top().load(std::memory_order_relaxed);
        while (!orphan_top().compare_exchange_weak(
            batch->next, batch, std::memory_order_release, std::memory_order_relaxed)) {}
    }
    /** ------------------------------------------------------------------------------------------- Destroy Row
     * @brief Destroys a row and deallocates its memory.
     * @param p Pointer to the row to be destroyed.
     */
    static void destroy_row(void* p) {
        if (p != nullptr) {
            delete static_cast<Row*>(p);
        }
    }
    /** ------------------------------------------------------------------------------------------- Thread Retired List
     * @brief Returns the retired list for the current thread.
     * @return A reference to the current thread's retired list.
     */
    static RetiredList& thread_retired() {
        static thread_local RetiredList retired;
        return retired;
    }
};
/** ----------------------------------------------------------------------------------------------- SliceMapT
 * @class SliceMapT
 * @brief The typed twin of SliceMap: the same claim/publish/find engine with `as()` typing the
 * payload view and `emplace()` constructing typed rows in place.
 * @tparam T The payload type rows carry.
 */
template<typename T>
class SliceMapT final : public SliceMap {
public:
    /** ------------------------------------------------------------------------------------------- Constructor with Capacity
     * @brief Claims slots for exactly the rows the request will produce; there is no resize.
     * @param capacity Number of rows this set will carry.
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
     * @brief Constructs a typed row in place from its arguments and publishes it under `id`.
     * Total atomic operations: the add_slice protocol
     * Total branches: 0
     * @param id The identifier for the row. Must be convertible to int64_t.
     * @param args Arguments forwarded to T's constructor.
     */
    template<typename ID, typename... Args>
        requires std::is_convertible_v<ID, int64_t>
    void emplace(ID id, Args&&... args) {
        Slice payload(sizeof(T));
        new (payload.data<void>()) T(std::forward<Args>(args)...);
        add_slice(id, std::move(payload));
    }
    /** ------------------------------------------------------------------------------------------- Access Payload as T
     * @brief Access the payload at the given slot as its native type.
     * @param index The slot of the row to access.
     * @return A reference to the payload.
     */
    T& as(size_t index) {
        return SliceMap::as<T>(index);
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
    WeakSlice(size_t, bool) {
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
    T* data() { return static_cast<T*>(span_.data()); }
    /** ----------------------------------------------------------------------------------------- Data
     * @brief Returns a typed pointer to the memory block.
     * @tparam T The type to cast the memory block to.
     */
    template<typename T>
    const T* data() const { return static_cast<const T*>(span_.data()); }
    /** ----------------------------------------------------------------------------------------- Get As
     * @brief Returns a typed reference to the memory block.
     * @tparam T The type to cast the memory block to.
     */
    template<typename T>
    T& get_as() { return *static_cast<T*>(span_.data()); }
    /** ----------------------------------------------------------------------------------------- Get As
     * @brief Returns a typed reference to the memory block.
     * @tparam T The type to cast the memory block to.
     */
    template<typename T>
    const T& get_as() const { return *static_cast<const T*>(span_.data()); }
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
    /** ----------------------------------------------------------------------------------------- Root Slice
     * @brief Returns a reference to the root Slice object. Since a `WeakSlice` is not a full
     * owning slice, this operation is discouraged since it requires a memory copy.
     * @return A new `Slice` object with the memory copied from the `WeakSlice`.
     */
    Slice root_slice() const {
        return slice();
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
        if (offset + size > span_.size()) [[unlikely]] {
            size = span_.size() - offset;
        }
        Slice new_slice(size);
        std::memcpy(new_slice.data<uint8_t>(), span_.data() + offset, size);
        return new_slice;
    }
};
} // namespace buffetalligator
