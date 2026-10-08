#pragma once
/** --------------------------------------------------------------------------------------------------------- Metal Backend
 * @file metal.hpp
 * @brief Private Metal device selection and prepared program entry points.
 */
#include <gpu/backend.hpp>

namespace buffetalligator {
bool metal_available();
const BuffetDescriptor* metal_placement();
bool metal_unified();
std::string metal_device_name();
uint64_t metal_max_buffer_bytes();
uint64_t metal_capacity_bytes();
std::unique_ptr<ShaderProgram> metal_prepare(const ShaderPrepareInfo& info);
uint32_t metal_thread_limit_x();
uint32_t metal_thread_limit_y();
uint32_t metal_thread_limit_z();
uint32_t metal_workgroup_limit_x();
uint32_t metal_workgroup_limit_y();
uint32_t metal_workgroup_limit_z();
} // namespace buffetalligator
