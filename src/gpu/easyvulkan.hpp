#pragma once
/** --------------------------------------------------------------------------------------------------------- EasyVulkan
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
class VulkanBuffer : public IBuffet {
public:
    struct VulkanStatics {
        vk::Device device;
        uint32_t memory_type_index;
        vk::BufferCreateInfo create_info;
        vk::MemoryRequirements memoryRequirements;
        vk::MemoryAllocateInfo alloc_info64mb;
        vk::MemoryAllocateInfo alloc_info_custom(size_t size) {
            return vk::MemoryAllocateInfo(size, memory_type_index);
        }
        vk::BufferUsageFlags usage;
        IBuffet* buf = nullptr;
        vk::DeviceAddress* addresses = nullptr;
        vk::DeviceAddress address_table;
        bool stopped = false;  ///< Set once the device is gone, so a late destructor frees nothing.
        VulkanStatics() {
            create_info = vk::BufferCreateInfo({}, 0, vk::BufferUsageFlags(), vk::SharingMode::eExclusive);
            alloc_info64mb = vk::MemoryAllocateInfo(64 * 1024 * 1024, memory_type_index);
        }
    };
    static VulkanStatics& statics() {
        static VulkanStatics s;
        return s;
    }
    friend class VulkanContext;
    /** ------------------------------------------------------------------------------------------- Set Up Once
     * @brief Caches the device, memory type, usage and the pre-built 64 MiB infos every buffer is made from.
     */
    static void set_up_once(vk::Device device, uint32_t memory_type_index, vk::BufferUsageFlags usage) {
        auto& allocator = statics(); allocator.device = device;
        allocator.memory_type_index = memory_type_index; allocator.usage = usage;
        allocator.create_info = vk::BufferCreateInfo({}, 64u * 1024u * 1024u, usage, vk::SharingMode::eExclusive);
        allocator.alloc_info64mb = vk::MemoryAllocateInfo(64u * 1024u * 1024u, memory_type_index);
        static_cast<void>(IBuffet::fast_allocator());
    }
    /// @brief Marks the device gone, so buffers destroyed after it free nothing.
    static void stop() { statics().stopped = true; }
    VulkanBuffer(size_t size_bytes, bool novel = false) : IBuffet(size_bytes, novel) {}
    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;
    VulkanBuffer(VulkanBuffer&&) = delete;
    VulkanBuffer& operator=(VulkanBuffer&&) = delete;
    /** ------------------------------------------------------------------------------------------- buffer
     * @brief The Vulkan buffer handle, for indirect dispatch commands recorded against it.
     * @return The buffer handle.
     */
    vk::Buffer buffer() const { return vk::Buffer(static_cast<VkBuffer>(buffer_)); }
};
/** --------------------------------------------------------------------------------------------------------- VulkanContext
 * @class VulkanContext
 * @brief Encapsulates the Vulkan context, including instance, device, queues, and memory properties.
 */
class VulkanContext {
private:
    vk::Instance instance_{};                    ///< Vulkan instance.
    vk::PhysicalDevice physical_device_{};       ///< Selected physical device.
    vk::Device device_{};                        ///< Logical device.
    std::vector<vk::Queue> compute_queues_;      ///< Queues flattened in compute-family order.
    std::vector<uint32_t> compute_families_;     ///< Unique compute-capable family indices.
    std::mutex pipeline_mutex_;
    std::vector<uint32_t> queue_family_slots_;   ///< Compute-family slot for each queue.
    inline static std::atomic<uint32_t> next_queue_slot_{0};  ///< Hands each producer thread a sticky queue slot.
    vk::PhysicalDeviceMemoryProperties memory_properties_{};  ///< Queried once at init.
    vk::PipelineCache pipeline_cache_{};         ///< Driver pipeline cache (in-process).
    DeviceProperties device_props_{};            ///< Queried device limits and features.
    vk::DeviceSize max_allocation_size_ = 0;     ///< Probed maximum single device allocation.
    vk::BufferUsageFlags buffer_usage_{};
    vk::MemoryAllocateFlagsInfo allocate_flags_{};
    uint32_t buffer_memory_type_index_ = UINT32_MAX;
    bool device_present_ = false;
    bool unified_memory_ = false;
    inline static std::atomic<bool> stop_{false};
    vk::BufferCreateInfo shared_buffer_info(vk::DeviceSize bytes, vk::BufferUsageFlags usage) const;
    uint32_t try_find_memory_type(uint32_t type_filter, vk::MemoryPropertyFlags wanted, vk::MemoryPropertyFlags excluded = {}) const{
        for (uint32_t i = 0; i < memory_properties_.memoryTypeCount; ++i) {
            const bool allowed = (type_filter & (1u << i)) != 0;
            const vk::MemoryPropertyFlags flags = memory_properties_.memoryTypes[i].propertyFlags;
            if (allowed && (flags & wanted) == wanted && !(flags & excluded)) return i;
        }
        return UINT32_MAX;
    }   
    VulkanContext() {
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
            std::vector<uint32_t> compute_families;
            instance_info.pApplicationInfo = &app_info;
            unified_memory_ = false;
            for (const vk::ExtensionProperties& ext : vk::enumerateInstanceExtensionProperties()) {
                if (std::strcmp(ext.extensionName.data(),
                        VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
                    unified_memory_ = true;
                }
            }
        #ifdef __APPLE__
            if (unified_memory_) {
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
                device_present_ = false;
                return;
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
                device_present_ = false;
                return;
            }
            if (!supported12.timelineSemaphore) {
                device_present_ = false;
                return;
            }
            bool internally_synchronized_queues_ = false;
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
            device_props_.max_allocation_count = limits.maxMemoryAllocationCount;
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
            bool unified_from_properties = false;
            constexpr vk::MemoryPropertyFlags unified_flags =
                vk::MemoryPropertyFlagBits::eDeviceLocal | vk::MemoryPropertyFlagBits::eHostVisible;
            for (uint32_t i = 0; i < memory_properties_.memoryTypeCount; ++i) {
                const bool both =
                    (memory_properties_.memoryTypes[i].propertyFlags & unified_flags) == unified_flags;
                const bool heap_local =
                    (memory_properties_.memoryHeaps[memory_properties_.memoryTypes[i].heapIndex].flags
                    & vk::MemoryHeapFlagBits::eDeviceLocal) == vk::MemoryHeapFlagBits::eDeviceLocal;
                if (both && heap_local) {
                    unified_from_properties = true;
                    break;
                }
            }
            device_props_.host_visible_memory_bytes = host_visible_bytes;
            device_props_.unified_memory = props2.properties.deviceType == vk::PhysicalDeviceType::eIntegratedGpu && unified_from_properties;
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
                device_present_ = false;
                return;
            }
            pipeline_cache_ = device_.createPipelineCache({});
            device_present_ = true;
            VulkanContext& ctx = *this;
            device_props_.gpu_free_bytes = device_free_bytes(ctx);
            VulkanBuffer::set_up_once(device_, buffer_memory_type_index_, buffer_usage_);
        } catch (...) {
            if (device_) device_.destroy();
            if (instance_) instance_.destroy();
            throw;
        }

    }
public:
    static VulkanContext& instance() { static VulkanContext inst; return inst; }
    ~VulkanContext(){
        if (!device_) {
            stop_.store(true, std::memory_order_release);
            return;
        }
        device_.waitIdle();
        VulkanBuffer::shutdown();
        VulkanBuffer::stop();
        stop_.store(true, std::memory_order_release);
        device_.destroyPipelineCache(pipeline_cache_);
        device_.destroy();
        instance_.destroy();
    }
    static const DeviceProperties& device_properties() { return instance().device_props_; }
    static uint64_t buffer_heap_headroom(){
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
    static vk::Device device() { return instance().device_; }
    static vk::PipelineCache pipeline_cache() { return instance().pipeline_cache_; }
    static std::mutex& pipeline_mutex() { return instance().pipeline_mutex_; }
    static uint32_t assign_queue() {
        VulkanContext& context = instance();
        const uint32_t count = uint32_t(context.compute_queues_.size());
        return context.next_queue_slot_.fetch_add(1, std::memory_order_relaxed) % count;
    }
    static void submit(uint32_t queue, const vk::SubmitInfo& info);
    static uint32_t queue_family(uint32_t queue) {
        const VulkanContext& context = instance();
        return context.compute_families_[context.queue_family_slots_[queue]];
    }
    static uint32_t buffer_memory_type_index() { return instance().buffer_memory_type_index_; }
    static vk::BufferUsageFlags buffer_usage() { return instance().buffer_usage_; }
    static const vk::MemoryAllocateFlagsInfo& allocate_flags() { return instance().allocate_flags_; }
    static bool device_present() {
        return instance().device_present_;
    }
    static bool stopped() { return stop_.load(std::memory_order_acquire); }
    static uint32_t queue_count() { return static_cast<uint32_t>(instance().compute_queues_.size()); }
    static uint64_t device_free_bytes(VulkanContext& ctx) {
        if (!ctx.device_props_.supports_memory_budget) return 0;
        vk::PhysicalDeviceMemoryBudgetPropertiesEXT budget{};
        vk::PhysicalDeviceMemoryProperties2 memory2{};
        memory2.pNext = &budget;
        ctx.physical_device_.getMemoryProperties2(&memory2);
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
    static bool device_unified() {
        return instance().device_properties().unified_memory;
    }
};
inline vk::BufferCreateInfo VulkanContext::shared_buffer_info(vk::DeviceSize bytes, vk::BufferUsageFlags usage) const {
    vk::BufferCreateInfo info({}, bytes, usage);
    if (compute_families_.size() > 1) {
        info.sharingMode = vk::SharingMode::eConcurrent;
        info.queueFamilyIndexCount = static_cast<uint32_t>(compute_families_.size());
        info.pQueueFamilyIndices = compute_families_.data();
    }
    return info;
}
inline void VulkanContext::submit(uint32_t queue, const vk::SubmitInfo& info) {
    static std::unique_ptr<std::mutex[]> locks(new std::mutex[queue_count()]);
    std::lock_guard<std::mutex> lock(locks[queue]);
    instance().compute_queues_[queue].submit(info);
}
/** --------------------------------------------------------------------------------------------------------- EZ GLSL Prelude
 * @brief Heads every shader body: the pushed device address and the array views a body reads through it.
 */
inline constexpr std::string_view EZ_GLSL_PRELUDE = R"glsl(#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int32 : require
#ifdef VULKAN_FLOAT16
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#endif
layout(push_constant) uniform EZPush { uint64_t address; } push;
layout(buffer_reference, std430, buffer_reference_align = 4) buffer U32Array { uint v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer I32Array { int v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer F32Array { float v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) buffer U64Array { uint64_t v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) buffer I64Array { int64_t v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer F32x4Array { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer U32x4Array { uvec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer I32x4Array { ivec4 v[]; };
)glsl";
} // namespace buffetalligator
