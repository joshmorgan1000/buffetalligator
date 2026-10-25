/** --------------------------------------------------------------------------------------------------------- Vulkan Implementation
 * @file vulkan.cpp
 * @brief Implements the Vulkan context, slabs, and the prepared Shader behind the public handle.
 */
#include <logging.hpp>
#include <alligator.hpp>
#include <alligator/kitchen.hpp>
#include <alligator/easygpu.hpp>
#include <gpu/easyvulkan.hpp>
#include <gpu/backend.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Context Instance
 * @brief Retains the lazily probed context until static owners and arena storage have retired.
 */
VulkanContext& VulkanContext::instance() {
    static VulkanContext* context = new VulkanContext;
    return *context;
}
/** --------------------------------------------------------------------------------------------------------- unified_from_memory_properties
 * @brief Detects shared device-local host access separately from physical device topology.
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
 * @brief Probes compatible compute devices and freezes the process Vulkan context.
 */
VulkanContext::VulkanContext() {
    try {
    const char* requested = std::getenv("ALLIGATOR_GPU_BACKEND");
    const std::string_view backend = requested == nullptr ? "auto" : requested;
    if (backend == "cpu" || backend == "metal" || backend == "cuda") return;

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
    } catch (const vk::SystemError& error) {
        if (error.code().value() == int(vk::Result::eErrorIncompatibleDriver)) return;
        throw;
    }
    // No physical GPU is a normal state: the singleton exists and reports device_present() == false.
    for (const vk::PhysicalDevice& device : instance_.enumeratePhysicalDevices()) {
        vk::PhysicalDeviceVulkan12Features candidate12{};
        vk::PhysicalDeviceFeatures2 candidate{};
        candidate.pNext = &candidate12;
        device.getFeatures2(&candidate);
        if (!candidate12.bufferDeviceAddress || !candidate.features.shaderInt64) continue;
        bool has_compute = false;
        for (const auto& family : device.getQueueFamilyProperties()) {
            has_compute |= family.queueCount != 0 && bool(family.queueFlags & vk::QueueFlagBits::eCompute);
        }
        const vk::MemoryPropertyFlags host_flags =
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
        const auto memory = device.getMemoryProperties();
        bool has_host_memory = false;
        for (uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
            has_host_memory |= (memory.memoryTypes[index].propertyFlags & host_flags) == host_flags;
        }
        if (!has_compute || !has_host_memory) continue;
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
        ALLIGATOR_GPU_THROW("VulkanContext: no compute-capable queue family");
    }
    vk::PhysicalDeviceVulkan12Features supported12{};
    vk::PhysicalDeviceInternallySynchronizedQueuesFeaturesKHR supported_isq{};
    vk::PhysicalDeviceFeatures2 supported{};
    supported.pNext = &supported12;
    const auto extensions = physical_device_.enumerateDeviceExtensionProperties();
    for (const auto& extension : extensions) {
        if (std::strcmp(extension.extensionName.data(),
                VK_KHR_INTERNALLY_SYNCHRONIZED_QUEUES_EXTENSION_NAME) == 0)
            supported12.pNext = &supported_isq;
    }
    physical_device_.getFeatures2(&supported);
    if (!supported12.bufferDeviceAddress) {
        ALLIGATOR_GPU_THROW("VulkanContext: device lacks bufferDeviceAddress; compute requires it");
    }
    if (!supported12.timelineSemaphore) {
        ALLIGATOR_GPU_THROW("VulkanContext: device lacks timelineSemaphore; Vulkan 1.2 requires it");
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
    enabled12.timelineSemaphore = VK_TRUE;
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
    vk::PhysicalDeviceMaintenance3Properties allocation_limits{};
    subgroup.pNext = &allocation_limits;
    vk::PhysicalDeviceProperties2 props2{};
    props2.pNext = &subgroup;
    physical_device_.getProperties2(&props2);
    const vk::PhysicalDeviceLimits& limits = props2.properties.limits;
    max_allocation_size_ = allocation_limits.maxMemoryAllocationSize;
    max_allocation_count_ = limits.maxMemoryAllocationCount;
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
    device_props_.max_allocation_bytes = std::min(max_allocation_size_,
        static_cast<vk::DeviceSize>(limits.maxStorageBufferRange));
    device_props_.max_allocation_count = max_allocation_count_;
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
    device_props_.supports_timeline_semaphore = true;
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
    device_props_.unified_memory = props2.properties.deviceType == vk::PhysicalDeviceType::eIntegratedGpu
        && unified_from_memory_properties(memory_properties_);
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
    placement_type_indices_.fill(buffer_memory_type_index_);
    placement_cpu_cached_.fill(bool(memory_properties_.memoryTypes[buffer_memory_type_index_].propertyFlags
        & vk::MemoryPropertyFlagBits::eHostCached));
    pipeline_cache_ = device_.createPipelineCache({});
    device_present_ = true;
    device_props_.gpu_free_bytes = poll_budget_headroom();
    alive_.store(true, std::memory_order_release);
} catch (...) {
    if (device_) device_.destroy();
    if (instance_) instance_.destroy();
    throw;
    }
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
/** --------------------------------------------------------------------------------------------------------- Buffer Heap Headroom
 * @brief Reports the live budget headroom of the heap backing the chosen buffer memory type.
 * @return Budget minus usage for the buffer heap, or UINT64_MAX without the budget extension.
 */
uint64_t VulkanContext::buffer_heap_headroom() {
    const VulkanContext& context = instance();
    if (!context.device_props_.supports_memory_budget) {
        return UINT64_MAX;
    }
    vk::PhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    vk::PhysicalDeviceMemoryProperties2 memory2{};
    memory2.pNext = &budget;
    context.physical_device_.getMemoryProperties2(&memory2);
    const uint32_t heap =
        context.memory_properties_.memoryTypes[context.buffer_memory_type_index_].heapIndex;
    return budget.heapBudget[heap] > budget.heapUsage[heap]
        ? budget.heapBudget[heap] - budget.heapUsage[heap] : 0ull;
}
/** --------------------------------------------------------------------------------------------------------- destructor
 * @brief Drain the device and tear down. Caller-owned buffers and shaders must already be destroyed.
 */
VulkanContext::~VulkanContext() {
    alive_.store(false, std::memory_order_release);
    if (!device_) {
        return;
    }
    device_.waitIdle();
    device_.destroyPipelineCache(pipeline_cache_);
    device_.destroy();
    instance_.destroy();
}
/** --------------------------------------------------------------------------------------------------------- Assign Queue
 * @brief Hands a prepared shader its own compute queue, round-robin across the device's queues.
 * @return The queue index.
 */
uint32_t VulkanContext::assign_queue() {
    VulkanContext& context = instance();
    const uint32_t count = uint32_t(context.compute_queues_.size());
    return context.next_queue_slot_.fetch_add(1, std::memory_order_relaxed) % count;
}
/** --------------------------------------------------------------------------------------------------------- Queue Family
 * @brief The queue family a queue index belongs to.
 * @param queue The queue index.
 * @return The family index.
 */
uint32_t VulkanContext::queue_family(uint32_t queue) {
    const VulkanContext& context = instance();
    return context.compute_families_[context.queue_family_slots_[queue]];
}
/** --------------------------------------------------------------------------------------------------------- Submit
 * @brief Submits to one queue under the per-queue atomic claim Vulkan's external synchronization rule for
 * vkQueueSubmit requires, skipping the claim when the device synchronizes its queues internally.
 * @param queue The queue index.
 * @param info The submission.
 */
void VulkanContext::submit(uint32_t queue, const vk::SubmitInfo& info) {
    VulkanContext& context = instance();
    if (context.internally_synchronized_queues_) {
        context.compute_queues_[queue].submit(info);
        return;
    }
    std::atomic_flag& claim = context.queue_claims_[queue];
    while (claim.test_and_set(std::memory_order_acquire)) {
        const uint64_t epoch = context.queue_epoch_.load(std::memory_order_acquire);
        if (claim.test(std::memory_order_acquire)) context.queue_epoch_.wait(epoch, std::memory_order_acquire);
    }
    try {
        context.compute_queues_[queue].submit(info);
    } catch (...) {
        claim.clear(std::memory_order_release);
        context.queue_epoch_.fetch_add(1, std::memory_order_release);
        context.queue_epoch_.notify_all();
        throw;
    }
    claim.clear(std::memory_order_release);
    context.queue_epoch_.fetch_add(1, std::memory_order_release);
    context.queue_epoch_.notify_all();
}
/** --------------------------------------------------------------------------------------------------------- buffer_create_info
 * @brief Shares a buffer across every compute family when the device exposes more than one.
 */
vk::BufferCreateInfo VulkanContext::buffer_create_info(vk::DeviceSize bytes, vk::BufferUsageFlags usage) {
    return instance().shared_buffer_info(bytes, usage);
}
/** --------------------------------------------------------------------------------------------------------- buffer_placement
 * @brief The VulkanBuffer descriptor: mapped, device-addressable storage on the probed memory type.
 * @return The placement.
 */
const BuffetDescriptor* VulkanContext::buffer_placement() {
    return BuffetDescriptors::descriptor<VulkanBuffer>();
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
/** --------------------------------------------------------------------------------------------------------- VulkanBuffer Constructor
 * @brief Allocates coherent device-visible storage through the probed placement.
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
    if (size_bytes == 0) return;
    if (!VulkanContext::device_present()) ALLIGATOR_GPU_THROW("VulkanBuffer requires a compatible device");
    VulkanContext& context = VulkanContext::instance();
    const vk::MemoryPropertyFlags required =
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    if (memory_type_index >= context.memory_properties_.memoryTypeCount
        || (context.memory_properties_.memoryTypes[memory_type_index].propertyFlags & required) != required)
        ALLIGATOR_GPU_THROW("VulkanBuffer requires a host-visible coherent memory type");
    const uint32_t heap = context.memory_properties_.memoryTypes[memory_type_index].heapIndex;
    if (size_bytes > context.max_allocation_size_
        || size_bytes > context.memory_properties_.memoryHeaps[heap].size)
        ALLIGATOR_GPU_THROW("VulkanBuffer exceeds the device allocation-size limit ("
            + std::to_string(size_bytes) + " requested, "
            + std::to_string(std::min<uint64_t>(context.max_allocation_size_,
                context.memory_properties_.memoryHeaps[heap].size)) + " probed)");
    try {
        buffer_ = context.device_.createBuffer(
            context.shared_buffer_info(size_bytes, context.buffer_usage_));
        const vk::MemoryRequirements requirements = context.device_.getBufferMemoryRequirements(buffer_);
        if (!(requirements.memoryTypeBits & (1u << memory_type_index)))
            ALLIGATOR_GPU_THROW("VulkanBuffer memory type is incompatible with the requested buffer");
        if (requirements.size > context.max_allocation_size_
            || requirements.size > context.memory_properties_.memoryHeaps[heap].size)
            ALLIGATOR_GPU_THROW("VulkanBuffer allocation requirements exceed the device limit");
        if (context.device_props_.supports_memory_budget) {
            vk::PhysicalDeviceMemoryBudgetPropertiesEXT budget;
            vk::PhysicalDeviceMemoryProperties2 memory_properties;
            memory_properties.pNext = &budget;
            context.physical_device_.getMemoryProperties2(&memory_properties);
            const uint64_t headroom = budget.heapBudget[heap] > budget.heapUsage[heap]
                ? budget.heapBudget[heap] - budget.heapUsage[heap] : 0;
            if (requirements.size > headroom)
                ALLIGATOR_GPU_THROW("VulkanBuffer allocation exceeds the current heap budget (heap "
                    + std::to_string(heap) + ": " + std::to_string(requirements.size)
                    + " requested, " + std::to_string(headroom) + " of "
                    + std::to_string(budget.heapBudget[heap]) + " budget left after "
                    + std::to_string(budget.heapUsage[heap]) + " in use)");
        }
        vk::MemoryAllocateInfo allocate_info(requirements.size, memory_type_index);
        allocate_info.pNext = &context.allocate_flags_;
        if (context.allocation_count_.fetch_add(1, std::memory_order_acq_rel)
            >= context.max_allocation_count_) {
            context.allocation_count_.fetch_sub(1, std::memory_order_release);
            ALLIGATOR_GPU_THROW("VulkanBuffer exceeds the device allocation-count limit");
        }
        try {
            memory_ = context.device_.allocateMemory(allocate_info);
        } catch (...) {
            context.allocation_count_.fetch_sub(1, std::memory_order_release);
            throw;
        }
        context.device_.bindBufferMemory(buffer_, memory_, 0);
        host_ = context.device_.mapMemory(memory_, 0, VK_WHOLE_SIZE);
        address_ = context.device_.getBufferAddress(vk::BufferDeviceAddressInfo(buffer_));
        std::memset(host_, 0, size_);
    } catch (...) {
        release();
        throw;
    }
}
/** --------------------------------------------------------------------------------------------------------- VulkanBuffer Release
 * @brief Releases completed or partially constructed Vulkan storage through its owning context.
 */
void VulkanBuffer::release() {
    if (buffer_ || memory_) {
        VulkanContext& context = VulkanContext::instance();
        const vk::Device device = context.device_;
        if (host_) device.unmapMemory(memory_);
        if (buffer_) device.destroyBuffer(buffer_);
        if (memory_) {
            device.freeMemory(memory_);
            context.allocation_count_.fetch_sub(1, std::memory_order_release);
        }
    }
    buffer_ = nullptr;
    memory_ = nullptr;
    host_ = nullptr;
    address_ = 0;
    size_ = 0;
}
/** --------------------------------------------------------------------------------------------------------- Shader Implementation
 * @struct Shader::Impl
 * @brief One compiled pipeline, one queue, one timeline semaphore, and IN_FLIGHT command buffers; a dispatch
 * records one Slice id into its slot's command buffer and submits. Nothing is copied anywhere.
 */
struct Shader::Impl {
    /// @brief The GLSL push block's bytes: the Slice as the host holds it, then whether it lists the rows.
    static constexpr uint32_t PUSH_BYTES = sizeof(Slice) + 4 * sizeof(uint32_t);
    static_assert(PUSH_BYTES == 80, "The push block must mirror the GLSL AlligatorPush block");
    /// @brief One dispatch handed to the retirement thread: which slot's timeline, which value, whose result.
    struct Pending {
        std::shared_ptr<ShaderResult::State> result;
        uint32_t slot = 0;
        uint64_t value = 0;
        bool stop = false;
    };
    /// @brief Dispatches that may be in flight before a caller waits for a slot to come back.
    static constexpr uint32_t IN_FLIGHT = 8;
    std::string name;
    std::atomic<uint32_t> workgroups;
    uint32_t queue = 0;                                       ///< This shader's compute queue.
    vk::PipelineLayout layout{};
    vk::Pipeline pipeline{};
    std::array<vk::CommandPool, IN_FLIGHT> pools{};           ///< One pool per slot; recording needs no lock.
    std::array<vk::CommandBuffer, IN_FLIGHT> commands{};
    std::array<vk::Semaphore, IN_FLIGHT> timelines{};         ///< One timeline per slot, counting its uses.
    std::atomic<uint64_t> next{0};                            ///< Picks the slot; its uses order themselves.
    moodycamel::BlockingConcurrentQueue<Pending> pending;     ///< Dispatches awaiting the retirement thread.
    std::thread retirement;
    Impl(std::string_view glsl, std::string_view label, uint32_t groups);
    ~Impl();
    void release();
    void dispatch(const Slice& slice, bool list, uint32_t rows, std::shared_ptr<ShaderResult::State> result);
    static void retire(Impl* self) noexcept;
};
/** --------------------------------------------------------------------------------------------------------- Shader Preparation
 * @brief Compiles the pipeline, makes one command pool, command buffer and timeline semaphore per slot, and
 * starts the retirement thread; a dispatch creates nothing.
 */
Shader::Impl::Impl(std::string_view glsl, std::string_view label, uint32_t groups) : name(label) {
    if (!VulkanContext::device_present()) ALLIGATOR_GPU_THROW("Shader preparation requires a Vulkan compute device");
    const DeviceProperties& properties = VulkanContext::device_properties();
    if (properties.max_workgroup_size[0] < 16 || properties.max_workgroup_size[1] < 4
        || properties.max_workgroup_invocations < 64)
        ALLIGATOR_GPU_THROW("Shader: device cannot execute the (16,4,1) local shape");
    workgroups.store(groups ? groups : std::max(1u, properties.max_workgroup_invocations / 64u),
        std::memory_order_relaxed);
    queue = VulkanContext::assign_queue();
    const vk::Device device = VulkanContext::device();
    const std::vector<uint32_t> words =
        shader_compile_glsl(public_shader_source(glsl), properties.supports_float16, name);
    try {
        const vk::PushConstantRange range(vk::ShaderStageFlagBits::eCompute, 0, PUSH_BYTES);
        layout = device.createPipelineLayout(vk::PipelineLayoutCreateInfo({}, 0, nullptr, 1, &range));
        {
            // vkCreateComputePipelines requires host access to the VkPipelineCache to be externally synchronized.
            std::lock_guard cache_lock(VulkanContext::pipeline_mutex());
            const vk::ShaderModule module =
                device.createShaderModule(vk::ShaderModuleCreateInfo({}, words.size() * 4, words.data()));
            const vk::PipelineShaderStageCreateInfo stage({}, vk::ShaderStageFlagBits::eCompute, module, "main");
            const auto built = device.createComputePipeline(VulkanContext::pipeline_cache(),
                vk::ComputePipelineCreateInfo({}, stage, layout));
            device.destroyShaderModule(module);
            if (built.result != vk::Result::eSuccess)
                ALLIGATOR_GPU_THROW("Shader pipeline: " + vk::to_string(built.result));
            pipeline = built.value;
        }
        const vk::SemaphoreTypeCreateInfo timeline_type(vk::SemaphoreType::eTimeline, 0);
        vk::SemaphoreCreateInfo semaphore_info{};
        semaphore_info.pNext = &timeline_type;
        for (uint32_t slot = 0; slot < IN_FLIGHT; ++slot) {
            pools[slot] = device.createCommandPool(vk::CommandPoolCreateInfo(
                vk::CommandPoolCreateFlagBits::eResetCommandBuffer, VulkanContext::queue_family(queue)));
            commands[slot] = device.allocateCommandBuffers(
                vk::CommandBufferAllocateInfo(pools[slot], vk::CommandBufferLevel::ePrimary, 1))[0];
            timelines[slot] = device.createSemaphore(semaphore_info);
        }
        retirement = std::thread(&Impl::retire, this);
    } catch (...) {
        release();
        throw;
    }
}
/** --------------------------------------------------------------------------------------------------------- Shader Teardown
 * @brief Stops the retirement thread after everything queued to it, waits for every slot's last use on the
 * device, and releases.
 */
Shader::Impl::~Impl() {
    if (retirement.joinable()) {
        pending.enqueue(Pending{nullptr, 0, 0, true});
        retirement.join();
        const vk::Device device = VulkanContext::device();
        const uint64_t issued = next.load(std::memory_order_acquire);
        for (uint32_t slot = 0; slot < IN_FLIGHT && slot < issued; ++slot) {
            const uint64_t uses = (issued - 1 - slot) / IN_FLIGHT + 1;
            const vk::SemaphoreWaitInfo wait({}, 1, &timelines[slot], &uses);
            static_cast<void>(device.waitSemaphores(wait, UINT64_MAX));
        }
    }
    release();
}
/** --------------------------------------------------------------------------------------------------------- Release
 * @brief Destroys the Vulkan objects this shader created.
 */
void Shader::Impl::release() {
    const vk::Device device = VulkanContext::device();
    for (uint32_t slot = 0; slot < IN_FLIGHT; ++slot) {
        if (pools[slot]) device.destroyCommandPool(pools[slot]);
        if (timelines[slot]) device.destroySemaphore(timelines[slot]);
        pools[slot] = nullptr;
        timelines[slot] = nullptr;
    }
    if (pipeline) device.destroyPipeline(pipeline);
    if (layout) device.destroyPipelineLayout(layout);
    pipeline = nullptr;
    layout = nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Dispatch
 * @brief Takes the next slot, waits on that slot's timeline for its previous use, records the one Slice and
 * the dimensions, and submits signaling the slot's next value.
 * @param slice The Slice, pushed as it is for the shader to read its payload through.
 * @param list Whether that Slice holds one Slice per workgroup row.
 * @param rows Workgroup rows along Y.
 * @param result The shared state completed when the dispatch retires.
 */
void Shader::Impl::dispatch(
    const Slice& slice,
    bool list,
    uint32_t rows,
    std::shared_ptr<ShaderResult::State> result
) {
    const uint64_t ticket = next.fetch_add(1, std::memory_order_relaxed);
    const uint32_t slot = uint32_t(ticket % IN_FLIGHT);
    const uint64_t use = ticket / IN_FLIGHT;
    const vk::Device device = VulkanContext::device();
    if (use != 0) {
        const vk::SemaphoreWaitInfo previous({}, 1, &timelines[slot], &use);
        while (device.waitSemaphores(previous, UINT64_MAX) == vk::Result::eTimeout) {}
    }
    const vk::CommandBuffer command = commands[slot];
    const uint32_t flags[4] = {list ? 1u : 0u, 0u, 0u, 0u};
    const vk::MemoryBarrier publication(vk::AccessFlagBits::eHostWrite,
        vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
    const vk::MemoryBarrier readback(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eHostRead);
    // sync
    command.reset();
    command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    command.pipelineBarrier(vk::PipelineStageFlagBits::eHost, vk::PipelineStageFlagBits::eComputeShader,
        {}, 1, &publication, 0, nullptr, 0, nullptr);
    command.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
    command.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(Slice), &slice);
    command.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, sizeof(Slice), sizeof(flags), flags);
    command.dispatch(workgroups.load(std::memory_order_relaxed), rows, 1);
    command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eHost,
        {}, 1, &readback, 0, nullptr, 0, nullptr);
    command.end();
    // async
    const uint64_t signal = use + 1;
    const vk::TimelineSemaphoreSubmitInfo timeline_info(0, nullptr, 1, &signal);
    vk::SubmitInfo info({}, {}, commands[slot], timelines[slot]);
    info.pNext = &timeline_info;
    pending.enqueue(Pending{std::move(result), slot, signal, false});
    try {
        // sync
        VulkanContext::submit(queue, info);
    } catch (...) {
        // Nothing is pending on this slot's timeline, so the host signals the value the device never will.
        device.signalSemaphore(vk::SemaphoreSignalInfo(timelines[slot], signal));
        throw;
    }
}
/** --------------------------------------------------------------------------------------------------------- Retirement Thread
 * @brief Takes each dispatch as it was queued, waits on its slot's timeline for its value, and completes its
 * result: the callback runs here and the result's semaphore is released.
 * @param self The shader.
 */
void Shader::Impl::retire(Impl* self) noexcept {
    const vk::Device device = VulkanContext::device();
    try {
        for (;;) {
            Pending item;
            self->pending.wait_dequeue(item);
            if (item.stop) return;
            const vk::SemaphoreWaitInfo wait({}, 1, &self->timelines[item.slot], &item.value);
            while (device.waitSemaphores(wait, UINT64_MAX) == vk::Result::eTimeout) {}
            if (item.result) ShaderResult::complete(*item.result);
        }
    } catch (const std::exception& error) {
        LOG_ERROR_STREAM << "Shader " << self->name << " retirement stopped: " << error.what();
    }
}
/** --------------------------------------------------------------------------------------------------------- Shader Construction
 * @brief Prepares every Vulkan object behind the one-pointer public handle.
 */
Shader::Shader(const ShaderSource& source, std::string_view name, uint32_t workgroups)
    : state_(std::make_unique<Impl>(source.glsl, name, workgroups)) {}
Shader::Shader(std::string_view glsl, std::string_view name, uint32_t workgroups)
    : Shader(ShaderSource{glsl, {}, {}}, name, workgroups) {}
Shader::Shader(Shader&&) noexcept = default;
Shader& Shader::operator=(Shader&&) noexcept = default;
Shader::~Shader() = default;
uint32_t Shader::workgroups() const { return state_->workgroups.load(std::memory_order_relaxed); }
/** --------------------------------------------------------------------------------------------------------- Shader Set Workgroups
 * @brief Sets the number of workgroups for the shader.
 * @param count The number of workgroups to set.
 */
void Shader::set_workgroups(uint32_t count) {
    state_->workgroups.store(count, std::memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Shader Dispatch
 * @brief A list Slice dispatches one row per id it holds; a single Slice dispatches one row that is the
 * Slice.
 * @param slices The Slice or list of Slices to dispatch.
 * @param count The number of rows to dispatch for a list Slice; ignored for a single Slice.
 * @param done Callback to invoke when the dispatch completes.
 * @param context Context to pass to the callback.
 */
ShaderResult Shader::operator()(
    const Slice& slices,
    size_t count,
    void (*done)(std::shared_ptr<void>),
    std::shared_ptr<void> context
) const {
    ShaderResult result(done, std::move(context));
    state_->dispatch(slices, true, uint32_t(count), result.state_);
    return result;
}
/** --------------------------------------------------------------------------------------------------------- Shader Dispatch (Single Slice)
 * @brief Dispatches a single Slice.
 * @param slice The Slice to dispatch.
 * @param done Callback to invoke when the dispatch completes.
 * @param context Context to pass to the callback.
 */
ShaderResult Shader::operator()(const Slice& slice, void (*done)(std::shared_ptr<void>),
    std::shared_ptr<void> context) const {
    ShaderResult result(done, std::move(context));
    state_->dispatch(slice, false, 1, result.state_);
    return result;
}
} // namespace buffetalligator
