/** --------------------------------------------------------------------------------------------------------- GPU Runtime
 * @file runtime.cpp
 * @brief Selects the native or Vulkan compute domain before arena construction.
 */
#include <gpu/runtime.hpp>
#include <gpu/backend.hpp>
#include <memory/lifetime.hpp>
#include <alligator/easyvulkan.hpp>
#if defined(BUFFETALLIGATOR_HAS_METAL)
#include <alligator/easymetal.hpp>
#include <metal/metal.hpp>
#endif
#include <cstdlib>
#include <string_view>

namespace buffetalligator {
namespace {
/** --------------------------------------------------------------------------------------------------------- GPU Selection
 * @brief Owns the immutable result of explicit selection or native-first capability probing.
 */
struct GPUSelection {
    GPUDevice device{GPUBackend::CPU,
        BuffetDescriptors::get(AlignedHeapBuffer::type_idx()), false, {}, nullptr};
    GPUSelection() {
        shader_runtime_initialize();
        const char* requested = std::getenv("ALLIGATOR_GPU_BACKEND");
        const std::string_view choice = requested == nullptr ? "auto" : requested;
        if (choice == "cpu") return;
        if (choice != "auto" && choice != "metal" && choice != "vulkan"
            && choice != "cuda") {
            ALLIGATOR_GPU_THROW("ALLIGATOR_GPU_BACKEND must be auto, cpu, metal, vulkan, or cuda");
        }
        if (choice == "cuda") {
            ALLIGATOR_GPU_THROW("This build has no qualified CUDA backend");
        }
#if defined(BUFFETALLIGATOR_HAS_METAL)
        if ((choice == "auto" || choice == "metal") && metal_available()) {
            device = {GPUBackend::Metal, metal_placement(), metal_unified(),
                metal_device_name(), &metal_prepare, metal_max_buffer_bytes(), metal_capacity_bytes()};
            LOG_INFO_STREAM << "GPU backend=metal; device=" << device.name;
            return;
        }
#endif
        if (choice == "metal") {
            ALLIGATOR_GPU_THROW("No compatible native Metal compute device is available");
        }
        if (VulkanContext::device_present()) {
            const DeviceProperties& properties = VulkanContext::device_properties();
            device = {GPUBackend::Vulkan, VulkanContext::buffer_placement(),
                VulkanContext::device_unified(), VulkanKernel::device_name(), &vulkan_prepare,
                properties.max_allocation_bytes,
                properties.unified_memory ? properties.host_visible_memory_bytes
                    : properties.device_local_memory_bytes};
            LOG_INFO_STREAM << "GPU backend=vulkan; device=" << device.name
                << "; max single allocation=" << device.placement_max_bytes << " bytes"
                << "; placement capacity=" << device.placement_capacity_bytes << " bytes";
        } else if (choice == "vulkan") {
            ALLIGATOR_GPU_THROW("No compatible Vulkan compute device is available");
        }
    }
};
} // namespace
const GPUDevice& gpu_device() {
    static RuntimeFinalizer lifetime(new GPUSelection, &RuntimeFinalizer::delete_owner<GPUSelection>);
    return static_cast<GPUSelection*>(lifetime.object)->device;
}
DeviceMemoryUsage gpu_memory_usage() {
    const auto& device = gpu_device();
#if defined(BUFFETALLIGATOR_HAS_METAL)
    if (device.kind == GPUBackend::Metal) return MetalBuffer::memory_usage();
#endif
    if (device.kind == GPUBackend::Vulkan) {
        const auto& properties = VulkanContext::instance().device_properties();
        return {properties.device_local_memory_bytes,
            properties.supports_memory_budget
                ? std::optional<uint64_t>(VulkanContext::device_free_bytes()) : std::nullopt,
            std::nullopt, std::nullopt};
    }
    return {};
}
/** --------------------------------------------------------------------------------------------------------- GPU Placement Headroom
 * @brief Reports the live headroom of the frozen device placement's backing heap.
 */
uint64_t gpu_placement_headroom() {
    const auto& device = gpu_device();
#if defined(BUFFETALLIGATOR_HAS_METAL)
    if (device.kind == GPUBackend::Metal) {
        const DeviceMemoryUsage usage = MetalBuffer::memory_usage();
        return usage.available_bytes.value_or(UINT64_MAX);
    }
#endif
    if (device.kind == GPUBackend::Vulkan) return VulkanContext::buffer_heap_headroom();
    return UINT64_MAX;
}
} // namespace buffetalligator
