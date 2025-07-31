#ifndef CSWIFT_NIO_BYTEBUFFER_HPP
#define CSWIFT_NIO_BYTEBUFFER_HPP

#include <cswift/detail/globals.hpp>

namespace cswift {

/**
 * @brief API for SwiftNIO ByteBuffer operations
 */
extern "C" {
    // ByteBuffer C bridge functions
    CSHandle cswift_nio_bytebuffer_allocate(int32_t capacity);
    void cswift_nio_bytebuffer_destroy(CSHandle handle);
    int32_t cswift_nio_bytebuffer_readable_bytes(CSHandle handle);
    int32_t cswift_nio_bytebuffer_writable_bytes(CSHandle handle);
    int32_t cswift_nio_bytebuffer_capacity(CSHandle handle);
    void cswift_nio_bytebuffer_clear(CSHandle handle);
    int32_t cswift_nio_bytebuffer_reader_index(CSHandle handle);
    int32_t cswift_nio_bytebuffer_writer_index(CSHandle handle);
    void cswift_nio_bytebuffer_move_reader_index(CSHandle handle, int32_t offset);
    void cswift_nio_bytebuffer_move_writer_index(CSHandle handle, int32_t offset);
    void cswift_nio_bytebuffer_discard_read_bytes(CSHandle handle);
    CSError cswift_nio_bytebuffer_write_bytes(CSHandle handle, const void* bytes, size_t length);
    CSError cswift_nio_bytebuffer_read_bytes(CSHandle handle, void* bytes, size_t length);
    CSError cswift_nio_bytebuffer_get_bytes(CSHandle handle, int32_t index, void* bytes, size_t length);
    CSError cswift_nio_bytebuffer_set_bytes(CSHandle handle, int32_t index, const void* bytes, size_t length);
    const void* cswift_nio_bytebuffer_read_pointer(CSHandle handle, size_t* length);
    void* cswift_nio_bytebuffer_write_pointer(CSHandle handle, size_t* length);
    
    // Donation architecture support
    CSHandle cswift_nio_bytebuffer_get_slab_wrapper(CSHandle handle);
}

/**
 * @brief Zero-copy byte buffer implementation from SwiftNIO
 * 
 * @details Provides efficient, zero-copy byte buffer operations compatible
 * with SwiftNIO's ByteBuffer. This class wraps the Swift implementation
 * and provides a C++ interface.
 * 
 * @note Thread-safety: Not thread-safe. Use external synchronization if needed.
 */
class CSNIOByteBuffer {
private:
    CSHandle handle;
    
public:
    /**
     * @brief Construct a new ByteBuffer with specified capacity
     * 
     * @param capacity Initial capacity in bytes
     * @throws CSException if allocation fails
     */
    explicit CSNIOByteBuffer(int32_t capacity = 256);
    
    /**
     * @brief Construct a ByteBuffer from an existing handle
     * 
     * @param existingHandle Handle to take ownership of
     * @note Takes ownership of the handle
     */
    explicit CSNIOByteBuffer(CSHandle existingHandle);
    
    /**
     * @brief Destructor - releases underlying Swift object
     */
    ~CSNIOByteBuffer();
    
    // Move constructor
    CSNIOByteBuffer(CSNIOByteBuffer&& other) noexcept;
    
    // Move assignment
    CSNIOByteBuffer& operator=(CSNIOByteBuffer&& other) noexcept;
    
    // Delete copy operations
    CSNIOByteBuffer(const CSNIOByteBuffer&) = delete;
    CSNIOByteBuffer& operator=(const CSNIOByteBuffer&) = delete;
    
    /**
     * @brief Get the internal handle
     * @return The internal handle
     * @note For internal use only
     */
    CSHandle getHandle() const;
    
    /**
     * @brief Get number of readable bytes
     * @return Number of bytes available for reading
     */
    int32_t readableBytes() const;
    
    /**
     * @brief Get number of writable bytes
     * @return Number of bytes available for writing
     */
    int32_t writableBytes() const;
    
    /**
     * @brief Get total capacity
     * @return Total buffer capacity in bytes
     */
    int32_t capacity() const;
    
    /**
     * @brief Clear the buffer (reset read/write indices)
     */
    void clear();
    
    /**
     * @brief Get current reader index
     * @return Current position for reading
     */
    int32_t readerIndex() const;
    
    /**
     * @brief Get current writer index
     * @return Current position for writing
     */
    int32_t writerIndex() const;
    
    /**
     * @brief Move reader index by offset
     * @param offset Number of bytes to move (can be negative)
     */
    void moveReaderIndex(int32_t offset);
    
    /**
     * @brief Move writer index by offset
     * @param offset Number of bytes to move (can be negative)
     */
    void moveWriterIndex(int32_t offset);
    
    /**
     * @brief Discard already read bytes to make more room
     */
    void discardReadBytes();
    
    /**
     * @brief Write bytes to buffer
     * @param data Data to write
     * @param length Number of bytes to write
     * @throws CSException if write fails
     */
    void writeBytes(const void* data, size_t length);
    
    /**
     * @brief Write bytes from vector
     * @param data Vector of bytes to write
     */
    void writeBytes(const std::vector<uint8_t>& data);
    
    /**
     * @brief Write string to buffer
     * @param str String to write (without null terminator)
     */
    void writeString(const std::string& str);
    
    /**
     * @brief Read bytes from buffer
     * @param data Destination buffer
     * @param length Number of bytes to read
     * @throws CSException if read fails
     */
    void readBytes(void* data, size_t length);
    
    /**
     * @brief Read bytes into vector
     * @param data Vector to read into (must be pre-sized)
     * @return Number of bytes actually read
     */
    size_t readBytes(std::vector<uint8_t>& data);
    
    /**
     * @brief Get bytes at specific index without moving reader
     * @param index Starting index
     * @param data Destination buffer
     * @param length Number of bytes to get
     * @throws CSException if operation fails
     */
    void getBytes(int32_t index, void* data, size_t length) const;
    
    /**
     * @brief Set bytes at specific index without moving writer
     * @param index Starting index
     * @param data Source data
     * @param length Number of bytes to set
     * @throws CSException if operation fails
     */
    void setBytes(int32_t index, const void* data, size_t length);
    
    /**
     * @brief Get zero-copy read pointer
     * @return Pair of pointer and available bytes
     * @note Pointer is valid until next buffer operation
     */
    std::pair<const void*, size_t> readPointer() const;
    
    /**
     * @brief Get zero-copy write pointer
     * @return Pair of pointer and available space
     * @note Pointer is valid until next buffer operation
     */
    std::pair<void*, size_t> writePointer();
    
    /**
     * @brief Write data using a callback with zero-copy pointer
     * @param callback Function that writes to the buffer
     * @return Number of bytes written
     */
    template<typename Func>
    size_t writeWithUnsafeMutableBytes(Func&& callback) {
        auto [ptr, available] = writePointer();
        if (!ptr || available == 0) {
            return 0;
        }
        
        size_t written = callback(ptr, available);
        if (written > 0) {
            moveWriterIndex(static_cast<int32_t>(written));
        }
        return written;
    }
    
    /**
     * @brief Read data using a callback with zero-copy pointer
     * @param callback Function that reads from the buffer
     * @return Number of bytes read
     */
    template<typename Func>
    size_t readWithUnsafeReadableBytes(Func&& callback) const {
        auto [ptr, available] = readPointer();
        if (!ptr || available == 0) {
            return 0;
        }
        
        return callback(ptr, available);
    }
};



} // namespace cswift

#endif // CSWIFT_NIO_BYTEBUFFER_HPP
