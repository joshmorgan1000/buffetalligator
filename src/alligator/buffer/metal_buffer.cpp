#include <alligator/buffer/metal_buffer.hpp>
#include <alligator/buffer/buffet_alligator.hpp>
#include <cstring>
#include <stdexcept>
#include <sstream>

#ifdef METAL_ENABLED
#include <sys/sysctl.h>

/**
 * @file metal_buffer.cpp
 * @brief Implementation of Metal GPU buffer with chaining support using cswift
 */

namespace alligator {


// Default buffer size if not specified (64 KB)
static constexpr size_t DEFAULT_METAL_BUFFER_SIZE = 65536;

// Static member initialization
std::unique_ptr<cswift::CSMTLDevice> MetalBuffer::default_device_;
std::unique_ptr<cswift::CSMTLCommandQueue> MetalBuffer::default_queue_;
std::atomic<bool> MetalBuffer::metal_initialized_{false};

void MetalBuffer::ensure_metal_initialized() {
    bool expected = false;
    if (metal_initialized_.compare_exchange_strong(expected, true)) {
        // Initialize default Metal device
        default_device_ = std::make_unique<cswift::CSMTLDevice>();
        
        // Create default command queue
        if (default_device_) {
            default_queue_ = default_device_->createCommandQueue();
        }
        
        // Metal initialized successfully
    }
}

MetalBuffer::MetalBuffer(size_t capacity, cswift::CSMTLDevice* device, int storage_mode) 
    : GPUBuffer(capacity, GPUMemoryType::DEVICE_LOCAL) {
    // Ensure Metal is initialized
    ensure_metal_initialized();
    
    // Use default size if not specified
    if (capacity == 0) {
        capacity = DEFAULT_METAL_BUFFER_SIZE;
    }
    
    buf_size_ = capacity;
    
    // Use provided device or default
    cswift::CSMTLDevice* mtl_device = device ? device : default_device_.get();
    
    // Determine storage mode
    if (storage_mode == 0) {
        // Default to shared mode for unified memory on Apple Silicon
        storage_mode_ = cswift::CSMTLStorageMode::Shared;
    } else {
        storage_mode_ = static_cast<cswift::CSMTLStorageMode>(storage_mode - 1);
    }
    
    try {
        // Create Metal buffer
        metal_buffer_ = mtl_device->createBuffer(capacity, storage_mode_);
        
        if (!metal_buffer_) {
            throw std::runtime_error("Metal buffer creation failed");
        }
        
        // Get CPU-accessible pointer
        mapped_data_ = metal_buffer_->contents();
        
        if (!mapped_data_ && storage_mode_ != cswift::CSMTLStorageMode::Private) {
            throw std::runtime_error("Metal buffer mapping failed");
        }
        
        // Initialize to zero
        if (mapped_data_) {
            clear(0);
        }
        
    } catch (const std::exception& e) {
        throw;
    }
}

MetalBuffer::~MetalBuffer() {

}

ChainBuffer* MetalBuffer::create_new(size_t size) {
    auto& alligator = BuffetAlligator::instance();
    return alligator.allocate_buffer(size, BFType::METAL);
}

void MetalBuffer::clear(uint8_t value) {
    if (mapped_data_) {
        std::memset(mapped_data_, value, buf_size_);
        
        // For managed storage, need to signal that CPU modified the buffer
        if (storage_mode_ == cswift::CSMTLStorageMode::Managed) {
            metal_buffer_->didModifyRange(0, buf_size_);
        }
    }
}

void MetalBuffer::deallocate() {
    metal_buffer_.reset();
    mapped_data_ = nullptr;
    buf_size_ = 0;
}

uint8_t* MetalBuffer::data() {
    return static_cast<uint8_t*>(mapped_data_);
}

const uint8_t* MetalBuffer::data() const {
    return static_cast<const uint8_t*>(mapped_data_);
}

void MetalBuffer::read(size_t offset, void* dst, size_t length) const {
    if (!dst || length == 0) return;
    
    if (offset + length > buf_size_) {
        // Read out of bounds: offset, length, buffer_size
        throw std::out_of_range("MetalBuffer read out of bounds");
    }
    
    if (mapped_data_) {
        // For managed storage, ensure GPU writes are visible to CPU
        if (storage_mode_ == cswift::CSMTLStorageMode::Managed) {
            // Note: In a real implementation, we'd need to ensure GPU work is complete
            // and potentially synchronize the buffer
        }
        
        std::memcpy(dst, static_cast<const uint8_t*>(mapped_data_) + offset, length);
    } else {
        throw std::runtime_error("Cannot read from GPU-only buffer");
    }
}

void MetalBuffer::write(size_t offset, const void* src, size_t length) {
    if (!src || length == 0) return;
    
    if (offset + length > buf_size_) {
        // Write out of bounds: offset, length, buffer_size
        throw std::out_of_range("MetalBuffer write out of bounds");
    }
    
    if (mapped_data_) {
        std::memcpy(static_cast<uint8_t*>(mapped_data_) + offset, src, length);
        
        // For managed storage, signal that CPU modified the buffer
        if (storage_mode_ == cswift::CSMTLStorageMode::Managed) {
            metal_buffer_->didModifyRange(offset, length);
        }
    } else {
        throw std::runtime_error("Cannot write to GPU-only buffer");
    }
}

void MetalBuffer::sync_gpu() {
    if (!default_queue_) {
        return;
    }
    
    // Create and commit an empty command buffer to ensure all GPU work completes
    CSHandle commandBuffer = cswift::cswift_mtl_command_buffer_create(default_queue_->getHandle());
    if (commandBuffer) {
        cswift::cswift_mtl_command_buffer_commit(commandBuffer);
        cswift::cswift_mtl_command_buffer_wait_until_completed(commandBuffer);
        cswift::cswift_mtl_command_buffer_destroy(commandBuffer);
    }
}

void MetalBuffer::copy_from_metal(const MetalBuffer& src, 
                                 size_t src_offset, 
                                 size_t dst_offset, 
                                 size_t length) {
    if (src_offset + length > src.buf_size_ || dst_offset + length > buf_size_) {
        throw std::out_of_range("Metal buffer copy out of bounds");
    }
    
    if (!default_queue_) {
        throw std::runtime_error("No command queue for Metal copy");
    }
    
    // Use GPU to copy between buffers
    CSHandle commandBufferHandle = cswift::cswift_mtl_command_buffer_create(default_queue_->getHandle());
    if (!commandBufferHandle) {
        throw std::runtime_error("Failed to create command buffer");
    }
    
    // TODO: Implement blit encoder for GPU copy
    // For now, perform CPU copy via mapped memory
    if (src.mapped_data_ && mapped_data_) {
        std::memcpy(
            static_cast<uint8_t*>(mapped_data_) + dst_offset,
            static_cast<const uint8_t*>(src.mapped_data_) + src_offset,
            length
        );
    }
    
    // Commit and wait for completion
    cswift::cswift_mtl_command_buffer_commit(commandBufferHandle);
    cswift::cswift_mtl_command_buffer_wait_until_completed(commandBufferHandle);
    cswift::cswift_mtl_command_buffer_destroy(commandBufferHandle);
}

void MetalBuffer::enqueue_compute(CSHandle compute_pipeline,
                                 size_t threads_per_grid,
                                 size_t threads_per_threadgroup) {
    if (!compute_pipeline) {
        // Null compute pipeline provided
        return;
    }
    
    if (!default_queue_) {
        // No command queue available for compute
        return;
    }
    
    // Create command buffer and compute encoder
    CSHandle commandBufferHandle = cswift::cswift_mtl_command_buffer_create(default_queue_->getHandle());
    if (!commandBufferHandle) {
        return;
    }
    
    // TODO: Implement compute encoder
    // The cswift library doesn't expose the compute encoder C API yet
    
    cswift::cswift_mtl_command_buffer_commit(commandBufferHandle);
    cswift::cswift_mtl_command_buffer_destroy(commandBufferHandle);
}

// GPU Buffer interface implementation
void* MetalBuffer::map(size_t offset, size_t size) {
    if (mapped_data_) {
        return static_cast<uint8_t*>(mapped_data_) + offset;
    }
    return nullptr;
}

void MetalBuffer::unmap() {
    // Metal buffers with shared storage mode are always mapped
    // No explicit unmap needed
}

bool MetalBuffer::upload(const void* src, size_t size, size_t offset) {
    if (!src || offset + size > buf_size_) {
        return false;
    }
    
    if (mapped_data_) {
        std::memcpy(static_cast<uint8_t*>(mapped_data_) + offset, src, size);
        return true;
    }
    return false;
}

bool MetalBuffer::download(void* dst, size_t size, size_t offset) {
    if (!dst || offset + size > buf_size_) {
        return false;
    }
    
    if (mapped_data_) {
        std::memcpy(dst, static_cast<const uint8_t*>(mapped_data_) + offset, size);
        return true;
    }
    return false;
}

bool MetalBuffer::copy_from(GPUBuffer* src, size_t size, size_t src_offset, size_t dst_offset) {
    if (!src || src_offset + size > src->buf_size_ || dst_offset + size > buf_size_) {
        return false;
    }
    
    // Try to cast to MetalBuffer for GPU copy
    MetalBuffer* metal_src = dynamic_cast<MetalBuffer*>(src);
    if (metal_src) {
        try {
            copy_from_metal(*metal_src, src_offset, dst_offset, size);
            return true;
        } catch (...) {
            return false;
        }
    }
    
    // Fallback to CPU copy
    if (src->is_mapped() && mapped_data_) {
        std::memcpy(
            static_cast<uint8_t*>(mapped_data_) + dst_offset,
            static_cast<uint8_t*>(src->map(src_offset, size)),
            size
        );
        return true;
    }
    
    return false;
}

void MetalBuffer::sync() {
    sync_gpu();
}

void* MetalBuffer::get_native_handle() {
    return metal_buffer_ ? metal_buffer_->getHandle() : nullptr;
}


} // namespace alligator
#endif
