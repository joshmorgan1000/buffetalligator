#pragma once
/** --------------------------------------------------------------------------------------------------------- VulkanKernel
 * @file vulkankernel.hpp
 * @brief The Vulkan substrate's shared device utilities (the old push-constant pipeline/dispatch
 * surface was deleted with the condemned lanes; kernels reach the device through gpu.hpp).
 */
#include <logging.hpp>
#include <alligator.hpp>
#include <cstdint>
#include <string>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- VulkanKernel
 * @class VulkanKernel
 * @brief Static surface over the Vulkan substrate for compute SPIR-V.
 */
class VulkanKernel {
public:
    /** ------------------------------------------------------------------------------------------- available
     * @brief True when a Vulkan compute device is present.
     * @return True if a Vulkan compute device is available, otherwise false.
     */
    static bool available();
    /** ------------------------------------------------------------------------------------------- device_name
     * @brief The device's name, empty without a device.
     * @return The name of the Vulkan device if present, otherwise an empty string.
     */
    static std::string device_name();
    /** ------------------------------------------------------------------------------------------- device_address
     * @brief The device address of a Slice's first byte, 0 when its memory is not GPU-visible.
     * @param slice The Slice for which to retrieve the device address.
     * @return The device address of the Slice's first byte if GPU-visible, otherwise 0.
     */
    static uint64_t device_address(const Slice& slice);
    /** ------------------------------------------------------------------------------------------- gpu_usage
     * @brief Bytes currently held in Vulkan slabs.
     * @return The number of bytes currently allocated on the Vulkan device.
     */
    static uint64_t gpu_usage();
};
} // namespace buffetalligator
