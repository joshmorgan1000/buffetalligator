#pragma once

#include <cstdint>
#include <atomic>
#include <span>
#include <optional>
#include <alligator/buffer/chain_buffer.hpp>

/**
 * @file heap_buffer.hpp
 * @brief Heap-allocated buffer implementation with chaining support
 * 
 * 30208000
 * 300691456
 */

namespace alligator {

/**
 * @brief Heap-allocated buffer implementation with chaining support
 * 
 * This buffer allocates memory on the heap using new[]/delete[].
 * It inherits from ChainBuffer to support automatic chaining
 * for efficient message passing.
 */
class HeapBuffer : public ChainBuffer {
private:
    uint8_t* data_{nullptr};         ///< Pointer to the heap-allocated data

public:
    /**
     * @brief Constructor
     * @param capacity Size of the buffer in bytes (0 uses default)
     */
    HeapBuffer(size_t capacity = 0);

    /**
     * @brief Destructor
     */
    ~HeapBuffer() override;

    bool is_local() const override {
        return true;  // Heap buffers are always locally accessible
    }

    bool is_file_backed() const override {
        return false;  // Heap buffers are not file-backed
    }

    bool is_shared() const override {
        return false;  // Heap buffers are not shared memory
    }

    /**
     * @brief Factory method to create a HeapBuffer
     * @param capacity Size in bytes
     * @return Pointer to the created HeapBuffer
     */
    static HeapBuffer* create(size_t capacity) {
        return new HeapBuffer(capacity);
    }
    
    /**
     * @brief Implement the create_new method from ChainBuffer
     * @param size Size in bytes
     * @return Pointer to a new HeapBuffer allocated via BuffetAlligator
     */
    ChainBuffer* create_new(size_t size) override;


    /**
     * @brief Clear the buffer, setting all bytes to zero
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
};

} // namespace alligator
