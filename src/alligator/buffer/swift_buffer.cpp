#include <alligator/buffer/swift_buffer.hpp>
#include <alligator/buffer/buffet_alligator.hpp>
#include <cstring>
#include <stdexcept>

/**
 * @file swift_buffer.cpp
 * @brief Implementation of Swift-backed buffer with chaining support
 */

namespace alligator {

// Default buffer size if not specified (64 KB)
static constexpr size_t DEFAULT_SWIFT_BUFFER_SIZE = 65536;

SwiftBuffer::SwiftBuffer(size_t capacity) {
    // Use default size if not specified
    if (capacity == 0) {
        capacity = DEFAULT_SWIFT_BUFFER_SIZE;
    }
    
    buf_size_ = capacity;
    
    try {
        // Create the Swift buffer with 64-byte alignment for SIMD operations
        swift_buffer_ = std::make_unique<cswift::CSSharedBuffer>(capacity, 64);
        
        // Cache the data pointer
        data_ = swift_buffer_->data<uint8_t>();
        
        if (!data_) {
            throw std::runtime_error("Failed to get data pointer from Swift buffer");
        }
        
        // Initialize to zero
        clear(0);
        
    } catch (const cswift::CSException& e) {
        throw std::runtime_error(std::string("Failed to create Swift buffer: ") + e.what());
    } catch (const std::exception& e) {
        throw;
    }
}

SwiftBuffer::~SwiftBuffer() {
    // unique_ptr will clean up swift_buffer_ automatically
}

ChainBuffer* SwiftBuffer::create_new(size_t size) {
    // Allocate through BuffetAlligator to ensure proper registration
    return get_buffet_alligator().allocate_buffer(
        static_cast<uint32_t>(size), 
        BFType::SWIFT
    );
}

void SwiftBuffer::clear(uint8_t value) {
    if (!data_) {
        return;
    }
    
    if (swift_buffer_) {
        if (value == 0) {
            // Use the optimized clear method for zero
            swift_buffer_->clear();
        } else {
            // Fill with the specified value
            std::memset(data_, value, buf_size_);
            swift_buffer_->setSize(0);  // Reset valid data size
        }
    }
}

void SwiftBuffer::deallocate() {
    // Release the Swift buffer
    swift_buffer_.reset();
    data_ = nullptr;
}

uint8_t* SwiftBuffer::data() {
    return data_;
}

const uint8_t* SwiftBuffer::data() const {
    return data_;
}

std::span<uint8_t> SwiftBuffer::get_span(size_t offset, size_t size) {
    if (!data_) {
        return std::span<uint8_t>();
    }
    
    if (offset > buf_size_) {
        return std::span<uint8_t>();
    }
    
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : size;
    
    if (offset + actual_size > buf_size_) {
        actual_size = buf_size_ - offset;
    }
    
    return std::span<uint8_t>(data_ + offset, actual_size);
}

std::span<const uint8_t> SwiftBuffer::get_span(const size_t& offset, const size_t& size) const {
    if (!data_) {
        return std::span<const uint8_t>();
    }
    
    if (offset > buf_size_) {
        return std::span<const uint8_t>();
    }
    
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : size;
    
    if (offset + actual_size > buf_size_) {
        actual_size = buf_size_ - offset;
    }
    
    return std::span<const uint8_t>(data_ + offset, actual_size);
}

} // namespace alligator
