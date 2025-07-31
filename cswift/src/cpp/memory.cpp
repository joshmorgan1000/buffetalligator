/**
 * @file memory.cpp
 * @brief Implementation of shared memory and GPU memory management classes
 * 
 * Implements zero-copy memory management for CPU and GPU buffers
 */

#include <cswift/detail/memory.hpp>
#include <cswift/cswift.hpp>
#include <cstring>
#include <algorithm>

namespace cswift {

// CSSharedBuffer implementation

CSSharedBuffer::CSSharedBuffer(size_t capacity, int32_t alignment) {
    handle = cswift_shared_buffer_create(capacity, alignment);
    if (!handle) {
        throw CSException(CS_ERROR_OUT_OF_MEMORY, "Failed to create shared buffer");
    }
    
    dataPtr = cswift_shared_buffer_data(handle);
    capacity_ = cswift_shared_buffer_capacity(handle);
}


CSSharedBuffer::~CSSharedBuffer() {
    if (handle) {
        cswift_shared_buffer_destroy(handle);
    }
}

CSSharedBuffer::CSSharedBuffer(CSSharedBuffer&& other) noexcept 
    : handle(other.handle), dataPtr(other.dataPtr), capacity_(other.capacity_), slab_(std::move(other.slab_)) {
    other.handle = nullptr;
    other.dataPtr = nullptr;
    other.capacity_ = 0;
}

CSSharedBuffer& CSSharedBuffer::operator=(CSSharedBuffer&& other) noexcept {
    if (this != &other) {
        if (handle) {
            cswift_shared_buffer_destroy(handle);
        }
        handle = other.handle;
        dataPtr = other.dataPtr;
        capacity_ = other.capacity_;
        slab_ = std::move(other.slab_);
        other.handle = nullptr;
        other.dataPtr = nullptr;
        other.capacity_ = 0;
    }
    return *this;
}

// resize() method removed - not in header

// Methods removed - not part of the header interface

size_t CSSharedBuffer::size() const {
    return cswift_shared_buffer_size(handle);
}

bool CSSharedBuffer::writeAt(size_t offset, const void* data, size_t length) {
    return cswift_shared_buffer_write_at(handle, offset, data, length) != 0;
}

size_t CSSharedBuffer::readAt(size_t offset, size_t length, void* outData) const {
    size_t bytesRead = 0;
    int32_t result = cswift_shared_buffer_read_at(handle, offset, length, outData, &bytesRead);
    return (result == CS_SUCCESS) ? bytesRead : 0;
}

int32_t CSSharedBuffer::alignment() const {
    return cswift_shared_buffer_alignment(handle);
}

void CSSharedBuffer::retain() {
    cswift_shared_buffer_retain(handle);
}

bool CSSharedBuffer::release() {
    return cswift_shared_buffer_release(handle) != 0;
}

// CSNWParameters implementation removed - types not defined

// CSGPUMemoryManager implementation

std::unique_ptr<CSGPUMemoryManager> CSGPUMemoryManager::create() {
    void* error = nullptr;
    CSHandle handle = cswift_gpu_memory_manager_create(&error);
    
    if (!handle) {
        std::string errorMsg = "Failed to create GPU memory manager";
        if (error) {
            errorMsg = static_cast<const char*>(error);
            free(error);
        }
        throw CSException(CS_ERROR_OPERATION_FAILED, errorMsg);
    }
    
    return std::unique_ptr<CSGPUMemoryManager>(new CSGPUMemoryManager(handle));
}

CSGPUMemoryManager::CSGPUMemoryManager(CSHandle handle) : handle_(handle) {}

CSGPUMemoryManager::~CSGPUMemoryManager() {
    if (handle_) {
        cswift_gpu_memory_manager_destroy(handle_);
    }
}

CSGPUMemoryManager::CSGPUMemoryManager(CSGPUMemoryManager&& other) noexcept 
    : handle_(other.handle_) {
    other.handle_ = nullptr;
}

CSGPUMemoryManager& CSGPUMemoryManager::operator=(CSGPUMemoryManager&& other) noexcept {
    if (this != &other) {
        if (handle_) {
            cswift_gpu_memory_manager_destroy(handle_);
        }
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

// Methods using undefined CSGPUBuffer type removed

void* CSGPUMemoryManager::allocateBuffer(const std::string& name, 
                                        size_t size, 
                                        CSGPUMemoryMode memoryMode,
                                        CSGPUMappingType mappingType) {
    void* ptr = cswift_gpu_allocate_buffer(
        handle_, 
        name.c_str(), 
        static_cast<int32_t>(size),
        static_cast<int32_t>(memoryMode),
        static_cast<int32_t>(mappingType)
    );
    
    if (!ptr) {
        throw CSException(CS_ERROR_OUT_OF_MEMORY, 
                        "Failed to allocate GPU buffer: " + name);
    }
    
    return ptr;
}

void* CSGPUMemoryManager::mapBuffer(const std::string& name, 
                                   const void* data, 
                                   size_t size,
                                   CSGPUMappingType mappingType) {
    void* ptr = cswift_gpu_map_buffer(
        handle_, 
        name.c_str(), 
        data, 
        static_cast<int32_t>(size),
        static_cast<int32_t>(mappingType)
    );
    
    if (!ptr) {
        throw CSException(CS_ERROR_OPERATION_FAILED, 
                        "Failed to map buffer: " + name);
    }
    
    return ptr;
}

void CSGPUMemoryManager::copyBuffer(const std::string& fromName, 
                                   const std::string& toName, 
                                   size_t size) {
    CSError result = static_cast<CSError>(cswift_gpu_copy_buffer(
        handle_, 
        fromName.c_str(), 
        toName.c_str(), 
        static_cast<int32_t>(size)
    ));
    
    if (result != CS_SUCCESS) {
        throw CSException(result, 
                        "Failed to copy buffer from " + fromName + " to " + toName);
    }
}

// CSGPUMemoryPool implementation removed - type not defined in header

} // namespace cswift