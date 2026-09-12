/** --------------------------------------------------------------------------------------------------------- Vulkan Storage
 * @file ba_vulkan.c
 * @brief Allocates Vulkan buffers with persistent host mappings and explicit device-local staging.
 */
#include "ba_vulkan.h"
#include <uv.h>
#include <stdlib.h>
#include <string.h>

/** --------------------------------------------------------------------------------------------------------- Allocation
 * @brief Stores one Vulkan allocation and its optional host-visible transfer buffer.
 */
typedef struct ba_vk_allocation {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkBuffer staging_buffer;
    VkDeviceMemory staging_memory;
    void* host;
    uint32_t properties;
} ba_vk_allocation;
/** --------------------------------------------------------------------------------------------------------- Context
 * @brief Caches device capabilities and serializes transfers shared by allocation and network workers.
 */
typedef struct ba_vk_context {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkFence fence;
    VkPhysicalDeviceMemoryProperties memory;
    uint32_t types[16];
    uint32_t staging_type;
    uv_mutex_t transfer_mutex;
    int status;
} ba_vk_context;
static ba_vk_context ba_vulkan;
static uv_once_t ba_vulkan_once = UV_ONCE_INIT;
/** --------------------------------------------------------------------------------------------------------- Extension
 * @brief Matches an advertised extension by its complete name.
 */
static int ba_vk_extension(const VkExtensionProperties* properties, uint32_t count,
                           const char* name) {
    for (uint32_t index = 0; index < count; ++index)
        if (!strcmp(properties[index].extensionName, name)) return 1;
    return 0;
}
/** --------------------------------------------------------------------------------------------------------- Memory Type
 * @brief Resolves exact required properties without substituting a different memory class.
 */
static uint32_t ba_vk_type(uint32_t required) {
    uint32_t selected = UINT32_MAX;
    unsigned best = UINT32_MAX;
    for (uint32_t index = 0; index < ba_vulkan.memory.memoryTypeCount; ++index) {
        uint32_t flags = ba_vulkan.memory.memoryTypes[index].propertyFlags;
        if ((flags & required) != required) continue;
        unsigned extra = 0;
        for (uint32_t bits = flags & ~required; bits; bits >>= 1) extra += bits & 1;
        if (extra < best) {
            selected = index;
            best = extra;
        }
    }
    return selected;
}
/** --------------------------------------------------------------------------------------------------------- Initialize Context
 * @brief Creates transfer resources once and caches memory types for the supported property combinations.
 */
static void ba_vk_start(void) {
    ba_vulkan.status = UV_EIO;
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) != VK_SUCCESS) return;
    VkExtensionProperties* extensions = calloc(count, sizeof(*extensions));
    if (!extensions) {
        ba_vulkan.status = UV_ENOMEM;
        return;
    }
    if (vkEnumerateInstanceExtensionProperties(NULL, &count, extensions) != VK_SUCCESS) {
        free(extensions);
        return;
    }
    const char* portability = "VK_KHR_portability_enumeration";
    const int portable = ba_vk_extension(extensions, count, portability);
    free(extensions);
    VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                     .pApplicationName = "BuffetAlligator",
                                     .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo instance = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .flags = portable ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0,
        .pApplicationInfo = &application,
        .enabledExtensionCount = portable ? 1u : 0u,
        .ppEnabledExtensionNames = portable ? &portability : NULL};
    if (vkCreateInstance(&instance, NULL, &ba_vulkan.instance) != VK_SUCCESS) return;
    if (vkEnumeratePhysicalDevices(ba_vulkan.instance, &count, NULL) != VK_SUCCESS || !count)
        goto failed;
    VkPhysicalDevice* devices = calloc(count, sizeof(*devices));
    if (!devices) goto failed;
    VkResult result = vkEnumeratePhysicalDevices(ba_vulkan.instance, &count, devices);
    if (result == VK_SUCCESS) ba_vulkan.physical = devices[0];
    free(devices);
    if (result != VK_SUCCESS) goto failed;
    vkGetPhysicalDeviceMemoryProperties(ba_vulkan.physical, &ba_vulkan.memory);
    for (uint32_t flags = 0; flags < 16; ++flags) ba_vulkan.types[flags] = ba_vk_type(flags);
    ba_vulkan.staging_type =
        ba_vk_type(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ba_vulkan.staging_type == UINT32_MAX) goto failed;
    vkGetPhysicalDeviceQueueFamilyProperties(ba_vulkan.physical, &count, NULL);
    VkQueueFamilyProperties* families = calloc(count, sizeof(*families));
    if (!families) goto failed;
    vkGetPhysicalDeviceQueueFamilyProperties(ba_vulkan.physical, &count, families);
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    free(families);
    if (family == count) goto failed;
    if (vkEnumerateDeviceExtensionProperties(ba_vulkan.physical, NULL, &count, NULL) != VK_SUCCESS)
        goto failed;
    extensions = calloc(count, sizeof(*extensions));
    if (!extensions) goto failed;
    result = vkEnumerateDeviceExtensionProperties(ba_vulkan.physical, NULL, &count, extensions);
    const char* subset = "VK_KHR_portability_subset";
    const int needs_subset = result == VK_SUCCESS && ba_vk_extension(extensions, count, subset);
    free(extensions);
    if (result != VK_SUCCESS) goto failed;
    float priority = 1;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                     .queueFamilyIndex = family,
                                     .queueCount = 1,
                                     .pQueuePriorities = &priority};
    VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                 .queueCreateInfoCount = 1,
                                 .pQueueCreateInfos = &queue,
                                 .enabledExtensionCount = needs_subset ? 1u : 0u,
                                 .ppEnabledExtensionNames = needs_subset ? &subset : NULL};
    if (vkCreateDevice(ba_vulkan.physical, &device, NULL, &ba_vulkan.device) != VK_SUCCESS)
        goto failed;
    vkGetDeviceQueue(ba_vulkan.device, family, 0, &ba_vulkan.queue);
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = family};
    if (vkCreateCommandPool(ba_vulkan.device, &pool, NULL, &ba_vulkan.pool) != VK_SUCCESS)
        goto failed;
    VkCommandBufferAllocateInfo command = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                           .commandPool = ba_vulkan.pool,
                                           .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                           .commandBufferCount = 1};
    if (vkAllocateCommandBuffers(ba_vulkan.device, &command, &ba_vulkan.command) != VK_SUCCESS)
        goto failed;
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(ba_vulkan.device, &fence, NULL, &ba_vulkan.fence) != VK_SUCCESS) goto failed;
    ba_vulkan.status = uv_mutex_init(&ba_vulkan.transfer_mutex);
    if (!ba_vulkan.status) return;
failed:
    if (ba_vulkan.fence) vkDestroyFence(ba_vulkan.device, ba_vulkan.fence, NULL);
    if (ba_vulkan.pool) vkDestroyCommandPool(ba_vulkan.device, ba_vulkan.pool, NULL);
    if (ba_vulkan.device) vkDestroyDevice(ba_vulkan.device, NULL);
    vkDestroyInstance(ba_vulkan.instance, NULL);
    ba_vulkan.device = VK_NULL_HANDLE;
}
/** --------------------------------------------------------------------------------------------------------- Initialize
 * @brief Probes Vulkan once and returns a stable result.
 */
int ba_vk_initialize(void) {
    uv_once(&ba_vulkan_once, ba_vk_start);
    return ba_vulkan.status;
}
/** --------------------------------------------------------------------------------------------------------- Supported
 * @brief Rejects invalid flags and unavailable memory classes explicitly.
 */
int ba_vk_supported(uint32_t properties) {
    if (!properties || properties >= 16 ||
        ((properties &
          (VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) &&
         !(properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)))
        return 0;
    return !ba_vk_initialize() && ba_vulkan.types[properties] != UINT32_MAX;
}
/** --------------------------------------------------------------------------------------------------------- Budget
 * @brief Returns half of the memory heap associated with the requested class.
 */
uint64_t ba_vk_budget(uint32_t properties) {
    uint32_t heap = ba_vulkan.memory.memoryTypes[ba_vulkan.types[properties]].heapIndex;
    return ba_vulkan.memory.memoryHeaps[heap].size / 2;
}
/** --------------------------------------------------------------------------------------------------------- Device
 * @brief Returns the device shared by all Vulkan allocation records.
 */
VkDevice ba_vk_device(void) { return ba_vulkan.device; }
/** --------------------------------------------------------------------------------------------------------- Buffer Allocation
 * @brief Creates transfer-capable storage and binds a compatible cached memory type.
 */
static int ba_vk_storage(size_t bytes, uint32_t type, VkBuffer* buffer, VkDeviceMemory* memory) {
    VkBufferCreateInfo description = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                      .size = bytes,
                                      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                      .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(ba_vulkan.device, &description, NULL, buffer) != VK_SUCCESS) return UV_EIO;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(ba_vulkan.device, *buffer, &requirements);
    if (!(requirements.memoryTypeBits & (1u << type))) return UV_ENOTSUP;
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                       .allocationSize = requirements.size,
                                       .memoryTypeIndex = type};
    if (vkAllocateMemory(ba_vulkan.device, &allocation, NULL, memory) != VK_SUCCESS)
        return UV_ENOMEM;
    return vkBindBufferMemory(ba_vulkan.device, *buffer, *memory, 0) == VK_SUCCESS ? 0 : UV_EIO;
}
/** --------------------------------------------------------------------------------------------------------- Free
 * @brief Releases staging and device allocations through their shared context.
 */
void ba_vk_free(void* allocation) {
    ba_vk_allocation* record = allocation;
    if (!record) return;
    if (record->host)
        vkUnmapMemory(ba_vulkan.device,
                      record->staging_memory ? record->staging_memory : record->memory);
    if (record->staging_buffer) vkDestroyBuffer(ba_vulkan.device, record->staging_buffer, NULL);
    if (record->staging_memory) vkFreeMemory(ba_vulkan.device, record->staging_memory, NULL);
    if (record->buffer) vkDestroyBuffer(ba_vulkan.device, record->buffer, NULL);
    if (record->memory) vkFreeMemory(ba_vulkan.device, record->memory, NULL);
    free(record);
}
/** --------------------------------------------------------------------------------------------------------- Allocate
 * @brief Allocates zeroed storage and stages non-host-visible memory without changing its requested flags.
 */
void* ba_vk_allocate(size_t bytes, uint32_t properties) {
    ba_vk_allocation* record = calloc(1, sizeof(*record));
    if (!record) return NULL;
    uint32_t type = ba_vulkan.types[properties];
    record->properties = ba_vulkan.memory.memoryTypes[type].propertyFlags;
    int status = ba_vk_storage(bytes, type, &record->buffer, &record->memory);
    if (!status && !(record->properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
        status = ba_vk_storage(bytes, ba_vulkan.staging_type, &record->staging_buffer,
                               &record->staging_memory);
    VkDeviceMemory mapping = record->staging_memory ? record->staging_memory : record->memory;
    if (!status &&
        vkMapMemory(ba_vulkan.device, mapping, 0, VK_WHOLE_SIZE, 0, &record->host) != VK_SUCCESS)
        status = UV_EIO;
    if (!status) status = ba_vk_zero(record, 0, bytes);
    if (status) {
        ba_vk_free(record);
        return NULL;
    }
    return record;
}
/** --------------------------------------------------------------------------------------------------------- Host Address
 * @brief Returns the permanently mapped host or staging address.
 */
void* ba_vk_host(void* allocation) { return ((ba_vk_allocation*)allocation)->host; }
/** --------------------------------------------------------------------------------------------------------- Buffer
 * @brief Returns the device-side Vulkan buffer.
 */
VkBuffer ba_vk_buffer(void* allocation) { return ((ba_vk_allocation*)allocation)->buffer; }
/** --------------------------------------------------------------------------------------------------------- Transfer
 * @brief Orders host staging writes or device buffer writes before publishing their destination view.
 */
int ba_vk_transfer(void* allocation, size_t offset, size_t bytes, int to_device) {
    ba_vk_allocation* record = allocation;
    if (!bytes) return 0;
    int status = 0;
    uv_mutex_lock(&ba_vulkan.transfer_mutex);
    if (!record->staging_buffer) {
        if (!(record->properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                         .memory = record->memory,
                                         .offset = 0,
                                         .size = VK_WHOLE_SIZE};
            VkResult result = to_device
                                  ? vkFlushMappedMemoryRanges(ba_vulkan.device, 1, &range)
                                  : vkInvalidateMappedMemoryRanges(ba_vulkan.device, 1, &range);
            if (result != VK_SUCCESS) status = UV_EIO;
        }
    } else {
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                          .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        if (vkResetCommandBuffer(ba_vulkan.command, 0) != VK_SUCCESS ||
            vkBeginCommandBuffer(ba_vulkan.command, &begin) != VK_SUCCESS)
            status = UV_EIO;
        if (!status) {
            if (!to_device) {
                VkBufferMemoryBarrier readable = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                                  .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT |
                                                                   VK_ACCESS_TRANSFER_WRITE_BIT,
                                                  .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                                  .buffer = record->buffer,
                                                  .offset = offset,
                                                  .size = bytes};
                vkCmdPipelineBarrier(
                    ba_vulkan.command,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &readable, 0, NULL);
            }
            VkBufferCopy copy = {.srcOffset = offset, .dstOffset = offset, .size = bytes};
            vkCmdCopyBuffer(ba_vulkan.command, to_device ? record->staging_buffer : record->buffer,
                            to_device ? record->buffer : record->staging_buffer, 1, &copy);
            VkBufferMemoryBarrier barrier = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = to_device ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
                                           : VK_ACCESS_HOST_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = to_device ? record->buffer : record->staging_buffer,
                .offset = offset,
                .size = bytes};
            vkCmdPipelineBarrier(ba_vulkan.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 to_device ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                           : VK_PIPELINE_STAGE_HOST_BIT,
                                 0, 0, NULL, 1, &barrier, 0, NULL);
            if (vkEndCommandBuffer(ba_vulkan.command) != VK_SUCCESS ||
                vkResetFences(ba_vulkan.device, 1, &ba_vulkan.fence) != VK_SUCCESS)
                status = UV_EIO;
        }
        if (!status) {
            VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                   .commandBufferCount = 1,
                                   .pCommandBuffers = &ba_vulkan.command};
            if (vkQueueSubmit(ba_vulkan.queue, 1, &submit, ba_vulkan.fence) != VK_SUCCESS ||
                vkWaitForFences(ba_vulkan.device, 1, &ba_vulkan.fence, VK_TRUE, UINT64_MAX) !=
                    VK_SUCCESS)
                status = UV_EIO;
        }
    }
    uv_mutex_unlock(&ba_vulkan.transfer_mutex);
    return status;
}
/** --------------------------------------------------------------------------------------------------------- Zero
 * @brief Clears recycled bytes in both the host view and the device allocation.
 */
int ba_vk_zero(void* allocation, size_t offset, size_t bytes) {
    memset((unsigned char*)ba_vk_host(allocation) + offset, 0, bytes);
    return ba_vk_transfer(allocation, offset, bytes, 1);
}
