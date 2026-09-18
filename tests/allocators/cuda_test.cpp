/** --------------------------------------------------------------------------------------------------------- CUDA Tests
 * @file cuda_test.cpp
 * @brief Verifies native CUDA access to host-visible Slice storage on a process-lifetime context.
 */
#include <alligator_cuda.hpp>
#include "../functional_support.hpp"
#include <string_view>

using namespace buffetalligator;
using functional::require;
/** --------------------------------------------------------------------------------------------------------- Check
 * @brief Stops the functional test when a CUDA operation fails.
 */
void check(CUresult result) { require(result == CUDA_SUCCESS, "CUDA API operation failed"); }
int main(int count, char** arguments) {
    if (count != 2) return 2;
    const CUresult initialized = cuInit(0);
    if (initialized == CUDA_ERROR_NO_DEVICE) {
        LOG_INFO_STREAM << "CUDA allocator test skipped: no CUDA device";
        return 77;
    }
    check(initialized);
    int devices = 0;
    check(cuDeviceGetCount(&devices));
    if (!devices) {
        LOG_INFO_STREAM << "CUDA allocator test skipped: no CUDA device";
        return 77;
    }
    CUdevice device;
    check(cuDeviceGet(&device, 0));
    const bool managed = std::string_view(arguments[1]) == "managed";
    int supported = 0;
    check(cuDeviceGetAttribute(&supported, managed ? CU_DEVICE_ATTRIBUTE_CONCURRENT_MANAGED_ACCESS :
                               CU_DEVICE_ATTRIBUTE_CAN_MAP_HOST_MEMORY, device));
    if (!supported) {
        LOG_INFO_STREAM << "CUDA allocator test skipped: requested memory kind is unsupported";
        return 77;
    }
    CUcontext context;
    check(cuDevicePrimaryCtxRetain(&context, device));
    check(cuCtxSetCurrent(context));
    const Placemat* placement = CudaAllocator::register_type(context,
        managed ? CudaMemoryKind::managed : CudaMemoryKind::mapped_host);
    CUcontext current;
    check(cuCtxGetCurrent(&current));
    require(current == context, "registration changed the caller's CUDA context");
    Slice parent(8192, true, placement);
    Slice view = parent.slice(256, 4096);
    require(view.data<unsigned char>()[0] == 0, "CUDA slab is not zero initialized");
    require(CudaAllocator::device_address(view) == CudaAllocator::device_address(parent) + 256,
            "CUDA subslice address is wrong");
    check(cuMemsetD8(CudaAllocator::device_address(view), 0x6d, view.size_bytes()));
    check(cuCtxSynchronize());
    require(view.data<unsigned char>()[0] == 0x6d && view.data<unsigned char>()[4095] == 0x6d,
            "CUDA writes missed the Slice mapping");
    require(parent.data<unsigned char>()[0] == 0, "CUDA fill escaped its range");
    parent = Slice();
    require(view.data<unsigned char>()[1] == 0x6d, "parent release invalidated a CUDA view");
    const auto usage = CudaAllocator::memory_usage();
    require(usage.capacity_bytes.value() > 0 &&
            usage.available_bytes.value() <= usage.capacity_bytes.value(), "CUDA memory query is invalid");
    require(!usage.process_bytes, "CUDA query invented process usage");
    LOG_INFO_STREAM << "CUDA allocator passed for " << arguments[1];
}
