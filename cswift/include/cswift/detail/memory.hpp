#ifndef CSWIFT_MEMORY_HPP
#define CSWIFT_MEMORY_HPP

#include <cswift/detail/globals.hpp>
#include <cswift/detail/network.hpp>
#include <cswift/detail/params.hpp>

namespace cswift {

/**
 * @brief Shared buffer for zero-copy operations between Swift and C++
 * 
 * This class provides high-performance, aligned memory buffers that can be
 * shared between Swift and C++ without copying data. Ideal for SIMD operations
 * and large data transfers.
 */
class CSSharedBuffer {
private:
    CSHandle handle;
    void* dataPtr;
    size_t capacity_;
    std::shared_ptr<std::vector<uint8_t>> slab_; // For donation architecture
    
public:
    /**
     * @brief Create shared buffer with specified capacity and alignment
     * 
     * @param capacity Buffer capacity in bytes
     * @param alignment Memory alignment (default: 64 bytes for SIMD)
     */
    explicit CSSharedBuffer(size_t capacity, int32_t alignment = 64);
    /**
     * @brief Get the underlying slab shared_ptr (if this buffer owns one)
     * 
     * @return The slab shared_ptr or nullptr if this buffer doesn't own a slab
     */
    std::shared_ptr<std::vector<uint8_t>> getSlab() const { return slab_; }
    
    ~CSSharedBuffer();
    
    // Move-only semantics
    CSSharedBuffer(CSSharedBuffer&& other) noexcept;
    
    CSSharedBuffer& operator=(CSSharedBuffer&& other) noexcept;
    
    CSSharedBuffer(const CSSharedBuffer&) = delete;
    CSSharedBuffer& operator=(const CSSharedBuffer&) = delete;
    
    /**
     * @brief Get direct pointer to buffer data for zero-copy access
     * 
     * @return Pointer to buffer data
     */
    template<typename T>
    T* data() {
        return static_cast<T*>(dataPtr);
    }
    
    template<typename T>
    const T* data() const {
        return static_cast<const T*>(dataPtr);
    }
    
    /**
     * @brief Get buffer capacity
     * 
     * @return Buffer capacity in bytes
     */
    size_t capacity() const {
        return capacity_;
    }
    
    /**
     * @brief Set the amount of valid data in buffer
     * 
     * @param size Size in bytes
     */
    void setSize(size_t size) {
        cswift_shared_buffer_set_size(handle, size);
    }
    
    /**
     * @brief Get the amount of valid data in buffer
     * 
     * @return Size in bytes
     */
    size_t size() const;
    
    /**
     * @brief Write data to buffer at specified offset
     * 
     * @param offset Offset in bytes
     * @param data Data to write
     * @param length Data length
     * @return true on success, false on failure
     */
    bool writeAt(size_t offset, const void* data, size_t length);
    
    /**
     * @brief Write typed data to buffer at specified offset
     * 
     * @param offset Offset in bytes
     * @param value Value to write
     * @return true on success, false on failure
     */
    template<typename T>
    bool writeAt(size_t offset, const T& value) {
        return writeAt(offset, &value, sizeof(T));
    }
    
    /**
     * @brief Write vector data to buffer at specified offset
     * 
     * @param offset Offset in bytes
     * @param vec Vector to write
     * @return true on success, false on failure
     */
    template<typename T>
    bool writeAt(size_t offset, const std::vector<T>& vec) {
        return writeAt(offset, vec.data(), vec.size() * sizeof(T));
    }
    
    /**
     * @brief Read data from buffer at specified offset
     * 
     * @param offset Offset in bytes
     * @param length Length to read
     * @param outData Output buffer (caller allocated)
     * @return Number of bytes actually read
     */
    size_t readAt(size_t offset, size_t length, void* outData) const;
    
    /**
     * @brief Read typed data from buffer at specified offset
     * 
     * @param offset Offset in bytes
     * @return Value read from buffer
     */
    template<typename T>
    T readAt(size_t offset) const {
        T value{};
        readAt(offset, sizeof(T), &value);
        return value;
    }
    
    /**
     * @brief Read vector data from buffer at specified offset
     * 
     * @param offset Offset in bytes
     * @param count Number of elements to read
     * @return Vector containing read data
     */
    template<typename T>
    std::vector<T> readVectorAt(size_t offset, size_t count) const {
        std::vector<T> result(count);
        size_t bytesRead = readAt(offset, count * sizeof(T), result.data());
        result.resize(bytesRead / sizeof(T));
        return result;
    }
    
    /**
     * @brief Get span view over buffer data (C++20)
     * 
     * @return Span over valid data
     */
    #if __cplusplus >= 202002L
    template<typename T>
    std::span<T> span() {
        return std::span<T>(static_cast<T*>(dataPtr), size() / sizeof(T));
    }
    
    template<typename T>
    std::span<const T> span() const {
        return std::span<const T>(static_cast<const T*>(dataPtr), size() / sizeof(T));
    }
    #endif
    
    /**
     * @brief Zero out the entire buffer
     */
    void clear() {
        std::memset(dataPtr, 0, capacity_);
        setSize(0);
    }
    
    /**
     * @brief Get buffer memory alignment
     * 
     * @return Memory alignment in bytes
     */
    int32_t alignment() const;
    
    /**
     * @brief Increment reference count (for shared ownership)
     */
    void retain();
    
    /**
     * @brief Decrement reference count
     * 
     * @return true if buffer was deallocated, false if still referenced
     */
    bool release();
    
    /**
     * @brief Get current reference count
     * 
     * @return Reference count
     */
    int32_t refCount() const {
        return cswift_shared_buffer_ref_count(handle);
    }
    
    /**
     * @brief Get zero-copy pointer to data at offset
     * 
     * @param offset Offset in bytes
     * @param length Length of data needed
     * @return Pointer to data or nullptr if invalid
     */
    template<typename T = void>
    const T* pointerAt(size_t offset, size_t length) const {
        return static_cast<const T*>(
            cswift_shared_buffer_pointer_at(handle, offset, length)
        );
    }
    
    /**
     * @brief Get zero-copy mutable pointer to data at offset
     * 
     * @param offset Offset in bytes
     * @param length Length of data needed
     * @return Mutable pointer to data or nullptr if invalid
     */
    template<typename T = void>
    T* mutablePointerAt(size_t offset, size_t length) {
        return static_cast<T*>(
            cswift_shared_buffer_mutable_pointer_at(handle, offset, length)
        );
    }
    
    /**
     * @brief Get zero-copy span view at specific offset
     * 
     * @param offset Offset in bytes
     * @param count Number of elements
     * @return std::span view of the data (C++20)
     */
    #if __cplusplus >= 202002L
    template<typename T>
    std::span<T> spanAt(size_t offset, size_t count) {
        T* ptr = mutablePointerAt<T>(offset, count * sizeof(T));
        return ptr ? std::span<T>(ptr, count) : std::span<T>();
    }
    
    template<typename T>
    std::span<const T> spanAt(size_t offset, size_t count) const {
        const T* ptr = pointerAt<T>(offset, count * sizeof(T));
        return ptr ? std::span<const T>(ptr, count) : std::span<const T>();
    }
    #endif
    
    /**
     * @brief Create a view of the buffer as Data without copying
     * 
     * @param outDataPtr Output pointer to underlying data
     * @return CS_SUCCESS or error code
     */
    CSError asDataView(const void** outDataPtr) const {
        return static_cast<CSError>(
            cswift_shared_buffer_as_data_view(handle, outDataPtr)
        );
    }
    
    CSHandle getHandle() const { return handle; }
};

// Enhanced CSNWParameters with custom framer support
inline void CSNWParameters::addCustomFramer(const CSNWProtocolFramer& framer) {
    int32_t result = cswift_nw_parameters_add_custom_framer(handle, framer.getHandle());
    if (result != CS_SUCCESS) {
        throw CSException(static_cast<CSError>(result), "Failed to add custom framer to parameters");
    }
}


// ============================================================================
// GPU-Direct Memory Management
// ============================================================================

/**
 * @brief GPU memory storage modes for zero-copy operations
 */
enum class CSGPUMemoryMode {
    Shared = 0,        ///< CPU+GPU accessible (unified memory)
    Managed = 1,       ///< System manages coherency
    Private = 2,       ///< GPU-only memory
    Memoryless = 3     ///< No backing store (mobile optimization)
};

/**
 * @brief GPU memory mapping types
 */
enum class CSGPUMappingType {
    ReadOnly = 0,
    WriteOnly = 1,
    ReadWrite = 2,
    WriteDiscard = 3   ///< Write-only, discard previous contents
};

/**
 * @brief GPU memory synchronization modes
 */
enum class CSGPUSyncMode {
    None = 0,          ///< No synchronization
    CPUToGPU = 1,      ///< CPU writes, GPU reads
    GPUToCPU = 2,      ///< GPU writes, CPU reads
    Bidirectional = 3  ///< Both directions
};

// Shared buffer C bridge functions for donation architecture
extern "C" {
    // Slab creation for donation architecture
    void* cswift_create_slab_wrapper(void* data, size_t size, void* context);
    void cswift_slab_wrapper_retain(void* wrapper);
    void cswift_slab_wrapper_release(void* wrapper);
    void* cswift_slab_wrapper_get_data(void* wrapper);
    size_t cswift_slab_wrapper_get_size(void* wrapper);
}

// GPU-Direct Memory C bridge functions
extern "C" {
    // GPU memory manager functions
    CSHandle cswift_gpu_memory_manager_create(void** error);
    void cswift_gpu_memory_manager_destroy(CSHandle manager);
    
    // Buffer allocation and mapping
    void* cswift_gpu_allocate_buffer(CSHandle manager, const char* name, int32_t size, 
                                    int32_t memoryMode, int32_t mappingType);
    void* cswift_gpu_map_buffer(CSHandle manager, const char* name, const void* data, 
                               int32_t size, int32_t mappingType);
    void* cswift_gpu_get_buffer_pointer(CSHandle manager, const char* name);
    
    // Buffer operations
    int32_t cswift_gpu_synchronize_buffer(CSHandle manager, const char* name, int32_t syncMode);
    int32_t cswift_gpu_copy_buffer(CSHandle manager, const char* fromName, const char* toName, int32_t size);
    int32_t cswift_gpu_deallocate_buffer(CSHandle manager, const char* name);
    
    // Texture operations
    void* cswift_gpu_create_texture(CSHandle manager, const char* bufferName, 
                                   int32_t width, int32_t height, int32_t pixelFormat);
    
    // Statistics
    int32_t cswift_gpu_get_memory_stats(CSHandle manager, int32_t* totalAllocated, 
                                       int32_t* bufferCount, float* memoryPressure);
}

/**
 * @brief GPU-Direct memory manager for zero-copy operations
 * 
 * Provides state-of-the-art zero-copy memory operations between CPU and GPU,
 * enabling production-grade ML performance without memory copies. This is the
 * same technology used in high-performance systems at NVIDIA, Google, and Meta.
 * 
 * Features:
 * - Unified memory sharing between CPU and GPU
 * - Hardware-accelerated memory transfers
 * - Zero-copy buffer access for ML operations
 * - Cross-platform GPU memory management
 * - Real-time performance monitoring
 * 
 * Example usage:
 * ```cpp
 * auto gpuMemory = CSGPUMemoryManager::create();
 * 
 * // Allocate unified memory buffer
 * auto* buffer = gpuMemory->allocateBuffer("ml_data", 1024*1024, 
 *                                         CSGPUMemoryMode::Shared);
 * 
 * // Use buffer for ML operations (zero-copy)
 * auto neuralEngine = CSNeuralEngineModel::create("model.mlmodel");
 * neuralEngine->predict(buffer, ...);  // Direct GPU access, no copies
 * ```
 */
class CSGPUMemoryManager {
private:
    CSHandle handle_;
    
public:
    /**
     * @brief Create GPU-Direct memory manager
     * 
     * @throw CSException on initialization failure
     */
    static std::unique_ptr<CSGPUMemoryManager> create();
    
    ~CSGPUMemoryManager();
    
    // Move-only semantics
    CSGPUMemoryManager(CSGPUMemoryManager&& other) noexcept;
    
    CSGPUMemoryManager& operator=(CSGPUMemoryManager&& other) noexcept;
    
    CSGPUMemoryManager(const CSGPUMemoryManager&) = delete;
    CSGPUMemoryManager& operator=(const CSGPUMemoryManager&) = delete;
    
    /**
     * @brief Allocate GPU-Direct buffer with zero-copy access
     * 
     * Creates a buffer that can be accessed directly by both CPU and GPU
     * without memory copies. This enables maximum performance for ML workloads.
     * 
     * @param name Unique buffer name for identification
     * @param size Size in bytes
     * @param memoryMode Memory sharing mode (default: Shared for zero-copy)
     * @param mappingType Access pattern (default: ReadWrite)
     * @return Pointer to allocated memory for CPU access
     * @throw CSException on allocation failure
     */
    void* allocateBuffer(const std::string& name, 
                        size_t size, 
                        CSGPUMemoryMode memoryMode = CSGPUMemoryMode::Shared,
                        CSGPUMappingType mappingType = CSGPUMappingType::ReadWrite);
    
    /**
     * @brief Map existing data to GPU buffer for zero-copy access
     * 
     * Maps existing CPU data to be accessible by GPU without copying.
     * This is ideal for ML models that need to process existing data.
     * 
     * @param name Unique buffer name
     * @param data Existing data to map
     * @param size Size of data in bytes
     * @param mappingType Access pattern (default: ReadOnly for input data)
     * @return Pointer to mapped memory
     * @throw CSException on mapping failure
     */
    void* mapBuffer(const std::string& name, 
                   const void* data, 
                   size_t size,
                   CSGPUMappingType mappingType = CSGPUMappingType::ReadOnly);
    
    /**
     * @brief Get buffer pointer for zero-copy access
     * 
     * Returns a pointer that can be used directly by CPU code while
     * the same memory is accessible to GPU operations.
     * 
     * @param name Buffer name
     * @return Pointer to buffer or nullptr if not found
     */
    void* getBufferPointer(const std::string& name) {
        return cswift_gpu_get_buffer_pointer(handle_, name.c_str());
    }
    
    /**
     * @brief Synchronize buffer between CPU and GPU
     * 
     * Ensures data consistency between CPU and GPU views of the buffer.
     * This is automatically handled for Shared memory mode but may be
     * needed for Managed mode.
     * 
     * @param name Buffer name
     * @param syncMode Synchronization direction
     * @throw CSException on sync failure
     */
    void synchronizeBuffer(const std::string& name, 
                          CSGPUSyncMode syncMode = CSGPUSyncMode::Bidirectional) {
        
        CSError result = static_cast<CSError>(cswift_gpu_synchronize_buffer(
            handle_, 
            name.c_str(), 
            static_cast<int32_t>(syncMode)
        ));
        
        if (result != CS_SUCCESS) {
            throw CSException(result, "Failed to synchronize buffer: " + name);
        }
    }
    
    /**
     * @brief Copy data between GPU buffers with hardware acceleration
     * 
     * Performs high-speed memory copy using GPU's memory subsystem.
     * This is faster than CPU memcpy for large buffers.  // ZERO_COPY_ALLOWED - performance comparison
     * 
     * @param fromName Source buffer name
     * @param toName Destination buffer name
     * @param size Size to copy (0 = entire buffer)
     * @throw CSException on copy failure
     */
    void copyBuffer(const std::string& fromName, 
                   const std::string& toName, 
                   size_t size = 0);
    
    /**
     * @brief Create GPU texture from buffer for ML operations
     * 
     * Creates a GPU texture that shares memory with an existing buffer.
     * This enables zero-copy image processing for ML models.
     * 
     * @param bufferName Source buffer name
     * @param width Texture width
     * @param height Texture height
     * @param pixelFormat Pixel format (0=RGBA8, 1=RGBA16F, 2=RGBA32F)
     * @return Pointer to texture memory (same as buffer)
     * @throw CSException on texture creation failure
     */
    void* createTexture(const std::string& bufferName, 
                       int32_t width, 
                       int32_t height, 
                       int32_t pixelFormat = 0) {
        
        void* ptr = cswift_gpu_create_texture(
            handle_, 
            bufferName.c_str(), 
            width, 
            height, 
            pixelFormat
        );
        
        if (!ptr) {
            throw CSException(CS_ERROR_OPERATION_FAILED, 
                            "Failed to create texture from buffer: " + bufferName);
        }
        
        return ptr;
    }
    
    /**
     * @brief Get GPU memory statistics
     * 
     * Returns real-time statistics about GPU memory usage.
     * Useful for monitoring and optimization.
     */
    struct MemoryStats {
        int32_t totalAllocated;      ///< Total allocated memory in bytes
        int32_t bufferCount;         ///< Number of allocated buffers
        float memoryPressure;        ///< Memory pressure (0.0-1.0)
    };
    
    MemoryStats getMemoryStats() const {
        MemoryStats stats{};
        
        cswift_gpu_get_memory_stats(
            handle_,
            &stats.totalAllocated,
            &stats.bufferCount,
            &stats.memoryPressure
        );
        
        return stats;
    }
    
    /**
     * @brief Deallocate GPU buffer
     * 
     * Releases GPU buffer and associated memory. The buffer pointer
     * becomes invalid after this call.
     * 
     * @param name Buffer name to deallocate
     * @throw CSException on deallocation failure
     */
    void deallocateBuffer(const std::string& name) {
        CSError result = static_cast<CSError>(cswift_gpu_deallocate_buffer(
            handle_, 
            name.c_str()
        ));
        
        if (result != CS_SUCCESS) {
            throw CSException(result, "Failed to deallocate buffer: " + name);
        }
    }
    
    CSHandle getHandle() const { return handle_; }

private:
    explicit CSGPUMemoryManager(CSHandle handle);
};

} // namespace cswift

#endif // CSWIFT_MEMORY_HPP
