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
std::unique_ptr<ShaderProgram> metal_prepare(const ShaderPrepareInfo& info);
} // namespace buffetalligator
