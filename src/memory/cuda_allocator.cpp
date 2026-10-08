/** --------------------------------------------------------------------------------------------------------- CUDA Allocator
 * @file cuda_allocator.cpp
 * @brief Allocates CudaBuffer storage as managed or mapped pinned host memory on the registered context.
 */
#include <alligator.hpp>
#if defined(BUFFETALLIGATOR_HAS_CUDA)
#include <alligator/easycuda.hpp>
#include <cuda.h>
#include <cstring>
#include <string>

namespace buffetalligator {
namespace {
/// @brief The caller-owned context every CudaBuffer allocates on, set once by register_type.
CUcontext registered_context = nullptr;
/// @brief The CUDA descriptor, set once by register_type.
const BuffetDescriptor* registered_descriptor = nullptr;
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
    ContextScope() { check_cuda(cuCtxPushCurrent(registered_context), "Activating CUDA context"); }
    ~ContextScope() {
        CUcontext previous;
        const CUresult result = cuCtxPopCurrent(&previous);
        if (result != CUDA_SUCCESS) LOG_ERROR_STREAM << "Restoring CUDA context failed: " << result;
    }
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;
};
}
/** --------------------------------------------------------------------------------------------------------- Allocate Managed
 * @brief Allocates and zeroes a managed buffer while the registered context is current.
 * @param buffer The buffer receiving the allocation.
 */
void CudaBuffer::allocate_managed(CudaBuffer& buffer) {
    ContextScope scope;
    check_cuda(cuMemAllocManaged(&buffer.device_, buffer.size_, CU_MEM_ATTACH_GLOBAL),
               "Allocating CUDA managed buffer");
    buffer.host_ = reinterpret_cast<void*>(buffer.device_);
    std::memset(buffer.host_, 0, buffer.size_);
}
/** --------------------------------------------------------------------------------------------------------- Allocate Mapped
 * @brief Allocates a zeroed pinned host buffer and resolves its CUDA address once.
 * @param buffer The buffer receiving the allocation.
 */
void CudaBuffer::allocate_mapped(CudaBuffer& buffer) {
    ContextScope scope;
    check_cuda(cuMemHostAlloc(&buffer.host_, buffer.size_,
        CU_MEMHOSTALLOC_DEVICEMAP | CU_MEMHOSTALLOC_PORTABLE), "Allocating CUDA mapped buffer");
    const CUresult result = cuMemHostGetDevicePointer(&buffer.device_, buffer.host_, 0);
    if (result != CUDA_SUCCESS) {
        cuMemFreeHost(buffer.host_);
        buffer.host_ = nullptr;
        check_cuda(result, "Resolving CUDA mapped address");
    }
    std::memset(buffer.host_, 0, buffer.size_);
}
/** --------------------------------------------------------------------------------------------------------- Release Managed
 * @brief Frees a managed buffer on the registered context.
 * @param buffer The buffer being released.
 */
void CudaBuffer::release_managed(CudaBuffer& buffer) {
    ContextScope scope;
    check_cuda(cuMemFree(buffer.device_), "Freeing CUDA managed buffer");
}
/** --------------------------------------------------------------------------------------------------------- Release Mapped
 * @brief Frees a pinned host buffer on the registered context.
 * @param buffer The buffer being released.
 */
void CudaBuffer::release_mapped(CudaBuffer& buffer) {
    ContextScope scope;
    check_cuda(cuMemFreeHost(buffer.host_), "Freeing CUDA mapped buffer");
}
/** --------------------------------------------------------------------------------------------------------- Register Type
 * @brief Probes the requested memory kind, binds its hooks, and registers the CUDA descriptor once.
 * @param context A caller-owned CUDA context that outlives every CudaBuffer.
 * @param kind Managed memory, or pinned host memory mapped into the device.
 * @return The registered descriptor.
 */
const BuffetDescriptor* CudaBuffer::register_type(CUcontext context, CudaMemoryKind kind) {
    if (registered_descriptor) ALLIGATOR_THROW("The CUDA buffet type is already registered");
    if (!context) ALLIGATOR_THROW("CUDA registration requires a context");
    registered_context = context;
    ContextScope scope;
    CUdevice device;
    check_cuda(cuCtxGetDevice(&device), "Querying CUDA device");
    int supported = 0;
    if (kind == CudaMemoryKind::managed) {
        check_cuda(cuDeviceGetAttribute(&supported, CU_DEVICE_ATTRIBUTE_CONCURRENT_MANAGED_ACCESS,
                                       device), "Querying concurrent managed access");
        if (!supported) {
            ALLIGATOR_THROW("Managed CUDA buffers require concurrent managed access; "
                            "select CudaMemoryKind::mapped_host for this device");
        }
        allocate_ = &CudaBuffer::allocate_managed;
        release_ = &CudaBuffer::release_managed;
    } else {
        check_cuda(cuDeviceGetAttribute(&supported, CU_DEVICE_ATTRIBUTE_CAN_MAP_HOST_MEMORY,
                                       device), "Querying mapped host support");
        if (!supported) ALLIGATOR_THROW("The CUDA device cannot map host memory");
        allocate_ = &CudaBuffer::allocate_mapped;
        release_ = &CudaBuffer::release_mapped;
    }
    const BuffetDescriptor* descriptor = BuffetDescriptors::descriptor_for(static_cast<CudaBuffer*>(nullptr));
    if (BuffetDescriptors::register_descriptor(descriptor) != type_idx()) {
        ALLIGATOR_THROW("CudaBuffer must be the first registered buffet type after the built-ins; "
                        "register it before any other custom type");
    }
    registered_descriptor = descriptor;
    return registered_descriptor;
}
/** --------------------------------------------------------------------------------------------------------- Memory Usage
 * @brief Queries capacity and free memory while preserving the caller's current context.
 * @return The device's memory usage.
 */
DeviceMemoryUsage CudaBuffer::memory_usage() {
    if (!registered_descriptor) ALLIGATOR_THROW("Register the CUDA buffet type before querying its memory");
    ContextScope scope;
    size_t available = 0;
    size_t capacity = 0;
    check_cuda(cuMemGetInfo(&available, &capacity), "Querying CUDA memory");
    return {capacity, available, std::nullopt, std::nullopt};
}
/** --------------------------------------------------------------------------------------------------------- Registered Device
 * @brief Resolves the device of the registered context, the handle every attribute query takes.
 */
static CUdevice registered_device() {
    ContextScope scope;
    CUdevice device;
    check_cuda(cuCtxGetDevice(&device), "Querying CUDA device");
    return device;
}
/** --------------------------------------------------------------------------------------------------------- Thread Limit X
 * @brief Queries the maximum number of threads per block along the X dimension.
 */
uint32_t CudaBuffer::thread_limit_x() {
    int value = 0;
    check_cuda(cuDeviceGetAttribute(&value, CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_X, registered_device()), "Querying thread limit X");
    return static_cast<uint32_t>(value);
}
/** --------------------------------------------------------------------------------------------------------- Thread Limit Y
 * @brief Queries the maximum number of threads per block along the Y dimension.
 */
uint32_t CudaBuffer::thread_limit_y() {
    int value = 0;
    check_cuda(cuDeviceGetAttribute(&value, CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Y, registered_device()), "Querying thread limit Y");
    return static_cast<uint32_t>(value);
}
/** --------------------------------------------------------------------------------------------------------- Thread Limit Z
 * @brief Queries the maximum number of threads per block along the Z dimension.
 */
uint32_t CudaBuffer::thread_limit_z() {
    int value = 0;
    check_cuda(cuDeviceGetAttribute(&value, CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Z, registered_device()), "Querying thread limit Z");
    return static_cast<uint32_t>(value);
}
/** --------------------------------------------------------------------------------------------------------- Workgroup Limit X
 * @brief Queries the maximum number of blocks per grid along the X dimension.
 */
uint32_t CudaBuffer::workgroup_limit_x() {
    int value = 0;
    check_cuda(cuDeviceGetAttribute(&value, CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_X, registered_device()), "Querying workgroup limit X");
    return static_cast<uint32_t>(value);
}
/** --------------------------------------------------------------------------------------------------------- Workgroup Limit Y
 * @brief Queries the maximum number of blocks per grid along the Y dimension.
 */
uint32_t CudaBuffer::workgroup_limit_y() {
    int value = 0;
    check_cuda(cuDeviceGetAttribute(&value, CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_Y, registered_device()), "Querying workgroup limit Y");
    return static_cast<uint32_t>(value);
}
/** --------------------------------------------------------------------------------------------------------- Workgroup Limit Z
 * @brief Queries the maximum number of blocks per grid along the Z dimension.
 */
uint32_t CudaBuffer::workgroup_limit_z() {
    int value = 0;
    check_cuda(cuDeviceGetAttribute(&value, CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_Z, registered_device()), "Querying workgroup limit Z");
    return static_cast<uint32_t>(value);
}
} // namespace buffetalligator
#endif // defined(BUFFETALLIGATOR_HAS_CUDA)
