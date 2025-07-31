#if CUDA_ENABLED
#include <alligator/buffer/cuda_buffer.hpp>
#include <alligator/buffer/buffet_alligator.hpp>
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <vector>

/**
 * @file cuda_buffer.cpp
 * @brief Implementation of CUDA unified memory buffer with chaining support
 */

namespace alligator {

// Default buffer size if not specified (64 KB)
static constexpr size_t DEFAULT_CUDA_BUFFER_SIZE = 65536;

// Static member initialization
std::atomic<bool> CUDABuffer::cuda_initialized_{false};
std::atomic<int> CUDABuffer::device_count_{0};

// Helper function to check CUDA errors
#define CUDA_CHECK(call) do { \
    cudaError_t error = call; \
    if (error != cudaSuccess) { \
        throw std::runtime_error(std::string("CUDA error: ") + cudaGetErrorString(error)); \
    } \
} while(0)

// Helper function for safe CUDA calls that don't throw
#define CUDA_CHECK_NOTHROW(call) do { \
    cudaError_t error = call; \
    if (error != cudaSuccess) { \
    } \
} while(0)

bool CUDABuffer::initialize_cuda() {
    bool expected = false;
    if (!cuda_initialized_.compare_exchange_strong(expected, true, 
                                                   std::memory_order_acquire)) {
        // Already initialized by another thread
        return cuda_initialized_.load(std::memory_order_acquire);
    }
    
    // Get device count
    int count = 0;
    cudaError_t error = cudaGetDeviceCount(&count);
    
    if (error != cudaSuccess) {
        cuda_initialized_.store(false, std::memory_order_release);
        return false;
    }
    
    if (count == 0) {
        cuda_initialized_.store(false, std::memory_order_release);
        return false;
    }
    
    device_count_.store(count, std::memory_order_release);
    
    // Skip device information logging
    
    return true;
}

CUDABuffer::CUDABuffer(size_t capacity, int device_id, bool prefetch_to_gpu) 
    : GPUBuffer(capacity), prefetch_to_gpu_(prefetch_to_gpu) {
    device_id_ = device_id;
    vendor_ = GPUVendor::NVIDIA;
    memory_type_ = GPUMemoryType::UNIFIED;
    
    // Initialize CUDA if needed
    if (!is_cuda_available()) {
        if (!initialize_cuda()) {
            throw std::runtime_error("Failed to initialize CUDA");
        }
    }
    
    // Use default size if not specified
    if (capacity == 0) {
        capacity = DEFAULT_CUDA_BUFFER_SIZE;
    }
    
    buf_size_ = capacity;
    
    // Set device if specified
    if (device_id_ >= 0) {
        CUDA_CHECK(cudaSetDevice(device_id_));
    } else {
        // Get current device
        CUDA_CHECK(cudaGetDevice(&device_id_));
    }
    
    // Allocate unified memory
    CUDA_CHECK(cudaMallocManaged(&unified_data_, capacity));
    
    if (!unified_data_) {
        throw std::runtime_error("Failed to allocate CUDA unified memory");
    }
    
    // Clear to zero
    clear(0);
    
    // Prefetch to GPU if requested
    if (prefetch_to_gpu_) {
        prefetch_async(device_id_, 0);
    }
}

CUDABuffer::~CUDABuffer() {
    if (unified_data_) {
        // Synchronize to ensure all operations are complete
        CUDA_CHECK_NOTHROW(cudaDeviceSynchronize());
        
        // Free the unified memory
        CUDA_CHECK_NOTHROW(cudaFree(unified_data_));
    }
}

ChainBuffer* CUDABuffer::create_new(size_t size) {
    // Allocate through BuffetAlligator to ensure proper registration
    return get_buffet_alligator().allocate_buffer(
        static_cast<uint32_t>(size), 
        BFType::CUDA
    );
}

void CUDABuffer::clear(uint8_t value) {
    if (!unified_data_) {
        return;
    }
    
    // Use cudaMemset for efficiency
    CUDA_CHECK_NOTHROW(cudaMemset(unified_data_, value, buf_size_));
}

void CUDABuffer::deallocate() {
    if (unified_data_) {
        // Synchronize before deallocation
        CUDA_CHECK_NOTHROW(cudaDeviceSynchronize());
        
        // Free the memory
        CUDA_CHECK_NOTHROW(cudaFree(unified_data_));
        unified_data_ = nullptr;
    }
}

uint8_t* CUDABuffer::data() {
    return static_cast<uint8_t*>(unified_data_);
}

const uint8_t* CUDABuffer::data() const {
    return static_cast<const uint8_t*>(unified_data_);
}

std::span<uint8_t> CUDABuffer::get_span(size_t offset, size_t size) {
    if (!unified_data_) {
        return std::span<uint8_t>();
    }
    
    if (offset > buf_size_) {
        return std::span<uint8_t>();
    }
    
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : size;
    
    if (offset + actual_size > buf_size_) {
        actual_size = buf_size_ - offset;
    }
    
    return std::span<uint8_t>(static_cast<uint8_t*>(unified_data_) + offset, actual_size);
}

std::span<const uint8_t> CUDABuffer::get_span(const size_t& offset, const size_t& size) const {
    if (!unified_data_) {
        return std::span<const uint8_t>();
    }
    
    if (offset > buf_size_) {
        return std::span<const uint8_t>();
    }
    
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : size;
    
    if (offset + actual_size > buf_size_) {
        actual_size = buf_size_ - offset;
    }
    
    return std::span<const uint8_t>(static_cast<const uint8_t*>(unified_data_) + offset, actual_size);
}

bool CUDABuffer::prefetch_async(int device_id, cudaStream_t stream) {
    if (!unified_data_) {
        return false;
    }
    
    // device_id of -1 means CPU
    int target_device = (device_id == -1) ? cudaCpuDeviceId : device_id;
    
    cudaError_t error = cudaMemPrefetchAsync(unified_data_, buf_size_, target_device, stream);
    if (error != cudaSuccess) {
        return false;
    }
    
    return true;
}

void CUDABuffer::sync() {
    CUDA_CHECK_NOTHROW(cudaDeviceSynchronize());
}

bool CUDABuffer::set_access_advice(int device_id, cudaMemoryAdvise advice) {
    if (!unified_data_) {
        return false;
    }
    
    cudaError_t error = cudaMemAdvise(unified_data_, buf_size_, advice, device_id);
    if (error != cudaSuccess) {
        return false;
    }
    
    return true;
}

// GPU interface implementation
void* CUDABuffer::map(size_t offset, size_t size) {
    if (!unified_data_ || is_mapped_) {
        return nullptr;
    }
    
    // CUDA unified memory is already accessible from CPU
    is_mapped_ = true;
    mapped_ptr_ = static_cast<uint8_t*>(unified_data_) + offset;
    return mapped_ptr_;
}

void CUDABuffer::unmap() {
    is_mapped_ = false;
    mapped_ptr_ = nullptr;
}

bool CUDABuffer::upload(const void* src, size_t size, size_t offset) {
    if (!unified_data_ || !src || offset + size > buf_size_) {
        return false;
    }
    
    // For unified memory, just copy
    std::memcpy(static_cast<uint8_t*>(unified_data_) + offset, src, size);
    
    // Optionally prefetch to GPU
    if (prefetch_to_gpu_) {
        prefetch_async(device_id_, 0);
    }
    
    return true;
}

bool CUDABuffer::download(void* dst, size_t size, size_t offset) {
    if (!unified_data_ || !dst || offset + size > buf_size_) {
        return false;
    }
    
    // Ensure GPU operations are complete
    sync();
    
    // Copy from unified memory
    std::memcpy(dst, static_cast<uint8_t*>(unified_data_) + offset, size);
    return true;
}

bool CUDABuffer::copy_from(GPUBuffer* src, size_t size, size_t src_offset, size_t dst_offset) {
    if (!src || !unified_data_) {
        return false;
    }
    
    // Check if source is also a CUDA buffer for optimized copy
    CUDABuffer* cuda_src = dynamic_cast<CUDABuffer*>(src);
    if (cuda_src && cuda_src->unified_data_) {
        // Device-to-device copy
        cudaError_t error = cudaMemcpy(
            static_cast<uint8_t*>(unified_data_) + dst_offset,
            static_cast<uint8_t*>(cuda_src->unified_data_) + src_offset,
            size,
            cudaMemcpyDeviceToDevice
        );
        return error == cudaSuccess;
    }
    
    // Fall back to download/upload for other GPU buffer types
    std::vector<uint8_t> temp(size);
    if (!src->download(temp.data(), size, src_offset)) {
        return false;
    }
    return upload(temp.data(), size, dst_offset);
}

void* CUDABuffer::get_native_handle() {
    return unified_data_;
}

} // namespace alligator
#endif // CUDA_ENABLED
