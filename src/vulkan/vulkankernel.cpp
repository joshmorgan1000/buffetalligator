/** --------------------------------------------------------------------------------------------------------- VulkanKernel
 * @file vulkankernel.cpp
 * @brief The shared device utilities for compute SPIR-V on the VulkanContext device; execution
 * goes through gpu.hpp's Shader and job-table engines (DESIGN.md Law 3/4/5).
 */
#include <logging.hpp>
#include <vulkan/vulkankernel.hpp>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkanhelpers.hpp>
#include <vulkan/vulkancontext.hpp>
#include <vulkan/vulkanbuffer.hpp>

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
    if (!VulkanContext::device_present()) return 0;
    const Placemat* placement = slice.placement();
    const bool vulkan = placement == Placemat::HOST_VISIBLE
        || placement == Placemat::HOST_CACHEABLE || placement == Placemat::DEVICE
        || placement == Placemat::UNIFIED || placement == VulkanContext::buffer_placement();
    if (!vulkan) return 0;
    Placemat::Handle* handle = Placemat::get_for(&slice);
    if (handle == nullptr) return 0;
    const VulkanBuffer* slab = static_cast<const VulkanBuffer*>(handle->substrate_handle);
    return slab->address() + static_cast<uint64_t>(
        static_cast<const uint8_t*>(slice.raw()) - static_cast<const uint8_t*>(slab->host()));
}
} // namespace buffetalligator
