/** --------------------------------------------------------------------------------------------------------- GPU Statics
 * @file shader.cpp
 * @brief The public GPU surface over the frozen backend selection; one definition each, so consumers link.
 */
#include <alligator.hpp>
#include <alligator/easygpu.hpp>
#include <gpu/backend.hpp>

namespace buffetalligator {
void GPU::select(GPUBackend backend) { GPUStatic::gpu_select(backend); }
GPUBackend GPU::backend() { return GPUStatic::gpu_device().kind; }
void* GPU::device_handle() { return &GPUStatic::gpu_device(); }
bool GPU::exists() { return GPUStatic::gpu_device().kind != GPUBackend::CPU; }
bool GPU::unified_memory() { return GPUStatic::gpu_device().unified; }
std::string GPU::device_name() { return GPUStatic::gpu_device().name; }
DeviceMemoryUsage GPU::memory_usage() { return GPUStatic::gpu_memory_usage(); }
GPUCapabilities GPU::capabilities() { return GPUStatic::gpu_capabilities(); }
uint32_t GPU::thread_limit_x() { return GPUStatic::gpu_thread_limit_x(); }
uint32_t GPU::thread_limit_y() { return GPUStatic::gpu_thread_limit_y(); }
uint32_t GPU::thread_limit_z() { return GPUStatic::gpu_thread_limit_z(); }
uint32_t GPU::workgroup_limit_x() { return GPUStatic::gpu_workgroup_limit_x(); }
uint32_t GPU::workgroup_limit_y() { return GPUStatic::gpu_workgroup_limit_y(); }
uint32_t GPU::workgroup_limit_z() { return GPUStatic::gpu_workgroup_limit_z(); }
} // namespace buffetalligator
