#pragma once

#include <cstdint>
#include <atomic>
#include <thread>
#include <memory>
#include <iostream>
#include <alligator/buffer/buffer.hpp>

namespace alligator {

// Forward declarations
class ChainBuffer;
class BufferPin;

// We can't forward declare Message with its requires clause here
// So we'll just avoid using it in this header

/**
 * @brief A struct to hold a claim on a buffer
 * 
 * This struct is used to represent a claim on a buffer, including the
 * buffer itself, the offset where the claim starts, the size of the claim,
 * and the space left in the buffer after the claim.
 */
struct BufferClaim {
    ChainBuffer* buffer;     ///< Pointer to the buffer that ended up being claimed
    size_t offset;           ///< Offset in the buffer where the claim starts
    size_t size;             ///< Size of the claim in bytes
    int64_t space_left;      ///< Space left in the buffer after the claim
};

class ChainBuffer;

/**
 * @brief A class to pin a buffer so it does not get garbage collected
 * 
 * Only a Message that owns a buffer can create a BufferPin for that buffer.
 * This ensures that random code cannot pin buffers they don't own.
 */
class BufferPin {
private:
    ChainBuffer* buffer_;
    explicit BufferPin(ChainBuffer* buffer) : buffer_(buffer) {}
    friend class ChainBuffer;
public:
    ~BufferPin();  // Implementation in source file
    ChainBuffer* get() const { return buffer_; }
    BufferPin(const BufferPin&) = delete;
    BufferPin& operator=(const BufferPin&) = delete;
    BufferPin(BufferPin&& other) noexcept : buffer_(other.buffer_) {
        other.buffer_ = nullptr;
    }
    BufferPin& operator=(BufferPin&& other) noexcept {
        if (this != &other) {
            // Will be handled by destructor
            buffer_ = other.buffer_;
            other.buffer_ = nullptr;
        }
        return *this;
    }
};

/**
 * @brief Base class for buffers that support chaining
 * 
 * This class provides the chaining functionality that can be inherited
 * by any buffer implementation (HeapBuffer, CudaBuffer, etc.)
 */
class ChainBuffer : public ICollectiveBuffer {
protected:
    std::atomic<bool> next_loading_{false};
    std::atomic<size_t> next_consumer_offset_{0};
    std::atomic<uint64_t> constructor_call_count_{0};  ///< Constructor call count
    std::atomic<uint64_t> destructor_call_count_{0};   ///< Destructor call count
    std::atomic<uint64_t> last_message_sequence_{UINT64_MAX};  ///< Sequence number of the last message
    std::atomic<bool> pinned_{false};
    std::atomic<uint64_t> ttl_{0};
    friend class BuffetAlligator;  ///< Allow BuffetAlligator to access private members
    friend class BufferPin;
public:
    std::atomic<ChainBuffer*> next{nullptr};
    std::atomic<uint64_t> consumed_timestamp_{std::numeric_limits<uint64_t>::max()};
    virtual ~ChainBuffer() = default;
    uint64_t increment_constructor_count() {
        return constructor_call_count_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t increment_destructor_count() {
        return destructor_call_count_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t get_constructor_count() {
        return constructor_call_count_.load(std::memory_order_relaxed);
    }
    uint64_t get_destructor_count() {
        return destructor_call_count_.load(std::memory_order_relaxed);
    }
    void set_last_message_sequence(uint64_t seq) {
        last_message_sequence_.store(seq, std::memory_order_release);
    }
    void set_ttl(uint64_t ttl) {
        ttl_.store(ttl, std::memory_order_release);
    }
    void mark_consumed() {
        uint64_t current_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        // Check to make sure that adding the ttl_ to current_time does not overflow the uint64_t
        if (std::numeric_limits<uint64_t>::max() - current_time < ttl_.load(std::memory_order_acquire)) {
            consumed_timestamp_.store(std::numeric_limits<uint64_t>::max(), std::memory_order_release);
        } else {
            consumed_timestamp_.store(current_time + ttl_.load(std::memory_order_acquire), std::memory_order_release);
        }
    }
    
    /**
     * @brief Factory method that derived classes must implement
     * @param size Size in bytes
     * @return Pointer to a new buffer of the same type
     */
    virtual ChainBuffer* create_new(size_t size) = 0;

    /**
     * @brief Get the next buffer in the chain, allocating it if necessary
     * 
     * This method will create a new buffer using the create_new() method if
     * the next buffer is not already allocated.
     * 
     * @return Pointer to the next ChainBuffer in the sequence
     */
    [[nodiscard]] ChainBuffer* get_next(bool allocate = true) {
        ChainBuffer* next_load = this->next.load(std::memory_order_acquire);
        if (!next_load && allocate) {
            bool expected = false;
            if (!next_loading_.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
                // Another thread is already loading the next buffer
                while (next_loading_.load(std::memory_order_acquire)) {
                    std::this_thread::yield();  // Wait for the other thread to finish
                }
                return this->next.load(std::memory_order_acquire);
            }
            // Allocate through the alligator to ensure registration
            next_load = create_new(buf_size_);
            if (!next_load) {
                next_loading_.store(false, std::memory_order_release);
                throw std::runtime_error("Failed to create next buffer");
            }
            next.store(next_load, std::memory_order_release);
            next_loading_.store(false, std::memory_order_release);
        }
        return next_load;
    }

    /**
     * @brief Atomically claim a chunk of the buffer, automatically advancing to the
     * next buffer to ensure there is enough space if necessary.
     */
    [[nodiscard]] BufferClaim claim(size_t size) {
        if (size > buf_size_) throw std::runtime_error("Claim size exceeds buffer size");
        BufferClaim claim;
        claim.buffer = this;
        claim.size = size;
        claim.offset = claim.buffer->next_offset_.fetch_add(size, std::memory_order_relaxed);
        claim.space_left = static_cast<int64_t>(buf_size_) - static_cast<int64_t>(claim.offset + size);
        while (claim.space_left < 0) {
            claim.buffer = claim.buffer->get_next();
            claim.offset = claim.buffer->next_offset_.fetch_add(size, std::memory_order_relaxed);
            claim.space_left = static_cast<int64_t>(claim.buffer->buf_size_) - static_cast<int64_t>(claim.offset + size);
        }
        return claim;
    }

    /**
     * @brief Atomically consume a chunk of the buffer, automatically advancing to the
     * next buffer to ensure there is enough space if necessary.
     * 
     * This method is used by consumers to claim a chunk of the buffer for reading.
     * It will automatically advance to the next buffer if the current one is full.
     */
    [[nodiscard]] BufferClaim consume(size_t size) {
        if (size > buf_size_) throw std::runtime_error("Consume size exceeds buffer size");
        BufferClaim claim;
        claim.buffer = this;
        claim.size = size;
        claim.offset = claim.buffer->next_consumer_offset_.fetch_add(size, std::memory_order_relaxed);
        claim.space_left = static_cast<int64_t>(buf_size_) - static_cast<int64_t>(claim.offset + size);
        while (claim.space_left < 0) {
            // Buffer is full, will need to go to the next buffer
            claim.buffer = claim.buffer->get_next(false);  // Don't allocate a new buffer, that's the producer's job
            claim.offset = claim.buffer->next_consumer_offset_.fetch_add(size, std::memory_order_relaxed);
            claim.space_left = static_cast<int64_t>(claim.buffer->buf_size_) - static_cast<int64_t>(claim.offset + size);
        }
        return claim;
    }

    BufferPin pin() {
        return BufferPin(this);
    }
};

}
