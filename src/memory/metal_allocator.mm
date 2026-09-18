/** --------------------------------------------------------------------------------------------------------- Metal Allocator
 * @file metal_allocator.mm
 * @brief Supplies shared Metal buffers through the arena placement callbacks.
 */
#include <alligator_allocators.hpp>
#include <Metal/Metal.h>
#include <memory>

namespace buffetalligator {
namespace {
/** --------------------------------------------------------------------------------------------------------- Metal Context
 * @brief Retains the caller's Metal device and its probed allocation limit.
 */
struct MetalContext {
    id<MTLDevice> device = nil;
    NSUInteger maximum_length = 0;
    const Placemat* placement = nullptr;
};
MetalContext context;
/** --------------------------------------------------------------------------------------------------------- Allocate
 * @brief Creates one zero-initialized shared Metal slab.
 */
Placemat::Handle* metal_allocate(size_t size, void*) {
    @autoreleasepool {
        if (size > context.maximum_length) ALLIGATOR_THROW("Metal slab exceeds maxBufferLength");
        auto handle = std::make_unique<Placemat::Handle>();
        id<MTLBuffer> buffer = [context.device newBufferWithLength:size
            options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache];
        if (!buffer) ALLIGATOR_THROW("Allocating Metal shared slab failed");
        handle->substrate_handle = (__bridge_retained void*)buffer;
        return handle.release();
    }
}
/** --------------------------------------------------------------------------------------------------------- Deallocate
 * @brief Releases the slab's retained Metal buffer during arena reclamation.
 */
void metal_deallocate(Placemat::Handle* handle, void*) {
    @autoreleasepool {
        id<MTLBuffer> buffer = (__bridge_transfer id<MTLBuffer>)handle->substrate_handle;
        (void)buffer;
    }
}
/** --------------------------------------------------------------------------------------------------------- Host Pointer
 * @brief Returns a shared Metal buffer's persistent host address.
 */
void* metal_host_pointer(Placemat::Handle* handle) {
    return [(__bridge id<MTLBuffer>)handle->substrate_handle contents];
}
/** --------------------------------------------------------------------------------------------------------- Context
 * @brief Returns the process-lifetime Metal placement context.
 */
void* metal_context() { return &context; }
}
/** --------------------------------------------------------------------------------------------------------- Register Type
 * @brief Registers shared Metal allocation callbacks before arena startup.
 */
const Placemat* MetalAllocator::register_type(void* device) {
    @autoreleasepool {
        if (context.placement) ALLIGATOR_THROW("The Metal placement is already registered");
        if (!device) ALLIGATOR_THROW("Metal registration requires an MTLDevice");
        context.device = (__bridge id<MTLDevice>)device;
        context.maximum_length = context.device.maxBufferLength;
        if (context.maximum_length < 64ull * 1024 * 1024) {
            ALLIGATOR_THROW("The Metal device cannot allocate a 64 MiB slab");
        }
        context.placement = BuffetMenu::get(BuffetMenu::register_type(
            "metal", 64ull * 1024 * 1024, 64, &metal_allocate, &metal_deallocate,
            &metal_host_pointer, &metal_context
        ));
        return context.placement;
    }
}
/** --------------------------------------------------------------------------------------------------------- Buffer
 * @brief Returns the borrowed native buffer for a Slice from this placement.
 */
void* MetalAllocator::buffer(const Slice& slice) {
    return Placemat::get_for(&slice)->substrate_handle;
}
/** --------------------------------------------------------------------------------------------------------- Buffer Offset
 * @brief Resolves a Slice's offset inside its native Metal buffer.
 */
uint64_t MetalAllocator::buffer_offset(const Slice& slice) {
    return static_cast<const char*>(slice.raw()) -
        static_cast<const char*>([(__bridge id<MTLBuffer>)buffer(slice) contents]);
}
/** --------------------------------------------------------------------------------------------------------- Memory Usage
 * @brief Queries native Metal allocation and working-set counters.
 */
DeviceMemoryUsage MetalAllocator::memory_usage() {
    if (!context.placement) ALLIGATOR_THROW("Register the Metal placement before querying its memory");
    return {std::nullopt, std::nullopt, context.device.currentAllocatedSize,
            context.device.recommendedMaxWorkingSetSize};
}
} // namespace buffetalligator
