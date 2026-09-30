/** --------------------------------------------------------------------------------------------------------- Metal Allocator
 * @file metal_allocator.mm
 * @brief Allocates MetalBuffer storage as shared, device-addressable MTLBuffers on the registered device.
 */
#include <alligator/easymetal.hpp>
#include <Metal/Metal.h>
#include <cstring>

namespace buffetalligator {
namespace {
/** --------------------------------------------------------------------------------------------------------- Metal Context
 * @brief Retains the caller's Metal device and its probed allocation limit.
 */
struct MetalContext {
    id<MTLDevice> device = nil;
    NSUInteger maximum_length = 0;
    const BuffetDescriptor* descriptor = nullptr;
};
MetalContext context;
}
/** --------------------------------------------------------------------------------------------------------- MetalBuffer Constructor
 * @brief Creates one zero-initialized shared MTLBuffer and records its contents and gpuAddress.
 * @param size Buffer size in bytes, rounded up to 64.
 */
MetalBuffer::MetalBuffer(size_t size) : size_((size + 63) & ~size_t{63}) {
    @autoreleasepool {
        if (size_ > context.maximum_length) ALLIGATOR_THROW("MetalBuffer exceeds maxBufferLength");
        id<MTLBuffer> native = [context.device newBufferWithLength:size_
            options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache];
        if (!native) ALLIGATOR_THROW("Allocating a shared MTLBuffer failed");
        host_ = [native contents];
        std::memset(host_, 0, size_);
        if (@available(macOS 13.0, iOS 16.0, *)) {
            address_ = native.gpuAddress;
        }
        buffer_ = (__bridge_retained void*)native;
    }
}
/** --------------------------------------------------------------------------------------------------------- MetalBuffer Release
 * @brief Drops the retained MTLBuffer.
 */
void MetalBuffer::release() {
    if (!buffer_) return;
    @autoreleasepool {
        id<MTLBuffer> native = (__bridge_transfer id<MTLBuffer>)buffer_;
        (void)native;
    }
    buffer_ = nullptr;
    host_ = nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Register Type
 * @brief Retains the caller's device, requires gpuAddress support, and registers the descriptor once.
 * @param device A bridged id<MTLDevice>.
 * @return The registered descriptor.
 */
const BuffetDescriptor* MetalBuffer::register_type(void* device) {
    @autoreleasepool {
        if (context.descriptor) ALLIGATOR_THROW("The Metal buffet type is already registered");
        if (!device) ALLIGATOR_THROW("Metal registration requires an MTLDevice");
        if (@available(macOS 13.0, iOS 16.0, *)) {
        } else {
            ALLIGATOR_THROW("MetalBuffer device addresses require macOS 13 or iOS 16");
        }
        context.device = (__bridge id<MTLDevice>)device;
        context.maximum_length = context.device.maxBufferLength;
        if (context.maximum_length < default_size()) {
            ALLIGATOR_THROW("The Metal device cannot allocate a 64 MiB buffer");
        }
        const BuffetDescriptor* descriptor =
            BuffetDescriptors::descriptor_for(static_cast<MetalBuffer*>(nullptr));
        if (BuffetDescriptors::register_descriptor(descriptor) != type_idx()) {
            ALLIGATOR_THROW("MetalBuffer must be the first registered buffet type after the built-ins; "
                            "register it before any other custom type");
        }
        context.descriptor = descriptor;
        return context.descriptor;
    }
}
/** --------------------------------------------------------------------------------------------------------- Memory Usage
 * @brief Queries native Metal allocation and working-set counters.
 * @return The device's memory usage.
 */
DeviceMemoryUsage MetalBuffer::memory_usage() {
    if (!context.descriptor) ALLIGATOR_THROW("Register the Metal buffet type before querying its memory");
    return {std::nullopt, std::nullopt, context.device.currentAllocatedSize,
            context.device.recommendedMaxWorkingSetSize};
}
} // namespace buffetalligator
