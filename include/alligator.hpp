#pragma once
/** --------------------------------------------------------------------------------------------------------- BuffetAlligator
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
#include <future>
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
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.hpp>
#if defined(BUFFETALLIGATOR_HAS_CUDA)
#include <cuda.h>
#endif
#include <moodycamel/concurrentqueue.h>
#include <moodycamel/blockingconcurrentqueue.h>

namespace buffetalligator {
class Alligator; class Buffet; class BuffetMenu; class Slice;
class Memory; class SliceQueue; class SliceChannel;
EXCEPTION_CLASS(Alligator)
#define ALLIGATOR_THROW(msg) throw AlligatorException(msg)
inline static constexpr size_t POOL_BITS = 25;
/** --------------------------------------------------------------------------------------------------------- Host Memory Usage
 * @struct HostMemoryUsage
 * @brief Reports physical capacity, estimated available memory, and this process's resident bytes.
 */
struct HostMemoryUsage {
    uint64_t physical_bytes;
    uint64_t available_bytes;
    uint64_t resident_bytes;
};
/** --------------------------------------------------------------------------------------------------------- AllocateMethod
 * @brief The method used to allocate a new handle within this placemat.
 * @param size The size of the allocation in bytes.
 * @param global_context The global context associated with the allocation (if applicable).
 * @return A pair containing the host pointer and the substrate handle of the newly allocated memory.
 */
using AllocateMethod = std::pair<void*, void*> (*)(size_t size, void* global_context);
/** --------------------------------------------------------------------------------------------------------- DeallocateMethod
 * @brief The method used to deallocate a handle within this placemat.
 * @param host_ptr The host pointer of the memory to be deallocated.
 * @param substrate_handle The substrate handle associated with the deallocation.
 * @return A pair containing the values that the host pointer and the substrate handle should be set to. We
 * would normally assume these would just be `nullptr`, but we don't like to make too many assumptions about
 * devices and stuff.
 */
using DeallocateMethod = std::pair<void*, void*> (*)(void* host_ptr, void* substrate_handle);
/** --------------------------------------------------------------------------------------------------------- GetContextMethod
 * @brief The method used to retrieve the context associated with a plate. This may be something like
 * `vk::Device` for Vulkan, or `CUcontext` for CUDA, but it must be a static method that the `Placemat` can
 * use in order to pass into the allocation and deallocation methods.
 * @return A pointer to the context associated with the plate.
 */
using GetContextMethod = void* (*)();
/** --------------------------------------------------------------------------------------------------------- DeviceAddressMethod
 * @brief The method that maps one plate's substrate handle to the device address of its first byte.
 * @param substrate_handle The substrate handle the allocation method returned.
 * @return The device address, 0 for memory no device can address.
 */
using DeviceAddressMethod = uint64_t (*)(void* substrate_handle);
/** --------------------------------------------------------------------------------------------------------- host_device_address
 * @brief The device address method of host-only placements, which have none.
 * @param substrate_handle The substrate handle, unused.
 * @return Always 0.
 */
inline uint64_t host_device_address(void*) { return 0; }
/** --------------------------------------------------------------------------------------------------------- ResizeMethod
 * @brief The method used to attempt an in-place resize of one whole-block allocation. Only
 * placements whose substrate can honor it (the C heap's realloc, for example) provide one.
 * @param host_ptr The host pointer of the block.
 * @param substrate_handle The freeable allocation handle.
 * @param new_size The requested size in bytes.
 * @return The resized host and substrate pair, or nulls to fall back to copy-based resizing.
 */
using ResizeMethod = std::pair<void*, void*> (*)(void* host_ptr, void* substrate_handle, size_t new_size);
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
/** --------------------------------------------------------------------------------------------------------- Placemat Construction Gate
 * @brief The gate guarding Placemat construction and its setters.
 */
SETUP_read_this(placemat_construction, 4 + 13, "You must know what you are doing in order to construct a placemat properly.");
/** --------------------------------------------------------------------------------------------------------- Host Pointer
 * @struct HostPtr
 * @brief A simple wrapper around a raw pointer to represent a host memory location.
 */
struct HostPtr {
    void* ptr = nullptr;
};
/** --------------------------------------------------------------------------------------------------------- GPUBuf
 * @struct GPUBuf
 * @brief Represents a GPU buffer with its address, size, and offset.
 */
struct alignas(16) GPUBuf {
    /// @brief GPU buffer address.
    uint64_t address = 0;
    /// @brief Size of the GPU buffer.
    uint32_t size = 0;
    /// @brief Offset of the GPU buffer within the memory.
    uint32_t offset = 0;
};
/** --------------------------------------------------------------------------------------------------------- Placemat
 * @class Placemat
 * @brief The `Placemat` or placement of a category of memory allocations within the BuffetAlligator
 * framework. Where does this memory exist? In heap? On a GPU? On disk? Each gets its own `Placemat`
 * instance that tells the framework how to talk to the underlying memory.
 */
class Placemat {
public:
    /// @brief Forward declaration of the `Plate` structure.
    struct Plate;
    /** ------------------------------------------------------------------------------------------- Allocate
     * @brief Returns the allocation method associated with this placemat.
     * @return The allocation method.
     */
    AllocateMethod allocate() const { return allocator_; }
    /** ------------------------------------------------------------------------------------------- Deallocate
     * @brief Returns the deallocation method associated with this placemat.
     * @return The deallocation method.
     */
    DeallocateMethod deallocate() const { return deallocator_; }
    /** ------------------------------------------------------------------------------------------- GetContext
     * @brief Returns the method used to retrieve the context associated with this placemat.
     * @return The context retrieval method.
     */
    GetContextMethod get_context() const { return get_context_; }
    /** ------------------------------------------------------------------------------------------- DeviceAddress
     * @brief Returns the method that maps a substrate handle to its device address.
     * @return The device address method.
     */
    DeviceAddressMethod device_address() const { return device_address_; }
    /** ------------------------------------------------------------------------------------------- Type
     * @brief Returns the registry identifier assigned to this placement.
     * @return The placement registry identifier.
     */
    uint8_t type() const { return type_; }
    /** ------------------------------------------------------------------------------------------- Name
     * @brief Returns the name of the placement.
     * @return The placement name.
     */
    const char* name() const { return name_; }
    /** ------------------------------------------------------------------------------------------- Alignment
     * @brief Returns the alignment requirement for allocations from this placement.
     * @return The alignment in bytes.
     */
    size_t alignment() const { return bump_alignment_; }
    /** ------------------------------------------------------------------------------------------- CallFreeLater
     * @brief Schedules the given plate to be freed at a later time.
     * @param plate The plate to be freed.
     */
    static void call_free_later(Plate* plate);
    /** ------------------------------------------------------------------------------------------- Record Slab Allocation
     * @brief Reports one completed slab allocation to the memory tracker; defined in slice.cpp.
     * @param plate The plate that now owns the slab.
     */
    static void record_slab_allocation(const Plate* plate);
    /** ------------------------------------------------------------------------------------------- Record Slab Release
     * @brief Reports one completed slab release to the memory tracker; defined in slice.cpp.
     * @param plate The plate whose slab was just returned.
     */
    static void record_slab_release(const Plate* plate);
    /** ------------------------------------------------------------------------------------------- Plate
     * @struct Plate
     * @brief A `Plate` is a specific allocation of memory within a given `Placemat`. One might
     * call these "slabs" in some contexts, they are large contiguous blocks of memory that the
     * framework hands out sub-slices of.
     */
    struct Plate {
        /// @brief The placemat that owns this plate.
        Placemat* placemat;
        /// @brief The size of the allocation in bytes.
        size_t size;
        /// @brief The host-visible base pointer of the allocation.
        void* host_ptr;
        /// @brief The special handle associated with this allocation.
        /// Example: vk::Device for Vulkan, or CUcontext for CUDA.
        void* substrate_handle;
        /// @brief The device address of the allocation's first byte, 0 for host-only memory.
        uint64_t device_base;
        /** ----------------------------------------------------------------------------- next atomic
         * @brief We try to pre-allocate a slab or two ahead of the current bump pointer
         * so that contention around boundaries is minimized when allocating new slices.
         */
        std::atomic<Plate*>* next_buf;
        /** ----------------------------------------------------------------------------- Next
         * @brief Retrieves or allocates the next handle in the pre-allocated slab
         * chain, or allocates a new handle if the next handle is not available yet.
         * @param ahead_alloc The number of plates to make sure are pre-allocated ahead
         * of the current plate.
         */
        Plate* next(size_t ahead_alloc) {
            Plate* current_next = next_buf->load(std::memory_order_acquire);
            while (current_next == nullptr
                || current_next == reinterpret_cast<Plate*>(-1ll)
            ) {
                if (next_buf->compare_exchange_strong(
                    current_next,
                    reinterpret_cast<Plate*>(-1ll),
                    std::memory_order_acq_rel)
                ) {
                    auto [host_ptr, substrate_handle] =
                        placemat->allocate()(size, placemat->get_context()());
                    current_next = new Plate(
                        placemat,
                        size,
                        host_ptr,
                        substrate_handle,
                        false
                    );
                    record_slab_allocation(current_next);
                    next_buf->store(current_next, std::memory_order_release);
                    break;
                }
                std::this_thread::yield();
                current_next = next_buf->load(std::memory_order_acquire);
            }
            if (ahead_alloc > 0) {
                (void)current_next->next(ahead_alloc - 1);
            }
            return current_next;
        }
        /**
         * @brief The slice associated with this plate. Now, I know what you're thinking:
         * "Isn't a slice supposed to be a peice of a handle? That seems kind of circular,
         * doesn't it?" and you would be correct in wondering that.
         * This is a trick that we use to keep the `Plate` alive until the final `Slice`
         * has gone out of scope. What we do is give the `Plate` one giant slice (which
         * we can then sub-slice), and when the bump pointer reaches the end of the slice,
         * we simply delete this `Slice` instance which decrements the reference count.
         * This "frees" the plate but at the same time keeps it alive *if* there are
         * still other `Slice`s out there referencing it.
         */
        std::atomic<Slice*>* slice;
        /// @brief When this reaches zero, that means nothing is referencing this handle
        /// anymore, so it gets freed or recycled.
        std::atomic<int>* ref_count;
        /** ----------------------------------------------------------------------------- Free Plate
         * @brief Decrements the reference count and frees the plate if it reaches zero.
         */
        /// @brief Parked reference count of a released plate, so a late claim can never revive it.
        inline static constexpr int RELEASED = INT32_MIN / 2;
        void free() {
            if (ref_count->fetch_sub(1, std::memory_order_acquire) == 1) {
                auto [a, b] = placemat->deallocate()(host_ptr, substrate_handle);
                host_ptr = a;
                substrate_handle = b;
                record_slab_release(this);
                if (slice->load(std::memory_order_acquire) != nullptr) {
                    std::string error_msg = "Hmm... something isn't right. The slice"
                        " should have been nullptr before freeing the plate.";
                    ALLIGATOR_THROW(error_msg);
                }
                // The atomics stay allocated so a late walker reads a parked count, never freed memory.
                ref_count->store(RELEASED, std::memory_order_release);
            }
        }
        /// @brief The bump pointer for this handle, used to slice the slab into smaller
        /// allocations.
        std::atomic<size_t>* bump;
        /** ----------------------------------------------------------------------------- Claim
         * @brief Claims a sub-allocation from this plate if the plate has it available.
         * If it does not, we try the next plate in the chain.
         * @param size The size of the sub-allocation to claim.
         * @return A tuple containing the plate pointer, the current bump pointer, and
         * the size of the allocation - this area in memory is now owned by the caller.
         */
        Slice claim(size_t size, bool novel_buffer = false);
        /** ----------------------------------------------------------------------------- Constructor
         * @brief Constructs a new Plate object.
         * @param placemat_ Pointer to the placemat this plate belongs to, so it knows
         * where it lives.
         * @param size_ Size of the plate.
         * @param host_ptr_ Host pointer associated with the plate.
         * @param substrate_handle_ Substrate handle associated with the plate.
         * @param final_course Indicates if this is a novel allocation or if it is part
         * of the chain of buffer allocations.
         */
        Plate(
            Placemat* placemat_,
            size_t size_,
            void* host_ptr_,
            void* substrate_handle_,
            bool final_course
        ) : placemat(placemat_)
        , size(final_course ? 0 : size_)
        , host_ptr(host_ptr_)
        , substrate_handle(substrate_handle_)
        , device_base(placemat_->device_address()(substrate_handle_))
        , next_buf(new std::atomic<Plate*>(nullptr))
        , slice(new std::atomic<Slice*>(nullptr))
        , ref_count(new std::atomic<int>(0))
        , bump(new std::atomic<size_t>(0)) {
            if (final_course) {
                bump->store(size_, std::memory_order_release);
            } else {
                slice->store(
                    placemat->create_main_slice(this),
                    std::memory_order_release
                );
            }
        }
        /** ----------------------------------------------------------------------------- Copy Constructor
         * @brief Constructs a new Plate object as a copy of another.
         * @param other The Plate object to copy from.
         */
        Plate(const Plate& other)
        : placemat(other.placemat)
        , size(other.size)
        , host_ptr(other.host_ptr)
        , substrate_handle(other.substrate_handle)
        , device_base(other.device_base)
        , slice(other.slice)
        , ref_count(other.ref_count)
        , bump(other.bump) {
            ref_count->fetch_add(1, std::memory_order_relaxed);
        }
        /** ----------------------------------------------------------------------------- Copy Assignment Operator
         * @brief Assigns the contents of one Plate object to another.
         * @param other The Plate object to assign from.
         * @return Reference to the assigned Plate object.
         */
        Plate& operator=(const Plate& other) {
            if (this != &other) {
                other.ref_count->fetch_add(1, std::memory_order_relaxed);
                free();
                placemat = other.placemat;
                size = other.size;
                host_ptr = other.host_ptr;
                substrate_handle = other.substrate_handle;
                device_base = other.device_base;
                slice = other.slice;
                ref_count = other.ref_count;
                bump = other.bump;
            }
            return *this;
        }
        /** ----------------------------------------------------------------------------- Move Constructor
         * @brief Constructs a new Plate object by moving from another.
         * @param other The Plate object to move from.
         */
        Plate(Plate&& other)
        : placemat(other.placemat)
        , size(other.size)
        , host_ptr(other.host_ptr)
        , substrate_handle(other.substrate_handle)
        , device_base(other.device_base)
        , slice(other.slice)
        , ref_count(other.ref_count)
        , bump(other.bump) {
            other.placemat = nullptr;
            other.size = 0;
            other.host_ptr = nullptr;
            other.substrate_handle = nullptr;
            other.slice = nullptr;
            other.ref_count = nullptr;
            other.bump = nullptr;
        }
        /** ----------------------------------------------------------------------------- Move Assignment Operator
         * @brief Assigns the contents of one Plate object to another by moving.
         * @param other The Plate object to move from.
         * @return Reference to the assigned Plate object.
         */
        Plate& operator=(Plate&& other) {
            if (this != &other) {
                free();
                placemat = other.placemat;
                size = other.size;
                host_ptr = other.host_ptr;
                substrate_handle = other.substrate_handle;
                device_base = other.device_base;
                slice = other.slice;
                ref_count = other.ref_count;
                bump = other.bump;
                other.placemat = nullptr;
                other.size = 0;
                other.host_ptr = nullptr;
                other.substrate_handle = nullptr;
                other.slice = nullptr;
                other.ref_count = nullptr;
                other.bump = nullptr;
            }
            return *this;
        }
        /** ----------------------------------------------------------------------------- Destructor
         * @brief Destroys the Plate object and frees its resources.
         */
        ~Plate() { free(); }
    };
    /** ------------------------------------------------------------------------------------------- Current Handle
     * @brief Retrieves the current handle (plate) associated with this placemat.
     * @return Pointer to the current plate.
     */
    Plate* current_plate() const {
        Plate* current = current_handle_.load(std::memory_order_acquire);
        while (current == nullptr || current == reinterpret_cast<Plate*>(-1ll)) [[unlikely]] {
            Plate* expected = nullptr;
            if (current == nullptr && current_handle_.compare_exchange_strong(
                expected, reinterpret_cast<Plate*>(-1ll),
                std::memory_order_acq_rel, std::memory_order_acquire)
            ) {
                std::pair<void*, void*> allocation;
                try {
                    allocation = allocator_(default_slab_size_, get_context_());
                } catch (...) {
                    current_handle_.store(nullptr, std::memory_order_release);
                    throw;
                }
                current = new Plate(
                    const_cast<Placemat*>(this), default_slab_size_,
                    allocation.first, allocation.second, false);
                record_slab_allocation(current);
                current_handle_.store(current, std::memory_order_release);
                (void)current->next(1);
                return current;
            }
            std::this_thread::yield();
            current = current_handle_.load(std::memory_order_acquire);
        }
        return current;
    }
    /** ------------------------------------------------------------------------------------------- Set Current Plate
     * @brief Advances the current plate from the one the caller exhausted to its successor,
     * refusing when another claimer already advanced it so the chain can never fork.
     * @param expected The plate the caller found full.
     * @param plate Pointer to the plate to set as current.
     * @return True when this call performed the swap.
     */
    bool set_current_plate(Plate* expected, Plate* plate) {
        return current_handle_.compare_exchange_strong(
            expected, plate, std::memory_order_acq_rel, std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- No copy/move */
    Placemat(const Placemat&) = delete;
    Placemat& operator=(const Placemat&) = delete;
    Placemat(Placemat&&) = delete;
    Placemat& operator=(Placemat&&) = delete;
    /** ------------------------------------------------------------------------------------------- Preloaded types */
    /// @brief Plain ol' heap allocation, calloc'd to zero first.
    static const Placemat* const HEAP;
    /// @brief Heap allocations aligned to 64 bytes for 512-bit registers.
    static const Placemat* const HEAP_ALIGNED;
    /// @brief Heap allocations aligned to the system page size.
    static const Placemat* PAGE_ALIGNED;
    /// @brief Vulkan buffer memory that is unified between host and device.
    static const Placemat* const UNIFIED;
    /// @brief Vulkan buffer memory that is visible to the host.
    static const Placemat* const HOST_VISIBLE;
    /// @brief Vulkan buffer memory that resides on the host.
    static const Placemat* const HOST;
    /// @brief Vulkan buffer memory that is cacheable by the host.
    static const Placemat* const HOST_CACHEABLE;
    /// @brief Vulkan buffer memory that resides on the device.
    static const Placemat* const DEVICE;
    /// @brief The heap built-in backing basic claims.
    static const Placemat* const BASIC_HEAP;
    /** ------------------------------------------------------------------------------------------- Constructor/Destructor
     * @brief Default constructor and destructor for Placemat. You must arm the placemat_construction
     * gate with set_read_this to prove that you know what you're doing. This prevents language models
     * from inappropriately constructing a Placemat to fix a localized bug, not realizing theyre likely
     * breaking a lot of things while doing so.
     */
    Placemat() {
        CHECK_read_this(placemat_construction);
    }
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Default destructor for Placemat.
     */
    ~Placemat() = default;
    /** ------------------------------------------------------------------------------------------- Set Name
     * @brief Sets the name of the Placemat. You must know what you are doing to call this method.
     * @param name The name to set for the Placemat.
     */
    void set_name(const char* name) {
        CHECK_read_this(placemat_construction);
        name_ = name;
    }
    /** ------------------------------------------------------------------------------------------- Set Type
     * @brief Sets the type of the Placemat. You must know what you are doing to call this method.
     * @param type The type to set for the Placemat.
     */
    void set_type(uint8_t type) {
        CHECK_read_this(placemat_construction);
        type_ = type;
    }
    /** ------------------------------------------------------------------------------------------- Set Bump Alignment
     * @brief Sets the bump alignment of the Placemat. You must know what you are doing to call
     * this method.
     * @param bump_alignment The bump alignment to set for the Placemat.
     */
    void set_bump_alignment(size_t bump_alignment) {
        CHECK_read_this(placemat_construction);
        bump_alignment_ = bump_alignment;
    }
    /** ------------------------------------------------------------------------------------------- Set Default Slab Size
     * @brief Sets the default slab size of the Placemat. You must know what you are doing to call
     * this method.
     * @param default_slab_size The default slab size to set for the Placemat.
     */
    void set_default_slab_size(size_t default_slab_size) {
        CHECK_read_this(placemat_construction);
        default_slab_size_ = default_slab_size;
    }
private:
    const char* name_ = nullptr;
    uint8_t type_ = 0;
    size_t bump_alignment_ = 64;
    size_t default_slab_size_ = 0;
    mutable std::atomic<Plate*> current_handle_{nullptr};
    AllocateMethod allocator_ = nullptr;
    DeallocateMethod deallocator_ = nullptr;
    GetContextMethod get_context_ = nullptr;
    DeviceAddressMethod device_address_ = &host_device_address;
    /// @brief Optional in-place resize hook; null on placements whose substrate cannot honor it.
    ResizeMethod resizer_ = nullptr;
    Slice* create_main_slice(const Plate* plate) const;
    Slice create_slice_from(const Plate* plate, size_t offset, size_t size) const;
    friend class BuffetMenu;
    friend class Alligator;
    friend class Slice;
    friend class Memory;
};
/** --------------------------------------------------------------------------------------------------------- SliceId
 * @struct SliceId
 * @brief A packed identifier for a memory slice.
 */
struct alignas(4) SliceId {
private:
    uint32_t id_;
    friend class BuffetMenu; friend class Alligator;
    friend class Slice; friend class Placemat;
public:
    explicit SliceId(uint32_t input = 0xFFFFFFFFu) { *this = reinterpret_cast<SliceId&>(input); }
    SliceId& operator=(const uint32_t& input) {
        uint32_t tmp = input; *this = reinterpret_cast<SliceId&>(tmp); return *this; }
    SliceId& operator=(const size_t& input) { uint32_t tmp = static_cast<uint32_t>(input);
        *this = reinterpret_cast<SliceId&>(tmp); return *this; }
    SliceId& operator=(const int& input) { uint32_t tmp = static_cast<uint32_t>(input);
        *this = reinterpret_cast<SliceId&>(tmp); return *this; }
    operator uint32_t&() { return *reinterpret_cast<uint32_t*>(this); }
    operator const uint32_t&() const { return *reinterpret_cast<const uint32_t*>(this); }
    explicit SliceId(size_t input) { uint32_t tmp = static_cast<uint32_t>(input);
        *this = reinterpret_cast<SliceId&>(tmp); }
    explicit SliceId(int input) { uint32_t tmp = static_cast<uint32_t>(input);
        *this = reinterpret_cast<SliceId&>(tmp); } SliceId(const SliceId& other) = default;
    SliceId& operator=(const SliceId& other) = default;
    SliceId(SliceId&& other) noexcept { *this = other; other = 0xFFFFFFFFu; }
    SliceId& operator=(SliceId&& other) noexcept { *this = other; other = 0xFFFFFFFFu; return *this; }
    Slice slice(size_t offset = 0, size_t size = SIZE_MAX) const;
};
SETUP_read_this(add_placement, 33 + 66, "You should really call the register_type method instead.")
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
     * @param name The name of the placement.
     * @param default_slab_size The default size of slabs for this placement.
     * @param bump_alignment The alignment requirement for bump allocations.
     * @param allocate The allocation function for this placement.
     * @param deallocate The deallocation function for this placement.
     * @param get_context The context retrieval function for this placement.
     * @param set_as_default Whether to set this placement as the default.
     * @param resize The optional in-place resize hook.
     * @param device_address The substrate-to-device-address hook, host-only by default.
     * @return The stable placement identifier.
     */
    static uint8_t register_type(
        const char* name,
        size_t default_slab_size,
        size_t bump_alignment,
        AllocateMethod allocate,
        DeallocateMethod deallocate,
        GetContextMethod get_context,
        bool set_as_default = false,
        ResizeMethod resize = nullptr,
        DeviceAddressMethod device_address = &host_device_address
    ) {
        set_read_this(placemat_construction, 4 + 13);
        std::unique_ptr<Placemat> placement = std::make_unique<Placemat>();
        unset_read_this(placemat_construction);
        placement->name_ = name;
        placement->default_slab_size_ = default_slab_size;
        placement->bump_alignment_ = bump_alignment;
        placement->allocator_ = allocate;
        placement->deallocator_ = deallocate;
        placement->get_context_ = get_context;
        placement->resizer_ = resize;
        placement->device_address_ = device_address;
        placement->type_ = static_cast<uint16_t>(instance().placement_indices_.size());
        if (set_as_default) {
            default_placement_slot() = placement.get();
        }
        const uint8_t type = static_cast<uint8_t>(instance().placement_indices_.size());
        instance().placement_indices_[name] = type;
        set_read_this(add_placement, 90 + 9);
        placemats()[type] = std::move(placement);
        unset_read_this(add_placement);
        instance().placemat_count_.fetch_add(1, std::memory_order_release);
        return type;
    }
    /** ------------------------------------------------------------------------------------------- Get
     * @brief Returns the registered Placemat for an identifier.
     * @param type The placement identifier.
     * @return The registered placement factory.
     */
    static Placemat* get(uint8_t type) {
        set_read_this(add_placement, 33 + 66);
        Placemat* placement = instance().placemats()[type].get();
        unset_read_this(add_placement);
        return placement;
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
        Placemat* placement = nullptr;
        if (it != instance().placement_indices_.end()) {
            set_read_this(add_placement, 33 + 66);
            placement = instance().placemats().at(it->second).get();
            unset_read_this(add_placement);
        }
        return placement;
    }
    /** ------------------------------------------------------------------------------------------- Count
     * @brief Returns the number of Placemat types registered so far.
     * @return The registered placement count.
     */
    static size_t count() {
        if (!instance().builtins_ready_.load(std::memory_order_acquire)) {
            ensure_builtins_slow();
        }
        return instance().placemat_count_.load(std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Default Placement
     * @brief Returns the default Placemat instance, registering the built-ins first if needed.
     * @return The default Placemat pointer.
     */
    static const Placemat*& default_placement() {
        if (!instance().builtins_ready_.load(std::memory_order_acquire)) {
            ensure_builtins_slow();
        }
        if (!instance().device_default_ready_.load(std::memory_order_acquire)) {
            ensure_device_default_slow();
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
    /** ------------------------------------------------------------------------------------------- Placemats
     * @brief Returns the flat index-to-Placemat table; the registration paths write it, gated
     * readers use it.
     * @return The 256-entry placemat table.
     */
    static std::array<std::unique_ptr<Placemat>, 256>& placemats() {
        static std::array<std::unique_ptr<Placemat>, 256> places;
        CHECK_read_this(add_placement);
        return places;
    }
    /** ------------------------------------------------------------------------------------------- No copy/move */
    BuffetMenu(const BuffetMenu&) = delete;
    BuffetMenu& operator=(const BuffetMenu&) = delete;
    BuffetMenu(BuffetMenu&&) = delete;
    BuffetMenu& operator=(BuffetMenu&&) = delete;
    ~BuffetMenu() = default;
private:
    /// @brief Mapping from placement names to their corresponding indices in the placements_ vector.
    std::unordered_map<std::string, size_t> placement_indices_;
    /// @brief List of registered change listeners along with their context pointers.
    std::vector<std::pair<void*, void (*)(void*)>> change_listeners_;
    /// @brief Set while the built-in placements are being registered.
    std::atomic<bool> builtins_claimed_{false};
    /// @brief Set once the built-in placements hold their stable identifiers.
    std::atomic<bool> builtins_ready_{false};
    /// @brief Set while the device probe decides the default placement.
    std::atomic<bool> device_default_claimed_{false};
    /// @brief Set once the device probe has decided the default placement.
    std::atomic<bool> device_default_ready_{false};
    /** ------------------------------------------------------------------------------------------- Ensure Builtins Slow
     * @brief Slow path that registers the built-in placements; defined in the library.
     */
    static void ensure_builtins_slow();
    /** ------------------------------------------------------------------------------------------- Ensure Device Default Slow
     * @brief Slow path that makes the probed buffer placement the default under unified memory
     * when no registration chose one; defined beside the Vulkan context.
     */
    static void ensure_device_default_slow();
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
        AllocateMethod allocate,
        DeallocateMethod deallocate,
        GetContextMethod get_context,
        bool set_as_default,
        ResizeMethod resize = nullptr,
        DeviceAddressMethod device_address = &host_device_address
    ) {
        if (default_slab_size < 64 * 1024 * 1024) default_slab_size = 64 * 1024 * 1024;
        auto& inst = instance();
        uint16_t type = static_cast<uint16_t>(inst.placement_indices_.size());
        set_read_this(placemat_construction, 15 + 2);
        auto placement = std::unique_ptr<Placemat>(new Placemat());
        unset_read_this(placemat_construction);
        placement->name_ = name;
        placement->allocator_ = allocate;
        placement->default_slab_size_ = default_slab_size;
        placement->bump_alignment_ = bump_alignment;
        placement->deallocator_ = deallocate;
        placement->get_context_ = get_context;
        placement->resizer_ = resize;
        placement->device_address_ = device_address;
        placement->type_ = type;
        if (set_as_default || default_placement_slot() == nullptr) {
            default_placement_slot() = placement.get();
        }
        inst.placement_indices_.emplace(name, type);
        set_read_this(add_placement, 60 + 39);
        inst.placemats()[static_cast<uint8_t>(type)] = std::move(placement);
        unset_read_this(add_placement);
        inst.placemat_count_.fetch_add(1, std::memory_order_release);
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
    std::atomic<size_t> placemat_count_{0};
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
    /** ------------------------------------------------------------------------------------------- Constructor - From SliceId
     * @brief Constructs a slice object that takes on the identity of the existing slice specified
     * by `slice_id`. This counts as an additional reference to that slice's underlying memory.
     * If there is no slice at that ID, then the Slice will not be set to that ID, but instead it
     * will be set to the null slice sentinel.
     * @param slice_id The identifier of the existing slice.
     */
    explicit Slice(SliceId slice_id = static_cast<SliceId>(0xFFFFFFFFu));
    /** ------------------------------------------------------------------------------------------- Constructor - Fresh Claim
     * @brief Claims a slice of pre-allocated memory in the buffet alligator's slab arena. The
     * slice is guaranteed to be zero-initialized.
     * @param size The size of the slice in bytes.
     */
    Slice(size_t size, const Placemat* placement);
    /** ------------------------------------------------------------------------------------------- Constructor - Fresh Claim
     * @brief Constructs a slice of memory with the specified size. If not a novel buffer, then
     * it will be claimed from a pre-allocated slab in the buffet alligator's arena.
     * @param size The size of the slice in bytes.
     * @param novel_buffer If true, then the slice is allocated as a novel buffer instead of being
     * a claim of a pre-allocated slab. This is ideal for slices that are long-lived to help
     * reduce fragmentation in the arena.
     */
    Slice(size_t size, bool novel_buffer = false, const Placemat* placement = default_placement());
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
     * @brief The alligator pool slot this slice occupies; the id bits are all ones when null.
     * @return The pool slot index.
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
private:
    SliceId id_;
    friend class Buffet;
    friend class Placemat;
    friend class SliceQueue;
    friend struct SliceNetworkAccess;
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
/** --------------------------------------------------------------------------------------------------------- Task
 * @struct Task<ReturnType, Func, Args...>
 * @brief Represents a task that can be executed with a return type and arguments.
 * @tparam ReturnType The return type of the task.
 * @tparam Func The type of the callable object.
 * @tparam Args The types of the arguments to the callable object.
 */
/** --------------------------------------------------------------------------------------------------------- Task
 * @struct Task
 * @brief One type-erased unit of executor work: a pair of static operation pointers (run and
 * destroy, both bound by make_task) plus the packed callable, arguments and promise it owns on
 * the heap. No virtuals and no std::function; every call lands on a static, concrete method.
 */
struct Task {
    /// @brief Runs the packed state and satisfies its promise; bound by make_task.
    void (*run_)(Task& task) = nullptr;
    /// @brief Destroys the packed state; bound by make_task.
    void (*destroy_)(Task& task) = nullptr;
    /// @brief The packed callable, arguments and promise owned by this task.
    void* state_ = nullptr;
    /// @brief The follow-on task, enqueued by the worker after this one completes.
    std::unique_ptr<Task> after_this_task = nullptr;
    Task() = default;
    Task(Task&& other) noexcept
    : run_(other.run_)
    , destroy_(other.destroy_)
    , state_(std::exchange(other.state_, nullptr))
    , after_this_task(std::move(other.after_this_task)) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (state_ != nullptr && destroy_ != nullptr) {
                destroy_(*this);
            }
            run_ = other.run_;
            destroy_ = other.destroy_;
            state_ = std::exchange(other.state_, nullptr);
            after_this_task = std::move(other.after_this_task);
        }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() {
        if (state_ != nullptr && destroy_ != nullptr) {
            destroy_(*this);
        }
    }
    /** ------------------------------------------------------------------------------------------- Execute
     * @brief Runs the packed callable with its arguments and satisfies its promise; exceptions
     * travel to the future.
     */
    void execute() {
        run_(*this);
    }
    /** ------------------------------------------------------------------------------------------- After this
     * @brief Schedules a task to be executed after the current task completes.
     * @tparam ReturnType2 The return type of the subsequent task.
     * @tparam Func2 The type of the callable object for the subsequent task.
     * @tparam Args2 The types of the arguments for the subsequent task.
     * @param f The callable object for the subsequent task.
     * @param a The arguments to pass to the subsequent task.
     * @return A future representing the result of the subsequent task.
     */
    template<typename ReturnType2 = void, typename Func2, typename... Args2>
    auto after_this(Func2 f, Args2... a) {
        auto [task, future] = make_task<ReturnType2>(std::move(f), std::move(a)...);
        after_this_task = std::make_unique<Task>(std::move(task));
        return std::move(future);
    }
};
/** --------------------------------------------------------------------------------------------------------- Task State
 * @struct TaskState
 * @brief The typed heap state behind one Task: the callable, its arguments and its promise, plus
 * the two static operations the Task carries. One instantiation per make_task call site.
 * @tparam ReturnType The return type of the callable.
 * @tparam Func The type of the callable object.
 * @tparam Args The types of the arguments to the callable.
 */
template<typename ReturnType, typename Func, typename... Args>
    requires std::is_invocable_r_v<ReturnType, Func, Args...>
struct TaskState {
    /// @brief The callable object representing the task.
    Func func;
    /// @brief The arguments to pass to the callable.
    std::tuple<Args...> args;
    /// @brief The promise used to store the result of the task execution.
    std::promise<ReturnType> promise;
    /** ------------------------------------------------------------------------------------------- Run
     * @brief Invokes the packed callable and satisfies the promise, routing exceptions to the future.
     * @param task The task whose state is being run.
     */
    static void run(Task& task) {
        TaskState* state = static_cast<TaskState*>(task.state_);
        try {
            if constexpr (std::is_same_v<ReturnType, void>) {
                std::apply(state->func, state->args);
                state->promise.set_value();
            } else {
                state->promise.set_value(std::apply(state->func, state->args));
            }
        } catch (...) {
            state->promise.set_exception(std::current_exception());
        }
    }
    /** ------------------------------------------------------------------------------------------- Destroy
     * @brief Destroys the packed state once the task has run or been dropped.
     * @param task The task whose state is being destroyed.
     */
    static void destroy(Task& task) {
        delete static_cast<TaskState*>(task.state_);
        task.state_ = nullptr;
    }
};
/** --------------------------------------------------------------------------------------------------------- Make Task
 * @brief The Task factory: packs one typed unit of work into an erased Task and returns the
 * future its promise will satisfy.
 * @tparam ReturnType The return type of the callable.
 * @tparam Func The type of the callable object.
 * @tparam Args The types of the arguments to the callable.
 * @param f The callable object.
 * @param a The arguments to pass to the callable.
 * @return The task and its future.
 */
template<typename ReturnType = void, typename Func, typename... Args>
    requires std::is_invocable_r_v<ReturnType, Func, Args...>
std::pair<Task, std::future<ReturnType>> make_task(Func f, Args... a) {
    using State = TaskState<ReturnType, Func, Args...>;
    Task task;
    task.state_ = new State{std::move(f), std::make_tuple(std::move(a)...), std::promise<ReturnType>()};
    task.run_ = &State::run;
    task.destroy_ = &State::destroy;
    std::future<ReturnType> future = static_cast<State*>(task.state_)->promise.get_future();
    return {std::move(task), std::move(future)};
}
class ConcurrentBitplane;
/** --------------------------------------------------------------------------------------------------------- Arena
 * @class Alligator
 * @brief Maintains one preallocated Buffer chain per registered Placemat.
 */
class Alligator {
private:
    std::atomic<uint64_t> next_slice_{};
    SliceId next_id();
    std::atomic<size_t> requested_thread_count_{1};
    std::atomic<bool> stop_signal_{false};
    moodycamel::BlockingConcurrentQueue<Task> task_queue_;
    struct WorkerThread;
    std::array<std::atomic<WorkerThread*>, 32> worker_threads_;
    struct GatorBuf;
    GatorBuf* plates_;
    GatorBuf* host_ptrs_;
    /// @brief The shared GPUBuf table, one plate the host writes and shaders read in place.
    Placemat::Plate* gpubufs_;
    std::unique_ptr<ConcurrentBitplane> occupancy_;
    Placemat::Plate*& plate(SliceId slice_id);
    const Placemat::Plate*& plate(const SliceId& slice_id) const;
    HostPtr* host_ptr(SliceId slice_id);
    const HostPtr* host_ptr(const SliceId& slice_id) const;
    GPUBuf* gpubuf(SliceId slice_id);
    const GPUBuf* gpubuf(const SliceId& slice_id) const;
    void destroy(SliceId slice_id);
    Alligator();
    ~Alligator();
    friend class Buffet;
    friend class Placemat;
    friend class Slice;
    friend class Memory;
    friend class VulkanKernel;
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
    /** ------------------------------------------------------------------------------------------- Plate For
     * @brief Resolves the plate backing a live slice; the substrate reaches its slab metadata
     * through the plate's substrate_handle.
     * @param slice The slice to resolve.
     * @return The backing plate, or nullptr when the slot is unoccupied.
     */
    static Placemat::Plate* plate_for(const Slice& slice) {
        return inst().plate(static_cast<SliceId>(slice.pool_index()));
    }
    /** ------------------------------------------------------------------------------------------- GPUBuf For
     * @brief The writable GPUBuf half for a live slice, nullptr when the slot is unoccupied.
     * @param slice The slice to resolve.
     * @return The slice's GPUBuf entry.
     */
    static GPUBuf* gpubuf_for(const Slice& slice) {
        return inst().gpubuf(static_cast<SliceId>(slice.pool_index()));
    }
    /** ------------------------------------------------------------------------------------------- Host Table
     * @brief The host pointer table's first entry; kernels index it by pool index at an 8-byte stride.
     * @return The table base.
     */
    static const HostPtr* host_table();
    /** ------------------------------------------------------------------------------------------- GPU Table
     * @brief The shared GPUBuf table's host mapping, the same bytes shaders read at gpu_table_address().
     * @return The table base.
     */
    static const GPUBuf* gpu_table();
    /** ------------------------------------------------------------------------------------------- GPU Table Address
     * @brief The shared GPUBuf table's device address for the kernel push block.
     * @return The device address, 0 without a compute device.
     */
    static uint64_t gpu_table_address();
    /** ------------------------------------------------------------------------------------------- Submit task
     * @brief Submits a task to the Alligator.
     * @tparam ReturnType The return type of the task.
     * @tparam Func The type of the callable object.
     * @tparam Args The types of the arguments to the callable object.
     * @param f The callable object to be submitted.
     * @param a The arguments to be passed to the callable object.
     * @return A std::future representing the result of the submitted task.
     */
    template<typename ReturnType = void, typename Func, typename... Args>
        requires std::is_invocable_r_v<ReturnType, Func, Args...>
    auto submit(Func f, Args... a) {
        auto [task, future] = make_task<ReturnType>(std::move(f), std::move(a)...);
        task_queue_.enqueue(std::move(task));
        return std::move(future);
    }
    /** ------------------------------------------------------------------------------------------- Submit task (pre-constructed)
     * @brief Submits a pre-constructed erased task to the Alligator; hold the future make_task
     * already returned for it.
     * @param task The pre-constructed task to be submitted.
     */
    void submit(Task&& task) {
        task_queue_.enqueue(std::move(task));
    }
};
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
/** --------------------------------------------------------------------------------------------------------- GPUException
 * @class GPUException
 * @brief A GPUException is thrown when a GPU operation fails.
 */
class GPUException : public threadsafe_logger::Exception {
public:
    using threadsafe_logger::Exception::Exception;
    using threadsafe_logger::Exception::what;
};
#define GPU_THROW(msg) throw GPUException(msg)
/// @brief Forward declaration of the internal shader state used by the Shader class.
class ShaderState;
/** --------------------------------------------------------------------------------------------------------- Shader
 * @class Shader
 * @brief Prepared `alligator_main(Slice)` GLSL: a call binds a list of slices and each workgroup
 * column processes its own slice in place.
 */
class Shader {
private:
    /// @brief Internal state of the shader, managed by the Shader class.
    std::unique_ptr<ShaderState> state_;
public:
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Compiles GLSL and prepares every Vulkan resource used by subsequent calls.
     * @param source GLSL defining `void alligator_main(Slice slice)`, the slice this workgroup owns.
     * @param name Diagnostic name reported by shader compilation errors.
     */
    explicit Shader(
        std::string_view source,
        std::string_view name = "alligator_shader"
    );
    /** ------------------------------------------------------------------------------------------- Move-only ownership */
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;
    /** ------------------------------------------------------------------------------------------- Destructor */
    ~Shader();
    /** ------------------------------------------------------------------------------------------- Shader Functor invocation
     * @brief Runs the shader over a list of slices: Y is shaped to the list's length and
     * workgroup column `i` receives `slices[i]`, writing its results into that slice.
     * @param slices Device-visible slices, one per workgroup column.
     * @param count How many slices are bound.
     * @param callback Optional function invoked with each slice once the shader has completed.
     * @param workgroups Number of 64-invocation workgroups along X per slice.
     * @return A shared pointer to a LightweightSemaphore that can optionally be waited on until
     * the shader has completed execution.
     */
    std::shared_ptr<moodycamel::LightweightSemaphore> operator()(
        const Slice* slices,
        size_t count,
        void (*callback)(Slice slice) = nullptr,
        uint32_t workgroups = 1
    ) const;
    /** ------------------------------------------------------------------------------------------- Shader Functor invocation (one)
     * @brief Runs the shader over a single slice.
     * @param slice The device-visible slice, processed in place.
     * @param callback Optional function invoked with the slice once the shader has completed.
     * @param workgroups Number of 64-invocation workgroups along X.
     * @return A shared pointer to a LightweightSemaphore that can optionally be waited on until
     * the shader has completed execution.
     */
    std::shared_ptr<moodycamel::LightweightSemaphore> operator()(
        const Slice& slice,
        void (*callback)(Slice slice) = nullptr,
        uint32_t workgroups = 1
    ) const;
};
static_assert(sizeof(Shader) == sizeof(void*), "Shader's public ABI must remain one opaque pointer.");
/** --------------------------------------------------------------------------------------------------------- GPU struct
 * @struct GPU
 * @brief Encapsulates static methods used for GPU compute operations.
 */
struct GPU {
    /** ------------------------------------------------------------------------------------------- available
     * @brief True when a Vulkan compute device is present.
     * @return True when the GPU can execute programs.
     */
    static bool exists();
    /** ------------------------------------------------------------------------------------------- unified_memory
     * @brief True when the device shares memory with the CPU, so stream Slices bind with no
     * copies.
     * @return True under unified memory.
     */
    static bool unified_memory();
    /** ------------------------------------------------------------------------------------------- device_name
     * @brief The compute device's name, empty when no device is present.
     * @return The device name.
     */
    static std::string device_name();
    /** ------------------------------------------------------------------------------------------- encode
     * @brief Flattens a recorded program into one Slice: header, register seeds, constants, then
     * the instruction stream as a contiguous run of 8-byte instructions.
     * @param program The recorded program.
     * @return The encoded program; place it, move it, or hand it to run() below like any Slice.
     */
    static Slice encode(const Shader& program);
    /** ------------------------------------------------------------------------------------------- decode
     * @brief Rebuilds a recorded program from its encoded Slice.
     * @param program An encoded program.
     * @return The program.
     * @throw ShaderException when the Slice is not an encoded program of this version.
     */
    static Shader decode(const Slice& program);
    /** ------------------------------------------------------------------------------------------- run
     * @brief Executes a recorded program on the GPU over the bound streams.
     * @param program The recorded program.
     * @param streams One Slice per workgroup column; column i processes streams[i] in place.
     * @param stream_count How many Slices are bound; becomes the dispatch's Y extent.
     */
    static void run(const Shader& program, Slice* streams, size_t stream_count);
    /** ------------------------------------------------------------------------------------------- run
     * @brief Decodes an encoded program and executes it on the GPU.
     * @param program A compile_glsl program (job-table engine, one job per Slice).
     * @param streams One Slice per job.
     * @param stream_count How many jobs this round binds.
     */
    static void run(const Slice& program, Slice* streams, size_t stream_count);
    /** ------------------------------------------------------------------------------------------- compile_glsl
     * @brief Compiles a kernel body under the alligator prelude into SPIR-V words held in a Slice.
     * The body defines main() against the prelude's Slice-addressing helpers; the workgroup shape
     * (16x4) and the 8-byte job-table push block are injected.
     * @param body GLSL defining main(); the shape is injected, not authored.
     * @return The SPIR-V words, one per four bytes.
     * @throw GPUException when compilation fails or no device is present.
     */
    static Slice compile_glsl(std::string_view body);
};
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
/** --------------------------------------------------------------------------------------------------------- Vulkan Exception
 * @class VulkanException
 * @brief Exception type for Vulkan-related errors.
 */
class VulkanException : public threadsafe_logger::Exception {
public:
    using threadsafe_logger::Exception::Exception;
    using threadsafe_logger::Exception::what;
};
#define ALLIGATOR_GPU_THROW(msg) throw VulkanException(msg)
/** --------------------------------------------------------------------------------------------------------- Compile Vulkan GLSL
 * @brief Compiles a complete Vulkan 1.2 compute shader into optimized SPIR-V.
 * @param source Complete GLSL source.
 * @param float16 Whether to define ALLIGATOR_FLOAT16 for the source.
 * @param name Diagnostic shader name.
 * @return Optimized SPIR-V words.
 */
std::vector<uint32_t> compile_vulkan_glsl(
    std::string_view source,
    bool float16,
    std::string_view name
);
/** --------------------------------------------------------------------------------------------------------- DeviceProperties
 * @struct DeviceProperties
 * @brief Queried physical-device limits relevant to dispatch sizing and batch planning.
 */
struct DeviceProperties {
    uint64_t gpu_free_bytes = 0;        ///< Device-local headroom from the budget query (0 without it).
    std::string device_name;                   ///< Device name.
    uint32_t max_workgroup_count[3];           ///< Max workgroup counts per dispatch dimension.
    uint32_t max_workgroup_size[3];            ///< Max workgroup sizes.
    uint32_t max_workgroup_invocations;        ///< Max invocations per workgroup.
    uint32_t max_shared_memory;                ///< Max shared memory per workgroup (bytes).
    uint32_t subgroup_size;                    ///< SIMD subgroup width.
    uint64_t device_local_memory_bytes;        ///< Total device-local heap size.
    uint64_t host_visible_memory_bytes;        ///< Device-local heap size reachable from the host.
    uint64_t max_storage_buffer_range;         ///< Max storage buffer range.
    uint32_t max_push_constants;               ///< Max push constant bytes.
    bool supports_subgroup_arithmetic;         ///< Subgroup arithmetic ops available.
    bool supports_subgroup_shuffle;            ///< Subgroup shuffle ops available.
    bool supports_buffer_device_address;       ///< Device-resolvable buffer addresses.
    bool supports_int64;                       ///< 64-bit integers in shaders.
    bool supports_int64_atomics;               ///< 64-bit buffer atomics.
    bool supports_float16;                     ///< 16-bit float arithmetic in shaders.
    bool supports_memory_budget;               ///< Live device-memory headroom queryable.
    bool supports_timeline_semaphore;          ///< Timeline semaphores available.
    bool unified_memory;                       ///< Host and device share physical memory.
};
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
    uint64_t size_ = 0;            ///< Size in bytes.
    VulkanBuffer() = default;
    friend class VulkanContext;
    friend struct VulkanStaticMethods;
public:
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
    /** ------------------------------------------------------------------------------------------- No copy/move */
    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;
    VulkanBuffer(VulkanBuffer&&) = delete;
    VulkanBuffer& operator=(VulkanBuffer&&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Unmap and destroy the Vulkan objects. Defined in vulkan.cpp.
     */
    ~VulkanBuffer();
    /** ------------------------------------------------------------------------------------------- address
     * @brief Retrieves the device address of the Vulkan buffer.
     * @return The device address.
     */
    uint64_t address() const { return address_; }
    /** ------------------------------------------------------------------------------------------- size
     * @brief Retrieves the size of the Vulkan buffer.
     * @return The size of the buffer in bytes.
     */
    uint64_t size() const { return size_; }
    /** ------------------------------------------------------------------------------------------- host
     * @brief Retrieves the host pointer of the Vulkan buffer.
     * @return The host pointer.
     */
    void* host() const { return host_; }
};
/** --------------------------------------------------------------------------------------------------------- PlacementIndex
 * @enum PlacementIndex
 * @brief The memory-type ladder rungs `VulkanContext` resolves, one per Placemat below.
 */
enum class PlacementIndex : uint8_t {
    HOST = 0, HOST_VISIBLE = 1, HOST_CACHEABLE = 2, DEVICE = 3, UNIFIED = 4, BASIC_HEAP = 5, UNSPECIFIED = 6, COUNT = 7
};
/** --------------------------------------------------------------------------------------------------------- VulkanContext
 * @class VulkanContext
 * @brief Singleton owner of the process-wide Vulkan compute substrate.
 */
class VulkanContext {
private:
    vk::Instance instance_{};                    ///< Vulkan instance.
    vk::PhysicalDevice physical_device_{};       ///< Selected physical device.
    vk::Device device_{};                        ///< Logical device.
    std::vector<vk::Queue> compute_queues_;      ///< Queues flattened in compute-family order.
    std::vector<uint32_t> compute_families_;     ///< Unique compute-capable family indices.
    std::vector<uint32_t> queue_family_slots_;   ///< Compute-family slot for each queue.
    std::unique_ptr<std::atomic_flag[]> queue_claims_; ///< Exclusive thread leases when driver synchronization is absent.
    inline static std::atomic<uint32_t> next_queue_slot_{0};  ///< Hands each producer thread a sticky queue slot.
    bool internally_synchronized_queues_ = false;  ///< Probed VK_KHR_internally_synchronized_queues feature.
    vk::PhysicalDeviceMemoryProperties memory_properties_{};  ///< Queried once at init.
    vk::PipelineCache pipeline_cache_{};         ///< Driver pipeline cache (in-process).
    DeviceProperties device_props_{};            ///< Queried device limits and features.
    /// @brief Usage flags every VulkanBuffer slab is created with.
    vk::BufferUsageFlags buffer_usage_{};
    /// @brief Allocation-flags chain (device address) shared by every slab allocation.
    vk::MemoryAllocateFlagsInfo allocate_flags_{};
    /// @brief Memory type resolved once for `buffer_usage_` (DEVICE_LOCAL|HOST_VISIBLE preferred).
    uint32_t buffer_memory_type_index_ = UINT32_MAX;
    /// @brief Memory type per Placement value (indexed by the Placement enum's underlying value).
    std::array<uint32_t, static_cast<size_t>(PlacementIndex::COUNT)> placement_type_indices_{};
    /// @brief Whether each Placement's memory type is HOST_CACHED (CPU reads at RAM speed).
    std::array<bool, static_cast<size_t>(PlacementIndex::COUNT)> placement_cpu_cached_{};
    /** ------------------------------------------------------------------------------------------- TransferUnit
     * @struct TransferUnit
     * @brief Per-thread one-shot transfer unit for slab-to-slab copies (egress staging).
     * Command pools stay externally synchronized with no opt-out, so each producer thread
     * owns its own pool, command buffer, and fence.
     */
    struct TransferUnit {
        vk::CommandPool pool{};       ///< This thread's command pool.
        vk::CommandBuffer command{};  ///< This thread's one-shot recording buffer.
        vk::Fence fence{};            ///< Signalled when this thread's copy retires.
    };
    /** ------------------------------------------------------------------------------------------- make_transfer_unit
     * @brief Create this thread's TransferUnit from its own pool.
     */
    static TransferUnit make_transfer_unit();
    /** ------------------------------------------------------------------------------------------- transfer_unit
     * @brief This thread's transfer unit, created on first use. Handles are reclaimed by
     * vkDestroyDevice at teardown, never individually.
     */
    static TransferUnit& transfer_unit();
    /// @brief True if the device is UMA (unified memory architecture) and supports
    /// host-visible device-local buffers.
    bool portability_available_ = false;
    /// @brief True if a Vulkan device was successfully created and is present.
    bool device_present_ = false;
    /// @brief False once the context destructor begins; slab teardown after that skips the device.
    inline static std::atomic<bool> alive_{false};
    /** ------------------------------------------------------------------------------------------- unified_from_memory_properties
     * @brief The one-query UMA test: a memory type carrying both DEVICE_LOCAL and HOST_VISIBLE
     * whose heap is device-local.
     * @param properties The queried memory properties.
     * @return True on unified-memory systems.
     */
    static bool unified_from_memory_properties(
        const vk::PhysicalDeviceMemoryProperties& properties
    );
    /** ------------------------------------------------------------------------------------------- shared_buffer_info
     * @brief Creates buffer metadata using the device's fixed compute-family sharing policy.
     */
    vk::BufferCreateInfo shared_buffer_info(vk::DeviceSize bytes, vk::BufferUsageFlags usage) const;
    /** ------------------------------------------------------------------------------------------- try_find_memory_type
     * @brief Find a memory type index matching the filter and all wanted flags.
     * @param type_filter Bitmask of allowed type indices (from VkMemoryRequirements).
     * @param wanted Required property flags.
     * @param excluded Property flags the type must not carry.
     * @return The type index, or UINT32_MAX when none matches.
     */
    uint32_t try_find_memory_type(
        uint32_t type_filter,
        vk::MemoryPropertyFlags wanted,
        vk::MemoryPropertyFlags excluded = {}
    ) const;
    /** ------------------------------------------------------------------------------------------- constructor
     * @brief Create the context and register it as the process GPU backend. Requests only 2
     * host-side workers from the inherited CPUCompute pool (submission/callback plumbing) rather
     * than one per hardware thread - a GPU context does not run compute on the CPU worker pool,
     * so hardware_concurrency() workers would sit idle for the service's entire lifetime.
     */
    VulkanContext();
    /** ------------------------------------------------------------------------------------------- poll_budget_headroom
     * @brief Sum the device-local budget headroom (budget - usage per heap) via VK_EXT_memory_budget.
     * Re-queryable at any time - the driver recomputes budget/usage per call.
     * @return Free device-local bytes, or 0 when the budget extension is absent.
     */
    uint64_t poll_budget_headroom() const;
    /// @brief Friend classes that need to access the private Vulkan objects and methods.
    friend class VulkanBuffer;
    friend struct VulkanStaticMethods;
    friend class VulkanPipeline;
    friend class Arena;
    friend class VulkanKernel;
    friend class ShaderState;
public:
    /** ------------------------------------------------------------------------------------------- Singleton instance
     * @brief The singleton VulkanCompute instance.
     */
    static VulkanContext& instance() {
        static VulkanContext inst;
        return inst;
    }
    /** ------------------------------------------------------------------------------------------- destructor
     * @brief Drain the device and tear down. Caller-owned buffers, rigs, and pipelines must
     * already be destroyed.
     */
    ~VulkanContext();
    /** ------------------------------------------------------------------------------------------- device_properties */
    static const DeviceProperties& device_properties() { return instance().device_props_; }
    /** ------------------------------------------------------------------------------------------- device
     * @brief The logical device, for teardown and object creation outside the friend set
     * (the job-table engine's state lives in an anonymous namespace and cannot be friended).
     */
    static vk::Device device() { return instance().device_; }
    /** ------------------------------------------------------------------------------------------- pipeline_cache
     * @brief The shared driver pipeline cache for one-time kernel preparation.
     */
    static vk::PipelineCache pipeline_cache() { return instance().pipeline_cache_; }
    /// Reserve a queue for this thread's lifetime. Never wrap onto an unsynchronized queue.
    /// Kernels acquire their lease during controller startup, before accepting requests.
    static uint32_t submission_queue_index();
    /** ------------------------------------------------------------------------------------------- submit_command_buffer
     * @brief Reset the fence and submit to this thread's sticky compute queue, lock-free.
     * @param command_buffer The recorded command buffer.
     * @param fence The submitter's fence, signalled on retirement.
     * @param callback Reserved by the in-flight callback scaffolding; pass nullptr.
     * @param callback_context Reserved; pass nullptr.
     */
    static void submit_command_buffer(
        vk::CommandBuffer command_buffer,
        vk::Fence fence,
        void (*callback)(void*),
        void* callback_context
    );
    /** ------------------------------------------------------------------------------------------- buffer_memory_type_index
     * @brief The resolved host-coherent storage-buffer memory type for the job-table engine.
     */
    static uint32_t buffer_memory_type_index() { return instance().buffer_memory_type_index_; }
    /** ------------------------------------------------------------------------------------------- allocate_flags
     * @brief The device-address allocation flags chain, by reference for pNext wiring.
     */
    static const vk::MemoryAllocateFlagsInfo& allocate_flags() { return instance().allocate_flags_; }
    /** ------------------------------------------------------------------------------------------- queue_family_index
     * @brief The compute queue family leased by this submitting thread.
     */
    static uint32_t queue_family_index();
    /** ------------------------------------------------------------------------------------------- submission_family_slot
     * @brief The submitting thread's index into the prepared compute-family command buffers.
     */
    static uint32_t submission_family_slot();
    /** ------------------------------------------------------------------------------------------- compute_families
     * @brief Unique compute-capable family indices prepared on the logical device.
     */
    static const std::vector<uint32_t>& compute_families() { return instance().compute_families_; }
    /** ------------------------------------------------------------------------------------------- buffer_create_info
     * @brief Shares a buffer across every compute family when the device exposes more than one.
     */
    static vk::BufferCreateInfo buffer_create_info(vk::DeviceSize bytes, vk::BufferUsageFlags usage);
    /** ------------------------------------------------------------------------------------------- placement_cpu_cached
     * @brief Whether a Placement's memory type reads at RAM speed from the CPU.
     * @param placement_index The Placement enum's underlying value.
     * @return True when the type is HOST_CACHED.
     */
    static bool placement_cpu_cached(uint8_t placement_index) {
        return instance().placement_cpu_cached_[placement_index];
    }
    /** ------------------------------------------------------------------------------------------- buffer_placement
     * @brief The placement resolved to the storage-buffer memory type the capability ladder
     * probed at init - the same type the job table and parameter buffers verify against their
     * memory requirements. Never a named-rung assumption: rung 6 carries the probed index, and
     * context init threw already if no host-coherent type exists. Heap without a device.
     * @return The placement.
     */
    static const Placemat* buffer_placement();
    /** ------------------------------------------------------------------------------------------- device_present
     * @brief Cheap "is a physical GPU present" probe. Software implementations do not count.
     * @return True when at least one non-CPU Vulkan device exists.
     */
    static bool device_present() {
        return instance().device_present_;
    }
    /** ------------------------------------------------------------------------------------------- queue_count
     * @brief Compute queues across all compute families, for GPU worker-count decisions.
     * @return Queue count, or 0 without a device.
     */
    static uint32_t queue_count();
    /** ------------------------------------------------------------------------------------------- internally_synchronized_queues
     * @brief Whether the compute family was created with VK_KHR_internally_synchronized_queues,
     * so producers need no external queue sync.
     * @return True when the feature is enabled.
     */
    static bool internally_synchronized_queues() {
        return instance().internally_synchronized_queues_;
    }
    /** ------------------------------------------------------------------------------------------- device_free_bytes
     * @brief Live device-local budget headroom, polled from the driver at call time.
     * @return Free device-local bytes, or 0 without a device or the budget extension.
     */
    static uint64_t device_free_bytes();
    /** ------------------------------------------------------------------------------------------- device_unified
     * @brief Whether the first non-CPU device is unified-memory, via the one-query memory-type
     * test. Cached after the first probe.
     * @return True on unified-memory systems.
     */
    static bool device_unified() {
        return instance().device_properties().unified_memory;
    }
    /** ------------------------------------------------------------------------------------------- exec_dim_reduce
     * @brief Determine the optimal execution dimension reduction for a given work item count.
     * @param work_item_count The total number of work items.
     * @return A pair containing the primary and optional secondary reduction factors.
     */
    static constexpr std::pair<size_t, std::optional<size_t>> exec_dim_reduce(
        size_t work_item_count
    ) {
        if ((work_item_count & 0xF) == 0) return {16, std::nullopt};
        if ((work_item_count % 15) == 0) return {15, std::nullopt};
        if ((work_item_count % 12) == 0) return {12, std::nullopt};
        if ((work_item_count % 11) == 0) return {11, std::nullopt};
        if ((work_item_count % 10) == 0) return {10, std::nullopt};
        if ((work_item_count % 9) == 0) return {9, std::nullopt};
        if ((work_item_count & 0x7) == 0) return {8, std::nullopt};
        if ((work_item_count % 7) == 0) return {7, std::nullopt};
        if ((work_item_count % 6) == 0) return {6, std::nullopt};
        if ((work_item_count % 5) == 0) return {5, std::nullopt};
        if ((work_item_count % 4) == 0) return {4, std::nullopt};
        if ((work_item_count % 3) == 0) return {3, std::nullopt};
        if ((work_item_count % 2) == 0) return {2, std::nullopt};
        if ((work_item_count & 0xF) == 0xF) return {16, 15};
        if ((work_item_count % 15) == 14) return {15, 14};
        if ((work_item_count % 12) == 11) return {12, 11};
        if ((work_item_count % 11) == 10) return {11, 10};
        if ((work_item_count % 10) == 9) return {10, 9};
        if ((work_item_count % 9) == 8) return {9, 8};
        if ((work_item_count & 0x7) == 7) return {8, 7};
        if ((work_item_count % 7) == 6) return {7, 6};
        if ((work_item_count % 6) == 5) return {6, 5};
        if ((work_item_count % 5) == 4) return {5, 4};
        if ((work_item_count % 4) == 3) return {4, 3};
        if ((work_item_count % 3) == 2) return {3, 2};
        return {2, 1};
    }
};
/** --------------------------------------------------------------------------------------------------------- VulkanKernel
 * @brief The Vulkan substrate's shared device utilities (the old push-constant pipeline/dispatch
 * surface was deleted with the condemned lanes; kernels reach the device through gpu.hpp).
 */
/** --------------------------------------------------------------------------------------------------------- VulkanKernel
 * @class VulkanKernel
 * @brief Static surface over the Vulkan substrate for compute SPIR-V.
 */
class VulkanKernel {
public:
    /** ------------------------------------------------------------------------------------------- available
     * @brief True when a Vulkan compute device is present.
     * @return True if a Vulkan compute device is available, otherwise false.
     */
    static bool available();
    /** ------------------------------------------------------------------------------------------- device_name
     * @brief The device's name, empty without a device.
     * @return The name of the Vulkan device if present, otherwise an empty string.
     */
    static std::string device_name();
    /** ------------------------------------------------------------------------------------------- device_address
     * @brief The device address of a Slice's first byte, 0 when its memory is not GPU-visible.
     * @param slice The Slice for which to retrieve the device address.
     * @return The device address of the Slice's first byte if GPU-visible, otherwise 0.
     */
    static uint64_t device_address(const Slice& slice);
    /** ------------------------------------------------------------------------------------------- gpu_usage
     * @brief Bytes currently held in Vulkan slabs.
     * @return The number of bytes currently allocated on the Vulkan device.
     */
    static uint64_t gpu_usage();
    /** ------------------------------------------------------------------------------------------- gpu_pool_address
     * @brief The device address of the alligator's GPUBuf table for shader-side slice
     * resolution; 0 while the table is host-only.
     * @return The table's device address.
     */
    static uint64_t gpu_pool_address();
    /** ------------------------------------------------------------------------------------------- table_placement
     * @brief The placement the shared GPUBuf table lives on: the coherent zero-copy rung with a
     * device, plain heap without one.
     * @return The placement.
     */
    static const Placemat* table_placement();
};
/** --------------------------------------------------------------------------------------------------------- ShaderState
 * @brief Shared prepared dispatch state for Shader and Kernel reference rings.
 */
/** --------------------------------------------------------------------------------------------------------- GPU Stage
 * @struct KernelGpuStage
 * @brief One dispatch in a Kernel's prepared GPU command sequence.
 */
struct KernelGpuStage {
    std::string_view glsl;      ///< The stage's GLSL source.
    uint32_t workgroups_x = 1;  ///< Workgroups along X.
    uint32_t workgroups_y = 1;  ///< Workgroups along Y.
    bool per_request = true;    ///< Whether the stage dispatches once per request.
};
/** --------------------------------------------------------------------------------------------------------- ShaderState
 * @class ShaderState
 * @brief Shared prepared resources for Shader, legacy jobs, and Kernel reference rings.
 * One controller owns an instance; dispatches on the same instance must not overlap.
 */
class ShaderState {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    /** ------------------------------------------------------------------------------------------- Constructor - Source
     * @brief Compiles GLSL and prepares every Vulkan resource used by subsequent dispatches.
     * @param source GLSL defining `void alligator_main(Slice slice)`.
     * @param name Diagnostic name reported by shader compilation errors.
     * @param references Optional reference list the shader indexes.
     * @param workgroups_x Workgroups along X for reference dispatches.
     */
    ShaderState(std::string_view source, std::string_view name,
        const Slice* references = nullptr, uint32_t workgroups_x = 1);
    /** ------------------------------------------------------------------------------------------- Constructor - Words
     * @brief Prepares dispatch state from already-compiled SPIR-V words.
     * @param words The SPIR-V words.
     * @param count The word count.
     * @param name Diagnostic name.
     * @param max_jobs The job-table capacity.
     */
    ShaderState(const uint32_t* words, size_t count, std::string_view name, size_t max_jobs);
    /** ------------------------------------------------------------------------------------------- Constructor - Stages
     * @brief Compiles and chains one dispatch per stage for Kernel command sequences.
     * @param stages The stage descriptors.
     * @param name Diagnostic name.
     * @param references The reference list the stages index.
     * @param resources The resource count published to the stages.
     */
    ShaderState(std::span<const KernelGpuStage> stages, std::string_view name,
        const Slice& references, uint32_t resources);
    ~ShaderState();
    /** ------------------------------------------------------------------------------------------- SPIR-V
     * @brief The compiled SPIR-V words.
     * @return The words.
     */
    const std::vector<uint32_t>& spirv() const;
    /** ------------------------------------------------------------------------------------------- Name
     * @brief The diagnostic name.
     * @return The name.
     */
    const std::string& name() const;
    /** ------------------------------------------------------------------------------------------- Capacity
     * @brief The job-table capacity.
     * @return The capacity.
     */
    size_t capacity() const;
    /** ------------------------------------------------------------------------------------------- Dispatch
     * @brief Binds one slice per workgroup column and dispatches.
     * @param streams The slices to bind.
     * @param count The stream count.
     * @param workgroups The workgroup count.
     */
    void dispatch(const Slice* streams, size_t count, uint32_t workgroups) const;
    /** ------------------------------------------------------------------------------------------- Write Job
     * @brief Publishes one job-table slot.
     * @param slot The slot index.
     * @param input The job's input slice.
     * @param host_handle The host handle mirrored to the device.
     */
    void write_job(size_t slot, const Slice& input, const void* host_handle);
    /** ------------------------------------------------------------------------------------------- Dispatch Jobs
     * @brief Dispatches the megakernel over the job table.
     * @param jobs The number of published jobs.
     */
    void dispatch(size_t jobs);
    /** ------------------------------------------------------------------------------------------- Dispatch References
     * @brief Dispatches the reference ring over a range.
     * @param first The first reference index.
     * @param count The reference count.
     */
    void dispatch_references(uint32_t first, uint32_t count);
};
/** --------------------------------------------------------------------------------------------------------- Vulkan GLSL Kernel Prelude
 * @brief GLSL prelude and host-side mirrors for Buffet Alligator's persistent megakernel dispatch.
 *
 * GPU-side model: one push constant (the job table address), one workgroup shape (16, 4, jobs).
 * The table holds one 16-byte descriptor per stage; entry k points at stage k's job array, an
 * array of 32-byte job records (a 16-byte input descriptor plus 16 bytes of host-only handle).
 * The job count arrives as gl_NumWorkGroups.z via indirect dispatch — nothing is pushed per
 * round, and the command buffer never changes.
 *
 * X (16) is the conceptual element lane, or area to shade. This number may change to 8 in the
 *     future so that Y and Z may be scaled up.
 * Y (4) is the author's split, free to mean whatever the kernel wants, as long as the value
 *     is not 1. The shape (N, 1, 1) is prohibited.
 * Z (jobs) is one workgroup per job; the invocation's z is its job index.
 */
/** --------------------------------------------------------------------------------------------------------- VULKAN_GLSL_KERNEL_CORE
 * @brief The 16-byte Slice descriptor mirror and every load/store helper over it. Contains no
 * #version line so it can be injected after a user's own, and is idempotent via its guard.
 */
inline constexpr std::string_view VULKAN_GLSL_KERNEL_CORE = R"glsl(#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int32 : require
#ifdef VULKAN_FLOAT16
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#endif
#ifndef VULKAN_GLSL_KERNEL_CORE_INCLUDED
#define VULKAN_GLSL_KERNEL_CORE_INCLUDED 1
// ------------------------------------------------------------------------------------------------- Kernel push block (16 bytes)
// One push block for every kernel family: the per-engine table address (the megakernel's job
// table, the public Shader's parameters list) and the global GPUBufRef pool's device address.
layout(push_constant) uniform AlligatorPush {
    uint64_t vulkan_table_address;  ///< This engine's table: job records or the parameters list
    uint64_t vulkan_pool_address;   ///< The global GPUBufRef table, shared host and device
} vulkan_push;
// ------------------------------------------------------------------------------------------------- Slice (16 bytes)
struct Slice {
    uint64_t device_address;  ///< The slab's device address
    uint32_t size;            ///< The claim's size in bytes
    uint32_t offset;          ///< The claim's byte offset within the slab
};
layout(buffer_reference, std430, buffer_reference_align = 8) buffer SliceRef {
    uint64_t device_address;
    uint32_t size;
    uint32_t offset;
};
// ------------------------------------------------------------------------------------------------- GPUBufRef pool (16-byte entries)
// The device half of the index-linked pool; host-side state lives in the CPUBufRef array at the same index.
struct GPUBufRef {
    uint64_t address;      ///< The slice's absolute device address
    uint32_t size;         ///< The slice's size in bytes
    uint32_t offset;       ///< The slice's byte offset within its slab, informational
};
layout(buffer_reference, std430, buffer_reference_align = 16) buffer GPUBufRefArray {
    GPUBufRef refs[];
};
Slice gpu_slice(uint32_t index) {
    GPUBufRef ref = GPUBufRefArray(vulkan_push.vulkan_pool_address).refs[index];
    Slice s;
    s.device_address = ref.address;
    s.size = ref.size;
    s.offset = 0u;
    return s;
}
uint64_t gpu_slice_address(uint32_t index) { return gpu_slice(index).device_address; }
uint gpu_slice_size(uint32_t index) {
    return GPUBufRefArray(vulkan_push.vulkan_pool_address).refs[index].size;
}
layout(buffer_reference, std430, buffer_reference_align = 4) buffer U32Array { uint v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer I32Array { int v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer F32Array { float v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) buffer U64Array { uint64_t v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) buffer I64Array { int64_t v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer F32x4Array { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer U32x4Array { uvec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer I32x4Array { ivec4 v[]; };
// ------------------------------------------------------------------------------------------------- Slice basics
uint64_t slice_address(Slice s) { return s.device_address + uint64_t(s.offset); }
bool slice_is_null(Slice s) { return s.size == 0u || s.device_address == 0ul; }
uint slice_size(Slice s) { return s.size; }
Slice slice_sub(Slice s, uint64_t offset, uint64_t length) {
    Slice r = s;
    r.offset = s.offset + uint32_t(offset);
    r.size = uint32_t(length);
    return r;
}
Slice slice_read(uint64_t address) {
    SliceRef ref = SliceRef(address);
    Slice s;
    s.device_address = ref.device_address;
    s.size = ref.size;
    s.offset = ref.offset;
    return s;
}
// ------------------------------------------------------------------------------------------------- Word-native loads
uint  slice_load_u32(Slice s, uint index) { return U32Array(slice_address(s)).v[index]; }
int   slice_load_i32(Slice s, uint index) { return I32Array(slice_address(s)).v[index]; }
float slice_load_f32(Slice s, uint index) { return F32Array(slice_address(s)).v[index]; }
uint64_t slice_load_u64(Slice s, uint index) { return U64Array(slice_address(s)).v[index]; }
int64_t  slice_load_i64(Slice s, uint index) { return I64Array(slice_address(s)).v[index]; }
vec4  slice_load_f32x4(Slice s, uint index) { return F32x4Array(slice_address(s)).v[index]; }
uvec4 slice_load_u32x4(Slice s, uint index) { return U32x4Array(slice_address(s)).v[index]; }
ivec4 slice_load_i32x4(Slice s, uint index) { return I32x4Array(slice_address(s)).v[index]; }
// ------------------------------------------------------------------------------------------------- Word-native stores
void slice_store_u32(Slice s, uint index, uint value) { U32Array(slice_address(s)).v[index] = value; }
void slice_store_i32(Slice s, uint index, int value) { I32Array(slice_address(s)).v[index] = value; }
void slice_store_f32(Slice s, uint index, float value) { F32Array(slice_address(s)).v[index] = value; }
void slice_store_u64(Slice s, uint index, uint64_t value) { U64Array(slice_address(s)).v[index] = value; }
void slice_store_i64(Slice s, uint index, int64_t value) { I64Array(slice_address(s)).v[index] = value; }
void slice_store_f32x4(Slice s, uint index, vec4 value) { F32x4Array(slice_address(s)).v[index] = value; }
void slice_store_u32x4(Slice s, uint index, uvec4 value) { U32x4Array(slice_address(s)).v[index] = value; }
void slice_store_i32x4(Slice s, uint index, ivec4 value) { I32x4Array(slice_address(s)).v[index] = value; }
// ------------------------------------------------------------------------------------------------- Sub-word loads (no 8/16-bit storage feature needed)
uint slice_load_u16(Slice s, uint index) {
    uint word = U32Array(slice_address(s) + uint64_t((index * 2u) & ~3u)).v[0];
    return (word >> ((index & 1u) * 16u)) & 0xFFFFu;
}
int slice_load_i16(Slice s, uint index) {
    uint word = U32Array(slice_address(s) + uint64_t((index * 2u) & ~3u)).v[0];
    return bitfieldExtract(int(word), int((index & 1u) * 16u), 16);
}
uint slice_load_u8(Slice s, uint index) {
    uint word = U32Array(slice_address(s) + uint64_t(index & ~3u)).v[0];
    return (word >> ((index & 3u) * 8u)) & 0xFFu;
}
int slice_load_i8(Slice s, uint index) {
    uint word = U32Array(slice_address(s) + uint64_t(index & ~3u)).v[0];
    return bitfieldExtract(int(word), int((index & 3u) * 8u), 8);
}
float slice_load_f16(Slice s, uint index) {
    uint word = U32Array(slice_address(s) + uint64_t((index * 2u) & ~3u)).v[0];
    return unpackHalf2x16(word)[index & 1u];
}
float slice_load_bf16(Slice s, uint index) { return uintBitsToFloat(slice_load_u16(s, index) << 16u); }
float slice_load_e5m2(Slice s, uint index) { return unpackHalf2x16(slice_load_u8(s, index) << 8u).x; }
// ------------------------------------------------------------------------------------------------- Sub-word stores (atomic read-modify-write so neighbouring lanes never clobber each other)
void slice_store_u16(Slice s, uint index, uint value) {
    U32Array words = U32Array(slice_address(s) + uint64_t((index * 2u) & ~3u));
    uint shift = (index & 1u) * 16u;
    atomicAnd(words.v[0], ~(0xFFFFu << shift));
    atomicOr(words.v[0], (value & 0xFFFFu) << shift);
}
void slice_store_u8(Slice s, uint index, uint value) {
    U32Array words = U32Array(slice_address(s) + uint64_t(index & ~3u));
    uint shift = (index & 3u) * 8u;
    atomicAnd(words.v[0], ~(0xFFu << shift));
    atomicOr(words.v[0], (value & 0xFFu) << shift);
}
void slice_store_i16(Slice s, uint index, int value) { slice_store_u16(s, index, uint(value)); }
void slice_store_i8(Slice s, uint index, int value) { slice_store_u8(s, index, uint(value)); }
void slice_store_f16(Slice s, uint index, float value) { slice_store_u16(s, index, packHalf2x16(vec2(value, 0.0)) & 0xFFFFu); }
void slice_store_bf16(Slice s, uint index, float value) { slice_store_u16(s, index, floatBitsToUint(value) >> 16u); }
void slice_store_e5m2(Slice s, uint index, float value) { slice_store_u8(s, index, (packHalf2x16(vec2(value, 0.0)) >> 8u) & 0xFFu); }
// ------------------------------------------------------------------------------------------------- 8-bit float codes: IEEE-style fields, bias 2^(E-1)-1, truncated from fp16 like e5m2 (shift and mask only)
uint fp8_to_f16_bits(uint code, uint mantissa_bits) { return ((code & 0x80u) << 8u) | ((code & 0x7Fu) << (10u - mantissa_bits)); }
uint f16_bits_to_fp8(uint half_bits, uint mantissa_bits) { return ((half_bits >> 8u) & 0x80u) | ((half_bits >> (10u - mantissa_bits)) & 0x7Fu); }
uvec4 fp8x4_codes(uint word) { return uvec4(word & 0xFFu, (word >> 8u) & 0xFFu, (word >> 16u) & 0xFFu, word >> 24u); }
vec4 fp8x4_to_f32x4(uint word, uint mantissa_bits, float scale) {
    uvec4 codes = fp8x4_codes(word);
    uint lo = fp8_to_f16_bits(codes.x, mantissa_bits) | (fp8_to_f16_bits(codes.y, mantissa_bits) << 16u);
    uint hi = fp8_to_f16_bits(codes.z, mantissa_bits) | (fp8_to_f16_bits(codes.w, mantissa_bits) << 16u);
    return vec4(unpackHalf2x16(lo), unpackHalf2x16(hi)) * scale;
}
uint fp8x4_from_f32x4(vec4 values, uint mantissa_bits, float inverse_scale) {
    uint lo = packHalf2x16(values.xy * inverse_scale);
    uint hi = packHalf2x16(values.zw * inverse_scale);
    return f16_bits_to_fp8(lo & 0xFFFFu, mantissa_bits) | (f16_bits_to_fp8(lo >> 16u, mantissa_bits) << 8u)
        | (f16_bits_to_fp8(hi & 0xFFFFu, mantissa_bits) << 16u) | (f16_bits_to_fp8(hi >> 16u, mantissa_bits) << 24u);
}
float e4m3_to_f32(uint code) { return unpackHalf2x16(fp8_to_f16_bits(code, 3u)).x * 256.0; }
float e3m4_to_f32(uint code) { return unpackHalf2x16(fp8_to_f16_bits(code, 4u)).x * 4096.0; }
uint f32_to_e4m3(float value) { return f16_bits_to_fp8(packHalf2x16(vec2(value * 0.00390625, 0.0)) & 0xFFFFu, 3u); }
uint f32_to_e3m4(float value) { return f16_bits_to_fp8(packHalf2x16(vec2(value * 0.000244140625, 0.0)) & 0xFFFFu, 4u); }
float slice_load_e4m3(Slice s, uint index) { return e4m3_to_f32(slice_load_u8(s, index)); }
float slice_load_e3m4(Slice s, uint index) { return e3m4_to_f32(slice_load_u8(s, index)); }
void slice_store_e4m3(Slice s, uint index, float value) { slice_store_u8(s, index, f32_to_e4m3(value)); }
void slice_store_e3m4(Slice s, uint index, float value) { slice_store_u8(s, index, f32_to_e3m4(value)); }
// ------------------------------------------------------------------------------------------------- Four-wide sub-word loads and stores (index4 counts groups of four elements; whole words, no atomics)
vec4 slice_load_f16x4(Slice s, uint index4) {
    uint64_t pair = slice_load_u64(s, index4);
    return vec4(unpackHalf2x16(uint(pair)), unpackHalf2x16(uint(pair >> 32u)));
}
vec4 slice_load_bf16x4(Slice s, uint index4) {
    uint64_t pair = slice_load_u64(s, index4);
    uint lo = uint(pair);
    uint hi = uint(pair >> 32u);
    return vec4(uintBitsToFloat(lo << 16u), uintBitsToFloat(lo & 0xFFFF0000u), uintBitsToFloat(hi << 16u), uintBitsToFloat(hi & 0xFFFF0000u));
}
vec4 slice_load_e5m2x4(Slice s, uint index4) { return fp8x4_to_f32x4(slice_load_u32(s, index4), 2u, 1.0); }
vec4 slice_load_e4m3x4(Slice s, uint index4) { return fp8x4_to_f32x4(slice_load_u32(s, index4), 3u, 256.0); }
vec4 slice_load_e3m4x4(Slice s, uint index4) { return fp8x4_to_f32x4(slice_load_u32(s, index4), 4u, 4096.0); }
void slice_store_f16x4(Slice s, uint index4, vec4 values) {
    slice_store_u64(s, index4, uint64_t(packHalf2x16(values.xy)) | (uint64_t(packHalf2x16(values.zw)) << 32u));
}
void slice_store_bf16x4(Slice s, uint index4, vec4 values) {
    uvec4 bits = floatBitsToUint(values) >> 16u;
    slice_store_u64(s, index4, uint64_t(bits.x | (bits.y << 16u)) | (uint64_t(bits.z | (bits.w << 16u)) << 32u));
}
void slice_store_e5m2x4(Slice s, uint index4, vec4 values) {
    uint lo = packHalf2x16(values.xy);
    uint hi = packHalf2x16(values.zw);
    slice_store_u32(s, index4, ((lo >> 8u) & 0xFFu) | ((lo >> 16u) & 0xFF00u) | ((hi & 0xFF00u) << 8u) | (hi & 0xFF000000u));
}
void slice_store_e4m3x4(Slice s, uint index4, vec4 values) { slice_store_u32(s, index4, fp8x4_from_f32x4(values, 3u, 0.00390625)); }
void slice_store_e3m4x4(Slice s, uint index4, vec4 values) { slice_store_u32(s, index4, fp8x4_from_f32x4(values, 4u, 0.000244140625)); }
#ifdef VULKAN_FLOAT16
// ------------------------------------------------------------------------------------------------- Four-wide loads widened to float16_t (shaderFloat16 devices)
f16vec4 fp8x4_to_f16x4(uint word, uint mantissa_bits, float16_t scale) {
    uvec4 codes = fp8x4_codes(word);
    uint lo = fp8_to_f16_bits(codes.x, mantissa_bits) | (fp8_to_f16_bits(codes.y, mantissa_bits) << 16u);
    uint hi = fp8_to_f16_bits(codes.z, mantissa_bits) | (fp8_to_f16_bits(codes.w, mantissa_bits) << 16u);
    return f16vec4(unpackFloat2x16(lo), unpackFloat2x16(hi)) * scale;
}
f16vec4 slice_load_f32x4_half(Slice s, uint index4) { return f16vec4(slice_load_f32x4(s, index4)); }
f16vec4 slice_load_f16x4_half(Slice s, uint index4) {
    uint64_t pair = slice_load_u64(s, index4);
    return f16vec4(unpackFloat2x16(uint(pair)), unpackFloat2x16(uint(pair >> 32u)));
}
f16vec4 slice_load_bf16x4_half(Slice s, uint index4) { return f16vec4(slice_load_bf16x4(s, index4)); }
f16vec4 slice_load_e5m2x4_half(Slice s, uint index4) { return fp8x4_to_f16x4(slice_load_u32(s, index4), 2u, float16_t(1.0)); }
f16vec4 slice_load_e4m3x4_half(Slice s, uint index4) { return fp8x4_to_f16x4(slice_load_u32(s, index4), 3u, float16_t(256.0)); }
f16vec4 slice_load_e3m4x4_half(Slice s, uint index4) { return fp8x4_to_f16x4(slice_load_u32(s, index4), 4u, float16_t(4096.0)); }
#endif
#endif
)glsl";
/** --------------------------------------------------------------------------------------------------------- VULKAN_GLSL_KERNEL_TAIL
 * @brief The megakernel tail: the job-table push block, the stage slot, the job decoder, and
 * the lane/part/job helpers. The host injects the workgroup shape; the author writes the loop
 * body against these helpers.
 */
inline constexpr std::string_view VULKAN_GLSL_KERNEL_TAIL = R"glsl(
#ifndef VULKAN_STAGE
#define VULKAN_STAGE 0u
#endif
// ------------------------------------------------------------------------------------------------- Job decoding
// The push block (table address + GPUBufRef pool) is declared in the kernel core above.
// A job record is 32 bytes: the input Slice descriptor followed by 16 bytes of host-only handle.
Slice vulkan_job_array() {
    return slice_read(vulkan_push.vulkan_table_address + uint64_t(VULKAN_STAGE) * 16ul);
}
Slice vulkan_job(uint job_index) {
    return slice_read(slice_address(vulkan_job_array()) + uint64_t(job_index) * 32ul);
}
// ------------------------------------------------------------------------------------------------- Shape helpers
uint vulkan_lane() { return gl_GlobalInvocationID.x; }        ///< X: element lane, 0..15
uint vulkan_part() { return gl_GlobalInvocationID.y; }        ///< Y: the author's split, 0..3
uint vulkan_job_index() { return gl_GlobalInvocationID.z; }   ///< Z: this invocation's job
uint vulkan_job_count() { return gl_NumWorkGroups.z; }        ///< Live jobs this dispatch
// ------------------------------------------------------------------------------------------------- Workgroup tree reduction (64 invocations)
// Every invocation must call this (the barriers are workgroup-wide); non-contributing parts
// pass 0.0. The total is valid at lane 0 of part 0.
shared float vulkan_reduce_scratch[64];
float vulkan_group_reduce_sum(float partial) {
    uint index = gl_LocalInvocationIndex;
    vulkan_reduce_scratch[index] = partial;
    barrier();
    if (index < 32u) vulkan_reduce_scratch[index] += vulkan_reduce_scratch[index + 32u];
    barrier();
    if (index < 16u) vulkan_reduce_scratch[index] += vulkan_reduce_scratch[index + 16u];
    barrier();
    if (index < 8u) vulkan_reduce_scratch[index] += vulkan_reduce_scratch[index + 8u];
    barrier();
    if (index < 4u) vulkan_reduce_scratch[index] += vulkan_reduce_scratch[index + 4u];
    barrier();
    if (index < 2u) vulkan_reduce_scratch[index] += vulkan_reduce_scratch[index + 2u];
    barrier();
    if (index == 0u) vulkan_reduce_scratch[0u] += vulkan_reduce_scratch[1u];
    barrier();
    return vulkan_reduce_scratch[0u];
}
)glsl";
/** --------------------------------------------------------------------------------------------------------- VULKAN_GLSL_L2_EXAMPLE
 * @brief Worked example: squared L2 distance, one job per (A, B) vector pair. The parameter
 * blob is { opcode, dimensions, precision, A[], B[] }; the result is one float per job, the
 * sum of squared differences. Lanes stride the dimensions by 16; part 0 contributes, the other
 * parts pass zero so every invocation reaches the reduction's barriers.
 */
inline constexpr std::string_view VULKAN_GLSL_L2_EXAMPLE = R"glsl(
void main() {
    Slice blob = vulkan_job(vulkan_job_index());
    float partial = 0.0;
    if (!slice_is_null(blob)) {
        const uint dimensions = slice_load_u32(blob, 1u);
        // Header is 12 bytes; vectors begin at the 16-byte mark
        Slice a = slice_sub(blob, 16u, uint64_t(dimensions) * 4ul);
        Slice b = slice_sub(blob, 16u + uint64_t(dimensions) * 4ul, uint64_t(dimensions) * 4ul);
        const float contributes = vulkan_part() == 0u ? 1.0 : 0.0;
        for (uint d = vulkan_lane(); d < dimensions; d += 16u) {
            const float diff = slice_load_f32(a, d) - slice_load_f32(b, d);
            partial += contributes * diff * diff;
        }
    }
    const float total = vulkan_group_reduce_sum(partial);
    if (vulkan_lane() == 0u && vulkan_part() == 0u && !slice_is_null(blob)) {
        slice_store_f32(blob, 0u, total);  // Result lands in the blob's opcode slot
    }
}
)glsl";
/** --------------------------------------------------------------------------------------------------------- kernel_shader_source
 * @brief Prepends the kernel prelude (core + tail + enforced workgroup shape) to a kernel body.
 * @param body GLSL defining main(); the shape is injected, not authored.
 * @return The full shader source.
 */
inline std::string kernel_shader_source(std::string_view body) {
    std::string source;
    source.reserve(13 + VULKAN_GLSL_KERNEL_CORE.size() + VULKAN_GLSL_KERNEL_TAIL.size() + body.size());
    source.append("#version 450\n");
    source.append(VULKAN_GLSL_KERNEL_CORE);
    source.append(VULKAN_GLSL_KERNEL_TAIL);
    source.append("layout(local_size_x = 16, local_size_y = 4, local_size_z = 1) in;\n");
    source.append(body);
    return source;
}
} // namespace buffetalligator
