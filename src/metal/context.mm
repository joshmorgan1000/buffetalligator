/** --------------------------------------------------------------------------------------------------------- Metal Context
 * @file context.mm
 * @brief Probes one native Metal device before any arena or pipeline allocation.
 */
#include <metal/context.hpp>
#include <metal/metal.hpp>
#include <memory/lifetime.hpp>
#include <algorithm>
#include <thread>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Constructor
 * @brief Establishes completion-state lifetime before the native context is registered for teardown.
 */
MetalContext::MetalContext() { shader_runtime_initialize(); }
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Detaches committed residency after every arena and prepared program has retired.
 */
MetalContext::~MetalContext() {
    if (@available(macOS 15.0, *)) {
        if (residency) [queue removeResidencySet:residency];
    }
}
/** --------------------------------------------------------------------------------------------------------- Instance
 * @brief Returns context storage without recursively selecting a backend.
 */
MetalContext& metal_context() {
    static RuntimeFinalizer lifetime(new MetalContext, &RuntimeFinalizer::delete_owner<MetalContext>);
    return *static_cast<MetalContext*>(lifetime.object);
}
/** --------------------------------------------------------------------------------------------------------- Initialize
 * @brief Probes the pointer ABI requirements and prepares the queue and supported residency mechanism.
 */
bool MetalContext::initialize(id<MTLDevice> selected) {
    if (!selected) return false;
    if (@available(macOS 13.0, *)) {
        if (![selected supportsFamily:MTLGPUFamilyMetal3]
            || selected.argumentBuffersSupport != MTLArgumentBuffersTier2
            || selected.maxBufferLength < MetalBuffer::default_size()
            || selected.maxThreadsPerThreadgroup.width < 16
            || selected.maxThreadsPerThreadgroup.height < 4) return false;
    } else { return false; }
    const size_t candidate_slots = std::max(1u, std::thread::hardware_concurrency());
    id<MTLCommandQueue> selected_queue =
        [selected newCommandQueueWithMaxCommandBufferCount:candidate_slots];
    if (!selected_queue) ALLIGATOR_GPU_THROW("Metal command queue allocation failed");
    id<MTLResidencySet> selected_residency = nil;
    if (@available(macOS 15.0, *)) {
        if ([selected supportsFamily:MTLGPUFamilyApple6]) {
            MTLResidencySetDescriptor* settings = [MTLResidencySetDescriptor new];
            settings.label = @"alligator indirect allocations";
            NSError* error = nil;
            selected_residency = [selected newResidencySetWithDescriptor:settings error:&error];
            if (!selected_residency) ALLIGATOR_GPU_THROW(std::string("Metal residency set: ")
                + (error ? error.description.UTF8String : "allocation failed"));
            [selected_queue addResidencySet:selected_residency];
        }
    }
    const BuffetDescriptor* placement =
        BuffetDescriptors::descriptor_for(static_cast<MetalBuffer*>(nullptr));
    BuffetDescriptors::register_descriptor(placement);
    device = selected;
    queue = selected_queue;
    residency = selected_residency;
    maximum_length = device.maxBufferLength;
    slots = candidate_slots;
    descriptor = placement;
    if (@available(macOS 13.3, *)) device.shouldMaximizeConcurrentCompilation = YES;
    LOG_INFO_STREAM << "Metal device " << device.name.UTF8String
        << ": max buffer=" << maximum_length << ", prepared slots=" << slots
        << ", shared memory=" << bool(device.hasUnifiedMemory)
        << ", residency sets=" << bool(residency);
    return true;
}
/** --------------------------------------------------------------------------------------------------------- Available
 * @brief Freezes the default native device while honoring prior explicit registration.
 */
bool metal_available() {
    MetalContext& context = metal_context();
    std::lock_guard lock(context.initialization);
    if (!context.probed) {
        @autoreleasepool {
            context.initialize(MTLCreateSystemDefaultDevice());
            context.probed = true;
        }
    }
    return context.descriptor != nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Placement
 * @brief Returns the descriptor registered by the successful device probe.
 */
const BuffetDescriptor* metal_placement() { return metal_context().descriptor; }
/** --------------------------------------------------------------------------------------------------------- Unified Memory
 * @brief Reports the native physical shared-memory capability.
 */
bool metal_unified() { return metal_context().device.hasUnifiedMemory; }
/** --------------------------------------------------------------------------------------------------------- Device Name
 * @brief Returns the selected native device name.
 */
std::string metal_device_name() { return metal_context().device.name.UTF8String; }
} // namespace buffetalligator
