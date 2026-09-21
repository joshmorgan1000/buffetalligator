/** --------------------------------------------------------------------------------------------------------- Vulkan Implementation
 * @file vulkan.cpp
 * @brief Implements Vulkan slabs, GLSL compilation, and the public prepared Shader interface.
 */
#include <logging.hpp>
#include <alligator.hpp>
#include <shaderc/shaderc.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Vulkan Static Methods
 * @struct VulkanStaticMethods
 * @brief Provides static methods for Vulkan buffer management.
 */
struct VulkanStaticMethods {
    VulkanStaticMethods() = delete; // Prevent instantiation of this static-only class.
    /** ------------------------------------------------------------------------------------------- Prepare Runtime
     * @brief Loads the Vulkan driver on the registering thread before the allocator worker starts.
     */
    static void prepare_runtime();
    /** ------------------------------------------------------------------------------------------- Vulkan Allocator
     * @brief Allocates a mapped Vulkan buffer of the specified size.
     * @param size The size of the Vulkan buffer to allocate.
     * @param context The rung index pointer the placement handed us.
     * @return The host mapping and the VulkanBuffer handle pair.
     */
    static std::pair<void*, void*> vulkan_allocator(size_t size, void* context);
    /** ------------------------------------------------------------------------------------------- Deallocate
     * @brief Destroys a Vulkan buffer and clears its host mapping.
     * @param host_ptr The host mapping of the buffer.
     * @param substrate_handle The VulkanBuffer handle backing the mapping.
     * @return The cleared host pointer and substrate handle pair.
     */
    static std::pair<void*, void*> vulkan_deallocate(void* host_ptr, void* substrate_handle);
    /** ------------------------------------------------------------------------------------------- Get Vulkan Context
     * @brief Retrieves the Vulkan context.
     * @return A pointer to the Vulkan context.
     */
    static void* vulkan_get_context();
    /** ------------------------------------------------------------------------------------------- Device Address
     * @brief The device address of a slab's first byte.
     * @param substrate_handle The VulkanBuffer handle backing the slab.
     * @return The buffer device address.
     */
    static uint64_t vulkan_device_address(void* substrate_handle);
};
/** --------------------------------------------------------------------------------------------------------- VulkanPlacements
 * @struct VulkanPlacements
 * @brief One Vulkan Placemat per ladder rung; the rung rides in the Placemat's context word and
 * the allocator resolves it to the memory type `VulkanContext` probed for it.
 */
struct VulkanPlacements {
    VulkanPlacements() = delete;
    /** ------------------------------------------------------------------------------------------- rung_context
     * @brief The `get_context` hook for one rung: a pointer to that rung's index.
     */
    template <uint8_t Rung>
    static void* rung_context() {
        VulkanStaticMethods::prepare_runtime();
        static uint8_t rung = Rung;
        return &rung;
    }
    /** ------------------------------------------------------------------------------------------- rung
     * @brief Registers (once) and returns the Vulkan Placemat for a ladder rung.
     */
    template <uint8_t Rung>
    static const Placemat* rung(const char* name) {
        static const Placemat* placemat = BuffetMenu::get(BuffetMenu::register_type(
            name, 64 * 1024 * 1024, 4096,
            &VulkanStaticMethods::vulkan_allocator, &VulkanStaticMethods::vulkan_deallocate,
            &VulkanPlacements::rung_context<Rung>, false, nullptr,
            &VulkanStaticMethods::vulkan_device_address));
        return placemat;
    }
    /** ------------------------------------------------------------------------------------------- Prime
     * @brief Registers every ladder rung before the first Slice allocates; the alligator's tracker
     * sizes its per-placement tables at the first slab, so a rung first touched later would index
     * past them (contract: process-lifetime Placemats register before the first Slice).
     */
    static void prime() {
        rung<0>("vulkan_host");
        rung<1>("vulkan_host_visible");
        rung<2>("vulkan_host_cacheable");
        rung<3>("vulkan_device");
        rung<4>("vulkan_unified");
        rung<5>("vulkan_basic_heap");
        rung<6>("vulkan_buffer");
    }
};
/** --------------------------------------------------------------------------------------------------------- make_transfer_unit
 * @brief Create this thread's TransferUnit from its own pool.
 */
VulkanContext::TransferUnit VulkanContext::make_transfer_unit() {
    VulkanContext& context = instance();
    TransferUnit unit;
    unit.pool = context.device_.createCommandPool(vk::CommandPoolCreateInfo(
        vk::CommandPoolCreateFlagBits::eResetCommandBuffer, queue_family_index()));
    unit.command = context.device_.allocateCommandBuffers(
        vk::CommandBufferAllocateInfo(unit.pool, vk::CommandBufferLevel::ePrimary, 1))[0];
    unit.fence = context.device_.createFence({});
    return unit;
}
/** --------------------------------------------------------------------------------------------------------- transfer_unit
 * @brief This thread's transfer unit, created on first use. Handles are reclaimed by
 * vkDestroyDevice at teardown, never individually.
 */
VulkanContext::TransferUnit& VulkanContext::transfer_unit() {
    thread_local TransferUnit unit = make_transfer_unit();
    return unit;
}
/** --------------------------------------------------------------------------------------------------------- unified_from_memory_properties
 * @brief The one-query UMA test: a memory type carrying both DEVICE_LOCAL and HOST_VISIBLE
 * whose heap is device-local.
 * @param properties The queried memory properties.
 * @return True on unified-memory systems.
 */
bool VulkanContext::unified_from_memory_properties(
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
/** --------------------------------------------------------------------------------------------------------- shared_buffer_info
 * @brief Creates buffer metadata using the device's fixed compute-family sharing policy.
 */
vk::BufferCreateInfo VulkanContext::shared_buffer_info(vk::DeviceSize bytes, vk::BufferUsageFlags usage) const {
    vk::BufferCreateInfo info({}, bytes, usage);
    if (compute_families_.size() > 1) {
        info.sharingMode = vk::SharingMode::eConcurrent;
        info.queueFamilyIndexCount = static_cast<uint32_t>(compute_families_.size());
        info.pQueueFamilyIndices = compute_families_.data();
    }
    return info;
}
/** --------------------------------------------------------------------------------------------------------- try_find_memory_type
 * @brief Find a memory type index matching the filter and all wanted flags.
 * @param type_filter Bitmask of allowed type indices (from VkMemoryRequirements).
 * @param wanted Required property flags.
 * @param excluded Property flags the type must not carry.
 * @return The type index, or UINT32_MAX when none matches.
 */
uint32_t VulkanContext::try_find_memory_type(
    uint32_t type_filter,
    vk::MemoryPropertyFlags wanted,
    vk::MemoryPropertyFlags excluded
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
/** --------------------------------------------------------------------------------------------------------- constructor
 * @brief Create the context and register it as the process GPU backend. Requests only 2
 * host-side workers from the inherited CPUCompute pool (submission/callback plumbing) rather
 * than one per hardware thread - a GPU context does not run compute on the CPU worker pool,
 * so hardware_concurrency() workers would sit idle for the service's entire lifetime.
 */
VulkanContext::VulkanContext() {
#ifdef __APPLE__
    ::setenv("MVK_CONFIG_LOG_LEVEL", "1", 0);
#endif
    vk::ApplicationInfo app_info("alligator", 1, "alligator", 1, VK_API_VERSION_1_2);
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
/** --------------------------------------------------------------------------------------------------------- poll_budget_headroom
 * @brief Sum the device-local budget headroom (budget - usage per heap) via VK_EXT_memory_budget.
 * Re-queryable at any time - the driver recomputes budget/usage per call.
 * @return Free device-local bytes, or 0 when the budget extension is absent.
 */
uint64_t VulkanContext::poll_budget_headroom() const {
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
/** --------------------------------------------------------------------------------------------------------- destructor
 * @brief Drain the device and tear down. Caller-owned buffers, rigs, and pipelines must
 * already be destroyed.
 */
VulkanContext::~VulkanContext() {
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
/// Kernels acquire their lease during controller startup, before accepting requests.
uint32_t VulkanContext::submission_queue_index() {
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
/** --------------------------------------------------------------------------------------------------------- submit_command_buffer
 * @brief Reset the fence and submit to this thread's sticky compute queue, lock-free.
 * @param command_buffer The recorded command buffer.
 * @param fence The submitter's fence, signalled on retirement.
 * @param callback Reserved by the in-flight callback scaffolding; pass nullptr.
 * @param callback_context Reserved; pass nullptr.
 */
void VulkanContext::submit_command_buffer(
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
/** --------------------------------------------------------------------------------------------------------- queue_family_index
 * @brief The compute queue family leased by this submitting thread.
 */
uint32_t VulkanContext::queue_family_index() {
    return instance().compute_families_[submission_family_slot()];
}
/** --------------------------------------------------------------------------------------------------------- submission_family_slot
 * @brief The submitting thread's index into the prepared compute-family command buffers.
 */
uint32_t VulkanContext::submission_family_slot() {
    return instance().queue_family_slots_[submission_queue_index()];
}
/** --------------------------------------------------------------------------------------------------------- buffer_create_info
 * @brief Shares a buffer across every compute family when the device exposes more than one.
 */
vk::BufferCreateInfo VulkanContext::buffer_create_info(vk::DeviceSize bytes, vk::BufferUsageFlags usage) {
    return instance().shared_buffer_info(bytes, usage);
}
/** --------------------------------------------------------------------------------------------------------- bit_placement
 * @brief Where device-shared bit stores (planes, survivor masks) live: the zero-copy rung
 * when a device is present (UNIFIED on unified-memory systems, HOST_CACHEABLE on discrete),
 * plain heap without one.
 * @return The placement.
 */
const Placemat* VulkanContext::bit_placement() {
    if (!instance().device_present_) return BuffetMenu::get("heap");
    return instance().device_props_.unified_memory
        ? VulkanPlacements::rung<4>("vulkan_unified")
        : VulkanPlacements::rung<2>("vulkan_host_cacheable");
}
/** --------------------------------------------------------------------------------------------------------- buffer_placement
 * @brief The placement resolved to the storage-buffer memory type the capability ladder
 * probed at init - the same type the job table and parameter buffers verify against their
 * memory requirements. Never a named-rung assumption: rung 6 carries the probed index, and
 * context init threw already if no host-coherent type exists. Heap without a device.
 * @return The placement.
 */
const Placemat* VulkanContext::buffer_placement() {
    if (!instance().device_present_) return BuffetMenu::get("heap");
    return VulkanPlacements::rung<6>("vulkan_buffer");
}
/** --------------------------------------------------------------------------------------------------------- queue_count
 * @brief Compute queues across all compute families, for GPU worker-count decisions.
 * @return Queue count, or 0 without a device.
 */
uint32_t VulkanContext::queue_count() {
    if (!instance().device_present_) return 0;
    return static_cast<uint32_t>(instance().compute_queues_.size());
}
/** --------------------------------------------------------------------------------------------------------- device_free_bytes
 * @brief Live device-local budget headroom, polled from the driver at call time.
 * @return Free device-local bytes, or 0 without a device or the budget extension.
 */
uint64_t VulkanContext::device_free_bytes() {
    if (!instance().device_present_) return 0;
    return instance().poll_budget_headroom();
}
namespace {
/** --------------------------------------------------------------------------------------------------------- Kernel Push
 * @struct KernelPush
 * @brief The host mirror of the kernel core's push block (vulkanglsl.hpp): the engine's table
 * address and the global GPUBufRef pool's device address, 16 bytes.
 */
struct KernelPush {
    uint64_t table;  ///< Job table (megakernel) or parameters list (public Shader)
    uint64_t pool;   ///< The global GPUBufRef table's device address
};
static_assert(sizeof(KernelPush) == 16, "KernelPush must mirror the GLSL push block");
/** --------------------------------------------------------------------------------------------------------- SliceMirror
 * @struct SliceMirror
 * @brief The host copy of the GLSL `Slice` descriptor (vulkanglsl.hpp): address, size, offset in bytes.
 */
struct alignas(16) SliceMirror {
    uint64_t device_address;  ///< Absolute device address of the claim's slab
    uint32_t size;            ///< Claim size in bytes
    uint32_t offset;          ///< Claim offset within the addressed buffer
};
static_assert(sizeof(SliceMirror) == 16, "SliceMirror must match the GLSL Slice descriptor.");
/// @brief Slices one prepared Shader binds per round; longer lists dispatch in rounds.
constexpr size_t PUBLIC_LIST_CAPACITY = 1024;
/** --------------------------------------------------------------------------------------------------------- Public GLSL Tail
 * @brief Binds the list descriptor's address and hands workgroup Y its own slice.
 */
inline constexpr std::string_view PUBLIC_GLSL_TAIL = R"glsl(
Slice vulkan_list() { return slice_read(vulkan_push.vulkan_table_address); }
uint vulkan_count() { return slice_size(vulkan_list()) / 16u; }
Slice vulkan_slice(uint index) { return slice_read(slice_address(vulkan_list()) + uint64_t(index) * 16ul); }
uint vulkan_index() { return gl_WorkGroupID.y; }
layout(local_size_x = 16, local_size_y = 4, local_size_z = 1) in;
)glsl";
/** --------------------------------------------------------------------------------------------------------- Public Shader Source
 * @brief Assembles the injected Slice ABI, fixed workgroup shape, user function, and entry point.
 * @param body The body of the shader function to be injected into the final source.
 * @return The complete GLSL source code as a string.
 */
std::string public_shader_source(std::string_view body) {
    constexpr std::string_view entry =
        "\nvoid main() { vulkan_main(vulkan_slice(vulkan_index())); }\n";
    std::string source;
    source.reserve(13 + VULKAN_GLSL_KERNEL_CORE.size() + PUBLIC_GLSL_TAIL.size()
        + body.size() + entry.size());
    source.append("#version 450\n");
    source.append(VULKAN_GLSL_KERNEL_CORE);
    source.append(PUBLIC_GLSL_TAIL);
    source.append(body);
    source.append(entry);
    return source;
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Compile Vulkan GLSL
 * @brief Compiles GLSL source code into Vulkan SPIR-V binary format.
 * @param source GLSL source code as a string view.
 * @param float16 Whether to enable 16-bit floating point support.
 * @param name Diagnostic name for error reporting.
 * @return A vector of 32-bit words representing the compiled SPIR-V binary.
 */
std::vector<uint32_t> compile_vulkan_glsl(
    std::string_view source,
    bool float16,
    std::string_view name
) {
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    if (float16) options.AddMacroDefinition("VULKAN_FLOAT16", "1");
    const std::string diagnostic_name(name);
    const shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv(
        source.data(), source.size(), shaderc_compute_shader, diagnostic_name.c_str(), options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        ALLIGATOR_GPU_THROW("Vulkan shader compile failed [" + diagnostic_name + "]: "
            + result.GetErrorMessage());
    }
    return std::vector<uint32_t>(result.cbegin(), result.cend());
}
/** --------------------------------------------------------------------------------------------------------- VulkanBuffer Constructor
 * @brief Create, allocate (DEVICE_LOCAL|HOST_VISIBLE preferred, else HOST_VISIBLE), bind, map,
 * zero, and query the device address.
 * @param size_bytes Buffer size in bytes.
 */
VulkanBuffer::VulkanBuffer(size_t size_bytes)
: VulkanBuffer(size_bytes, VulkanContext::instance().buffer_memory_type_index_) {}
/** --------------------------------------------------------------------------------------------------------- VulkanBuffer Constructor (typed)
 * @brief The same five-call allocation against an explicit memory type index.
 * @param size_bytes Buffer size in bytes.
 * @param memory_type_index The memory type to allocate from.
 */
VulkanBuffer::VulkanBuffer(size_t size_bytes, uint32_t memory_type_index)
: size_(size_bytes) {
    if (!VulkanContext::device_present()) {
        host_ = std::malloc(size_bytes);
        std::memset(host_, 0, size_bytes);
        address_ = 0;
        return;
    }
    buffer_ = VulkanContext::instance().device_.createBuffer(
        VulkanContext::buffer_create_info(size_bytes, VulkanContext::instance().buffer_usage_));
    const vk::MemoryRequirements requirements = VulkanContext::instance().device_.getBufferMemoryRequirements(buffer_);
    vk::MemoryAllocateInfo allocate_info(requirements.size, memory_type_index);
    allocate_info.pNext = &VulkanContext::instance().allocate_flags_;
    memory_ = VulkanContext::instance().device_.allocateMemory(allocate_info);
    VulkanContext::instance().device_.bindBufferMemory(buffer_, memory_, 0);
    host_ = VulkanContext::instance().device_.mapMemory(memory_, 0, VK_WHOLE_SIZE);
    address_ = VulkanContext::instance().device_.getBufferAddress(vk::BufferDeviceAddressInfo(buffer_));
    std::memset(host_, 0, size_);
}
/** --------------------------------------------------------------------------------------------------------- VulkanBuffer Destructor
 * @brief Unmap and free through the owning context, unless that context has already been torn down.
 */
VulkanBuffer::~VulkanBuffer() {
    if (!memory_) {
        std::free(host_);
        return;
    }
    if (!VulkanContext::alive_.load(std::memory_order_acquire)) {
        return;
    }
    VulkanContext::instance().device_.unmapMemory(memory_);
    VulkanContext::instance().device_.destroyBuffer(buffer_);
    VulkanContext::instance().device_.freeMemory(memory_);
}
/** --------------------------------------------------------------------------------------------------------- GPU::exists
 * @brief Checks if a Vulkan compute device is present.
 * @return True if a Vulkan compute device is available, false otherwise.
 */
bool GPU::exists() {
    return VulkanContext::device_present();
}
/** --------------------------------------------------------------------------------------------------------- GPU::unified_memory
 * @brief Checks if the Vulkan compute device uses unified memory.
 * @return True if the device has unified memory, false otherwise.
 */
bool GPU::unified_memory() {
    return VulkanContext::device_present() && VulkanContext::device_unified();
}
/** --------------------------------------------------------------------------------------------------------- GPU::device_name
 * @brief Retrieves the name of the Vulkan compute device.
 * @return The device name as a string.
 */
std::string GPU::device_name() {
    return VulkanKernel::device_name();
}
/** --------------------------------------------------------------------------------------------------------- GPU::compile_glsl
 * @brief Compiles GLSL source code into a Vulkan SPIR-V binary.
 * @param body The body of the shader function to be injected into the final source.
 * @return A Slice containing the compiled SPIR-V words.
 */
Slice GPU::compile_glsl(std::string_view body) {
    if (!VulkanContext::device_present()) {
        GPU_THROW("GPU::compile_glsl: no Vulkan compute device is present.");
    }
    if (!VulkanContext::device_properties().supports_int64) {
        GPU_THROW("GPU::compile_glsl: the prelude addresses memory with uint64_t, "
            "but the device lacks shaderInt64.");
    }
    const std::string assembled = kernel_shader_source(body);
    std::vector<uint32_t> spirv;
    try {
        spirv = compile_vulkan_glsl(
            assembled,
            VulkanContext::device_properties().supports_float16,
            "vulkan_gpu_compile_glsl");
    } catch (const std::exception& error) {
        GPU_THROW(std::string("GPU::compile_glsl: ") + error.what());
    }
    Slice words(4 * spirv.size());
    std::memcpy(words.raw(), spirv.data(), words.size_bytes());
    return words;
}
/** --------------------------------------------------------------------------------------------------------- ShaderState
 * @class ShaderState
 * @brief Owns one pipeline, persistent parameter block, immutable command buffer, and fence.
 */
struct ShaderState::Impl {
    enum class Format { Slices, Jobs, References };
    Format format;
    std::vector<uint32_t> words;
    std::string name;
    size_t limit;
    uint32_t reference_workgroups_x = 1;
    vk::Pipeline pipeline{};
    std::vector<std::vector<uint32_t>> stage_words;
    std::vector<KernelGpuStage> stages;
    std::vector<vk::Pipeline> stage_pipelines;
    uint32_t resources = 0;

    vk::PipelineLayout layout{};
    vk::ShaderModule module{};
    vk::Buffer buffer{};
    vk::DeviceMemory memory{};
    uint8_t* mapped = nullptr;
    uint64_t address = 0;
    std::vector<vk::CommandPool> command_pools;
    std::vector<vk::CommandBuffer> commands;
    vk::Fence fence{};

    Impl(std::vector<uint32_t> code, std::string_view label, Format fmt,
         size_t capacity, const Slice* refs = nullptr)
        : format(fmt), words(std::move(code)), name(label), limit(capacity) {
        try { prepare(refs); } catch (...) { release(); throw; }
    }
    Impl(std::vector<std::vector<uint32_t>> code, std::span<const KernelGpuStage> sequence,
         std::string_view label, const Slice& refs, uint32_t resource_ref)
        : format(Format::References), words(code.front()), name(label), limit(refs.size<uint32_t>()),
          stage_words(std::move(code)), stages(sequence.begin(), sequence.end()), resources(resource_ref) {
        try { prepare(&refs); } catch (...) { release(); throw; }
    }
    ~Impl() { release(); }

    void release() {
        const auto device = VulkanContext::device();
        for (const auto pool : command_pools) device.destroyCommandPool(pool);
        if (fence) device.destroyFence(fence);
        if (pipeline) device.destroyPipeline(pipeline);
        for (auto entry : stage_pipelines) device.destroyPipeline(entry);
        if (layout) device.destroyPipelineLayout(layout);
        if (module) device.destroyShaderModule(module);
        if (mapped) device.unmapMemory(memory);
        if (buffer) device.destroyBuffer(buffer);
        if (memory) device.freeMemory(memory);
    }
    void prepare(const Slice* refs) {
        const auto device = VulkanContext::device();
        const auto& props = VulkanContext::device_properties();
        if (props.max_workgroup_size[0] < 16 || props.max_workgroup_size[1] < 4
            || props.max_workgroup_invocations < 64)
            GPU_THROW("Kernel: device cannot execute the (16,4,1) local shape");
        limit = std::min(limit, size_t(props.max_workgroup_count[format == Format::Slices ? 1 : 2]));
        module = device.createShaderModule(vk::ShaderModuleCreateInfo({}, words.size() * 4, words.data()));
        const vk::PushConstantRange range(vk::ShaderStageFlagBits::eCompute, 0, sizeof(KernelPush));
        layout = device.createPipelineLayout(vk::PipelineLayoutCreateInfo({}, 0, nullptr, 1, &range));
        const vk::PipelineShaderStageCreateInfo stage({}, vk::ShaderStageFlagBits::eCompute, module, "main");
        const auto created = device.createComputePipeline(VulkanContext::pipeline_cache(),
            vk::ComputePipelineCreateInfo({}, stage, layout));
        if (created.result != vk::Result::eSuccess) GPU_THROW("Kernel pipeline: " + vk::to_string(created.result));
        pipeline = created.value;
        device.destroyShaderModule(module); module = nullptr;
        for (size_t index = 1; index < stage_words.size(); ++index) {
            const auto& code = stage_words[index];
            module = device.createShaderModule(vk::ShaderModuleCreateInfo({}, code.size() * 4, code.data()));
            const vk::PipelineShaderStageCreateInfo pass({}, vk::ShaderStageFlagBits::eCompute, module, "main");
            const auto built = device.createComputePipeline(VulkanContext::pipeline_cache(),
                vk::ComputePipelineCreateInfo({}, pass, layout));
            if (built.result != vk::Result::eSuccess) GPU_THROW("Kernel stage pipeline: " + vk::to_string(built.result));
            stage_pipelines.push_back(built.value);
            device.destroyShaderModule(module); module = nullptr;
        }
        const size_t bytes = 32 + (format == Format::References ? stages.size() * 16
            : limit * (format == Format::Jobs ? 32 : 16));

        buffer = device.createBuffer(VulkanContext::buffer_create_info(bytes,
            vk::BufferUsageFlagBits::eShaderDeviceAddress | vk::BufferUsageFlagBits::eStorageBuffer
            | vk::BufferUsageFlagBits::eIndirectBuffer));
        const auto requirements = device.getBufferMemoryRequirements(buffer);
        const uint32_t type = VulkanContext::buffer_memory_type_index();
        if (!(requirements.memoryTypeBits & (1u << type))) GPU_THROW("Kernel: incompatible coherent memory type");
        vk::MemoryAllocateInfo allocation(requirements.size, type);
        allocation.pNext = &VulkanContext::allocate_flags();
        memory = device.allocateMemory(allocation);
        device.bindBufferMemory(buffer, memory, 0);
        mapped = static_cast<uint8_t*>(device.mapMemory(memory, 0, VK_WHOLE_SIZE));
        std::memset(mapped, 0, bytes);
        address = device.getBufferAddress(vk::BufferDeviceAddressInfo(buffer));
        if (format == Format::References) {
            *reinterpret_cast<uint64_t*>(mapped) = VulkanKernel::device_address(*refs);
            reinterpret_cast<uint32_t*>(mapped)[2] = uint32_t(refs->size<uint32_t>() - 1);
            reinterpret_cast<uint32_t*>(mapped)[7] = resources;
        } else {
            *reinterpret_cast<SliceMirror*>(mapped) = SliceMirror{address, 0, 32};
        }
        command_pools.reserve(VulkanContext::compute_families().size());
        commands.reserve(VulkanContext::compute_families().size());
        fence = device.createFence({});
        for (const uint32_t family : VulkanContext::compute_families()) {
            const auto command_pool = device.createCommandPool(vk::CommandPoolCreateInfo({}, family));
            command_pools.push_back(command_pool);
            const auto command = device.allocateCommandBuffers(vk::CommandBufferAllocateInfo(
                command_pool, vk::CommandBufferLevel::ePrimary, 1))[0];
            commands.push_back(command);
            const KernelPush push{address, VulkanKernel::gpu_pool_address()};
            static_cast<void>(command.begin(vk::CommandBufferBeginInfo{}));
            command.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            command.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
            if (stages.empty()) {
                command.dispatchIndirect(buffer, 16);
            } else {
                for (size_t index = 0; index < stages.size(); ++index) {
                    command.bindPipeline(vk::PipelineBindPoint::eCompute,
                        index == 0 ? pipeline : stage_pipelines[index - 1]);
                    command.dispatchIndirect(buffer, 32 + index * 16);
                    const vk::MemoryBarrier dependency(vk::AccessFlagBits::eShaderWrite,
                        vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite
                        | vk::AccessFlagBits::eIndirectCommandRead);
                    command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                        vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eDrawIndirect,
                        {}, 1, &dependency, 0, nullptr, 0, nullptr);
                }
            }

            // Coherent allocation removes flush/invalidate, not the device-to-host dependency.
            const vk::MemoryBarrier readback(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eHostRead);
            command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eHost,
                {}, 1, &readback, 0, nullptr, 0, nullptr);
            command.end();
        }
    }
    void submit(uint32_t x, uint32_t y, uint32_t z) {
        *reinterpret_cast<vk::DispatchIndirectCommand*>(mapped + 16) = vk::DispatchIndirectCommand(x, y, z);
        VulkanContext::submit_command_buffer(
            commands[VulkanContext::submission_family_slot()], fence, nullptr, nullptr);
        const auto result = VulkanContext::device().waitForFences(fence, VK_TRUE, UINT64_MAX);
        if (result != vk::Result::eSuccess) GPU_THROW("Kernel fence wait: " + vk::to_string(result));
    }
};
namespace {
void require_kernel_device() {
    if (!VulkanContext::device_present()) GPU_THROW("Kernel: no Vulkan compute device");
    if (!VulkanContext::device_properties().supports_int64) GPU_THROW("Kernel: shaderInt64 is required");
}
std::string reference_shader_source(std::string_view body) {
    std::string source("#version 450\n");
    source.append(VULKAN_GLSL_KERNEL_CORE);
    source.append(R"glsl(
layout(local_size_x = 16, local_size_y = 4, local_size_z = 1) in;
uint vulkan_index() { return gl_WorkGroupID.z; }
Slice vulkan_resources() { return gpu_slice(U32Array(vulkan_push.vulkan_table_address).v[7]); }

uint vulkan_request() {
    uint64_t table = vulkan_push.vulkan_table_address;
    uint mask = U32Array(table).v[2];
    uint first = U32Array(table).v[3];
    return U32Array(U64Array(table).v[0]).v[(first + vulkan_index()) & mask];
}
)glsl");
    source.append(body);
    source.append(R"glsl(
void main() {
    Slice invocation = gpu_slice(vulkan_request());
    vulkan_main(gpu_slice(slice_load_u32(invocation, 0u)), gpu_slice(slice_load_u32(invocation, 1u)));
}
)glsl");
    return source;
}
} // namespace
ShaderState::ShaderState(std::string_view source, std::string_view name, const Slice* references, uint32_t workgroups_x) {
    require_kernel_device();
    if (!workgroups_x || workgroups_x > VulkanContext::device_properties().max_workgroup_count[0])
        GPU_THROW("Kernel: workgroups_x is outside the device limit");
    auto words = compile_vulkan_glsl(references ? reference_shader_source(source) : public_shader_source(source),
        VulkanContext::device_properties().supports_float16, name);
    impl_ = std::make_unique<Impl>(std::move(words), name,
        references ? Impl::Format::References : Impl::Format::Slices,
        references ? references->size<uint32_t>() : PUBLIC_LIST_CAPACITY, references);
    impl_->reference_workgroups_x = workgroups_x;
}
ShaderState::ShaderState(std::span<const KernelGpuStage> stages, std::string_view name,
                        const Slice& references, uint32_t resources) {
    require_kernel_device();
    const auto& props = VulkanContext::device_properties();
    std::vector<std::vector<uint32_t>> code;
    for (const auto& stage : stages) {
        if (!stage.workgroups_x || stage.workgroups_x > props.max_workgroup_count[0]
            || stage.workgroups_y > props.max_workgroup_count[1])
            GPU_THROW("Kernel stage dispatch exceeds the device shape");
        code.push_back(compile_vulkan_glsl(reference_shader_source(stage.glsl), props.supports_float16, name));
    }
    impl_ = std::make_unique<Impl>(std::move(code), stages, name, references, resources);
}
ShaderState::ShaderState(const uint32_t* words, size_t count, std::string_view name, size_t max_jobs) {

    require_kernel_device();
    impl_ = std::make_unique<Impl>(std::vector<uint32_t>(words, words + count), name, Impl::Format::Jobs, max_jobs);
}
ShaderState::~ShaderState() = default;
const std::vector<uint32_t>& ShaderState::spirv() const { return impl_->words; }
const std::string& ShaderState::name() const { return impl_->name; }
size_t ShaderState::capacity() const { return impl_->limit; }
void ShaderState::dispatch(const Slice* streams, size_t count, uint32_t workgroups) const {
    workgroups = std::min(workgroups, VulkanContext::device_properties().max_workgroup_count[0]);
    for (size_t first = 0; first < count; first += impl_->limit) {
        const size_t n = std::min(impl_->limit, count - first);
        auto* entries = reinterpret_cast<SliceMirror*>(impl_->mapped + 32);
        for (size_t i = 0; i < n; ++i) {
            entries[i] = SliceMirror{VulkanKernel::device_address(streams[first + i]),
                uint32_t(streams[first + i].size_bytes()), 0};
        }
        reinterpret_cast<SliceMirror*>(impl_->mapped)->size = uint32_t(n * 16);
        impl_->submit(workgroups, uint32_t(n), 1);
    }
}
void ShaderState::write_job(size_t slot, const Slice& input, const void* host_handle) {
    uint8_t* record = impl_->mapped + 32 + slot * 32;
    *reinterpret_cast<SliceMirror*>(record) = SliceMirror{VulkanKernel::device_address(input), uint32_t(input.size_bytes()), 0};
    std::memcpy(record + 16, host_handle, 16);
}
void ShaderState::dispatch(size_t jobs) {
    reinterpret_cast<SliceMirror*>(impl_->mapped)->size = uint32_t(jobs * 32);
    impl_->submit(1, 1, uint32_t(jobs));
}
void ShaderState::dispatch_references(uint32_t first, uint32_t count) {
    reinterpret_cast<uint32_t*>(impl_->mapped)[3] = first;
    for (size_t index = 0; index < impl_->stages.size(); ++index) {
        const auto& stage = impl_->stages[index];
        *reinterpret_cast<vk::DispatchIndirectCommand*>(impl_->mapped + 32 + index * 16) =
            vk::DispatchIndirectCommand(stage.workgroups_x, stage.workgroups_y, stage.per_request ? count : 1);
    }
    impl_->submit(impl_->reference_workgroups_x, 1, count);

}
/** --------------------------------------------------------------------------------------------------------- Shader::Shader
 * @brief Constructs a Shader by compiling the given GLSL source and preparing the Vulkan resources.
 * @param source GLSL defining the public Shader function `void vulkan_main(Slice slice)`.
 * @param name Diagnostic name reported by shader compilation errors.
 */
Shader::Shader(std::string_view source, std::string_view name) {
    try {
        state_ = std::make_unique<ShaderState>(source, name);
    } catch (const vk::SystemError& error) {
        GPU_THROW(std::string("Vulkan Shader setup failed: ") + error.what());
    }
}
/** --------------------------------------------------------------------------------------------------------- Shader move */
Shader::Shader(Shader&& other) noexcept = default;
Shader& Shader::operator=(Shader&& other) noexcept = default;
/** --------------------------------------------------------------------------------------------------------- Shader::~Shader */
Shader::~Shader() = default;
/** --------------------------------------------------------------------------------------------------------- Shader::operator() */
std::shared_ptr<moodycamel::LightweightSemaphore> Shader::operator()(
    const Slice* slices,
    size_t count,
    void (*callback)(Slice slice),
    uint32_t workgroups
) const {
    if (!state_) GPU_THROW("Vulkan Shader: moved-from Shader cannot dispatch");
    // Public Shader remains synchronous; Kernel owns asynchronous scheduling and completion.
    state_->dispatch(slices, count, workgroups);
    if (callback != nullptr) {
        for (size_t i = 0; i < count; ++i) callback(slices[i].slice());
    }
    std::shared_ptr<moodycamel::LightweightSemaphore> done =
        std::make_shared<moodycamel::LightweightSemaphore>();
    done->signal();
    return done;
}
/** --------------------------------------------------------------------------------------------------------- Shader::operator() (one) */
std::shared_ptr<moodycamel::LightweightSemaphore> Shader::operator()(
    const Slice& slice,
    void (*callback)(Slice slice),
    uint32_t workgroups
) const {
    return (*this)(&slice, 1, callback, workgroups);
}
/** --------------------------------------------------------------------------------------------------------- GPU::run (Shader)
 * @brief Runs a prepared Shader over a list of slices, one workgroup column each.
 */
void GPU::run(const Shader& program, Slice* streams, size_t stream_count) {
    program(streams, stream_count);
}
namespace {
/** --------------------------------------------------------------------------------------------------------- Encoded Program Header
 * @struct EncodedProgramHeader
 * @brief The 32-byte encoded-program header: magic, version, stream/pass shape, then the SPIR-V
 * words (padded to 8 bytes) and the diagnostic name (NUL-terminated, padded to 8) follow it.
 */
struct EncodedProgramHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t stream_count;
    uint32_t workgroups_hint;
    uint32_t pass_count;
    uint32_t register_seed_count;
    uint32_t constant_count;
    uint32_t reserved;
};
static_assert(sizeof(EncodedProgramHeader) == 32, "EncodedProgramHeader must be 32 bytes.");
constexpr uint32_t ENCODED_PROGRAM_MAGIC =
    uint32_t('N') | (uint32_t('B') << 8) | (uint32_t('G') << 16) | (uint32_t('P') << 24);
constexpr uint32_t ENCODED_PROGRAM_VERSION = 1u;
/** --------------------------------------------------------------------------------------------------------- shader_state_of
 * @brief Extracts the ShaderState* out of a Shader's single-pointer ABI (gpu.hpp's own static_assert).
 */
ShaderState* shader_state_of(const Shader& shader) {
    static_assert(sizeof(Shader) == sizeof(ShaderState*),
        "Shader must stay exactly one pointer for this extraction to be valid.");
    ShaderState* state;
    std::memcpy(&state, &shader, sizeof(ShaderState*));
    return state;
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- GPU::encode */
Slice GPU::encode(const Shader& program) {
    const ShaderState* state = shader_state_of(program);
    if (state == nullptr) GPU_THROW("GPU::encode: moved-from Shader cannot be encoded.");
    const std::vector<uint32_t>& spirv = state->spirv();
    const std::string& name = state->name();
    const size_t spirv_bytes = spirv.size() * sizeof(uint32_t);
    const size_t spirv_padded = (spirv_bytes + 7u) & ~size_t{7};
    const size_t name_bytes = name.size() + 1;
    const size_t name_padded = (name_bytes + 7u) & ~size_t{7};
    Slice encoded(sizeof(EncodedProgramHeader) + spirv_padded + name_padded);
    uint8_t* base = encoded.data<uint8_t>();
    /// stream_count 0: the list's own descriptor carries N at dispatch time.
    EncodedProgramHeader header{ENCODED_PROGRAM_MAGIC, ENCODED_PROGRAM_VERSION, 0u, 1u, 1u, 0u, 0u, 0u};
    std::memcpy(base, &header, sizeof(header));
    std::memcpy(base + sizeof(header), spirv.data(), spirv_bytes);
    std::memcpy(base + sizeof(header) + spirv_padded, name.c_str(), name_bytes);
    return encoded;
}
/** --------------------------------------------------------------------------------------------------------- GPU::decode */
Shader GPU::decode(const Slice&) {
    GPU_THROW("GPU::decode: blocked on a locked-header gap (report G0-5) — Shader has no "
        "accessible way to construct from a prepared ShaderState; needs friend struct GPU "
        "or a private Shader(std::unique_ptr<ShaderState>) constructor in gpu.hpp.");
}
namespace vulkan {
namespace {
/** --------------------------------------------------------------------------------------------------------- JobTableSlot
 * @struct JobTableSlot
 * @brief One cached job-table engine, keyed by its compiled program's stable host address (Law 3:
 * prepared once on first sight, looked up lock-free on every later GPU::run call).
 */
struct JobTableSlot {
    std::atomic<const void*> key{nullptr};
    std::atomic<ShaderState*> state{nullptr};
};
constexpr size_t JOB_TABLE_REGISTRY_SLOTS = 16;
constexpr size_t JOB_TABLE_ROUND_CAPACITY = 1024;
/** --------------------------------------------------------------------------------------------------------- job_table_engine_for
 * @brief Looks up, or on first sight prepares, the megakernel engine behind one compile_glsl program.
 * @param program A Slice returned by GPU::compile_glsl, expected to outlive every call using it.
 */
ShaderState& job_table_engine_for(const Slice& program) {
    static JobTableSlot registry[JOB_TABLE_REGISTRY_SLOTS];
    const void* key = program.raw();
    for (JobTableSlot& slot : registry) {
        const void* seen = slot.key.load(std::memory_order_acquire);
        if (seen == nullptr) {
            const void* expected = nullptr;
            if (slot.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel)) {
                slot.state.store(
                    new ShaderState(program.data<uint32_t>(), program.size<uint32_t>(),
                        "vulkan_gpu_run_job_table", JOB_TABLE_ROUND_CAPACITY),
                    std::memory_order_release);
            } else if (expected != key) {
                continue;
            }
        } else if (seen != key) {
            continue;
        }
        ShaderState* state = slot.state.load(std::memory_order_acquire);
        while (state == nullptr) state = slot.state.load(std::memory_order_acquire);
        return *state;
    }
    GPU_THROW("GPU::run: job-table program registry is full (raise JOB_TABLE_REGISTRY_SLOTS)");
}
} // namespace
} // namespace vulkan
/** --------------------------------------------------------------------------------------------------------- GPU::run (job table)
 * @brief Runs a compile_glsl program with one stream per job, in rounds of the engine's table capacity.
 */
void GPU::run(const Slice& program, Slice* streams, size_t stream_count) {
    auto& engine = vulkan::job_table_engine_for(program);
    const uint64_t host_handle[2] = {0, 0};
    for (size_t start = 0; start < stream_count; start += engine.capacity()) {
        const size_t round = std::min(engine.capacity(), stream_count - start);
        for (size_t i = 0; i < round; ++i) {
            engine.write_job(i, streams[start + i], host_handle);
        }
        engine.dispatch(round);
    }
}
/** --------------------------------------------------------------------------------------------------------- Prepare Vulkan Runtime
 * @brief Avoids waiting for a worker to dlopen the driver while extension import holds the loader lock.
 */
void VulkanStaticMethods::prepare_runtime() {
    static_cast<void>(VulkanContext::instance());
}
/** --------------------------------------------------------------------------------------------------------- Vulkan Allocator
 * @brief Allocates one mapped, device-addressable slab on the rung named by `context`.
 * @param size The slab size in bytes.
 * @param context The rung index (from the Placemat's `get_context`).
 * @return The host mapping and the VulkanBuffer handle pair.
 */
std::pair<void*, void*> VulkanStaticMethods::vulkan_allocator(size_t size, void* context) {
    const uint8_t rung = *static_cast<const uint8_t*>(context);
    VulkanBuffer* slab = new VulkanBuffer(size, VulkanContext::instance().placement_type_indices_[rung]);
    return {slab->host(), slab};
}
/** --------------------------------------------------------------------------------------------------------- Deallocate
 * @brief Destroys the slab behind a handle.
 * @param host_ptr The host mapping of the slab.
 * @param substrate_handle The VulkanBuffer handle backing the mapping.
 * @return The cleared host pointer and substrate handle pair.
 */
std::pair<void*, void*> VulkanStaticMethods::vulkan_deallocate(void* host_ptr, void* substrate_handle) {
    static_cast<void>(host_ptr);
    delete static_cast<VulkanBuffer*>(substrate_handle);
    return {nullptr, nullptr};
}
/** --------------------------------------------------------------------------------------------------------- Device Address
 * @brief The device address of a slab's first byte.
 * @param substrate_handle The VulkanBuffer handle backing the slab.
 * @return The buffer device address.
 */
uint64_t VulkanStaticMethods::vulkan_device_address(void* substrate_handle) {
    return static_cast<const VulkanBuffer*>(substrate_handle)->address();
}
/** --------------------------------------------------------------------------------------------------------- Get Vulkan Context
 * @brief The un-rung'd context hook: the default rung index.
 * @return A pointer to the HOST_VISIBLE rung index.
 */
void* VulkanStaticMethods::vulkan_get_context() {
    return VulkanPlacements::rung_context<static_cast<uint8_t>(PlacementIndex::HOST_VISIBLE)>();
}
/** --------------------------------------------------------------------------------------------------------- Placement table
 * @brief The Placemat behind each alligator placement: the heap built-in for the host-only rungs,
 * one Vulkan Placemat per device rung.
 */
const Placemat* const Placemat::HOST = BuffetMenu::get("heap");
const Placemat* const Placemat::HOST_VISIBLE = VulkanPlacements::rung<1>("vulkan_host_visible");
const Placemat* const Placemat::HOST_CACHEABLE = VulkanPlacements::rung<2>("vulkan_host_cacheable");
const Placemat* const Placemat::DEVICE = VulkanPlacements::rung<3>("vulkan_device");
const Placemat* const Placemat::UNIFIED = VulkanPlacements::rung<4>("vulkan_unified");
const Placemat* const Placemat::BASIC_HEAP = BuffetMenu::get("heap");
} // namespace buffetalligator
