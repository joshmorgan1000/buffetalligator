/** --------------------------------------------------------------------------------------------------------- VulkanKernel
 * @file vulkankernel.cpp
 * @brief Shared Vulkan device utilities for prepared shaders and Slice addressing.
 */
#include <logging.hpp>
#include <alligator.hpp>
#include <alligator/easyvulkan.hpp>
#include <memory/tracker.hpp>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- VulkanKernel::available
 * @brief Checks if a Vulkan device is available.
 * @return True if a Vulkan device is present, otherwise false.
 */
bool VulkanKernel::available() {
    return VulkanContext::device_present();
}
/** --------------------------------------------------------------------------------------------------------- VulkanKernel::device_name
 * @brief Retrieves the name of the Vulkan device if it is present.
 * @return The name of the Vulkan device if present, otherwise an empty string.
 */
std::string VulkanKernel::device_name() {
    return VulkanContext::device_present()
        ? VulkanContext::device_properties().device_name
        : std::string();
}
/** --------------------------------------------------------------------------------------------------------- VulkanKernel::device_address
 * @brief Retrieves the device address of a Slice's first byte if it is GPU-visible.
 * @param slice The Slice for which to retrieve the device address.
 * @return The device address of the Slice's first byte if GPU-visible, otherwise 0.
 */
uint64_t VulkanKernel::device_address(const Slice& slice) {
    return Alligator::device_address(slice);
}
/** --------------------------------------------------------------------------------------------------------- VulkanKernel::gpu_pool_address
 * @brief The device address of the arena directory, 0 without a compute device.
 * @return The directory's device address.
 */
uint64_t VulkanKernel::gpu_pool_address() {
    return Alligator::gpu_directory_address();
}
/** --------------------------------------------------------------------------------------------------------- GPU Usage
 * @brief Reports bytes retained in Vulkan descriptor allocations.
 */
uint64_t VulkanKernel::gpu_usage() {
    return Memory::placement_usage(*VulkanContext::buffer_placement());
}
} // namespace buffetalligator
