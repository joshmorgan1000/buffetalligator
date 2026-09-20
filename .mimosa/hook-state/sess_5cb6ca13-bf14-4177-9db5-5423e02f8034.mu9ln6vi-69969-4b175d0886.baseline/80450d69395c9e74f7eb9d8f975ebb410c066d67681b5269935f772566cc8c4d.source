/** --------------------------------------------------------------------------------------------------------- Vulkan Tests
 * @file vulkan_test.cpp
 * @brief Verifies GPU writes through mapped Slice storage on a process-lifetime Vulkan device.
 */
#include <alligator_vulkan.hpp>
#include "../functional_support.hpp"
#include <cstring>
#include <atomic>
#include <source_location>
#include <vector>

using namespace buffetalligator;
using functional::require;
std::atomic<unsigned> validation_errors{0};
/** --------------------------------------------------------------------------------------------------------- Validation Callback
 * @brief Counts validation errors so a diagnostic fails the functional test.
 */
VKAPI_ATTR VkBool32 VKAPI_CALL validation_message(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* message, void*
) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        validation_errors.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR_STREAM << message->pMessage;
    }
    return VK_FALSE;
}
/** --------------------------------------------------------------------------------------------------------- Check
 * @brief Stops the functional test when a Vulkan operation fails.
 */
void check(VkResult result, std::source_location location = std::source_location::current()) {
    if (result != VK_SUCCESS) {
        LOG_ERROR_STREAM << "Vulkan result " << result << " at line " << location.line();
        require(false, "Vulkan API operation failed");
    }
}
int main() {
    uint32_t count = 0;
    check(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr));
    std::vector<VkExtensionProperties> extensions(count);
    check(vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()));
    std::vector<const char*> enabled;
    bool debug_utils = false;
    VkInstanceCreateInfo instance_information{};
    instance_information.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    for (const auto& extension : extensions) {
        if (std::strcmp(extension.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
            debug_utils = true;
        }
        if (std::strcmp(extension.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
            enabled.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            instance_information.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
    }
    VkDebugUtilsMessengerCreateInfoEXT messenger_information{};
    messenger_information.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    messenger_information.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger_information.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messenger_information.pfnUserCallback = &validation_message;
    check(vkEnumerateInstanceLayerProperties(&count, nullptr));
    std::vector<VkLayerProperties> layers(count);
    check(vkEnumerateInstanceLayerProperties(&count, layers.data()));
    const char* validation = "VK_LAYER_KHRONOS_validation";
    for (const auto& layer : layers) {
        if (std::strcmp(layer.layerName, validation) == 0) {
            instance_information.enabledLayerCount = 1;
            instance_information.ppEnabledLayerNames = &validation;
        }
    }
    if (debug_utils && instance_information.enabledLayerCount) {
        enabled.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        instance_information.pNext = &messenger_information;
    }
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.apiVersion = VK_API_VERSION_1_1;
    instance_information.pApplicationInfo = &application;
    instance_information.enabledExtensionCount = enabled.size();
    instance_information.ppEnabledExtensionNames = enabled.data();
    VkInstance instance;
    const VkResult created = vkCreateInstance(&instance_information, nullptr, &instance);
    if (created == VK_ERROR_INCOMPATIBLE_DRIVER) {
        LOG_INFO_STREAM << "Vulkan allocator test skipped: no compatible Vulkan driver";
        return 77;
    }
    check(created);
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (debug_utils && instance_information.enabledLayerCount) {
        auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        check(create_messenger(instance, &messenger_information, nullptr, &messenger));
    }
    check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    if (!count) {
        LOG_INFO_STREAM << "Vulkan allocator test skipped: no Vulkan device";
        vkDestroyInstance(instance, nullptr);
        return 77;
    }
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    VkPhysicalDevice physical = devices.front();
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t index = 0; index < count; ++index) {
        if (families[index].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = index; break; }
    }
    require(family != UINT32_MAX, "Vulkan device has no compute queue");
    check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr));
    extensions.resize(count);
    check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data()));
    enabled.clear();
    for (const auto& extension : extensions) {
        if (std::strcmp(extension.extensionName, "VK_KHR_portability_subset") == 0 ||
            std::strcmp(extension.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0) {
            enabled.push_back(extension.extensionName);
        }
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_information{};
    queue_information.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_information.queueFamilyIndex = family;
    queue_information.queueCount = 1;
    queue_information.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_information{};
    device_information.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_information.queueCreateInfoCount = 1;
    device_information.pQueueCreateInfos = &queue_information;
    device_information.enabledExtensionCount = enabled.size();
    device_information.ppEnabledExtensionNames = enabled.data();
    VkDevice device;
    check(vkCreateDevice(physical, &device_information, nullptr, &device));
    const Placemat* placement = VulkanAllocator::register_type(physical, device);
    Slice parent(8192, true, placement);
    Slice view = parent.slice(256, 4096);
    require(view.data<uint32_t>()[0] == 0, "Vulkan slab is not zero initialized");
    require(VulkanAllocator::buffer_offset(view) == 256, "Vulkan subslice offset is wrong");
    VkCommandPoolCreateInfo pool_information{};
    pool_information.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_information.queueFamilyIndex = family;
    VkCommandPool pool;
    check(vkCreateCommandPool(device, &pool_information, nullptr, &pool));
    VkCommandBufferAllocateInfo command_information{};
    command_information.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_information.commandPool = pool;
    command_information.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_information.commandBufferCount = 1;
    VkCommandBuffer command;
    check(vkAllocateCommandBuffers(device, &command_information, &command));
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    check(vkBeginCommandBuffer(command, &begin));
    vkCmdFillBuffer(command, VulkanAllocator::buffer(view), 256, 4096, 0x6d6d6d6d);
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = VulkanAllocator::buffer(view);
    barrier.offset = 256;
    barrier.size = 4096;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
    check(vkEndCommandBuffer(command));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkFenceCreateInfo fence_information{};
    fence_information.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence;
    check(vkCreateFence(device, &fence_information, nullptr, &fence));
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    check(vkQueueSubmit(queue, 1, &submit, fence));
    check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    require(view.data<uint32_t>()[0] == 0x6d6d6d6d &&
            view.data<uint32_t>()[1023] == 0x6d6d6d6d, "GPU writes missed the Slice mapping");
    require(parent.data<uint32_t>()[0] == 0, "GPU fill escaped its range");
    parent = Slice();
    require(view.data<uint32_t>()[1] == 0x6d6d6d6d, "parent release invalidated a Vulkan view");
    const auto usage = VulkanAllocator::memory_usage();
    require(usage.capacity_bytes.value() > 0, "Vulkan heap capacity is missing");
    require(usage.process_bytes.has_value() == usage.budget_bytes.has_value(), "partial Vulkan budget");
    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
    require(validation_errors.load(std::memory_order_relaxed) == 0, "Vulkan validation reported errors");
    LOG_INFO_STREAM << "Vulkan allocator passed; validation layer "
                    << (instance_information.enabledLayerCount ? "enabled" : "unavailable");
}
