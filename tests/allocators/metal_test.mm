/** --------------------------------------------------------------------------------------------------------- Metal Tests
 * @file metal_test.mm
 * @brief Verifies native Metal access to shared Slice storage and device usage reporting.
 */
#include <alligator_allocators.hpp>
#include "../functional_support.hpp"
#include <Metal/Metal.h>

using namespace buffetalligator;
using functional::require;
int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            LOG_INFO_STREAM << "Metal allocator test skipped: no Metal device";
            return 77;
        }
        const Placemat* placement = MetalAllocator::register_type((__bridge void*)device);
        Slice parent(8192, true, placement);
        Slice view = parent.slice(256, 4096);
        require(view.data<unsigned char>()[0] == 0, "Metal slab is not zero initialized");
        require(MetalAllocator::buffer_offset(view) == 256, "Metal subslice offset is wrong");
        id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)MetalAllocator::buffer(view);
        require(buffer.device == device, "Metal allocator used another device");
        require(static_cast<char*>(buffer.contents) + 256 == view.raw(), "Metal view was copied");
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
        [encoder fillBuffer:buffer range:NSMakeRange(256, 4096) value:0x6d];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        require(command.status == MTLCommandBufferStatusCompleted, "Metal GPU fill failed");
        require(view.data<unsigned char>()[0] == 0x6d && view.data<unsigned char>()[4095] == 0x6d,
                "GPU writes were not visible through the Slice");
        require(parent.data<unsigned char>()[0] == 0, "GPU fill escaped the subslice");
        parent = Slice();
        require(view.data<unsigned char>()[2] == 0x6d, "Metal parent release invalidated a view");
        const auto usage = MetalAllocator::memory_usage();
        require(usage.process_bytes.value() >= 8192 && usage.budget_bytes.value() > 0,
                "Metal usage query is incomplete");
        require(!usage.available_bytes && !usage.capacity_bytes, "Metal query invented free memory");
        LOG_INFO_STREAM << "Metal allocator passed on " << device.name.UTF8String;
    }
}
