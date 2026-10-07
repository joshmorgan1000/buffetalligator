/** --------------------------------------------------------------------------------------------------------- Vulkan Implementation
 * @file vulkan.cpp
 * @brief Implements Vulkan slabs, GLSL compilation, and the public prepared Shader interface.
 */
#include <logging.hpp>
#include <alligator.hpp>
#include <alligator/kitchen.hpp>
#include <alligator/easygpu.hpp>
#include <alligator/easyvulkan.hpp>
#include <vulkan/shader_state.hpp>
#include <memory/tracker.hpp>
#include <memory/lifetime.hpp>
#include <condition_variable>
#include <mutex>
#include <exception>
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Context Instance
 * @brief Retains the lazily probed context until static owners and arena storage have retired.
 */
VulkanContext& VulkanContext::instance() {
    static RuntimeFinalizer lifetime(new VulkanContext, &RuntimeFinalizer::delete_owner<VulkanContext>);
    return *static_cast<VulkanContext*>(lifetime.object);
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
    shader_runtime_initialize();
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
        ALLIGATOR_GPU_THROW("VulkanCompute::init_instance_and_device: no compute-capable queue family");
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
 * @brief Drain the device and tear down. Caller-owned buffers, rigs, and pipelines must
 * already be destroyed.
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
/** --------------------------------------------------------------------------------------------------------- Submission Queue
 * @brief Acquires one queue for the duration of a host submission.
 */
uint32_t VulkanContext::submission_queue_index() {
    VulkanContext& context = instance();
    const uint32_t count = uint32_t(context.compute_queues_.size());
    const uint32_t first = context.next_queue_slot_.fetch_add(1, std::memory_order_relaxed) % count;
    if (context.internally_synchronized_queues_) return first;
    for (;;) {
        const uint64_t epoch = context.queue_epoch_.load(std::memory_order_acquire);
        for (uint32_t index = 0; index < count; ++index) {
            const uint32_t slot = (first + index) % count;
            if (!context.queue_claims_[slot].test_and_set(std::memory_order_acquire)) return slot;
        }
        context.queue_epoch_.wait(epoch, std::memory_order_acquire);
    }
}
/** --------------------------------------------------------------------------------------------------------- Submit Command Buffer
 * @brief Submits the command matching one acquired queue's family and releases the queue on every path.
 */
void VulkanContext::submit_command_buffer(std::span<const vk::CommandBuffer> commands, vk::Fence fence) {
    VulkanContext& context = instance();
    const uint32_t queue = submission_queue_index();
    try {
        context.device_.resetFences(fence);
        const vk::CommandBuffer command = commands[context.queue_family_slots_[queue]];
        const vk::SubmitInfo submit({}, {}, command);
        context.compute_queues_[queue].submit(submit, fence);
    } catch (...) {
        if (!context.internally_synchronized_queues_) {
            context.queue_claims_[queue].clear(std::memory_order_release);
            context.queue_epoch_.fetch_add(1, std::memory_order_release);
            context.queue_epoch_.notify_all();
        }
        throw;
    }
    if (!context.internally_synchronized_queues_) {
        context.queue_claims_[queue].clear(std::memory_order_release);
        context.queue_epoch_.fetch_add(1, std::memory_order_release);
        context.queue_epoch_.notify_all();
    }
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
    return BuffetDescriptors::get(VulkanBuffer::type_idx());
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

} // namespace
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
#ifdef BUFFETALLIGATOR_SHADER_TESTING
static std::atomic<bool> shader_test_fail_preparation{false};
static std::atomic<bool> shader_test_fail_submission{false};
static std::atomic<bool> shader_test_hold_submission{false};
static std::atomic<uint32_t> shader_test_live_slots{0};
#endif
/** --------------------------------------------------------------------------------------------------------- Shader State Resources
 * @brief Shares immutable pipelines while preparing independent mapped submission slots.
 */
struct VulkanShaderResources {
    using Format = ShaderFormat;
    Format format;
    std::vector<uint32_t> words;
    std::string name;
    size_t limit;
    vk::Pipeline pipeline{};
    std::vector<std::vector<uint32_t>> stage_words;
    std::vector<KernelGpuStage> stages;
    std::vector<vk::Pipeline> stage_pipelines;
    uint32_t resources = 0;
    vk::PipelineLayout layout{};
    vk::ShaderModule module{};
    /** ------------------------------------------------------------------------------------------- Submission Slot
     * @brief Owns every mutable resource referenced by one admitted dispatch.
     */
    struct Slot {
        std::unique_ptr<VulkanBuffer> storage;
        uint8_t* mapped = nullptr;
        Slice parameters;
        std::vector<vk::CommandPool> command_pools;
        std::vector<vk::CommandBuffer> commands;
        vk::Fence fence{};
#ifdef BUFFETALLIGATOR_SHADER_TESTING
        Slot() { shader_test_live_slots.fetch_add(1, std::memory_order_relaxed); }
#endif
        /** ----------------------------------------------------------------------------- Destructor
         * @brief Releases a retired slot's Vulkan resources.
         */
        ~Slot() {
            const auto device = VulkanContext::device();
            for (const auto pool : command_pools) device.destroyCommandPool(pool);
            if (fence) device.destroyFence(fence);
#ifdef BUFFETALLIGATOR_SHADER_TESTING
            shader_test_live_slots.fetch_sub(1, std::memory_order_relaxed);
#endif
        }
        /** ----------------------------------------------------------------------------- Submit
         * @brief Submits prepared commands and waits for host-visible retirement on a waiter thread.
         */
        void submit(uint32_t width, uint32_t height, uint32_t depth) {
#ifdef BUFFETALLIGATOR_SHADER_TESTING
            shader_test_hold_submission.wait(true, std::memory_order_acquire);
            if (shader_test_fail_submission.exchange(false, std::memory_order_acq_rel))
                ALLIGATOR_GPU_THROW("Injected Shader submission failure");
#endif
            *reinterpret_cast<vk::DispatchIndirectCommand*>(mapped + 16) =
                vk::DispatchIndirectCommand(width, height, depth);
            VulkanContext::submit_command_buffer(commands, fence);
        }
        void wait() {
            for (;;) {
                const auto result = VulkanContext::device().waitForFences(fence, VK_TRUE, 1000000000ull);
                if (result == vk::Result::eSuccess) return;
                if (result != vk::Result::eTimeout)
                    ALLIGATOR_GPU_THROW("Shader fence wait: " + vk::to_string(result));
                LOG_INFO_STREAM << "Waiting for the GPU dispatch to retire";
            }
        }
    };
    std::vector<std::unique_ptr<Slot>> slots;
    Slice references;
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Prepares one immutable program and all device-queue submission slots.
     */
    VulkanShaderResources(std::vector<uint32_t> code, std::string_view label, Format selected,
        size_t capacity, const Slice* refs = nullptr)
        : format(selected), words(std::move(code)), name(label), limit(capacity) {
        if (refs) references = refs->slice();
        try { prepare(refs); } catch (...) { release(); throw; }
    }
    /** ------------------------------------------------------------------------------------------- Staged Constructor
     * @brief Prepares a reference program's immutable pipeline sequence and slots.
     */
    VulkanShaderResources(std::vector<std::vector<uint32_t>> code, std::span<const KernelGpuStage> sequence,
        std::string_view label, const Slice& refs, uint32_t resource_ref)
        : format(Format::References), words(code.front()), name(label), limit(refs.size<uint32_t>()),
          stage_words(std::move(code)), stages(sequence.begin(), sequence.end()), resources(resource_ref),
          references(refs.slice()) {
        try { prepare(&refs); } catch (...) { release(); throw; }
    }
    ~VulkanShaderResources() { release(); }
    /** ------------------------------------------------------------------------------------------- Release
     * @brief Destroys retired slots before their shared pipelines and layout.
     */
    void release() {
        slots.clear();
        const auto device = VulkanContext::device();
        if (pipeline) device.destroyPipeline(pipeline);
        for (auto entry : stage_pipelines) device.destroyPipeline(entry);
        if (layout) device.destroyPipelineLayout(layout);
        if (module) device.destroyShaderModule(module);
    }
    /** ------------------------------------------------------------------------------------------- Prepare
     * @brief Compiles immutable pipelines and records queue-family-compatible command buffers.
     */
    void prepare(const Slice* refs) {
        const auto device = VulkanContext::device();
        const auto& properties = VulkanContext::device_properties();
        if (properties.max_workgroup_size[0] < 16 || properties.max_workgroup_size[1] < 4
            || properties.max_workgroup_invocations < 64)
            ALLIGATOR_GPU_THROW("Shader: device cannot execute the (16,4,1) local shape");
        limit = std::min(limit, size_t(properties.max_workgroup_count[format == Format::Slices ? 1 : 2]));
        const vk::PushConstantRange range(vk::ShaderStageFlagBits::eCompute, 0, sizeof(KernelPush));
        layout = device.createPipelineLayout(vk::PipelineLayoutCreateInfo({}, 0, nullptr, 1, &range));
        stage_pipelines.reserve(stage_words.empty() ? 0 : stage_words.size() - 1);
        {
            std::lock_guard cache_lock(VulkanContext::instance().pipeline_mutex_);
            module = device.createShaderModule(vk::ShaderModuleCreateInfo({}, words.size() * 4, words.data()));
            const vk::PipelineShaderStageCreateInfo stage({}, vk::ShaderStageFlagBits::eCompute, module, "main");
            const auto built = device.createComputePipeline(VulkanContext::pipeline_cache(),
                vk::ComputePipelineCreateInfo({}, stage, layout));
            if (built.result != vk::Result::eSuccess)
                ALLIGATOR_GPU_THROW("Shader pipeline: " + vk::to_string(built.result));
            pipeline = built.value;
            device.destroyShaderModule(module);
            module = nullptr;
            for (size_t index = 1; index < stage_words.size(); ++index) {
                const auto& code = stage_words[index];
                module = device.createShaderModule(vk::ShaderModuleCreateInfo({}, code.size() * 4, code.data()));
                const vk::PipelineShaderStageCreateInfo pass({}, vk::ShaderStageFlagBits::eCompute, module, "main");
                const auto created = device.createComputePipeline(VulkanContext::pipeline_cache(),
                    vk::ComputePipelineCreateInfo({}, pass, layout));
                if (created.result != vk::Result::eSuccess)
                    ALLIGATOR_GPU_THROW("Shader stage pipeline: " + vk::to_string(created.result));
                stage_pipelines.push_back(created.value);
                device.destroyShaderModule(module);
                module = nullptr;
            }
        }
        slots.reserve(VulkanContext::queue_count());
        for (uint32_t slot_index = 0; slot_index < VulkanContext::queue_count(); ++slot_index) {
            auto slot = std::make_unique<Slot>();
            const size_t bytes = 32 + stages.size() * 16;
            slot->storage = std::make_unique<VulkanBuffer>(bytes);
#ifdef BUFFETALLIGATOR_SHADER_TESTING
            if (shader_test_fail_preparation.exchange(false, std::memory_order_acq_rel))
                ALLIGATOR_GPU_THROW("Injected Shader preparation failure");
#endif
            slot->mapped = static_cast<uint8_t*>(slot->storage->host());
            if (format == Format::References) {
                *reinterpret_cast<uint64_t*>(slot->mapped) = VulkanKernel::device_address(*refs);
                reinterpret_cast<uint32_t*>(slot->mapped)[2] = uint32_t(refs->size<uint32_t>() - 1);
                reinterpret_cast<uint32_t*>(slot->mapped)[7] = resources;
            } else {
                slot->parameters = Slice(limit * (format == Format::Jobs ? 32 : sizeof(Slice)),
                    VulkanContext::buffer_placement());
                *reinterpret_cast<uint32_t*>(slot->mapped) = slot->parameters.id();
            }
            slot->fence = device.createFence({});
            slot->command_pools.reserve(VulkanContext::compute_families().size());
            slot->commands.reserve(VulkanContext::compute_families().size());
            for (const uint32_t family : VulkanContext::compute_families()) {
                const auto pool = device.createCommandPool(vk::CommandPoolCreateInfo({}, family));
                slot->command_pools.push_back(pool);
                const auto command = device.allocateCommandBuffers(vk::CommandBufferAllocateInfo(
                    pool, vk::CommandBufferLevel::ePrimary, 1))[0];
                slot->commands.push_back(command);
                const KernelPush push{slot->storage->address(), VulkanKernel::gpu_pool_address()};
                static_cast<void>(command.begin(vk::CommandBufferBeginInfo{}));
                const vk::MemoryBarrier publication(vk::AccessFlagBits::eHostWrite,
                    vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite
                    | vk::AccessFlagBits::eIndirectCommandRead);
                command.pipelineBarrier(vk::PipelineStageFlagBits::eHost,
                    vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eDrawIndirect,
                    {}, 1, &publication, 0, nullptr, 0, nullptr);
                command.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
                command.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
                if (stages.empty()) {
                    command.dispatchIndirect(slot->storage->buffer_, 16);
                } else {
                    for (size_t index = 0; index < stages.size(); ++index) {
                        command.bindPipeline(vk::PipelineBindPoint::eCompute,
                            index == 0 ? pipeline : stage_pipelines[index - 1]);
                        command.dispatchIndirect(slot->storage->buffer_, 32 + index * 16);
                        const vk::MemoryBarrier dependency(vk::AccessFlagBits::eShaderWrite,
                            vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite
                            | vk::AccessFlagBits::eIndirectCommandRead);
                        command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                            vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eDrawIndirect,
                            {}, 1, &dependency, 0, nullptr, 0, nullptr);
                    }
                }
                const vk::MemoryBarrier readback(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eHostRead);
                command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                    vk::PipelineStageFlagBits::eHost, {}, 1, &readback, 0, nullptr, 0, nullptr);
                command.end();
            }
            slots.push_back(std::move(slot));
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Vulkan Prepared Operations
 * @brief Exposes native submission and retirement through concrete private backend functions.
 */
struct VulkanPreparedOps {
    static void destroy(ShaderProgram& program) noexcept {
        delete static_cast<VulkanShaderResources*>(program.native);
    }
    static void submit(ShaderProgram&, ShaderSlot& slot, uint32_t width, uint32_t height, uint32_t depth) {
        static_cast<VulkanShaderResources::Slot*>(slot.native)->submit(width, height, depth);
    }
    static void wait(ShaderProgram&, ShaderSlot& slot) {
        static_cast<VulkanShaderResources::Slot*>(slot.native)->wait();
    }
};
/** --------------------------------------------------------------------------------------------------------- Vulkan Prepare
 * @brief Builds immutable Vulkan pipelines and queue-compatible native submission slots.
 */
std::unique_ptr<ShaderProgram> vulkan_prepare(const ShaderPrepareInfo& info) {
    if (!VulkanContext::device_present()) ALLIGATOR_GPU_THROW("No compatible Vulkan compute device");
    if (info.capacity == 0) ALLIGATOR_GPU_THROW("Shader preparation requires nonzero parameter capacity");
    if (info.stages.empty() && info.words.empty() && info.source.glsl.empty())
        ALLIGATOR_GPU_THROW("Vulkan Shader preparation requires GLSL or SPIR-V source");
    const auto& properties = VulkanContext::device_properties();
    std::unique_ptr<VulkanShaderResources> native;
    if (!info.stages.empty()) {
        std::vector<std::vector<uint32_t>> code;
        code.reserve(info.stages.size());
        for (const auto& stage : info.stages) {
            if (!stage.workgroups_x || stage.workgroups_x > properties.max_workgroup_count[0]
                || !stage.workgroups_y || stage.workgroups_y > properties.max_workgroup_count[1])
                ALLIGATOR_GPU_THROW("Kernel stage dispatch exceeds the device shape");
            ShaderPrepareInfo pass = info;
            pass.source.glsl = stage.glsl;
            code.push_back(shader_compile_glsl(shader_glsl_source(pass), properties.supports_float16, info.name));
        }
        native = std::make_unique<VulkanShaderResources>(std::move(code), info.stages,
            info.name, *info.references, info.resources);
    } else {
        auto words = info.words.empty()
            ? shader_compile_glsl(shader_glsl_source(info), properties.supports_float16, info.name)
            : std::vector<uint32_t>(info.words.begin(), info.words.end());
        native = std::make_unique<VulkanShaderResources>(std::move(words), info.name,
            info.format, info.capacity, info.references);
    }
    auto program = std::make_unique<ShaderProgram>();
    program->capacity = native->limit;
    program->max_workgroups_x = properties.max_workgroup_count[0];
    program->spirv = std::move(native->words);
    program->slots.reserve(native->slots.size());
    for (const auto& owned : native->slots) {
        auto slot = std::make_unique<ShaderSlot>();
        slot->parameters = std::move(owned->parameters);
        slot->mapped = owned->mapped;
        slot->native = owned.get();
        program->slots.push_back(std::move(slot));
    }
    static const ShaderBackendOps operations{
        &VulkanPreparedOps::destroy, &VulkanPreparedOps::submit, &VulkanPreparedOps::wait};
    program->ops = &operations;
    program->native = native.release();
    return program;
}
} // namespace buffetalligator
