#pragma once

#include <alligator/buffer/chain_buffer.hpp>
#include <cstdint>
#include <memory>
#include <functional>

namespace alligator {

/**
 * @enum GPUMemoryType
 * @brief Enumeration of GPU memory types
 */
enum class GPUMemoryType : uint32_t {
    DEVICE_LOCAL = 1,      ///< Memory local to the GPU device (fastest)
    HOST_VISIBLE = 2,      ///< Memory visible to the host CPU
    HOST_COHERENT = 4,     ///< Memory that is coherent between host and device
    HOST_CACHED = 8,       ///< Memory that is cached on the host
    UNIFIED = 16           ///< Unified memory accessible by both CPU and GPU
};

/**
 * @enum GPUVendor
 * @brief Enumeration of GPU vendors
 */
enum class GPUVendor : uint32_t {
    UNKNOWN = 0,
    NVIDIA = 1,
    AMD = 2,
    INTEL = 3,
    APPLE = 4,
    QUALCOMM = 5
};

/**
 * @class GPUBuffer
 * @brief Abstract base class for GPU buffers extending ChainBuffer
 * 
 * This class provides a common interface for GPU buffers across different
 * GPU APIs (Vulkan, CUDA, Metal, etc.). It extends ChainBuffer to support
 * chaining and efficient message passing.
 */
class GPUBuffer : public ChainBuffer {
protected:
    GPUMemoryType memory_type_{GPUMemoryType::DEVICE_LOCAL};
    GPUVendor vendor_{GPUVendor::UNKNOWN};
    uint32_t device_id_{0};
    bool is_mapped_{false};
    void* mapped_ptr_{nullptr};

public:
    /**
     * @brief Constructor
     * @param capacity Size of the buffer in bytes
     * @param memory_type Type of GPU memory to allocate
     */
    GPUBuffer(size_t capacity, GPUMemoryType memory_type) 
        : memory_type_(memory_type) {
        buf_size_ = capacity;
    }

    /**
     * @brief Destructor
     */
    virtual ~GPUBuffer() = default;

    /**
     * @brief Get the GPU memory type
     * @return The memory type of this buffer
     */
    GPUMemoryType get_memory_type() const { return memory_type_; }

    /**
     * @brief Get the GPU vendor
     * @return The vendor of the GPU this buffer is allocated on
     */
    GPUVendor get_vendor() const { return vendor_; }

    /**
     * @brief Get the device ID
     * @return The ID of the GPU device this buffer is allocated on
     */
    uint32_t get_device_id() const { return device_id_; }

    /**
     * @brief Map the GPU buffer to host memory
     * @param offset Offset in the buffer to start mapping
     * @param size Size to map (0 for entire buffer)
     * @return Pointer to the mapped memory, or nullptr on failure
     */
    virtual void* map(size_t offset = 0, size_t size = 0) = 0;

    /**
     * @brief Unmap the GPU buffer from host memory
     */
    virtual void unmap() = 0;

    /**
     * @brief Copy data from host memory to GPU buffer
     * @param src Source data pointer
     * @param size Size of data to copy
     * @param offset Offset in GPU buffer
     * @return true on success, false on failure
     */
    virtual bool upload(const void* src, size_t size, size_t offset = 0) = 0;

    /**
     * @brief Copy data from GPU buffer to host memory
     * @param dst Destination data pointer
     * @param size Size of data to copy
     * @param offset Offset in GPU buffer
     * @return true on success, false on failure
     */
    virtual bool download(void* dst, size_t size, size_t offset = 0) = 0;

    /**
     * @brief Copy data from another GPU buffer
     * @param src Source GPU buffer
     * @param size Size of data to copy
     * @param src_offset Offset in source buffer
     * @param dst_offset Offset in destination buffer
     * @return true on success, false on failure
     */
    virtual bool copy_from(GPUBuffer* src, size_t size, size_t src_offset = 0, size_t dst_offset = 0) = 0;

    /**
     * @brief Synchronize GPU operations on this buffer
     * 
     * This ensures all pending GPU operations on this buffer are complete
     * before returning.
     */
    virtual void sync() = 0;

    /**
     * @brief Check if the buffer is currently mapped
     * @return true if mapped, false otherwise
     */
    bool is_mapped() const { return is_mapped_; }

    /**
     * @brief Get the native GPU handle (API-specific)
     * @return Native handle as void* (cast to appropriate type)
     */
    virtual void* get_native_handle() = 0;

    /**
     * @brief ICollectiveBuffer interface implementation
     */
    bool is_local() const override {
        // GPU buffers are only locally accessible if they're host-visible
        return (static_cast<uint32_t>(memory_type_) & static_cast<uint32_t>(GPUMemoryType::HOST_VISIBLE)) != 0;
    }

    bool is_file_backed() const override {
        return false;  // GPU buffers are not file-backed
    }

    bool is_shared() const override {
        // Unified memory can be considered shared
        return memory_type_ == GPUMemoryType::UNIFIED;
    }

    /**
     * @brief Clear the buffer with a specific value
     * @param value Value to clear with (default 0)
     * 
     * Note: This may be implemented as a GPU kernel for efficiency
     */
    virtual void clear(uint8_t value = 0) override = 0;

    /**
     * @brief Get raw data pointer (may require mapping)
     * @return Pointer to buffer data
     */
    uint8_t* data() override {
        if (!is_mapped_ && is_local()) {
            mapped_ptr_ = map();
        }
        return static_cast<uint8_t*>(mapped_ptr_);
    }

    const uint8_t* data() const override {
        return const_cast<GPUBuffer*>(this)->data();
    }

    /**
     * @brief Callback for async GPU operations
     */
    using CompletionCallback = std::function<void(bool success)>;

    /**
     * @brief Async upload with completion callback
     * @param src Source data
     * @param size Size to upload
     * @param offset Buffer offset
     * @param callback Completion callback
     */
    virtual void upload_async(const void* src, size_t size, size_t offset, CompletionCallback callback) {
        // Default implementation: synchronous upload then callback
        bool success = upload(src, size, offset);
        if (callback) callback(success);
    }

    /**
     * @brief Async download with completion callback
     * @param dst Destination buffer
     * @param size Size to download
     * @param offset Buffer offset
     * @param callback Completion callback
     */
    virtual void download_async(void* dst, size_t size, size_t offset, CompletionCallback callback) {
        // Default implementation: synchronous download then callback
        bool success = download(dst, size, offset);
        if (callback) callback(success);
    }
};

} // namespace alligator
