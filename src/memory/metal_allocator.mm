/** --------------------------------------------------------------------------------------------------------- Metal Allocator
 * @file metal_allocator.mm
 * @brief Owns shared Metal buffers registered for indirect access until their final retained use.
 */
#include <alligator/easymetal.hpp>
#include <metal/context.hpp>
#include <algorithm>
#include <cstring>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Constructor
 * @brief Allocates checked granules and publishes their native allocation to the residency registry.
 */
MetalBuffer::MetalBuffer(size_t size) {
    MetalContext& context = metal_context();
    if (!context.descriptor) ALLIGATOR_THROW("MetalBuffer requires a registered Metal device");
    if (size > (uint64_t{UINT32_MAX} << 6))
        ALLIGATOR_THROW("MetalBuffer exceeds the 64-byte-granule size limit");
    const size_t bytes = (size + 63) & ~size_t{63};
    if (bytes > context.maximum_length) ALLIGATOR_THROW("MetalBuffer exceeds maxBufferLength");
    if (bytes == 0) return;
    @autoreleasepool {
        id<MTLBuffer> native = [context.device newBufferWithLength:bytes
            options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeTracked];
        if (!native) ALLIGATOR_THROW("Allocating a shared Metal buffer failed");
        const uint64_t address = native.gpuAddress;
        if (!address) ALLIGATOR_THROW("Metal buffer has no GPU address");
        std::memset(native.contents, 0, bytes);
        {
            std::lock_guard lock(context.allocations);
            context.resources.push_back(native);
            if (@available(macOS 15.0, *)) {
                if (context.residency) {
                    [context.residency addAllocation:native];
                    context.dirty = true;
                }
            }
        }
        host_ = native.contents;
        address_ = address;
        size_ = bytes;
        buffer_ = (__bridge_retained void*)native;
    }
}
/** --------------------------------------------------------------------------------------------------------- Release
 * @brief Removes an allocation after its final Slice owner and accepted GPU user have retired.
 */
void MetalBuffer::release() {
    if (!buffer_) return;
    @autoreleasepool {
        id<MTLBuffer> native = (__bridge_transfer id<MTLBuffer>)buffer_;
        MetalContext& context = metal_context();
        std::lock_guard lock(context.allocations);
        if (@available(macOS 15.0, *)) {
            if (context.residency) {
                [context.residency removeAllocation:native];
                [context.residency commit];
                context.dirty = false;
            }
        }
        const auto found = std::find(context.resources.begin(), context.resources.end(), native);
        context.resources.erase(found);
    }
    buffer_ = nullptr;
    host_ = nullptr;
    address_ = 0;
    size_ = 0;
}
/** --------------------------------------------------------------------------------------------------------- Register Type
 * @brief Registers one explicitly selected device before backend selection is frozen.
 */
const BuffetDescriptor* MetalBuffer::register_type(void* device) {
    MetalContext& context = metal_context();
    std::lock_guard lock(context.initialization);
    id<MTLDevice> selected = (__bridge id<MTLDevice>)device;
    if (context.descriptor) {
        if (context.device != selected) ALLIGATOR_THROW("A different Metal device is already registered");
        return context.descriptor;
    }
    if (!context.initialize(selected)) ALLIGATOR_THROW("Metal device cannot execute the Slice pointer ABI");
    context.probed = true;
    return context.descriptor;
}
/** --------------------------------------------------------------------------------------------------------- Memory Usage
 * @brief Reports current native allocation and recommended working-set counters.
 */
DeviceMemoryUsage MetalBuffer::memory_usage() {
    const MetalContext& context = metal_context();
    if (!context.descriptor) ALLIGATOR_THROW("Metal memory usage requires a registered device");
    return {std::nullopt, std::nullopt, context.device.currentAllocatedSize,
        context.device.recommendedMaxWorkingSetSize};
}
} // namespace buffetalligator
