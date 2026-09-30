#pragma once
/** --------------------------------------------------------------------------------------------------------- Metal Context
 * @file context.hpp
 * @brief Owns the frozen Metal device, queue, and synchronized indirect-resource registry.
 */
#include <alligator/easymetal.hpp>
#include <gpu/backend.hpp>
#include <Metal/Metal.h>
#include <mutex>
#include <vector>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Metal Context
 * @brief Keeps native resources alive until arena and prepared-program teardown completes.
 */
struct MetalContext {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLResidencySet> residency = nil;
    std::mutex initialization;
    std::mutex allocations;
    std::vector<id<MTLResource>> resources;
    const BuffetDescriptor* descriptor = nullptr;
    size_t maximum_length = 0;
    size_t slots = 0;
    bool probed = false;
    bool dirty = false;
    MetalContext();
    ~MetalContext();
    bool initialize(id<MTLDevice> selected);
};
MetalContext& metal_context();
} // namespace buffetalligator
