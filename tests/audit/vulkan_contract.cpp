/** --------------------------------------------------------------------------------------------------------- Device Write Contract
 * @file vulkan_contract.cpp
 * @brief Verifies GPU-written Slice bytes through the library's actual readback implementation.
 */
#include "../test_support.hpp"
#include <alligator.hpp>
extern "C" {
#include "memory/ba_vulkan.h"
}
#include <array>
#include <cstdio>

namespace {
uint32_t queue_family;
}
/** --------------------------------------------------------------------------------------------------------- Capture Device Creation
 * @brief Observes the real queue-family selection while forwarding unchanged device creation.
 */
extern "C" VKAPI_ATTR VkResult VKAPI_CALL ba_audit_create_device(
    VkPhysicalDevice physical, const VkDeviceCreateInfo* description,
    const VkAllocationCallbacks* allocator, VkDevice* device
) {
    queue_family = description->pQueueCreateInfos[0].queueFamilyIndex;
    return vkCreateDevice(physical, description, allocator, device);
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Writes device buffers directly before asking Slice synchronization to expose their contents.
 */
int main() {
    test_support::start(__FILE__);
    using namespace buffetalligator;
    try {
        if (ba_vk_initialize()) return 77;
        std::array<Slice, 4> slices;
        const uint32_t flags[] = {1, 2, 6, 14};
        for (size_t index = 0; index < slices.size(); ++index) {
            if (!ba_vk_supported(flags[index])) continue;
            slices[index] = Slice(4096, BuffetMenu::vulkan(flags[index]));
        }
        const VkDevice device = BuffetMenu::vulkan_device();
        VkQueue queue;
        vkGetDeviceQueue(device, queue_family, 0, &queue);
        VkCommandPoolCreateInfo pool_description{};
        pool_description.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_description.queueFamilyIndex = queue_family;
        VkCommandPool pool;
        TEST_EQUAL(vkCreateCommandPool(device, &pool_description, nullptr, &pool), VK_SUCCESS, "command pool failed");
        VkCommandBufferAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocation.commandPool = pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        VkCommandBuffer command;
        TEST_EQUAL(vkAllocateCommandBuffers(device, &allocation, &command), VK_SUCCESS, "command allocation failed");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        TEST_EQUAL(vkBeginCommandBuffer(command, &begin), VK_SUCCESS, "command begin failed");
        for (size_t index = 0; index < slices.size(); ++index) {
            if (!slices[index]) continue;
            const auto buffer = slices[index].vulkan_buffer();
            vkCmdFillBuffer(command, buffer.buffer, buffer.offset, buffer.range, 0xa1937000u + flags[index]);
            VkBufferMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = buffer.buffer;
            barrier.offset = buffer.offset;
            barrier.size = buffer.range;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                0, 0, nullptr, 1, &barrier, 0, nullptr);
        }
        TEST_EQUAL(vkEndCommandBuffer(command), VK_SUCCESS, "command end failed");
        VkSubmitInfo submission{};
        submission.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submission.commandBufferCount = 1;
        submission.pCommandBuffers = &command;
        TEST_EQUAL(vkQueueSubmit(queue, 1, &submission, VK_NULL_HANDLE), VK_SUCCESS, "device write failed");
        TEST_EQUAL(vkQueueWaitIdle(queue), VK_SUCCESS, "device write completion failed");
        for (size_t index = 0; index < slices.size(); ++index) {
            if (!slices[index]) continue;
            slices[index].vulkan_sync(false);
            for (size_t word = 0; word < slices[index].size<uint32_t>(); ++word)
                TEST_EQUAL(slices[index].data<uint32_t>()[word], 0xa1937000u + flags[index],
                    "GPU-written bytes did not reach the Slice host view");
            std::printf("GPU-written readback passed: flags=%u words=%zu\n", flags[index], slices[index].size<uint32_t>());
        }
        vkDestroyCommandPool(device, pool, nullptr);
        for (auto& slice : slices) slice.free();
        BuffetMenu::shutdown();
        return 0;
    } catch (const std::exception& error) {
        test_support::fail("Unexpected exception; source is the last test checkpoint",
            test_support::last_operation, "successful completion", error.what(), test_support::last_location);
    }
}
