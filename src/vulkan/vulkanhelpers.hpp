#pragma once
/** --------------------------------------------------------------------------------------------------------- Vulkan Helpers
 * @file vulkanhelpers.hpp
 * @brief Header for Vulkan helper functions and utilities.
 */
#include <logging.hpp>
#include <alligator.hpp>
#include <vulkan/vulkan.hpp>
#include <memory>
#include <string_view>
#include <vector>

namespace buffetalligator {
class VulkanContext;
/** --------------------------------------------------------------------------------------------------------- Vulkan Exception
 * @class VulkanException
 * @brief Exception type for Vulkan-related errors.
 */
class VulkanException : public threadsafe_logger::Exception {
public:
    using threadsafe_logger::Exception::Exception;
    using threadsafe_logger::Exception::what;
};
#define ALLIGATOR_GPU_THROW(msg) throw VulkanException(msg)
/** --------------------------------------------------------------------------------------------------------- Compile Vulkan GLSL
 * @brief Compiles a complete Vulkan 1.2 compute shader into optimized SPIR-V.
 * @param source Complete GLSL source.
 * @param float16 Whether to define ALLIGATOR_FLOAT16 for the source.
 * @param name Diagnostic shader name.
 * @return Optimized SPIR-V words.
 */
std::vector<uint32_t> compile_vulkan_glsl(
    std::string_view source,
    bool float16,
    std::string_view name
);
/** --------------------------------------------------------------------------------------------------------- DeviceProperties
 * @struct DeviceProperties
 * @brief Queried physical-device limits relevant to dispatch sizing and batch planning.
 */
struct DeviceProperties {
    uint64_t gpu_free_bytes = 0;        ///< Device-local headroom from the budget query (0 without it).
    std::string device_name;                   ///< Device name.
    uint32_t max_workgroup_count[3];           ///< Max workgroup counts per dispatch dimension.
    uint32_t max_workgroup_size[3];            ///< Max workgroup sizes.
    uint32_t max_workgroup_invocations;        ///< Max invocations per workgroup.
    uint32_t max_shared_memory;                ///< Max shared memory per workgroup (bytes).
    uint32_t subgroup_size;                    ///< SIMD subgroup width.
    uint64_t device_local_memory_bytes;        ///< Total device-local heap size.
    uint64_t host_visible_memory_bytes;        ///< Device-local heap size reachable from the host.
    uint64_t max_storage_buffer_range;         ///< Max storage buffer range.
    uint32_t max_push_constants;               ///< Max push constant bytes.
    bool supports_subgroup_arithmetic;         ///< Subgroup arithmetic ops available.
    bool supports_subgroup_shuffle;            ///< Subgroup shuffle ops available.
    bool supports_buffer_device_address;       ///< Device-resolvable buffer addresses.
    bool supports_int64;                       ///< 64-bit integers in shaders.
    bool supports_int64_atomics;               ///< 64-bit buffer atomics.
    bool supports_float16;                     ///< 16-bit float arithmetic in shaders.
    bool supports_memory_budget;               ///< Live device-memory headroom queryable.
    bool supports_timeline_semaphore;          ///< Timeline semaphores available.
    bool unified_memory;                       ///< Host and device share physical memory.
};
} // namespace buffetalligator
