/** --------------------------------------------------------------------------------------------------------- Vulkan Allocator
 * @file vulkan_allocator.cpp
 * @brief Supplies persistently mapped coherent buffers through the arena placement callbacks.
 */
#include <alligator_vulkan.hpp>
#include <cstring>
#include <memory>
#include <vector>

namespace buffetalligator {
namespace {
/** --------------------------------------------------------------------------------------------------------- Vulkan Context
 * @brief Holds the single caller-owned device and its probed allocation properties.
 */
struct VulkanContext {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory{};
    uint32_t memory_type = UINT32_MAX;
    bool memory_budget = false;
    const Placemat* placement = nullptr;
};
VulkanContext context;
/** --------------------------------------------------------------------------------------------------------- Check Vulkan
 * @brief Reports Vulkan failures at allocation and query boundaries.
 */
void check_vulkan(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        ALLIGATOR_THROW(std::string(operation) + ": VkResult " + std::to_string(result));
    }
}
/** --------------------------------------------------------------------------------------------------------- Vulkan Allocation
 * @brief Owns only the native slab resources and their host mapping.
 */
struct VulkanAllocation {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapping = nullptr;
    VulkanAllocation() = default;
    VulkanAllocation(const VulkanAllocation&) = delete;
    VulkanAllocation& operator=(const VulkanAllocation&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Releases a slab or a partially constructed allocation through its owning context.
     */
    ~VulkanAllocation() {
        if (mapping) vkUnmapMemory(context.device, memory);
        if (buffer) vkDestroyBuffer(context.device, buffer, nullptr);
        if (memory) vkFreeMemory(context.device, memory, nullptr);
    }
};
/** --------------------------------------------------------------------------------------------------------- Create Buffer
 * @brief Creates one buffer with the fixed usage shared by all slabs in this placement.
 */
VkBuffer create_buffer(size_t size) {
    VkBufferCreateInfo information{};
    information.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    information.size = size;
    information.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    information.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    check_vulkan(vkCreateBuffer(context.device, &information, nullptr, &buffer),
                 "Creating Vulkan slab buffer");
    return buffer;
}
/** --------------------------------------------------------------------------------------------------------- Allocate
 * @brief Allocates, binds, maps, and zeroes a slab before it enters the arena chain.
 */
Placemat::Handle* vulkan_allocate(size_t size, void*) {
    auto handle = std::make_unique<Placemat::Handle>();
    auto allocation = std::make_unique<VulkanAllocation>();
    allocation->buffer = create_buffer(size);
    VkMemoryDedicatedRequirements dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
    VkMemoryRequirements2 requirements{};
    requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    requirements.pNext = &dedicated;
    VkBufferMemoryRequirementsInfo2 request{};
    request.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2;
    request.buffer = allocation->buffer;
    vkGetBufferMemoryRequirements2(context.device, &request, &requirements);
    VkMemoryDedicatedAllocateInfo binding{};
    binding.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    binding.buffer = allocation->buffer;
    VkMemoryAllocateInfo information{};
    information.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    information.pNext = dedicated.requiresDedicatedAllocation ? &binding : nullptr;
    information.allocationSize = requirements.memoryRequirements.size;
    information.memoryTypeIndex = context.memory_type;
    check_vulkan(vkAllocateMemory(context.device, &information, nullptr, &allocation->memory),
                 "Allocating Vulkan slab memory");
    check_vulkan(vkBindBufferMemory(context.device, allocation->buffer, allocation->memory, 0),
                 "Binding Vulkan slab memory");
    check_vulkan(vkMapMemory(context.device, allocation->memory, 0, VK_WHOLE_SIZE, 0,
                            &allocation->mapping), "Mapping Vulkan slab memory");
    std::memset(allocation->mapping, 0, size);
    handle->substrate_handle = allocation.release();
    return handle.release();
}
/** --------------------------------------------------------------------------------------------------------- Deallocate
 * @brief Releases a completed Vulkan slab during arena reclamation.
 */
void vulkan_deallocate(Placemat::Handle* handle, void*) {
    delete static_cast<VulkanAllocation*>(handle->substrate_handle);
}
/** --------------------------------------------------------------------------------------------------------- Host Pointer
 * @brief Returns the persistent mapping established during slab creation.
 */
void* vulkan_host_pointer(Placemat::Handle* handle) {
    return static_cast<VulkanAllocation*>(handle->substrate_handle)->mapping;
}
/** --------------------------------------------------------------------------------------------------------- Context
 * @brief Returns the process-lifetime Vulkan placement context.
 */
void* vulkan_context() { return &context; }
/** --------------------------------------------------------------------------------------------------------- Allocation
 * @brief Resolves the native allocation backing a Slice from this placement.
 */
VulkanAllocation& allocation_for(const Slice& slice) {
    return *static_cast<VulkanAllocation*>(Placemat::get_for(&slice)->substrate_handle);
}
}
/** --------------------------------------------------------------------------------------------------------- Register Type
 * @brief Probes one device and installs its allocation callbacks before arena startup.
 */
const Placemat* VulkanAllocator::register_type(VkPhysicalDevice physical_device, VkDevice device) {
    if (context.placement) ALLIGATOR_THROW("The Vulkan placement is already registered");
    if (!physical_device || !device) ALLIGATOR_THROW("Vulkan registration requires both device handles");
    context.physical = physical_device;
    context.device = device;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_1) ALLIGATOR_THROW("Vulkan 1.1 or newer is required");
    vkGetPhysicalDeviceMemoryProperties(physical_device, &context.memory);
    VulkanAllocation probe;
    probe.buffer = create_buffer(64ull * 1024 * 1024);
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, probe.buffer, &requirements);
    int best_score = -1;
    for (uint32_t index = 0; index < context.memory.memoryTypeCount; ++index) {
        const VkMemoryPropertyFlags flags = context.memory.memoryTypes[index].propertyFlags;
        constexpr VkMemoryPropertyFlags required =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (!(requirements.memoryTypeBits & (1u << index)) || (flags & required) != required) continue;
        const int score = ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? 2 : 0) +
            ((flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? 1 : 0);
        if (score > best_score) {
            best_score = score;
            context.memory_type = index;
        }
    }
    if (best_score < 0) ALLIGATOR_THROW("The Vulkan device has no coherent host-visible buffer memory");
    uint32_t extension_count = 0;
    check_vulkan(vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count,
                                                     nullptr), "Counting Vulkan extensions");
    std::vector<VkExtensionProperties> extensions(extension_count);
    check_vulkan(vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count,
                                                     extensions.data()), "Reading Vulkan extensions");
    for (const auto& extension : extensions) {
        if (std::strcmp(extension.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0) {
            context.memory_budget = true;
        }
    }
    context.placement = BuffetMenu::get(BuffetMenu::register_type(
        "vulkan", 64ull * 1024 * 1024, 64, &vulkan_allocate, &vulkan_deallocate,
        &vulkan_host_pointer, &vulkan_context
    ));
    return context.placement;
}
/** --------------------------------------------------------------------------------------------------------- Buffer
 * @brief Returns the native buffer for a Slice from this placement.
 */
VkBuffer VulkanAllocator::buffer(const Slice& slice) { return allocation_for(slice).buffer; }
/** --------------------------------------------------------------------------------------------------------- Buffer Offset
 * @brief Resolves the Slice's byte offset inside its slab buffer.
 */
VkDeviceSize VulkanAllocator::buffer_offset(const Slice& slice) {
    return static_cast<const char*>(slice.raw()) -
        static_cast<const char*>(allocation_for(slice).mapping);
}
/** --------------------------------------------------------------------------------------------------------- Memory Usage
 * @brief Queries the selected heap's dynamic budget when advertised by the physical device.
 */
DeviceMemoryUsage VulkanAllocator::memory_usage() {
    if (!context.placement) ALLIGATOR_THROW("Register the Vulkan placement before querying its memory");
    const uint32_t heap = context.memory.memoryTypes[context.memory_type].heapIndex;
    DeviceMemoryUsage usage{context.memory.memoryHeaps[heap].size,
                           std::nullopt, std::nullopt, std::nullopt};
    if (context.memory_budget) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
        budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
        VkPhysicalDeviceMemoryProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        properties.pNext = &budget;
        vkGetPhysicalDeviceMemoryProperties2(context.physical, &properties);
        usage.process_bytes = budget.heapUsage[heap];
        usage.budget_bytes = budget.heapBudget[heap];
    }
    return usage;
}
} // namespace buffetalligator
