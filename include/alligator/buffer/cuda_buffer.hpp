#pragma once

#include <cstdint>
#include <atomic>
#include <span>
#include <optional>
#include <alligator/buffer/gpu_buffer.hpp>
#if CUDA_ENABLED
#include <cuda_runtime.h>

/**
 * @file cuda_buffer.hpp
 * @brief CUDA GPU buffer implementation with chaining support
 * 
 * This buffer allocates unified memory using CUDA API for zero-copy
 * operations between CPU and GPU. It provides seamless data access
 * from both host and device code without explicit memory transfers.
 */

namespace alligator {

/**
 * @brief CUDA GPU buffer implementation with chaining support
 * 
 * This buffer allocates unified memory using cudaMallocManaged, which
 * provides automatic memory migration between CPU and GPU. It inherits
 * from ChainBuffer to support automatic chaining for efficient message
 * passing with zero-copy GPU access.
 * 
 * Key features:
 * - Unified Memory: Accessible from both CPU and GPU without explicit copies
 * - Automatic migration: CUDA runtime handles data movement as needed
 * - Page-locked memory: Ensures consistent performance
 * - Prefetching support: Can hint to CUDA runtime about upcoming access patterns
 */
class CUDABuffer : public GPUBuffer {
private:
    void* unified_data_{nullptr};           ///< Unified memory pointer
    bool prefetch_to_gpu_{false};           ///< Whether to prefetch to GPU on allocation
    
    // Static members for CUDA initialization
    static std::atomic<bool> cuda_initialized_;  ///< CUDA initialization flag
    static std::atomic<int> device_count_;       ///< Number of CUDA devices

public:
    /**
     * @brief Constructor
     * @param capacity Size of the buffer in bytes (0 uses default)
     * @param device_id CUDA device ID (-1 for default device)
     * @param prefetch_to_gpu If true, prefetch data to GPU after allocation
     */
    CUDABuffer(size_t capacity = 0, int device_id = -1, bool prefetch_to_gpu = false);

    /**
     * @brief Destructor
     */
    ~CUDABuffer() override;

    bool is_local() const override {
        return true;  // Unified memory is locally accessible
    }

    bool is_file_backed() const override {
        return false;  // CUDA buffers are not file-backed
    }

    bool is_shared() const override {
        return false;  // CUDA buffers are not shared memory (though they are unified)
    }

    /**
     * @brief Factory method to create a CUDABuffer
     * @param capacity Size in bytes
     * @return Pointer to the created CUDABuffer
     */
    static CUDABuffer* create(size_t capacity) {
        return new CUDABuffer(capacity);
    }
    
    /**
     * @brief Implement the create_new method from ChainBuffer
     * @param size Size in bytes
     * @return Pointer to a new CUDABuffer allocated via BuffetAlligator
     */
    ChainBuffer* create_new(size_t size) override;

    /**
     * @brief Clear the buffer, setting all bytes to specified value
     * @param value Value to set (uses cudaMemset)
     */
    void clear(uint8_t value) override;

    /**
     * @brief Deallocate the buffer memory
     */
    void deallocate() override;

    /**
     * @brief Get a pointer to the raw data
     * @return Pointer to the buffer data
     */
    uint8_t* data() override;

    /**
     * @brief Get a pointer to the raw data (const version)
     * @return Pointer to the buffer data
     */
    const uint8_t* data() const override;

    /**
     * @brief Get a span view of the buffer data
     * @param offset Offset in bytes from the beginning
     * @param size Size in bytes (0 for remaining)
     * @return Span view of the buffer data
     */
    std::span<uint8_t> get_span(size_t offset = 0, size_t size = 0) override;

    /**
     * @brief Get a span view of the buffer data (const version)
     */
    std::span<const uint8_t> get_span(const size_t& offset = 0, const size_t& size = 0) const override;

    // GPU interface implementation
    void* map(size_t offset = 0, size_t size = 0) override;
    void unmap() override;
    bool upload(const void* src, size_t size, size_t offset = 0) override;
    bool download(void* dst, size_t size, size_t offset = 0) override;
    bool copy_from(GPUBuffer* src, size_t size, size_t src_offset = 0, size_t dst_offset = 0) override;
    void sync() override;
    void* get_native_handle() override;

    /**
     * @brief Prefetch buffer to specific device
     * @param device_id Target device ID (-1 for CPU)
     * @param stream CUDA stream for async prefetch (0 for default stream)
     * @return true on success, false on failure
     */
    bool prefetch_async(int device_id, cudaStream_t stream = 0);


    /**
     * @brief Get memory access advice for specific device
     * @param device_id Device ID to optimize for
     * @param advice CUDA memory advice (e.g., cudaMemAdviseSetPreferredLocation)
     * @return true on success, false on failure
     */
    bool set_access_advice(int device_id, cudaMemoryAdvise advice);


    /**
     * @brief Initialize CUDA runtime (called automatically on first use)
     * @return true if successful, false otherwise
     */
    static bool initialize_cuda();

    /**
     * @brief Get number of available CUDA devices
     * @return Number of CUDA devices or 0 if none/not initialized
     */
    static int get_device_count() {
        return device_count_.load(std::memory_order_acquire);
    }

    /**
     * @brief Check if CUDA is available and initialized
     * @return true if CUDA is ready, false otherwise
     */
    static bool is_cuda_available() {
        return cuda_initialized_.load(std::memory_order_acquire) && 
               device_count_.load(std::memory_order_acquire) > 0;
    }
};

} // namespace alligator
#else // CUDA_ENABLED
// Stub implementation for non-CUDA builds
namespace alligator {
class CUDABuffer : public GPUBuffer {
public:
    CUDABuffer(size_t capacity = 0, int device_id = -1, bool prefetch_to_gpu = false) 
        : GPUBuffer(capacity, GPUMemoryType::DEVICE_LOCAL) {
        throw std::runtime_error("CUDABuffer is not available in this build. "
                                 "Ensure CUDA is enabled and properly configured.");
    }
};
} // namespace alligator
#endif // CUDA_ENABLED
