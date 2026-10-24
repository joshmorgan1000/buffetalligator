/** --------------------------------------------------------------------------------------------------------- VulkanKernel
 * @file vulkankernel.cpp
 * @brief Shared Vulkan device utilities for prepared shaders and Chomp addressing.
 */
#include <logging.hpp>
#include <alligator.hpp>
#include <gpu/easyvulkan.hpp>
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
/** --------------------------------------------------------------------------------------------------------- VulkanKernel::gpu_pool_address
 * @brief The device address of the arena directory, 0 without a compute device.
 * @return The directory's device address.
 */
uint64_t VulkanKernel::gpu_pool_address() {
    return Alligator::inst().gpu_directory_address();
}
/** --------------------------------------------------------------------------------------------------------- GPU Usage
 * @brief Reports bytes retained in Vulkan descriptor allocations.
 */
uint64_t VulkanKernel::gpu_usage() {
    return Memory::placement_usage(*VulkanContext::buffer_placement());
}
} // namespace buffetalligator
