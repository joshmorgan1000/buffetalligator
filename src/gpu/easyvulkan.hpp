#pragma once
/**
 * @file easyvulkan.hpp
 * @brief Provides helper functions and abstractions for working with Vulkan in the Alligator framework.
 */
#include <mutex>
#include <logging.hpp>
#include <alligator.hpp>
#include <vulkan/vulkan.hpp>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace buffetalligator {
struct BuffetDescriptor; class Chomp;
/** --------------------------------------------------------------------------------------------------------- Vulkan Exception
 * @class VulkanException
 * @brief Exception type for Vulkan-related errors.
 */
class VulkanException : public threadsafe_logger::Exception {
public:
    using threadsafe_logger::Exception::Exception;
    using threadsafe_logger::Exception::what;
};
#define ALLIGATOR_VULKAN_THROW(msg) throw VulkanException(msg)
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
    uint64_t max_allocation_bytes;             ///< Probed ceiling for one device memory allocation.
    uint32_t max_allocation_count;             ///< Probed ceiling for live device memory allocations.
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
    : buffer_(std::exchange(other.buffer_, nullptr)), memory_(std::exchange(other.memory_, nullptr))
    , host_(std::exchange(other.host_, nullptr)), address_(std::exchange(other.address_, 0))
    , size_(std::exchange(other.size_, 0)) {}
    VulkanBuffer& operator=(VulkanBuffer&& other) noexcept {
        std::swap(buffer_, other.buffer_); std::swap(memory_, other.memory_); std::swap(host_, other.host_);
        std::swap(address_, other.address_); std::swap(size_, other.size_); return *this;
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
            ALLIGATOR_VULKAN_THROW("VulkanBuffer exceeds the 64-byte-granule size limit");
        return new VulkanBuffer((size + 63) & ~size_t{63});
    }
    static size_t default_size() { return 64 * 1024 * 1024; }
    static size_t* type_idx() { static size_t idx = 1; return &idx; }
    static const char* type_name() { return "VulkanBuffer"; }
    static uint64_t device_address(void* ptr) { return static_cast<VulkanBuffer*>(ptr)->address_; }
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
/** --------------------------------------------------------------------------------------------------------- PlacementIndex
 * @enum PlacementIndex
 * @brief The memory-type ladder rungs resolved by the Vulkan context.
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
    std::atomic<uint64_t> queue_epoch_{0};
    std::mutex pipeline_mutex_;
    inline static std::atomic<uint32_t> next_queue_slot_{0};  ///< Hands each producer thread a sticky queue slot.
    bool internally_synchronized_queues_ = false;  ///< Probed VK_KHR_internally_synchronized_queues feature.
    vk::PhysicalDeviceMemoryProperties memory_properties_{};  ///< Queried once at init.
    vk::PipelineCache pipeline_cache_{};         ///< Driver pipeline cache (in-process).
    DeviceProperties device_props_{};            ///< Queried device limits and features.
    vk::DeviceSize max_allocation_size_ = 0;     ///< Probed maximum single device allocation.
    uint32_t max_allocation_count_ = 0;          ///< Probed maximum live device allocation count.
    std::atomic<uint32_t> allocation_count_{0};  ///< Reserved and live library device allocations.
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
    friend class Alligator;
    friend class VulkanKernel;
public:
    /** ------------------------------------------------------------------------------------------- Singleton instance
     * @brief The singleton VulkanCompute instance.
     */
    static VulkanContext& instance();
    /** ------------------------------------------------------------------------------------------- destructor
     * @brief Drain the device and tear down. Caller-owned buffers, rigs, and pipelines must
     * already be destroyed.
     */
    ~VulkanContext();
    /** ------------------------------------------------------------------------------------------- device_properties */
    static const DeviceProperties& device_properties() { return instance().device_props_; }
    /** ------------------------------------------------------------------------------------------- buffer_heap_headroom
     * @brief Live budget headroom of the heap backing the chosen buffer memory type.
     */
    static uint64_t buffer_heap_headroom();
    /** ------------------------------------------------------------------------------------------- device
     * @brief The logical device, for object creation and teardown by prepared shaders.
     */
    static vk::Device device() { return instance().device_; }
    /** ------------------------------------------------------------------------------------------- pipeline_cache
     * @brief The shared driver pipeline cache for one-time kernel preparation.
     */
    static vk::PipelineCache pipeline_cache() { return instance().pipeline_cache_; }
    /** ------------------------------------------------------------------------------------------- pipeline_mutex
     * @brief Vulkan requires host access to a VkPipelineCache in vkCreateComputePipelines to be
     * externally synchronized; this is the one lock preparation takes.
     */
    static std::mutex& pipeline_mutex() { return instance().pipeline_mutex_; }
    /** ------------------------------------------------------------------------------------------- assign_queue
     * @brief Hands a prepared shader its own compute queue, round-robin across the device's queues.
     * @return The queue index.
     */
    static uint32_t assign_queue();
    /** ------------------------------------------------------------------------------------------- queue_family
     * @brief The queue family a queue index belongs to, for the command pool recorded against it.
     * @param queue The queue index.
     * @return The family index.
     */
    static uint32_t queue_family(uint32_t queue);
    /** ------------------------------------------------------------------------------------------- submit
     * @brief Submits to one queue; vkQueueSubmit requires the queue to be externally synchronized unless
     * the device has internally synchronized queues, and the per-queue atomic claim is that synchronization.
     * @param queue The queue index.
     * @param info The submission.
     */
    static void submit(uint32_t queue, const vk::SubmitInfo& info);
    /** ------------------------------------------------------------------------------------------- buffer_memory_type_index
     * @brief The resolved host-coherent storage-buffer memory type for the job-table engine.
     */
    static uint32_t buffer_memory_type_index() { return instance().buffer_memory_type_index_; }
    /** ------------------------------------------------------------------------------------------- allocate_flags
     * @brief The device-address allocation flags chain, by reference for pNext wiring.
     */
    static const vk::MemoryAllocateFlagsInfo& allocate_flags() { return instance().allocate_flags_; }
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
    static const ::buffetalligator::BuffetDescriptor* buffer_placement();
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
    /** ------------------------------------------------------------------------------------------- gpu_usage
     * @brief Bytes currently held in Vulkan slabs, or zero while allocation tracking is off.
     * @return The number of bytes currently allocated on the Vulkan device.
     */
    static uint64_t gpu_usage();
    /** ------------------------------------------------------------------------------------------- gpu_pool_address
     * @brief The device address of the realm directory shaders resolve Chomp ids through, 0
     * while it is host-only.
     * @return The directory's device address.
     */
    static uint64_t gpu_pool_address();
};
/** --------------------------------------------------------------------------------------------------------- Vulkan GLSL Kernel Prelude
 * @brief The GLSL side of the Chomp: one push block carrying the dispatch header's address and the
 * realm directory's address, and every helper that resolves a 4-byte Chomp id through the directory.
 *
 * The command buffer never changes. The host writes a 64-byte header (count, indirect dimensions) and
 * the bound Chomp ids behind it; workgroup row Y resolves its own id from there.
 *
 * X (16) is the conceptual element lane, or area to shade.
 * Y (4) is the author's split, free to mean whatever the kernel wants.
 */
/** --------------------------------------------------------------------------------------------------------- VULKAN_GLSL_KERNEL_CORE
 * @brief The 4-byte Chomp pool ID and every load/store helper over it. Contains no
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
// ------------------------------------------------------------------------------------------------- Push block (16 bytes)
// One Chomp per dispatch: its id, whether it is a list of the rows' ids, and the realm directory.
layout(push_constant) uniform AlligatorPush {
    uint32_t vulkan_chomp_id;       ///< The dispatched Chomp
    uint32_t vulkan_chomp_list;     ///< Nonzero when that Chomp holds one id per workgroup row
    uint64_t vulkan_pool_address;   ///< The realm directory, shared host and device
} vulkan_push;
// ------------------------------------------------------------------------------------------------- Chomp (4-byte pool ID)
struct Chomp {
    uint32_t id;  ///< Encoded placement, realm, and realm-local slot
};
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer ChompRef {
    uint32_t id;
};
// ------------------------------------------------------------------------------------------------- GPUBufRef pool (16-byte entries)
// The realm directory locates the mapped GPUBuf records shared by CPU and GPU.
struct GPUBufRef {
    uint64_t address;      ///< The backing slab's device address
    uint32_t size;         ///< The rounded chomp length in 64-byte granules
    uint32_t offset;       ///< The slab-relative offset in 64-byte granules
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer GPUBufRefArray {
    GPUBufRef refs[];
};
Chomp gpu_chomp(uint32_t index) { return Chomp(index); }
layout(buffer_reference, std430, buffer_reference_align = 4) buffer U32Array { uint v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer I32Array { int v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer F32Array { float v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) buffer U64Array { uint64_t v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) buffer I64Array { int64_t v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer F32x4Array { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer U32x4Array { uvec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer I32x4Array { ivec4 v[]; };
// ------------------------------------------------------------------------------------------------- Chomp basics
// The id is type in the low 6 bits, region in the next 10, claim in the top 16; a directory entry is 16 bytes.
uint64_t chomp_table(Chomp s) {
    return U64Array(vulkan_push.vulkan_pool_address).v[((s.id >> 6u) & 1023u) * 2u];
}
GPUBufRef chomp_ref(Chomp s) { return GPUBufRefArray(chomp_table(s)).refs[s.id >> 16u]; }
uint64_t chomp_address(Chomp s) {
    GPUBufRef ref = chomp_ref(s);
    return ref.address + (uint64_t(ref.offset) << 6);
}
bool chomp_is_null(Chomp s) { return s.id == 0xFFFFFFFFu; }
uint64_t chomp_size(Chomp s) {
    return uint64_t(chomp_ref(s).size) << 6;
}
Chomp chomp_read(uint64_t address) { return Chomp(ChompRef(address).id); }
uint64_t gpu_chomp_address(uint32_t index) { return chomp_address(gpu_chomp(index)); }
uint64_t gpu_chomp_size(uint32_t index) { return chomp_size(gpu_chomp(index)); }
// ------------------------------------------------------------------------------------------------- Word-native loads
uint  chomp_load_u32(Chomp s, uint index) { return U32Array(chomp_address(s)).v[index]; }
int   chomp_load_i32(Chomp s, uint index) { return I32Array(chomp_address(s)).v[index]; }
float chomp_load_f32(Chomp s, uint index) { return F32Array(chomp_address(s)).v[index]; }
uint64_t chomp_load_u64(Chomp s, uint index) { return U64Array(chomp_address(s)).v[index]; }
int64_t  chomp_load_i64(Chomp s, uint index) { return I64Array(chomp_address(s)).v[index]; }
vec4  chomp_load_f32x4(Chomp s, uint index) { return F32x4Array(chomp_address(s)).v[index]; }
uvec4 chomp_load_u32x4(Chomp s, uint index) { return U32x4Array(chomp_address(s)).v[index]; }
ivec4 chomp_load_i32x4(Chomp s, uint index) { return I32x4Array(chomp_address(s)).v[index]; }
// ------------------------------------------------------------------------------------------------- Word-native stores
void chomp_store_u32(Chomp s, uint index, uint value) { U32Array(chomp_address(s)).v[index] = value; }
void chomp_store_i32(Chomp s, uint index, int value) { I32Array(chomp_address(s)).v[index] = value; }
void chomp_store_f32(Chomp s, uint index, float value) { F32Array(chomp_address(s)).v[index] = value; }
void chomp_store_u64(Chomp s, uint index, uint64_t value) { U64Array(chomp_address(s)).v[index] = value; }
void chomp_store_i64(Chomp s, uint index, int64_t value) { I64Array(chomp_address(s)).v[index] = value; }
void chomp_store_f32x4(Chomp s, uint index, vec4 value) { F32x4Array(chomp_address(s)).v[index] = value; }
void chomp_store_u32x4(Chomp s, uint index, uvec4 value) { U32x4Array(chomp_address(s)).v[index] = value; }
void chomp_store_i32x4(Chomp s, uint index, ivec4 value) { I32x4Array(chomp_address(s)).v[index] = value; }
// ------------------------------------------------------------------------------------------------- Sub-word loads (no 8/16-bit storage feature needed)
uint chomp_load_u16(Chomp s, uint index) {
    uint word = U32Array(chomp_address(s) + uint64_t((index * 2u) & ~3u)).v[0];
    return (word >> ((index & 1u) * 16u)) & 0xFFFFu;
}
int chomp_load_i16(Chomp s, uint index) {
    uint word = U32Array(chomp_address(s) + uint64_t((index * 2u) & ~3u)).v[0];
    return bitfieldExtract(int(word), int((index & 1u) * 16u), 16);
}
uint chomp_load_u8(Chomp s, uint index) {
    uint word = U32Array(chomp_address(s) + uint64_t(index & ~3u)).v[0];
    return (word >> ((index & 3u) * 8u)) & 0xFFu;
}
int chomp_load_i8(Chomp s, uint index) {
    uint word = U32Array(chomp_address(s) + uint64_t(index & ~3u)).v[0];
    return bitfieldExtract(int(word), int((index & 3u) * 8u), 8);
}
float chomp_load_f16(Chomp s, uint index) {
    uint word = U32Array(chomp_address(s) + uint64_t((index * 2u) & ~3u)).v[0];
    return unpackHalf2x16(word)[index & 1u];
}
float chomp_load_bf16(Chomp s, uint index) { return uintBitsToFloat(chomp_load_u16(s, index) << 16u); }
float chomp_load_e5m2(Chomp s, uint index) { return unpackHalf2x16(chomp_load_u8(s, index) << 8u).x; }
// ------------------------------------------------------------------------------------------------- Sub-word stores (atomic read-modify-write so neighbouring lanes never clobber each other)
void chomp_store_u16(Chomp s, uint index, uint value) {
    U32Array words = U32Array(chomp_address(s) + uint64_t((index * 2u) & ~3u));
    uint shift = (index & 1u) * 16u;
    atomicAnd(words.v[0], ~(0xFFFFu << shift));
    atomicOr(words.v[0], (value & 0xFFFFu) << shift);
}
void chomp_store_u8(Chomp s, uint index, uint value) {
    U32Array words = U32Array(chomp_address(s) + uint64_t(index & ~3u));
    uint shift = (index & 3u) * 8u;
    atomicAnd(words.v[0], ~(0xFFu << shift));
    atomicOr(words.v[0], (value & 0xFFu) << shift);
}
void chomp_store_i16(Chomp s, uint index, int value) { chomp_store_u16(s, index, uint(value)); }
void chomp_store_i8(Chomp s, uint index, int value) { chomp_store_u8(s, index, uint(value)); }
void chomp_store_f16(Chomp s, uint index, float value) { chomp_store_u16(s, index, packHalf2x16(vec2(value, 0.0)) & 0xFFFFu); }
void chomp_store_bf16(Chomp s, uint index, float value) { chomp_store_u16(s, index, floatBitsToUint(value) >> 16u); }
void chomp_store_e5m2(Chomp s, uint index, float value) { chomp_store_u8(s, index, (packHalf2x16(vec2(value, 0.0)) >> 8u) & 0xFFu); }
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
float chomp_load_e4m3(Chomp s, uint index) { return e4m3_to_f32(chomp_load_u8(s, index)); }
float chomp_load_e3m4(Chomp s, uint index) { return e3m4_to_f32(chomp_load_u8(s, index)); }
void chomp_store_e4m3(Chomp s, uint index, float value) { chomp_store_u8(s, index, f32_to_e4m3(value)); }
void chomp_store_e3m4(Chomp s, uint index, float value) { chomp_store_u8(s, index, f32_to_e3m4(value)); }
// ------------------------------------------------------------------------------------------------- Four-wide sub-word loads and stores (index4 counts groups of four elements; whole words, no atomics)
vec4 chomp_load_f16x4(Chomp s, uint index4) {
    uint64_t pair = chomp_load_u64(s, index4);
    return vec4(unpackHalf2x16(uint(pair)), unpackHalf2x16(uint(pair >> 32u)));
}
vec4 chomp_load_bf16x4(Chomp s, uint index4) {
    uint64_t pair = chomp_load_u64(s, index4);
    uint lo = uint(pair);
    uint hi = uint(pair >> 32u);
    return vec4(uintBitsToFloat(lo << 16u), uintBitsToFloat(lo & 0xFFFF0000u), uintBitsToFloat(hi << 16u), uintBitsToFloat(hi & 0xFFFF0000u));
}
vec4 chomp_load_e5m2x4(Chomp s, uint index4) { return fp8x4_to_f32x4(chomp_load_u32(s, index4), 2u, 1.0); }
vec4 chomp_load_e4m3x4(Chomp s, uint index4) { return fp8x4_to_f32x4(chomp_load_u32(s, index4), 3u, 256.0); }
vec4 chomp_load_e3m4x4(Chomp s, uint index4) { return fp8x4_to_f32x4(chomp_load_u32(s, index4), 4u, 4096.0); }
void chomp_store_f16x4(Chomp s, uint index4, vec4 values) {
    chomp_store_u64(s, index4, uint64_t(packHalf2x16(values.xy)) | (uint64_t(packHalf2x16(values.zw)) << 32u));
}
void chomp_store_bf16x4(Chomp s, uint index4, vec4 values) {
    uvec4 bits = floatBitsToUint(values) >> 16u;
    chomp_store_u64(s, index4, uint64_t(bits.x | (bits.y << 16u)) | (uint64_t(bits.z | (bits.w << 16u)) << 32u));
}
void chomp_store_e5m2x4(Chomp s, uint index4, vec4 values) {
    uint lo = packHalf2x16(values.xy);
    uint hi = packHalf2x16(values.zw);
    chomp_store_u32(s, index4, ((lo >> 8u) & 0xFFu) | ((lo >> 16u) & 0xFF00u) | ((hi & 0xFF00u) << 8u) | (hi & 0xFF000000u));
}
void chomp_store_e4m3x4(Chomp s, uint index4, vec4 values) { chomp_store_u32(s, index4, fp8x4_from_f32x4(values, 3u, 0.00390625)); }
void chomp_store_e3m4x4(Chomp s, uint index4, vec4 values) { chomp_store_u32(s, index4, fp8x4_from_f32x4(values, 4u, 0.000244140625)); }
#ifdef VULKAN_FLOAT16
// ------------------------------------------------------------------------------------------------- Four-wide loads widened to float16_t (shaderFloat16 devices)
f16vec4 fp8x4_to_f16x4(uint word, uint mantissa_bits, float16_t scale) {
    uvec4 codes = fp8x4_codes(word);
    uint lo = fp8_to_f16_bits(codes.x, mantissa_bits) | (fp8_to_f16_bits(codes.y, mantissa_bits) << 16u);
    uint hi = fp8_to_f16_bits(codes.z, mantissa_bits) | (fp8_to_f16_bits(codes.w, mantissa_bits) << 16u);
    return f16vec4(unpackFloat2x16(lo), unpackFloat2x16(hi)) * scale;
}
f16vec4 chomp_load_f32x4_half(Chomp s, uint index4) { return f16vec4(chomp_load_f32x4(s, index4)); }
f16vec4 chomp_load_f16x4_half(Chomp s, uint index4) {
    uint64_t pair = chomp_load_u64(s, index4);
    return f16vec4(unpackFloat2x16(uint(pair)), unpackFloat2x16(uint(pair >> 32u)));
}
f16vec4 chomp_load_bf16x4_half(Chomp s, uint index4) { return f16vec4(chomp_load_bf16x4(s, index4)); }
f16vec4 chomp_load_e5m2x4_half(Chomp s, uint index4) { return fp8x4_to_f16x4(chomp_load_u32(s, index4), 2u, float16_t(1.0)); }
f16vec4 chomp_load_e4m3x4_half(Chomp s, uint index4) { return fp8x4_to_f16x4(chomp_load_u32(s, index4), 3u, float16_t(256.0)); }
f16vec4 chomp_load_e3m4x4_half(Chomp s, uint index4) { return fp8x4_to_f16x4(chomp_load_u32(s, index4), 4u, float16_t(4096.0)); }
#endif
#endif
)glsl";
} // namespace buffetalligator
