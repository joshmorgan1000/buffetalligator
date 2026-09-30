#pragma once
/** --------------------------------------------------------------------------------------------------------- EasyCUDA
 * @file easycuda.hpp
 * @brief Header file for the EasyCUDA utility functions and classes.
 */
#if defined(BUFFETALLIGATOR_HAS_CUDA)
#if defined(BUFFETALLIGATOR_HAS_METAL)
#error "CUDA and Metal share buffet type index 2; a build carries one or the other"
#endif
#include <alligator.hpp>
#include <alligator/easygpu.hpp>
#include <cuda.h>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- CUDA Memory Kind
 * @brief Selects managed memory or explicitly mapped pinned host memory.
 */
enum class CudaMemoryKind { managed, mapped_host };
/** --------------------------------------------------------------------------------------------------------- CudaBuffer
 * @class CudaBuffer
 * @brief One host-accessible CUDA allocation on the registered context; the CUDA buffet type.
 */
class alignas(16) CudaBuffer {
private:
    void* host_ = nullptr;      ///< Host address of the allocation.
    CUdeviceptr device_ = 0;    ///< CUDA address of the allocation.
    size_t size_ = 0;           ///< Size in bytes.
    /// @brief Allocation hook bound by register_type for the probed memory kind.
    inline static void (*allocate_)(CudaBuffer& buffer) = nullptr;
    /// @brief Release hook bound by register_type for the probed memory kind.
    inline static void (*release_)(CudaBuffer& buffer) = nullptr;
    static void allocate_managed(CudaBuffer& buffer);
    static void allocate_mapped(CudaBuffer& buffer);
    static void release_managed(CudaBuffer& buffer);
    static void release_mapped(CudaBuffer& buffer);
public:
    CudaBuffer() = default;
    /** ------------------------------------------------------------------------------------------- Allocating Constructor
     * @brief Allocates and zeroes one buffer on the registered context.
     * @param size Buffer size in bytes, rounded up to 64.
     */
    explicit CudaBuffer(size_t size) : size_((size + 63) & ~size_t{63}) { allocate_(*this); }
    /** ------------------------------------------------------------------------------------------- Move-only ownership */
    CudaBuffer(const CudaBuffer&) = delete;
    CudaBuffer& operator=(const CudaBuffer&) = delete;
    CudaBuffer(CudaBuffer&& other) noexcept
    : host_(std::exchange(other.host_, nullptr)), device_(std::exchange(other.device_, 0))
    , size_(std::exchange(other.size_, 0)) {}
    CudaBuffer& operator=(CudaBuffer&& other) noexcept {
        std::swap(host_, other.host_); std::swap(device_, other.device_); std::swap(size_, other.size_);
        return *this;
    }
    ~CudaBuffer() { if (host_) release_(*this); }
    operator bool() const { return host_ != nullptr; }
    bool operator==(std::nullptr_t) const { return host_ == nullptr; }
    bool operator!=(std::nullptr_t) const { return host_ != nullptr; }
    /** ------------------------------------------------------------------------------------------- Buffet Hooks
     * @brief The static hooks BuffetDescriptors::descriptor_for binds for this type.
     */
    static void deleter_impl(void* ptr) { delete static_cast<CudaBuffer*>(ptr); }
    static void (*deleter())(void*) { return deleter_impl; }
    static void* host_ptr_impl(void* ptr, size_t offset) {
        return static_cast<uint8_t*>(static_cast<CudaBuffer*>(ptr)->host_) + offset;
    }
    static void* host_ptr(void* ptr, size_t offset) { return host_ptr_impl(ptr, offset); }
    static void* (*host_ptr())(void*, size_t) { return host_ptr_impl; }
    static size_t size_of_impl(void* ptr) { return static_cast<CudaBuffer*>(ptr)->size_; }
    static size_t size_of(void* ptr) { return size_of_impl(ptr); }
    static size_t (*size_of())(void*) { return size_of_impl; }
    static void* factory(size_t size) { return new CudaBuffer(size); }
    static size_t default_size() { return 64 * 1024 * 1024; }
    static size_t type_idx() { return 2; }
    static const char* type_name() { return "CudaBuffer"; }
    static uint64_t device_address(void* ptr) { return static_cast<CudaBuffer*>(ptr)->device_; }
    size_t size() const { return size_; }
    void* raw() { return host_; }
    const void* raw() const { return host_; }
    /** ------------------------------------------------------------------------------------------- Register Type
     * @brief Probes the context's device for the requested memory kind, binds the matching
     * allocation hooks, and registers the CUDA buffet descriptor at type index 2.
     * @param context A caller-owned CUDA context that outlives every CudaBuffer.
     * @param kind Managed memory, or pinned host memory mapped into the device.
     * @return The registered descriptor.
     */
    static const BuffetDescriptor* register_type(
        CUcontext context,
        CudaMemoryKind kind = CudaMemoryKind::managed
    );
    /** ------------------------------------------------------------------------------------------- Memory Usage
     * @brief Reports device capacity and free bytes from cuMemGetInfo.
     * @return The device's memory usage.
     */
    static DeviceMemoryUsage memory_usage();
};
static_assert(IsABuffetType<CudaBuffer>, "CudaBuffer must satisfy IsABuffetType");
} // namespace buffetalligator
#endif // defined(BUFFETALLIGATOR_HAS_CUDA)