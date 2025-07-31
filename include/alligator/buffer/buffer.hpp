#pragma once

#include <cstdint>
#include <span>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <functional>
#include <thread>
#include <deque>
#include <atomic>
#include <future>

#ifndef MAX_BUFFER_BIT
/**
 * By default, allow up to 256 K active buffers concurrently
 * This requires 2 MB of heap to keep track of all the pointers needed for
 * the buffer registry, which is why it is configurable
 */
#define MAX_BUFFER_BIT 22
#endif

#define MAX_BUFFERS static_cast<uint32_t>(1 << MAX_BUFFER_BIT)
#ifndef HIGH_BIT
#define HIGH_BIT static_cast<uint32_t>(1 << 31)
#endif
#define BUFFER_ID_MASK static_cast<uint32_t>((1 << MAX_BUFFER_BIT) - 1)
#define BUFFER_ID_VALID_MASK static_cast<uint32_t>(BUFFER_ID_MASK | HIGH_BIT)
#define EMPTY_BUFFER_IDENTIFIER static_cast<uint32_t>((1 << 31) | BUFFER_ID_MASK)

namespace alligator {

static std::atomic<uint64_t> allocation_counter_;
static std::atomic<uint64_t> deallocation_counter_;

/**
 * @class ICollectiveBuffer
 * @brief Interface for buffers with smart allocation and reference counting
 * 
 * This interface defines the methods required for buffers that can be
 * sequenced collectively, allowing for efficient message passing and
 * synchronization between producers and consumers.
 * 
 * The constructor by contract should automatically allocate the buffer
 * memory and set the size.
 */
class ICollectiveBuffer {
public:
    virtual ~ICollectiveBuffer() = default;

    /**
     * @brief The unique ID for this buffer. This ID should be set once at
     * construction and never changed.
     */
    std::atomic<uint32_t> id_{EMPTY_BUFFER_IDENTIFIER};  ///< Unique ID for this buffer

    /**
     * @brief If the buffer is registered in the global buffer registry,
     * it won't have its ID set to EMPTY_BUFFER_IDENTIFIER.
     */
    bool is_registered() const {
        return (id_.load(std::memory_order_acquire) & BUFFER_ID_VALID_MASK) != EMPTY_BUFFER_IDENTIFIER;
    }

    /**
     * @brief Size of the buffer in bytes. This should be set at construction
     * and never changed.
     */
    size_t buf_size_;

    /**
     * @brief Flag indicating if the buffer is locally accessible by
     * the CPU. Some buffers may be allocated on a GPU or other device,
     * and may need to go through extra steps to access.
     * 
     * This value should be set once at construction and never changed.
     */
    virtual bool is_local() const = 0;

    /**
     * @brief Flag indicating if the buffer is file-backed.
     * 
     * This should be set once at construction and never changed.
     */
    virtual bool is_file_backed() const = 0;

    /**
     * @brief The next offset in the buffer for writing data.
     * 
     * Atomically managed to ensure thread safety when multiple threads
     * are writing to the buffer concurrently.
     */
    std::atomic<size_t> next_offset_{0};

    /**
     * @brief Check if the buffer is full
     * 
     * This method checks if the next offset has reached or exceeded
     * the buffer size, indicating that no more data can be written.
     * 
     * @return true if the buffer is full, false otherwise
     */
    [[nodiscard]] constexpr bool full() const {
        return next_offset_.load(std::memory_order_relaxed) >= buf_size_;
    }

    /**
     * @brief Flag indicating if the buffer is shared memory, accessible
     * by multiple separate processes on the same system.
     * 
     * This should be set once at construction and never changed.
     */
    virtual bool is_shared() const = 0;


    /**
     * @brief Clear the buffer, setting all bytes to a specific value
     * 
     * @param value The value to set each byte to (default is 0)
     */
    virtual void clear(uint8_t value = 0) = 0;

    /**
     * @brief Deallocate the buffer memory
     */
    virtual void deallocate() = 0;

    /**
     * @brief Get a pointer to the raw data
     * @return Pointer to the buffer data
     */
    virtual uint8_t* data() = 0;
    virtual const uint8_t* data() const = 0;

    /**
     * @brief Get a span view of the buffer
     * 
     * This method provides a span view of the buffer data,
     * allowing for efficient access to the underlying memory.
     * 
     * @param offset Offset in bytes from the beginning of the buffer
     * @param size Size in bytes to read (0 for full buffer, which is the default)
     * 
     * @return Span view of the buffer data
     */
    virtual std::span<uint8_t> get_span(size_t offset, size_t size) = 0;
    virtual std::span<const uint8_t> get_span(const size_t& offset, const size_t& size) const = 0;

    /**
     * @brief Get a typed pointer at a specific index. Assumes the entire buffer is of type T.
     * @tparam T The type to cast the pointer to
     * @param index Index of the object in the buffer
     * @return Typed pointer to the data at the specified offset
     */
    template<typename T>
        requires std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>
    [[nodiscard]] T* get(size_t index) { return &(reinterpret_cast<T*>(data())[index]); }
    template<typename T>
        requires std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>
    [[nodiscard]] const T* get(const size_t& index) const { return &(reinterpret_cast<const T*>(data())[index]); }
};

/**
 * @brief Concept for buffer types that ensures they have a static `create(size_t)` method
 * so they know how to create new instances of themselves.
 */
template<typename T>
concept BufferType = requires(T t, size_t size) {
    { T::create(size) } -> std::same_as<T*>;
} && std::is_base_of_v<ICollectiveBuffer, T>;

}
