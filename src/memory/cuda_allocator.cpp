/** --------------------------------------------------------------------------------------------------------- CUDA Allocator
 * @file cuda_allocator.cpp
 * @brief Supplies CUDA managed or mapped-host slabs through the arena placement callbacks.
 */
#include <alligator_cuda.hpp>
#include <cstring>
#include <memory>

namespace buffetalligator {
namespace {
/** --------------------------------------------------------------------------------------------------------- CUDA Context
 * @brief Holds the caller-owned context shared by the placement callbacks.
 */
struct CudaContext {
    CUcontext native = nullptr;
    const Placemat* placement = nullptr;
};
CudaContext context;
/** --------------------------------------------------------------------------------------------------------- Check CUDA
 * @brief Reports CUDA failures at allocation and query boundaries.
 */
void check_cuda(CUresult result, const char* operation) {
    if (result == CUDA_SUCCESS) return;
    ALLIGATOR_THROW(std::string(operation) + ": CUresult " + std::to_string(result));
}
/** --------------------------------------------------------------------------------------------------------- Context Scope
 * @brief Activates the registered context while preserving the calling thread's context stack.
 */
struct ContextScope {
    ContextScope() { check_cuda(cuCtxPushCurrent(context.native), "Activating CUDA context"); }
    ~ContextScope() {
        CUcontext previous;
        const CUresult result = cuCtxPopCurrent(&previous);
        if (result != CUDA_SUCCESS) LOG_ERROR_STREAM << "Restoring CUDA context failed: " << result;
    }
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;
};
/** --------------------------------------------------------------------------------------------------------- CUDA Allocation
 * @brief Holds the host and device addresses of one slab.
 */
struct CudaAllocation {
    void* host = nullptr;
    CUdeviceptr device = 0;
};
/** --------------------------------------------------------------------------------------------------------- Allocate Managed
 * @brief Allocates and initializes a managed slab while its context is current.
 */
Placemat::Handle* managed_allocate(size_t size, void*) {
    ContextScope scope;
    auto handle = std::make_unique<Placemat::Handle>();
    auto allocation = std::make_unique<CudaAllocation>();
    check_cuda(cuMemAllocManaged(&allocation->device, size, CU_MEM_ATTACH_GLOBAL),
               "Allocating CUDA managed slab");
    allocation->host = reinterpret_cast<void*>(allocation->device);
    std::memset(allocation->host, 0, size);
    handle->substrate_handle = allocation.release();
    return handle.release();
}
/** --------------------------------------------------------------------------------------------------------- Allocate Mapped
 * @brief Allocates a pinned host slab and resolves its CUDA address once.
 */
Placemat::Handle* mapped_allocate(size_t size, void*) {
    ContextScope scope;
    auto handle = std::make_unique<Placemat::Handle>();
    auto allocation = std::make_unique<CudaAllocation>();
    check_cuda(cuMemHostAlloc(&allocation->host, size,
        CU_MEMHOSTALLOC_DEVICEMAP | CU_MEMHOSTALLOC_PORTABLE), "Allocating CUDA mapped slab");
    const CUresult result = cuMemHostGetDevicePointer(&allocation->device, allocation->host, 0);
    if (result != CUDA_SUCCESS) {
        cuMemFreeHost(allocation->host);
        check_cuda(result, "Resolving CUDA mapped address");
    }
    std::memset(allocation->host, 0, size);
    handle->substrate_handle = allocation.release();
    return handle.release();
}
/** --------------------------------------------------------------------------------------------------------- Free Managed
 * @brief Releases a managed slab on the arena reclamation thread.
 */
void managed_deallocate(Placemat::Handle* handle, void*) {
    ContextScope scope;
    std::unique_ptr<CudaAllocation> allocation(static_cast<CudaAllocation*>(handle->substrate_handle));
    check_cuda(cuMemFree(allocation->device), "Freeing CUDA managed slab");
}
/** --------------------------------------------------------------------------------------------------------- Free Mapped
 * @brief Releases a pinned host slab on the arena reclamation thread.
 */
void mapped_deallocate(Placemat::Handle* handle, void*) {
    ContextScope scope;
    std::unique_ptr<CudaAllocation> allocation(static_cast<CudaAllocation*>(handle->substrate_handle));
    check_cuda(cuMemFreeHost(allocation->host), "Freeing CUDA mapped slab");
}
/** --------------------------------------------------------------------------------------------------------- Host Pointer
 * @brief Returns the host address established during allocation.
 */
void* cuda_host_pointer(Placemat::Handle* handle) {
    return static_cast<CudaAllocation*>(handle->substrate_handle)->host;
}
/** --------------------------------------------------------------------------------------------------------- Context
 * @brief Returns the process-lifetime CUDA placement context.
 */
void* cuda_context() { return &context; }
}
/** --------------------------------------------------------------------------------------------------------- Register Type
 * @brief Probes CUDA memory capabilities and selects concrete placement callbacks once.
 */
const Placemat* CudaAllocator::register_type(CUcontext native, CudaMemoryKind kind) {
    if (context.placement) ALLIGATOR_THROW("The CUDA placement is already registered");
    if (!native) ALLIGATOR_THROW("CUDA registration requires a context");
    context.native = native;
    ContextScope scope;
    CUdevice device;
    check_cuda(cuCtxGetDevice(&device), "Querying CUDA device");
    int supported = 0;
    if (kind == CudaMemoryKind::managed) {
        check_cuda(cuDeviceGetAttribute(&supported, CU_DEVICE_ATTRIBUTE_CONCURRENT_MANAGED_ACCESS,
                                       device), "Querying concurrent managed access");
        if (!supported) {
            ALLIGATOR_THROW("Background managed slabs require concurrent managed access; "
                            "select mapped_host for this device");
        }
    } else if (kind == CudaMemoryKind::mapped_host) {
        check_cuda(cuDeviceGetAttribute(&supported, CU_DEVICE_ATTRIBUTE_CAN_MAP_HOST_MEMORY,
                                       device), "Querying mapped host support");
        if (!supported) ALLIGATOR_THROW("The CUDA device cannot map host memory");
    } else {
        ALLIGATOR_THROW("Unknown CUDA memory kind");
    }
    context.placement = BuffetMenu::get(BuffetMenu::register_type(
        "cuda", 64ull * 1024 * 1024, 64,
        kind == CudaMemoryKind::managed ? &managed_allocate : &mapped_allocate,
        kind == CudaMemoryKind::managed ? &managed_deallocate : &mapped_deallocate,
        &cuda_host_pointer, &cuda_context
    ));
    return context.placement;
}
/** --------------------------------------------------------------------------------------------------------- Device Address
 * @brief Resolves the native device address including a Slice's offset.
 */
CUdeviceptr CudaAllocator::device_address(const Slice& slice) {
    const auto* allocation = static_cast<CudaAllocation*>(Placemat::get_for(&slice)->substrate_handle);
    return allocation->device + (static_cast<const char*>(slice.raw()) -
                                 static_cast<const char*>(allocation->host));
}
/** --------------------------------------------------------------------------------------------------------- Memory Usage
 * @brief Queries capacity and free memory while preserving the caller's current context.
 */
DeviceMemoryUsage CudaAllocator::memory_usage() {
    if (!context.placement) ALLIGATOR_THROW("Register the CUDA placement before querying its memory");
    ContextScope scope;
    size_t available = 0;
    size_t capacity = 0;
    check_cuda(cuMemGetInfo(&available, &capacity), "Querying CUDA memory");
    return {capacity, available, std::nullopt, std::nullopt};
}
} // namespace buffetalligator
