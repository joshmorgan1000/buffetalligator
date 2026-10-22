/** --------------------------------------------------------------------------------------------------------- GPU Runtime
 * @file runtime.cpp
 * @brief Selects the native or Vulkan compute domain before arena construction.
 */
#include <gpu/runtime.hpp>
#include <gpu/backend.hpp>
#include <alligator/easyvulkan.hpp>
#if defined(BUFFETALLIGATOR_HAS_CUDA)
#include <alligator/easycuda.hpp>
#include <cuda_runtime.h>
#endif
#include <atomic>
#include <cstdlib>
#include <string>
#include <string_view>

namespace buffetalligator {
namespace {
/// @brief The backend GPU::select recorded, or -1 when the environment and the auto order decide.
constinit std::atomic<int> requested_backend{-1};
/// @brief Set as the freeze begins, so a later select can tell it arrived too late.
constinit std::atomic<bool> selection_started{false};
/** --------------------------------------------------------------------------------------------------------- Backend Name
 * @brief The ALLIGATOR_GPU_BACKEND spelling of a backend.
 * @param backend The backend.
 * @return Its name.
 */
constexpr std::string_view backend_name(GPUBackend backend) {
    constexpr std::string_view names[] = {"cpu", "vulkan", "metal", "cuda"};
    return names[static_cast<int>(backend)];
}
/** --------------------------------------------------------------------------------------------------------- GPU Selection
 * @brief Owns the immutable result of explicit selection or Vulkan-first capability probing; native Metal runs
 * only when asked for or when no Vulkan device exists.
 */
struct GPUSelection {
    GPUDevice device{GPUBackend::CPU,
        BuffetDescriptors::get(AlignedHeapBuffer::type_idx()), false, {}, nullptr};
    GPUSelection() {
        shader_runtime_initialize();
        selection_started.store(true, std::memory_order_seq_cst);
        const char* environment = std::getenv("ALLIGATOR_GPU_BACKEND");
        const int selected = requested_backend.load(std::memory_order_seq_cst);
        /// A set environment choice other than auto overrides GPU::select, which overrides the auto order.
        const std::string_view choice = environment != nullptr && std::string_view(environment) != "auto"
            ? std::string_view(environment)
            : selected >= 0 ? backend_name(static_cast<GPUBackend>(selected)) : std::string_view("auto");
        if (choice == "cpu") return;
        if (choice != "auto" && choice != "metal" && choice != "vulkan"
            && choice != "cuda") {
            ALLIGATOR_GPU_THROW("ALLIGATOR_GPU_BACKEND must be auto, cpu, metal, vulkan, or cuda");
        }
        if (choice == "cuda") {
            ALLIGATOR_GPU_THROW("This build has no qualified CUDA backend");
        }
        if ((choice == "auto" || choice == "vulkan") && VulkanContext::device_present()) {
            const DeviceProperties& properties = VulkanContext::device_properties();
            device = {GPUBackend::Vulkan, VulkanContext::buffer_placement(),
                VulkanContext::device_unified(), VulkanKernel::device_name(), &vulkan_prepare,
                properties.max_allocation_bytes,
                properties.unified_memory ? properties.host_visible_memory_bytes
                    : properties.device_local_memory_bytes};
            LOG_INFO_STREAM << "GPU backend=vulkan; device=" << device.name
                << "; max single allocation=" << device.placement_max_bytes << " bytes"
                << "; placement capacity=" << device.placement_capacity_bytes << " bytes";
            return;
        }
        if (choice == "vulkan") {
            ALLIGATOR_GPU_THROW("No compatible Vulkan compute device is available");
        }
        if (choice == "metal") {
            ALLIGATOR_GPU_THROW("No compatible native Metal compute device is available");
        }
    }
};
} // namespace
const GPUDevice& gpu_device() {
    static GPUSelection* selection = new GPUSelection;
    return selection->device;
}
/** --------------------------------------------------------------------------------------------------------- GPU Select
 * @brief Records the choice first, so a freeze that starts concurrently either reads it or is seen here.
 */
void gpu_select(GPUBackend backend) {
    requested_backend.store(static_cast<int>(backend), std::memory_order_seq_cst);
    if (selection_started.load(std::memory_order_seq_cst) && gpu_device().kind != backend) {
        ALLIGATOR_GPU_THROW("GPU::select(" + std::string(backend_name(backend))
            + ") arrived after the process froze on " + std::string(backend_name(gpu_device().kind))
            + "; select the backend before the first Slice or GPU operation");
    }
}
DeviceMemoryUsage gpu_memory_usage() {
    const auto& device = gpu_device();
    const auto& properties = VulkanContext::instance().device_properties();
    return {properties.device_local_memory_bytes,
        properties.supports_memory_budget
            ? std::optional<uint64_t>(VulkanContext::device_free_bytes()) : std::nullopt,
        std::nullopt, std::nullopt};
    return {};
}
/** --------------------------------------------------------------------------------------------------------- GPU Thread Limit X
 * @brief Returns the maximum number of threads per workgroup along the X dimension for the selected backend.
 */
uint32_t gpu_thread_limit_x() {
    const auto& device = gpu_device();
#if defined(BUFFETALLIGATOR_HAS_CUDA)
    if (device.kind == GPUBackend::CUDA) return CudaBuffer::thread_limit_x();
#endif
    if (device.kind == GPUBackend::Vulkan) return VulkanContext::device_properties().max_workgroup_size[0];
    return 1;
}
/** --------------------------------------------------------------------------------------------------------- GPU Thread Limit Y
 * @brief Returns the maximum number of threads per workgroup along the Y dimension for the selected backend.
 */
uint32_t gpu_thread_limit_y() {
    const auto& device = gpu_device();
#if defined(BUFFETALLIGATOR_HAS_CUDA)
    if (device.kind == GPUBackend::CUDA) return CudaBuffer::thread_limit_y();
#endif
    if (device.kind == GPUBackend::Vulkan) return VulkanContext::device_properties().max_workgroup_size[1];
    return 1;
}
/** --------------------------------------------------------------------------------------------------------- GPU Thread Limit Z
 * @brief Returns the maximum number of threads per workgroup along the Z dimension for the selected backend.
 */
uint32_t gpu_thread_limit_z() {
    const auto& device = gpu_device();
#if defined(BUFFETALLIGATOR_HAS_CUDA)
    if (device.kind == GPUBackend::CUDA) return CudaBuffer::thread_limit_z();
#endif
    if (device.kind == GPUBackend::Vulkan) return VulkanContext::device_properties().max_workgroup_size[2];
    return 1;
}
/** --------------------------------------------------------------------------------------------------------- GPU Workgroup Limit X
 * @brief Returns the maximum number of workgroups per dispatch along the X dimension for the selected backend.
 */
uint32_t gpu_workgroup_limit_x() {
    const auto& device = gpu_device();
#if defined(BUFFETALLIGATOR_HAS_CUDA)
    if (device.kind == GPUBackend::CUDA) return CudaBuffer::workgroup_limit_x();
#endif
    if (device.kind == GPUBackend::Vulkan) return VulkanContext::device_properties().max_workgroup_count[0];
    return 1;
}
/** --------------------------------------------------------------------------------------------------------- GPU Workgroup Limit Y
 * @brief Returns the maximum number of workgroups per dispatch along the Y dimension for the selected backend.
 */
uint32_t gpu_workgroup_limit_y() {
    const auto& device = gpu_device();
#if defined(BUFFETALLIGATOR_HAS_CUDA)
    if (device.kind == GPUBackend::CUDA) return CudaBuffer::workgroup_limit_y();
#endif
    if (device.kind == GPUBackend::Vulkan) return VulkanContext::device_properties().max_workgroup_count[1];
    return 1;
}
/** --------------------------------------------------------------------------------------------------------- GPU Workgroup Limit Z
 * @brief Returns the maximum number of workgroups per dispatch along the Z dimension for the selected backend.
 */
uint32_t gpu_workgroup_limit_z() {
    const auto& device = gpu_device();
#if defined(BUFFETALLIGATOR_HAS_CUDA)
    if (device.kind == GPUBackend::CUDA) return CudaBuffer::workgroup_limit_z();
#endif
    if (device.kind == GPUBackend::Vulkan) return VulkanContext::device_properties().max_workgroup_count[2];
    return 1;
}
/** --------------------------------------------------------------------------------------------------------- GPU Placement Headroom
 * @brief Reports the live headroom of the frozen device placement's backing heap.
 */
uint64_t gpu_placement_headroom() {
    const auto& device = gpu_device();
    if (device.kind == GPUBackend::Vulkan) return VulkanContext::buffer_heap_headroom();
    return UINT64_MAX;
}
} // namespace buffetalligator
