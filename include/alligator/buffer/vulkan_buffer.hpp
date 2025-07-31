#pragma once

#include <cstdint>
#include <atomic>
#include <span>
#include <optional>
#include <alligator/buffer/gpu_buffer.hpp>
#include <vulkan/vulkan.h>

/**
 * @file vulkan_buffer.hpp
 * @brief Vulkan GPU buffer implementation with chaining support
 * 
 * This buffer allocates HOST_VISIBLE | HOST_COHERENT memory using Vulkan API
 * for zero-copy operations between CPU and GPU. It automatically finds and
 * initializes a suitable Vulkan device if one is not provided.
 */

namespace alligator {

/**
 * @brief Vulkan GPU buffer implementation with chaining support
 * 
 * This buffer allocates HOST_VISIBLE | HOST_COHERENT memory using Vulkan API.
 * It inherits from GPUBuffer (which extends ChainBuffer) to support automatic 
 * chaining for efficient message passing with zero-copy GPU access. The buffer 
 * automatically finds and initializes a Vulkan device on first use.
 */
class VulkanBuffer : public GPUBuffer {
private:
    VkDevice device_{VK_NULL_HANDLE};              ///< Vulkan logical device
    VkBuffer buffer_{VK_NULL_HANDLE};              ///< Vulkan buffer handle
    VkDeviceMemory device_memory_{VK_NULL_HANDLE}; ///< Vulkan device memory
    void* mapped_data_{nullptr};                   ///< Persistently mapped memory pointer
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE}; ///< Physical device for memory properties
    VkMemoryPropertyFlags memory_properties_{0};   ///< Memory property flags
    
    // Static members for shared Vulkan context
    static VkInstance vulkan_instance_;            ///< Vulkan instance
    static VkDevice default_device_;                ///< Default Vulkan device
    static VkPhysicalDevice default_physical_device_; ///< Default physical device
    static std::atomic<bool> vulkan_initialized_;  ///< Vulkan initialization flag

public:
    /**
     * @brief Constructor
     * @param capacity Size of the buffer in bytes (0 uses default)
     */
    VulkanBuffer(size_t capacity = 0);

    /**
     * @brief Destructor
     */
    ~VulkanBuffer() override;

    bool is_local() const override {
        return true;  // HOST_VISIBLE memory is locally accessible
    }

    bool is_file_backed() const override {
        return false;  // Vulkan buffers are not file-backed
    }

    bool is_shared() const override {
        return false;  // Vulkan buffers are not shared memory (though they can be exported)
    }

    /**
     * @brief Factory method to create a VulkanBuffer
     * @param capacity Size in bytes
     * @return Pointer to the created VulkanBuffer
     */
    static VulkanBuffer* create(size_t capacity) {
        return new VulkanBuffer(capacity);
    }
    
    /**
     * @brief Implement the create_new method from ChainBuffer
     * @param size Size in bytes
     * @return Pointer to a new VulkanBuffer allocated via BuffetAlligator
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

    /**
     * @brief Get the Vulkan buffer handle
     * @return VkBuffer handle
     */
    VkBuffer get_buffer() const { return buffer_; }

    /**
     * @brief Get the Vulkan device memory handle
     * @return VkDeviceMemory handle
     */
    VkDeviceMemory get_memory() const { return device_memory_; }

    /**
     * @brief Get the Vulkan device handle
     * @return VkDevice handle
     */
    VkDevice get_device() const { return device_; }

    /**
     * @brief Initialize Vulkan with provided device handles
     * @param device Vulkan logical device
     * @param physical_device Vulkan physical device
     */
    static void initialize_vulkan(VkDevice device, VkPhysicalDevice physical_device) {
        default_device_ = device;
        default_physical_device_ = physical_device;
        vulkan_initialized_.store(true, std::memory_order_release);
    }

    /**
     * @brief Automatically initialize Vulkan by creating instance and device
     * @return true if successful, false otherwise
     */
    static bool auto_initialize_vulkan();

private:

    /**
     * @brief Create the Vulkan buffer with HOST_VISIBLE | HOST_COHERENT memory
     */
    void create_buffer();

    /**
     * @brief Find suitable memory type for the buffer
     * @param type_filter Memory type filter
     * @param properties Required memory properties
     * @return Memory type index
     */
    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties);

public:
    /**
     * @brief GPU buffer interface implementation
     */
    
    /**
     * @brief Map the GPU buffer to host memory
     * @param offset Offset in the buffer to start mapping
     * @param size Size to map (0 for entire buffer)
     * @return Pointer to the mapped memory, or nullptr on failure
     */
    void* map(size_t offset = 0, size_t size = 0) override;

    /**
     * @brief Unmap the GPU buffer from host memory
     */
    void unmap() override;

    /**
     * @brief Copy data from host memory to GPU buffer
     * @param src Source data pointer
     * @param size Size of data to copy
     * @param offset Offset in GPU buffer
     * @return true on success, false on failure
     */
    bool upload(const void* src, size_t size, size_t offset = 0) override;

    /**
     * @brief Copy data from GPU buffer to host memory
     * @param dst Destination data pointer
     * @param size Size of data to copy
     * @param offset Offset in GPU buffer
     * @return true on success, false on failure
     */
    bool download(void* dst, size_t size, size_t offset = 0) override;

    /**
     * @brief Copy data from another GPU buffer
     * @param src Source GPU buffer
     * @param size Size of data to copy
     * @param src_offset Offset in source buffer
     * @param dst_offset Offset in destination buffer
     * @return true on success, false on failure
     */
    bool copy_from(GPUBuffer* src, size_t size, size_t src_offset = 0, size_t dst_offset = 0) override;

    /**
     * @brief Synchronize GPU operations on this buffer
     */
    void sync() override;

    /**
     * @brief Get the native GPU handle
     * @return Native handle as void* (cast to VkBuffer)
     */
    void* get_native_handle() override { return reinterpret_cast<void*>(buffer_); }
};

} // namespace alligator
