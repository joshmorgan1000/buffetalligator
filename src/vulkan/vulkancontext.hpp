#pragma once
/** --------------------------------------------------------------------------------------------------------- VulkanContext
 * @file vulkancontext.hpp
 * @brief Owns the process Vulkan instance, device, queues, pipeline cache, and probed device facts.
 */
#include <logging.hpp>
#include <alligator.hpp>
#include <vulkan/vulkanhelpers.hpp>
#include <vulkan/vulkanbuffer.hpp>
#include <cstring>
#include <array>
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.hpp>

namespace buffetalligator {
class VulkanContext;
class Arena;
class GPUImpl;
class ShaderState;
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
    static TransferUnit make_transfer_unit() {
        VulkanContext& context = instance();
        TransferUnit unit;
        unit.pool = context.device_.createCommandPool(vk::CommandPoolCreateInfo(
            vk::CommandPoolCreateFlagBits::eResetCommandBuffer, queue_family_index()));
        unit.command = context.device_.allocateCommandBuffers(
            vk::CommandBufferAllocateInfo(unit.pool, vk::CommandBufferLevel::ePrimary, 1))[0];
        unit.fence = context.device_.createFence({});
        return unit;
    }
    /** ------------------------------------------------------------------------------------------- transfer_unit
     * @brief This thread's transfer unit, created on first use. Handles are reclaimed by
     * vkDestroyDevice at teardown, never individually.
     */
    static TransferUnit& transfer_unit() {
        thread_local TransferUnit unit = make_transfer_unit();
        return unit;
    }
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
    ) {
        constexpr vk::MemoryPropertyFlags unified_flags =
            vk::MemoryPropertyFlagBits::eDeviceLocal | vk::MemoryPropertyFlagBits::eHostVisible;
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            const bool both =
                (properties.memoryTypes[i].propertyFlags & unified_flags) == unified_flags;
            const bool heap_local =
                (properties.memoryHeaps[properties.memoryTypes[i].heapIndex].flags
                & vk::MemoryHeapFlagBits::eDeviceLocal) == vk::MemoryHeapFlagBits::eDeviceLocal;
            if (both && heap_local) {
                return true;
            }
        }
        return false;
    }
    /** ------------------------------------------------------------------------------------------- shared_buffer_info
     * @brief Creates buffer metadata using the device's fixed compute-family sharing policy.
     */
    vk::BufferCreateInfo shared_buffer_info(vk::DeviceSize bytes, vk::BufferUsageFlags usage) const {
        vk::BufferCreateInfo info({}, bytes, usage);
        if (compute_families_.size() > 1) {
            info.sharingMode = vk::SharingMode::eConcurrent;
            info.queueFamilyIndexCount = static_cast<uint32_t>(compute_families_.size());
            info.pQueueFamilyIndices = compute_families_.data();
        }
        return info;
    }
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
    ) const {
        for (uint32_t i = 0; i < memory_properties_.memoryTypeCount; ++i) {
            const bool allowed = (type_filter & (1u << i)) != 0;
            const vk::MemoryPropertyFlags flags = memory_properties_.memoryTypes[i].propertyFlags;
            if (allowed && (flags & wanted) == wanted && !(flags & excluded)) {
                return i;
            }
        }
        return UINT32_MAX;
    }
    /** ------------------------------------------------------------------------------------------- constructor
     * @brief Create the context and register it as the process GPU backend. Requests only 2
     * host-side workers from the inherited CPUCompute pool (submission/callback plumbing) rather
     * than one per hardware thread - a GPU context does not run compute on the CPU worker pool,
     * so hardware_concurrency() workers would sit idle for the service's entire lifetime.
     */
    VulkanContext() {
#ifdef __APPLE__
        ::setenv("MVK_CONFIG_LOG_LEVEL", "1", 0);
#endif
        vk::ApplicationInfo app_info("nebula", 1, "nebula", 1, VK_API_VERSION_1_2);
        std::vector<const char*> instance_extensions;
        vk::InstanceCreateInfo instance_info{};
        instance_info.pApplicationInfo = &app_info;
        portability_available_ = false;
        for (const vk::ExtensionProperties& ext : vk::enumerateInstanceExtensionProperties()) {
            if (std::strcmp(ext.extensionName.data(),
                    VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
                portability_available_ = true;
            }
        }
#ifdef __APPLE__
        if (portability_available_) {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
            instance_info.flags = vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
        }
#endif
        instance_info.enabledExtensionCount = static_cast<uint32_t>(instance_extensions.size());
        instance_info.ppEnabledExtensionNames = instance_extensions.data();
        try {
            instance_ = vk::createInstance(instance_info);
        } catch (const vk::SystemError&) {
            return;
        }
        // No physical GPU is a normal state: the singleton exists and reports device_present() == false.
        for (const vk::PhysicalDevice& device : instance_.enumeratePhysicalDevices()) {
            const vk::PhysicalDeviceType type = device.getProperties().deviceType;
            if (type == vk::PhysicalDeviceType::eDiscreteGpu) {
                physical_device_ = device;
                break;
            }
            if (type == vk::PhysicalDeviceType::eIntegratedGpu
                || (type == vk::PhysicalDeviceType::eVirtualGpu && !physical_device_)) {
                physical_device_ = device;
            }
        }
        if (!physical_device_) {
            instance_.destroy();
            instance_ = nullptr;
            return;
        }
        const std::vector<vk::QueueFamilyProperties> families =
            physical_device_.getQueueFamilyProperties();
        for (uint32_t index = 0; index < families.size(); ++index) {
            if (families[index].queueFlags & vk::QueueFlagBits::eCompute)
                compute_families_.push_back(index);
        }
        if (compute_families_.empty()) {
            ALLIGATOR_GPU_THROW("VulkanCompute::init_instance_and_device: no compute-capable queue family");
        }
        vk::PhysicalDeviceVulkan12Features supported12{};
        vk::PhysicalDeviceInternallySynchronizedQueuesFeaturesKHR supported_isq{};
        vk::PhysicalDeviceFeatures2 supported{};
        supported.pNext = &supported12;
        supported12.pNext = &supported_isq;
        physical_device_.getFeatures2(&supported);
        if (!supported12.bufferDeviceAddress) {
            ALLIGATOR_GPU_THROW("VulkanCompute::init_instance_and_device: device lacks bufferDeviceAddress; compute requires it");
        }
        std::vector<const char*> device_extensions;
        for (const vk::ExtensionProperties& ext : physical_device_.enumerateDeviceExtensionProperties()) {
            if (std::strcmp(ext.extensionName.data(), "VK_KHR_portability_subset") == 0) {
                device_extensions.push_back("VK_KHR_portability_subset");
            }
            if (std::strcmp(ext.extensionName.data(),
                    VK_KHR_INTERNALLY_SYNCHRONIZED_QUEUES_EXTENSION_NAME) == 0
                && supported_isq.internallySynchronizedQueues == VK_TRUE) {
                device_extensions.push_back(VK_KHR_INTERNALLY_SYNCHRONIZED_QUEUES_EXTENSION_NAME);
                internally_synchronized_queues_ = true;
            }
            if (std::strcmp(ext.extensionName.data(), VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0) {
                device_extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
                device_props_.supports_memory_budget = true;
            }
        }
        vk::PhysicalDeviceVulkan12Features enabled12{};
        enabled12.bufferDeviceAddress = VK_TRUE;
        enabled12.shaderBufferInt64Atomics = supported12.shaderBufferInt64Atomics;
        enabled12.shaderFloat16 = supported12.shaderFloat16;
        vk::PhysicalDeviceInternallySynchronizedQueuesFeaturesKHR enabled_isq{};
        enabled_isq.internallySynchronizedQueues = VK_TRUE;
        if (internally_synchronized_queues_) {
            enabled12.pNext = &enabled_isq;
        }
        vk::PhysicalDeviceFeatures2 enabled{};
        enabled.features.shaderInt64 = supported.features.shaderInt64;
        enabled.pNext = &enabled12;
        uint32_t total_queues = 0;
        uint32_t maximum_family_queues = 0;
        for (const uint32_t family : compute_families_) {
            total_queues += families[family].queueCount;
            maximum_family_queues = std::max(maximum_family_queues, families[family].queueCount);
        }
        const std::vector<float> queue_priorities(maximum_family_queues, 1.0f);
        const vk::DeviceQueueCreateFlags queue_flags = internally_synchronized_queues_
            ? vk::DeviceQueueCreateFlags(VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR)
            : vk::DeviceQueueCreateFlags{};
        std::vector<vk::DeviceQueueCreateInfo> queue_infos;
        queue_infos.reserve(compute_families_.size());
        for (const uint32_t family : compute_families_)
            queue_infos.emplace_back(queue_flags, family, families[family].queueCount,
                queue_priorities.data());
        vk::DeviceCreateInfo device_info{};
        device_info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
        device_info.pQueueCreateInfos = queue_infos.data();
        device_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
        device_info.ppEnabledExtensionNames = device_extensions.data();
        device_info.pNext = &enabled;
        device_ = physical_device_.createDevice(device_info);
        compute_queues_.reserve(total_queues);
        queue_family_slots_.reserve(total_queues);
        queue_claims_ = std::make_unique<std::atomic_flag[]>(total_queues);
        for (uint32_t slot = 0; slot < compute_families_.size(); ++slot) {
            const uint32_t family = compute_families_[slot];
            for (uint32_t index = 0; index < families[family].queueCount; ++index) {
                compute_queues_.push_back(device_.getQueue2(
                    vk::DeviceQueueInfo2(queue_flags, family, index)));
                queue_family_slots_.push_back(slot);
            }
        }
        memory_properties_ = physical_device_.getMemoryProperties();
        vk::PhysicalDeviceSubgroupProperties subgroup{};
        vk::PhysicalDeviceProperties2 props2{};
        props2.pNext = &subgroup;
        physical_device_.getProperties2(&props2);
        const vk::PhysicalDeviceLimits& limits = props2.properties.limits;
        device_props_.device_name = props2.properties.deviceName.data();
        device_props_.max_workgroup_count[0] = limits.maxComputeWorkGroupCount[0];
        device_props_.max_workgroup_count[1] = limits.maxComputeWorkGroupCount[1];
        device_props_.max_workgroup_count[2] = limits.maxComputeWorkGroupCount[2];
        device_props_.max_workgroup_size[0] = limits.maxComputeWorkGroupSize[0];
        device_props_.max_workgroup_size[1] = limits.maxComputeWorkGroupSize[1];
        device_props_.max_workgroup_size[2] = limits.maxComputeWorkGroupSize[2];
        device_props_.max_workgroup_invocations = limits.maxComputeWorkGroupInvocations;
        device_props_.max_shared_memory = limits.maxComputeSharedMemorySize;
        device_props_.subgroup_size = subgroup.subgroupSize;
        device_props_.max_storage_buffer_range = limits.maxStorageBufferRange;
        device_props_.max_push_constants = limits.maxPushConstantsSize;
        device_props_.supports_subgroup_arithmetic =
            (subgroup.supportedOperations & vk::SubgroupFeatureFlagBits::eArithmetic)
                == vk::SubgroupFeatureFlagBits::eArithmetic;
        device_props_.supports_subgroup_shuffle =
            (subgroup.supportedOperations & vk::SubgroupFeatureFlagBits::eShuffle)
                == vk::SubgroupFeatureFlagBits::eShuffle;
        device_props_.supports_buffer_device_address = true;
        device_props_.supports_int64 = supported.features.shaderInt64 == VK_TRUE;
        device_props_.supports_int64_atomics = supported12.shaderBufferInt64Atomics == VK_TRUE;
        device_props_.supports_float16 = supported12.shaderFloat16 == VK_TRUE;
        device_props_.supports_timeline_semaphore = supported12.timelineSemaphore == VK_TRUE;
        uint64_t device_local_bytes = 0;
        for (uint32_t i = 0; i < memory_properties_.memoryHeapCount; ++i) {
            if (memory_properties_.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
                device_local_bytes += memory_properties_.memoryHeaps[i].size;
            }
        }
        device_props_.device_local_memory_bytes = device_local_bytes;
        /// Host-visible total: every heap reachable through at least one HOST_VISIBLE memory
        /// type, each backing heap counted once via a seen mask (16 heaps is the spec cap).
        uint64_t host_visible_bytes = 0;
        uint16_t seen_heaps = 0;
        for (uint32_t i = 0; i < memory_properties_.memoryTypeCount; ++i) {
            const vk::MemoryPropertyFlags flags = memory_properties_.memoryTypes[i].propertyFlags;
            if ((flags & vk::MemoryPropertyFlagBits::eHostVisible) == vk::MemoryPropertyFlagBits::eHostVisible) {
                const uint32_t heap_index = memory_properties_.memoryTypes[i].heapIndex;
                const uint16_t heap_bit = static_cast<uint16_t>(1u << heap_index);
                if ((seen_heaps & heap_bit) == 0) {
                    seen_heaps |= heap_bit;
                    host_visible_bytes += memory_properties_.memoryHeaps[heap_index].size;
                }
            }
        }
        device_props_.host_visible_memory_bytes = host_visible_bytes;
        device_props_.unified_memory = unified_from_memory_properties(memory_properties_);
        // Initialize allocation template
        buffer_usage_ = vk::BufferUsageFlagBits::eStorageBuffer
            | vk::BufferUsageFlagBits::eShaderDeviceAddress
            | vk::BufferUsageFlagBits::eTransferSrc
            | vk::BufferUsageFlagBits::eTransferDst
            | vk::BufferUsageFlagBits::eIndirectBuffer;
        allocate_flags_ = vk::MemoryAllocateFlagsInfo(vk::MemoryAllocateFlagBits::eDeviceAddress);
        vk::Buffer probe = device_.createBuffer(shared_buffer_info(256, buffer_usage_));
        const vk::MemoryRequirements requirements = device_.getBufferMemoryRequirements(probe);
        device_.destroyBuffer(probe);
        const vk::MemoryPropertyFlags coherent =
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
        const vk::MemoryPropertyFlags cached = coherent | vk::MemoryPropertyFlagBits::eHostCached;
        // Measured 2026-08-23: uncached host-visible types read at 0.06 GB/s on RDNA3.5 iGPUs
        buffer_memory_type_index_ = try_find_memory_type(
            requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal | cached);
        if (buffer_memory_type_index_ == UINT32_MAX) {
            buffer_memory_type_index_ = try_find_memory_type(requirements.memoryTypeBits, cached);
        }
        if (buffer_memory_type_index_ == UINT32_MAX) {
            buffer_memory_type_index_ = try_find_memory_type(
                requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal | coherent);
        }
        if (buffer_memory_type_index_ == UINT32_MAX) {
            buffer_memory_type_index_ = try_find_memory_type(requirements.memoryTypeBits, coherent);
        }
        if (buffer_memory_type_index_ == UINT32_MAX) {
            ALLIGATOR_GPU_THROW("VulkanContext: no host-visible memory type for storage buffers");
        }
        const vk::MemoryPropertyFlags device_local = vk::MemoryPropertyFlagBits::eDeviceLocal;
        const auto ladder = [&](std::initializer_list<vk::MemoryPropertyFlags> rungs) {
            for (const vk::MemoryPropertyFlags rung : rungs) {
                const uint32_t found = try_find_memory_type(requirements.memoryTypeBits, rung);
                if (found != UINT32_MAX) return found;
            }
            return buffer_memory_type_index_;
        };
        placement_type_indices_[0] = ladder({device_local | cached, cached});                    // HOST
        /// HOST_VISIBLE means uncached on the CPU side (a streaming ring's payload reads go to RAM);
        /// only a device with no such type falls back to a cached one.
        const uint32_t uncached = try_find_memory_type(
            requirements.memoryTypeBits, device_local | coherent, vk::MemoryPropertyFlagBits::eHostCached);
        placement_type_indices_[1] = uncached != UINT32_MAX ? uncached : try_find_memory_type(
            requirements.memoryTypeBits, coherent, vk::MemoryPropertyFlagBits::eHostCached);
        if (placement_type_indices_[1] == UINT32_MAX) placement_type_indices_[1] = ladder({device_local | coherent, coherent});
        placement_type_indices_[2] = ladder({device_local | cached, cached});                   // HOST_CACHEABLE
        placement_type_indices_[3] = ladder({device_local | cached, device_local | coherent});  // DEVICE
        placement_type_indices_[4] = ladder({device_local | cached, cached});                   // UNIFIED
        placement_type_indices_[5] = buffer_memory_type_index_;                                 // BASIC_HEAP (never Vulkan-allocated)
        placement_type_indices_[6] = buffer_memory_type_index_;                                 // UNSPECIFIED
        for (size_t index = 0; index < placement_type_indices_.size(); ++index) {
            placement_cpu_cached_[index] =
                (memory_properties_.memoryTypes[placement_type_indices_[index]].propertyFlags
                    & vk::MemoryPropertyFlagBits::eHostCached) == vk::MemoryPropertyFlagBits::eHostCached;
        }
        placement_cpu_cached_[static_cast<size_t>(PlacementIndex::BASIC_HEAP)] = true;
        pipeline_cache_ = device_.createPipelineCache({});
        device_present_ = true;
        device_props_.gpu_free_bytes = poll_budget_headroom();
        alive_.store(true, std::memory_order_release);
    }
    /** ------------------------------------------------------------------------------------------- poll_budget_headroom
     * @brief Sum the device-local budget headroom (budget - usage per heap) via VK_EXT_memory_budget.
     * Re-queryable at any time - the driver recomputes budget/usage per call.
     * @return Free device-local bytes, or 0 when the budget extension is absent.
     */
    uint64_t poll_budget_headroom() const {
        if (!device_props_.supports_memory_budget) {
            return 0;
        }
        vk::PhysicalDeviceMemoryBudgetPropertiesEXT budget{};
        vk::PhysicalDeviceMemoryProperties2 memory2{};
        memory2.pNext = &budget;
        physical_device_.getMemoryProperties2(&memory2);
        uint64_t free_bytes = 0;
        for (uint32_t i = 0; i < memory2.memoryProperties.memoryHeapCount; ++i) {
            const bool device_local = (memory2.memoryProperties.memoryHeaps[i].flags
                & vk::MemoryHeapFlagBits::eDeviceLocal) != vk::MemoryHeapFlags{};
            const uint64_t headroom = budget.heapBudget[i] > budget.heapUsage[i]
                ? budget.heapBudget[i] - budget.heapUsage[i]
                : 0ull;
            free_bytes += device_local ? headroom : 0ull;
        }
        return free_bytes;
    }
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
    ~VulkanContext() {
        // Join the slab allocator before destroying the device it uses for deferred frees.
        BuffetMenu::shutdown();
        alive_.store(false, std::memory_order_release);
        if (!device_) {
            return;
        }
        device_.waitIdle();
        device_.destroyPipelineCache(pipeline_cache_);
        device_.destroy();
        instance_.destroy();
    }
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
    static uint32_t submission_queue_index() {
        VulkanContext& context = instance();
        struct Lease {
            VulkanContext* owner;
            uint32_t index;
            bool exclusive;
            ~Lease() {
                if (exclusive && VulkanContext::alive_.load(std::memory_order_acquire))
                    owner->queue_claims_[index].clear(std::memory_order_release);
            }
        };
        thread_local Lease lease = [&]() -> Lease {
            const uint32_t count = uint32_t(context.compute_queues_.size());
            if (context.internally_synchronized_queues_)
                return {&context, context.next_queue_slot_.fetch_add(1, std::memory_order_relaxed) % count, false};
            for (uint32_t i = 0; i < count; ++i)
                if (!context.queue_claims_[i].test_and_set(std::memory_order_acquire)) return {&context, i, true};
            ALLIGATOR_GPU_THROW("Vulkan: all compute queues have an owner; reduce GPU controller threads");
        }();
        return lease.index;
    }
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
    ) {
        VulkanContext& context = instance();
        context.device_.resetFences(fence);
        const uint32_t queue_slot = submission_queue_index();
        vk::SubmitInfo submit_info{};
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer;
        context.compute_queues_[queue_slot].submit(submit_info, fence);
    }
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
    static uint32_t queue_family_index() {
        return instance().compute_families_[submission_family_slot()];
    }
    /** ------------------------------------------------------------------------------------------- submission_family_slot
     * @brief The submitting thread's index into the prepared compute-family command buffers.
     */
    static uint32_t submission_family_slot() {
        return instance().queue_family_slots_[submission_queue_index()];
    }
    /** ------------------------------------------------------------------------------------------- compute_families
     * @brief Unique compute-capable family indices prepared on the logical device.
     */
    static const std::vector<uint32_t>& compute_families() { return instance().compute_families_; }
    /** ------------------------------------------------------------------------------------------- buffer_create_info
     * @brief Shares a buffer across every compute family when the device exposes more than one.
     */
    static vk::BufferCreateInfo buffer_create_info(vk::DeviceSize bytes, vk::BufferUsageFlags usage) {
        return instance().shared_buffer_info(bytes, usage);
    }
    /** ------------------------------------------------------------------------------------------- placement_cpu_cached
     * @brief Whether a Placement's memory type reads at RAM speed from the CPU.
     * @param placement_index The Placement enum's underlying value.
     * @return True when the type is HOST_CACHED.
     */
    static bool placement_cpu_cached(uint8_t placement_index) {
        return instance().placement_cpu_cached_[placement_index];
    }
    /** ------------------------------------------------------------------------------------------- bit_placement
     * @brief Where device-shared bit stores (planes, survivor masks) live: the zero-copy rung
     * when a device is present (UNIFIED on unified-memory systems, HOST_CACHEABLE on discrete),
     * plain heap without one.
     * @return The placement.
     */
    static const Placemat* bit_placement() {
        if (!instance().device_present_) return BuffetMenu::get("heap");
        return instance().device_props_.unified_memory
            ? VulkanPlacements::rung<4>("vulkan_unified")
            : VulkanPlacements::rung<2>("vulkan_host_cacheable");
    }
    /** ------------------------------------------------------------------------------------------- buffer_placement
     * @brief The placement resolved to the storage-buffer memory type the capability ladder
     * probed at init - the same type the job table and parameter buffers verify against their
     * memory requirements. Never a named-rung assumption: rung 6 carries the probed index, and
     * context init threw already if no host-coherent type exists. Heap without a device.
     * @return The placement.
     */
    static const Placemat* buffer_placement() {
        if (!instance().device_present_) return BuffetMenu::get("heap");
        return VulkanPlacements::rung<6>("vulkan_buffer");
    }
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
    static uint32_t queue_count() {
        if (!instance().device_present_) return 0;
        return static_cast<uint32_t>(instance().compute_queues_.size());
    }
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
    static uint64_t device_free_bytes() {
        if (!instance().device_present_) return 0;
        return instance().poll_budget_headroom();
    }
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
} // namespace buffetalligator
