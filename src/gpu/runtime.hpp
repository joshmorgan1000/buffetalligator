#pragma once
/** --------------------------------------------------------------------------------------------------------- GPU Runtime
 * @file runtime.hpp
 * @brief Freezes one compute address domain before Slice metadata is allocated.
 */
#include <alligator/easygpu.hpp>
#include <cstdint>
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
    uint64_t placement_max_bytes = UINT64_MAX;      ///< Probed ceiling for one novel buffer in `placement`.
    uint64_t placement_capacity_bytes = UINT64_MAX; ///< Probed total bytes `placement`'s memory domain holds.
};
/** --------------------------------------------------------------------------------------------------------- GPU Device
 * @brief Probes and freezes the process compute backend on first use.
 */
const GPUDevice& gpu_device();
/** --------------------------------------------------------------------------------------------------------- GPU Memory Usage
 * @brief Returns available native memory counters for the selected backend.
 */
DeviceMemoryUsage gpu_memory_usage();
/** --------------------------------------------------------------------------------------------------------- GPU Placement Headroom
 * @brief Live budget headroom of the device placement's backing heap, SIZE_MAX when unbounded.
 */
uint64_t gpu_placement_headroom();
/** --------------------------------------------------------------------------------------------------------- GPU Thread Limits
 * @brief Returns the maximum number of threads per workgroup along each dimension for the selected backend.
 */
uint32_t gpu_thread_limit_x();
/** --------------------------------------------------------------------------------------------------------- GPU Thread Limit Y
 * @brief Returns the maximum number of threads per workgroup along the Y dimension for the selected backend.
 */
uint32_t gpu_thread_limit_y();
/** --------------------------------------------------------------------------------------------------------- GPU Thread Limit Z
 * @brief Returns the maximum number of threads per workgroup along the Z dimension for the selected backend.
 */
uint32_t gpu_thread_limit_z();
/** --------------------------------------------------------------------------------------------------------- GPU Workgroup Limits
 * @brief Returns the maximum number of workgroups per dispatch along each dimension for the selected backend.
 */
uint32_t gpu_workgroup_limit_x();
/** --------------------------------------------------------------------------------------------------------- GPU Workgroup Limit Y
 * @brief Returns the maximum number of workgroups per dispatch along the Y dimension for the selected backend.
 */
uint32_t gpu_workgroup_limit_y();
/** --------------------------------------------------------------------------------------------------------- GPU Workgroup Limit Z
 * @brief Returns the maximum number of workgroups per dispatch along the Z dimension for the selected backend.
 */
uint32_t gpu_workgroup_limit_z();
} // namespace buffetalligator
