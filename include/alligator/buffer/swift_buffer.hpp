#pragma once

#include <cstdint>
#include <atomic>
#include <span>
#include <optional>
#include <memory>
#include <alligator/buffer/chain_buffer.hpp>
#include <cswift/cswift.hpp>

/**
 * @file swift_buffer.hpp
 * @brief Swift-backed buffer implementation with chaining support
 * 
 * This buffer uses Apple's Swift Network.framework ByteBuffer (via cswift)
 * for zero-copy network operations. It provides excellent performance for
 * network-related messaging and supports both local and network transports.
 */

namespace alligator {

/**
 * @brief Swift-backed buffer implementation with chaining support
 * 
 * This buffer uses cswift::CSSharedBuffer which provides high-performance,
 * aligned memory buffers that can be shared between Swift and C++ without
 * copying data. It inherits from ChainBuffer to support automatic chaining
 * for efficient message passing.
 * 
 * Key features:
 * - Zero-copy operations between Swift and C++
 * - Aligned memory for SIMD operations (64-byte default)
 * - Integration with Swift's Network.framework for network operations
 * - Support for donation architecture via slab sharing
 */
class SwiftBuffer : public ChainBuffer {
private:
    std::unique_ptr<cswift::CSSharedBuffer> swift_buffer_;  ///< The underlying Swift buffer
    uint8_t* data_{nullptr};                                ///< Cached data pointer

public:
    /**
     * @brief Constructor
     * @param capacity Size of the buffer in bytes (0 uses default)
     */
    SwiftBuffer(size_t capacity = 0);

    /**
     * @brief Destructor
     */
    ~SwiftBuffer() override;

    bool is_local() const override {
        return true;  // Swift buffers are locally accessible
    }

    bool is_file_backed() const override {
        return false;  // Swift buffers are not file-backed
    }

    bool is_shared() const override {
        return true;  // Swift buffers can be shared via donation architecture
    }

    /**
     * @brief Factory method to create a SwiftBuffer
     * @param capacity Size in bytes
     * @return Pointer to the created SwiftBuffer
     */
    static SwiftBuffer* create(size_t capacity) {
        return new SwiftBuffer(capacity);
    }
    
    /**
     * @brief Implement the create_new method from ChainBuffer
     * @param size Size in bytes
     * @return Pointer to a new SwiftBuffer allocated via BuffetAlligator
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
     * @brief Get the underlying Swift buffer handle
     * @return CSHandle for interop with Swift
     */
    CSHandle get_swift_handle() const { 
        return swift_buffer_ ? swift_buffer_->getHandle() : nullptr; 
    }

    /**
     * @brief Get the underlying slab for donation architecture
     * @return Shared pointer to the slab or nullptr
     */
    std::shared_ptr<std::vector<uint8_t>> get_slab() const {
        return swift_buffer_ ? swift_buffer_->getSlab() : nullptr;
    }

    /**
     * @brief Get buffer memory alignment
     * @return Memory alignment in bytes (typically 64)
     */
    int32_t alignment() const {
        return swift_buffer_ ? swift_buffer_->alignment() : 0;
    }
};

} // namespace alligator
