#pragma once
/** --------------------------------------------------------------------------------------------------------- GPU Runtime
 * @file runtime.hpp
 * @brief Freezes one compute address domain before Slice metadata is allocated.
 */
#include <alligator/easygpu.hpp>
#include <memory>
#include <string>

namespace buffetalligator {
struct ShaderProgram;
struct ShaderPrepareInfo;
enum class GPUBackend { CPU, Vulkan, Metal, CUDA };
/** --------------------------------------------------------------------------------------------------------- GPU Device
 * @brief Holds the selected device's immutable placement and preparation entry point.
 */
struct GPUDevice {
    GPUBackend kind;
    const BuffetDescriptor* placement;
    bool unified;
    std::string name;
    std::unique_ptr<ShaderProgram> (*prepare)(const ShaderPrepareInfo&);
};
/** --------------------------------------------------------------------------------------------------------- GPU Device
 * @brief Probes and freezes the process compute backend on first use.
 */
const GPUDevice& gpu_device();
/** --------------------------------------------------------------------------------------------------------- GPU Memory Usage
 * @brief Returns available native memory counters for the selected backend.
 */
DeviceMemoryUsage gpu_memory_usage();
} // namespace buffetalligator
