#pragma once
/** --------------------------------------------------------------------------------------------------------- GPU Backend
 * @file backend.hpp
 * @brief Backend selection frozen at first use, and the GLSL assembly shared by prepared shaders.
 */
#include <alligator.hpp>
#include <alligator/easygpu.hpp>
#include <gpu/easyvulkan.hpp>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Backend Name
 * @brief The ALLIGATOR_GPU_BACKEND spelling of a backend.
 * @param backend The backend.
 * @return Its name.
 */
constexpr std::string_view backend_name(GPUBackend backend) {
    constexpr std::string_view names[] = {"cpu", "vulkan", "metal", "cuda"};
    return names[static_cast<int>(backend)];
}
/** --------------------------------------------------------------------------------------------------------- Public Shader Source
 * @brief Wraps a user body defining `void alligator_main(uint slice)` with the Slice id ABI and the entry point.
 * @param body The user's GLSL body.
 * @return The GLSL after the EZ prelude.
 */
std::string public_shader_source(std::string_view body);
/** --------------------------------------------------------------------------------------------------------- GPU Device
 * @brief Holds the selected device's probed capacities.
 */
struct GPUDevice {
    GPUBackend kind;
    bool unified;
    std::string name;
    uint64_t placement_max_bytes = UINT64_MAX;      ///< Probed ceiling for one novel buffer.
    uint64_t placement_capacity_bytes = UINT64_MAX; ///< Probed total bytes the device's memory domain holds.
};
/** --------------------------------------------------------------------------------------------------------- GPU Selection
 * @brief Owns the immutable result of explicit selection or Vulkan-first capability probing.
 */
struct GPUSelection {
    GPUDevice device{GPUBackend::CPU, false, {}};
    GPUSelection();
};
/** --------------------------------------------------------------------------------------------------------- GPU Static
 * @struct GPUStatic
 * @brief Static access to the selected GPU device and its probed limits.
 */
struct GPUStatic {
    /** ------------------------------------------------------------------------------------------- GPU Selection
     * @brief Freezes the backend on first call and returns the selection.
     */
    static GPUSelection& gpu_selection() {
        static GPUSelection* selection = new GPUSelection;
        return *selection;
    }
    /** ------------------------------------------------------------------------------------------- GPU Device
     * @brief Returns the selected GPU device.
     */
    static GPUDevice& gpu_device() { return gpu_selection().device; }
    /** ------------------------------------------------------------------------------------------- Requested Backend
     * @brief The backend GPU::select recorded, or -1 when the environment and the auto order decide.
     */
    static std::atomic<int>& requested_backend() {
        static std::atomic<int>* backend = new std::atomic<int>(-1);
        return *backend;
    }
    /** ------------------------------------------------------------------------------------------- Selection Started
     * @brief Set as the freeze begins, so a later select can tell it arrived too late.
     */
    static std::atomic<bool>& selection_started() {
        static std::atomic<bool>* started = new std::atomic<bool>(false);
        return *started;
    }
    /** ------------------------------------------------------------------------------------------- GPU Select
     * @brief Records the choice first, so a freeze that starts concurrently either reads it or is seen here.
     */
    static void gpu_select(GPUBackend backend) {
        requested_backend().store(static_cast<int>(backend), std::memory_order_seq_cst);
        if (selection_started().load(std::memory_order_seq_cst) && gpu_device().kind != backend) {
            ALLIGATOR_GPU_THROW("GPU::select(" + std::string(backend_name(backend))
                + ") arrived after the process froze on " + std::string(backend_name(gpu_device().kind))
                + "; select the backend before the first Slice or GPU operation");
        }
    }
    /** ------------------------------------------------------------------------------------------- GPU Memory Usage
     * @brief Device-local capacity and the live budget headroom the driver reports.
     */
    static DeviceMemoryUsage gpu_memory_usage() {
        const DeviceProperties& properties = VulkanContext::device_properties();
        return {properties.device_local_memory_bytes, VulkanContext::device_free_bytes(VulkanContext::instance())};
    }
    /** ------------------------------------------------------------------------------------------- GPU Capabilities
     * @brief The frozen Vulkan device's probed facts as plain data.
     */
    static GPUCapabilities gpu_capabilities() {
        GPUCapabilities capabilities;
        if (gpu_device().kind != GPUBackend::Vulkan) return capabilities;
        const DeviceProperties& properties = VulkanContext::device_properties();
        capabilities.present = true;
        capabilities.unified = properties.unified_memory;
        capabilities.supports_float16 = properties.supports_float16;
        capabilities.supports_int64 = properties.supports_int64;
        capabilities.supports_memory_budget = properties.supports_memory_budget;
        capabilities.supports_timeline_semaphore = properties.supports_timeline_semaphore;
        capabilities.queue_count = VulkanContext::queue_count();
        capabilities.subgroup_size = properties.subgroup_size;
        capabilities.max_workgroup_invocations = properties.max_workgroup_invocations;
        capabilities.device_local_bytes = properties.device_local_memory_bytes;
        capabilities.host_visible_bytes = properties.host_visible_memory_bytes;
        capabilities.device_local_headroom_bytes = VulkanContext::device_free_bytes(VulkanContext::instance());
        capabilities.device_name = properties.device_name;
        return capabilities;
    }
    /** ------------------------------------------------------------------------------------------- Thread Limits
     * @brief Probed invocation limits per workgroup along each dimension.
     */
    static uint32_t gpu_thread_limit_x() {
        return gpu_device().kind == GPUBackend::Vulkan ? VulkanContext::device_properties().max_workgroup_size[0] : 1;
    }
    static uint32_t gpu_thread_limit_y() {
        return gpu_device().kind == GPUBackend::Vulkan ? VulkanContext::device_properties().max_workgroup_size[1] : 1;
    }
    static uint32_t gpu_thread_limit_z() {
        return gpu_device().kind == GPUBackend::Vulkan ? VulkanContext::device_properties().max_workgroup_size[2] : 1;
    }
    /** ------------------------------------------------------------------------------------------- Workgroup Limits
     * @brief Probed workgroup counts per dispatch along each dimension.
     */
    static uint32_t gpu_workgroup_limit_x() {
        return gpu_device().kind == GPUBackend::Vulkan ? VulkanContext::device_properties().max_workgroup_count[0] : 1;
    }
    static uint32_t gpu_workgroup_limit_y() {
        return gpu_device().kind == GPUBackend::Vulkan ? VulkanContext::device_properties().max_workgroup_count[1] : 1;
    }
    static uint32_t gpu_workgroup_limit_z() {
        return gpu_device().kind == GPUBackend::Vulkan ? VulkanContext::device_properties().max_workgroup_count[2] : 1;
    }
    /** ------------------------------------------------------------------------------------------- GPU Placement Headroom
     * @brief Reports the live headroom of the frozen device's backing heap.
     */
    static uint64_t gpu_placement_headroom() {
        return gpu_device().kind == GPUBackend::Vulkan ? VulkanContext::buffer_heap_headroom() : UINT64_MAX;
    }
};
/** --------------------------------------------------------------------------------------------------------- GPU Selection Constructor
 * @brief A set environment choice other than auto overrides GPU::select, which overrides the auto order.
 */
inline GPUSelection::GPUSelection() {
    GPUStatic::selection_started().store(true, std::memory_order_seq_cst);
    const char* environment = std::getenv("ALLIGATOR_GPU_BACKEND");
    const int selected = GPUStatic::requested_backend().load(std::memory_order_seq_cst);
    const std::string_view choice = environment != nullptr && std::string_view(environment) != "auto"
        ? std::string_view(environment)
        : selected >= 0 ? backend_name(static_cast<GPUBackend>(selected)) : std::string_view("auto");
    if (choice == "cpu") return;
    if (choice != "auto" && choice != "metal" && choice != "vulkan" && choice != "cuda") {
        ALLIGATOR_GPU_THROW("ALLIGATOR_GPU_BACKEND must be auto, cpu, metal, vulkan, or cuda");
    }
    if (choice == "cuda") ALLIGATOR_GPU_THROW("This build has no qualified CUDA backend");
    if ((choice == "auto" || choice == "vulkan") && VulkanContext::device_present()) {
        const DeviceProperties& properties = VulkanContext::device_properties();
        device = {GPUBackend::Vulkan, VulkanContext::device_unified(), properties.device_name,
            properties.max_allocation_bytes,
            properties.unified_memory ? properties.host_visible_memory_bytes : properties.device_local_memory_bytes};
        LOG_INFO_STREAM << "GPU backend=vulkan; device=" << device.name
            << "; max single allocation=" << device.placement_max_bytes << " bytes"
            << "; placement capacity=" << device.placement_capacity_bytes << " bytes";
        return;
    }
    if (choice == "vulkan") ALLIGATOR_GPU_THROW("No compatible Vulkan compute device is available");
    if (choice == "metal") ALLIGATOR_GPU_THROW("No compatible native Metal compute device is available");
}
} // namespace buffetalligator
