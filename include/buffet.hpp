#pragma once
/** --------------------------------------------------------------------------------------------------------- Buffet
 * @file buffet.hpp
 * @brief Umbrella header for the Buffet library.
 */
#include <alligator.hpp>
#include <alligator/atomics.hpp>
#include <alligator/containers.hpp>
#include <alligator/easygpu.hpp>
#include <alligator/easymmap.hpp>
#include <alligator/kitchen.hpp>
#if defined(BUFFETALLIGATOR_HAS_CUDA)
#include <alligator/easycuda.hpp>
#endif
#if defined(BUFFETALLIGATOR_HAS_METAL)
#include <alligator/easymetal.hpp>
#endif
#if defined(BUFFETALLIGATOR_HAS_VULKAN)
#include <alligator/easyvulkan.hpp>
#endif
