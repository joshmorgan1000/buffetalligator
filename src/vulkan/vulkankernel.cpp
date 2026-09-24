/** --------------------------------------------------------------------------------------------------------- VulkanKernel
 * @file vulkankernel.cpp
 * @brief The shared device utilities for compute SPIR-V on the VulkanContext device; execution
 * goes through gpu.hpp's Shader and job-table engines (DESIGN.md Law 3/4/5).
 */
#include <logging.hpp>
#include <alligator.hpp>

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
    if (slice.is_null()) return 0;
    return Alligator::gpubuf_for(slice)->address;
}
/** --------------------------------------------------------------------------------------------------------- VulkanKernel::gpu_pool_address
 * @brief The device address of the alligator's shared GPUBuf table, 0 without a compute device.
 * @return The table's device address.
 */
uint64_t VulkanKernel::gpu_pool_address() {
    return Alligator::gpu_table_address();
}
} // namespace buffetalligator
