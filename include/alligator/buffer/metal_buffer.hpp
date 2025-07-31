#pragma once

#include <cstdint>
#include <atomic>
#include <span>
#include <optional>
#include <alligator/buffer/gpu_buffer.hpp>
#include <cswift/cswift.hpp>
/**
 * @file metal_buffer.hpp
 * @brief Metal GPU buffer implementation with chaining support
 * 
 * This buffer allocates shared memory using Apple's Metal framework
 * for zero-copy operations between CPU and GPU on Apple Silicon.
 * It provides optimal performance for ML/AI workloads on macOS/iOS.
 */
namespace alligator {

#ifdef METAL_ENABLED

/**
 * @brief Metal GPU buffer implementation with chaining support
 * 
 * This buffer uses Metal's shared storage mode on Apple Silicon (M1/M2/M3)
 * to provide unified memory that's accessible from both CPU and GPU without
 * copying. It integrates with the ChainBuffer architecture for automatic
 * slab management and donation.
 * 
 * Key features:
 * - Zero-copy CPU/GPU access on Apple Silicon
 * - Automatic chaining for large allocations
 * - Integration with Metal Performance Shaders (MPS)
 * - Support for GPU-only private storage mode
 */
class MetalBuffer : public GPUBuffer {
private:
    std::unique_ptr<cswift::CSMTLBuffer> metal_buffer_;  ///< cswift Metal buffer
    void* mapped_data_{nullptr};                         ///< CPU-accessible pointer
    cswift::CSMTLStorageMode storage_mode_;              ///< Storage mode
    
    // Static members for Metal initialization
    static std::unique_ptr<cswift::CSMTLDevice> default_device_;        ///< Default Metal device
    static std::unique_ptr<cswift::CSMTLCommandQueue> default_queue_;   ///< Default command queue
    static std::atomic<bool> metal_initialized_;                        ///< Metal initialization flag
    
    /**
     * @brief Initialize Metal resources if needed
     */
    static void ensure_metal_initialized();

public:
    /**
     * @brief Constructor
     * @param capacity Size of the buffer in bytes (0 uses default)
     * @param device Metal device to use (nullptr for default)
     * @param storage_mode Storage mode (0=auto-detect, 1=shared, 2=managed, 3=private)
     */
    MetalBuffer(size_t capacity = 0, 
                cswift::CSMTLDevice* device = nullptr,
                int storage_mode = 0);

    /**
     * @brief Destructor
     */
    ~MetalBuffer() override;

    bool is_local() const override {
        return storage_mode_ != cswift::CSMTLStorageMode::Private;  // Private storage is GPU-only
    }

    bool is_file_backed() const override {
        return false;  // Metal buffers are not file-backed
    }

    bool is_shared() const override {
        return storage_mode_ == cswift::CSMTLStorageMode::Shared;  // True unified memory on Apple Silicon
    }

    /**
     * @brief Factory method to create a MetalBuffer
     * @param capacity Size in bytes
     * @return Pointer to the created MetalBuffer
     */
    static MetalBuffer* create(size_t capacity) {
        return new MetalBuffer(capacity);
    }
    
    /**
     * @brief Implement the create_new method from ChainBuffer
     * @param size Size in bytes
     * @return Pointer to a new MetalBuffer allocated via BuffetAlligator
     */
    ChainBuffer* create_new(size_t size) override;

    /**
     * @brief Clear the buffer, setting all bytes to specified value
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
     * @brief Read data from buffer
     */
    void read(size_t offset, void* dst, size_t length) const;

    /**
     * @brief Write data to buffer
     */
    void write(size_t offset, const void* src, size_t length);
    
    /**
     * @brief Get a span view of the buffer data
     */
    std::span<uint8_t> get_span(size_t offset, size_t size) override {
        if (offset + size > buf_size_) {
            throw std::out_of_range("MetalBuffer span out of bounds");
        }
        return std::span<uint8_t>(data() + offset, size);
    }
    
    /**
     * @brief Get a const span view of the buffer data
     */
    std::span<const uint8_t> get_span(const size_t& offset, const size_t& size) const override {
        if (offset + size > buf_size_) {
            throw std::out_of_range("MetalBuffer span out of bounds");
        }
        return std::span<const uint8_t>(data() + offset, size);
    }

    /**
     * @brief Get the underlying Metal buffer handle
     * @return cswift Metal buffer
     */
    cswift::CSMTLBuffer* get_metal_buffer() { 
        return metal_buffer_.get(); 
    }

    /**
     * @brief Get the underlying Metal buffer handle (const version)
     * @return cswift Metal buffer
     */
    const cswift::CSMTLBuffer* get_metal_buffer() const { 
        return metal_buffer_.get(); 
    }

    /**
     * @brief Get the default Metal device
     * @return Default Metal device
     */
    static cswift::CSMTLDevice* get_default_device() {
        ensure_metal_initialized();
        return default_device_.get();
    }

    /**
     * @brief Get the default command queue
     * @return Default command queue
     */
    static cswift::CSMTLCommandQueue* get_default_queue() {
        ensure_metal_initialized();
        return default_queue_.get();
    }

    /**
     * @brief Synchronize GPU operations
     * @details Forces GPU to complete all pending operations
     */
    void sync_gpu();

    /**
     * @brief Copy data from another Metal buffer
     * @param src Source Metal buffer
     * @param src_offset Offset in source buffer
     * @param dst_offset Offset in destination buffer
     * @param length Number of bytes to copy
     */
    void copy_from_metal(const MetalBuffer& src, 
                        size_t src_offset, 
                        size_t dst_offset, 
                        size_t length);

    /**
     * @brief Enqueue GPU compute operation on this buffer
     * @param compute_pipeline Metal compute pipeline state to execute
     * @param threads_per_grid Total number of threads
     * @param threads_per_threadgroup Number of threads per threadgroup
     */
    void enqueue_compute(CSHandle compute_pipeline,
                        size_t threads_per_grid,
                        size_t threads_per_threadgroup);

    // GPU Buffer interface implementation
    void* map(size_t offset = 0, size_t size = 0) override;
    void unmap() override;
    bool upload(const void* src, size_t size, size_t offset = 0) override;
    bool download(void* dst, size_t size, size_t offset = 0) override;
    bool copy_from(GPUBuffer* src, size_t size, size_t src_offset = 0, size_t dst_offset = 0) override;
    void sync() override;
    void* get_native_handle() override;
};

#else // !METAL_ENABLED

// Stub implementation for non-Apple platforms
class MetalBuffer : public GPUBuffer {
public:
    MetalBuffer(size_t capacity = 0, void* = nullptr, int = 0) 
        : GPUBuffer(capacity, GPUMemoryType::DEVICE_LOCAL) {
        throw std::runtime_error("Metal support is not enabled in this build");
    }
    
    ~MetalBuffer() override = default;
    
    bool is_local() const override { return false; }
    bool is_file_backed() const override { return false; }
    bool is_shared() const override { return false; }
    
    static MetalBuffer* create(size_t) { return nullptr; }
    ChainBuffer* create_new(size_t) override { return nullptr; }
    void clear(uint8_t) override {}
    void deallocate() override {}
    uint8_t* data() override { return nullptr; }
    const uint8_t* data() const override { return nullptr; }
    void read(size_t, void*, size_t) const {}
    void write(size_t, const void*, size_t) {}
    std::span<uint8_t> get_span(size_t, size_t) override { return {}; }
    std::span<const uint8_t> get_span(const size_t&, const size_t&) const override { return {}; }
};

#endif // METAL_ENABLED

} // namespace alligator
