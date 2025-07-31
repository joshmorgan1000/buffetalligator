#include <cswift/detail/nio.hpp>
#include <cswift/detail/memory.hpp>

namespace cswift {

// CSNIOByteBuffer implementations
CSNIOByteBuffer::CSNIOByteBuffer(int32_t capacity) {
    handle = cswift_nio_bytebuffer_allocate(capacity);
    if (!handle) {
        throw CSException(CS_ERROR_OUT_OF_MEMORY, "Failed to allocate ByteBuffer");
    }
}

CSNIOByteBuffer::CSNIOByteBuffer(CSHandle existingHandle) : handle(existingHandle) {
    if (!handle) {
        throw CSException(CS_ERROR_NULL_POINTER, "Cannot create ByteBuffer from null handle");
    }
}

CSNIOByteBuffer::~CSNIOByteBuffer() {
    if (handle) {
        cswift_nio_bytebuffer_destroy(handle);
    }
}

CSNIOByteBuffer::CSNIOByteBuffer(CSNIOByteBuffer&& other) noexcept : handle(other.handle) {
    other.handle = nullptr;
}

CSNIOByteBuffer& CSNIOByteBuffer::operator=(CSNIOByteBuffer&& other) noexcept {
    if (this != &other) {
        if (handle) {
            cswift_nio_bytebuffer_destroy(handle);
        }
        handle = other.handle;
        other.handle = nullptr;
    }
    return *this;
}

CSHandle CSNIOByteBuffer::getHandle() const { 
    return handle; 
}

int32_t CSNIOByteBuffer::readableBytes() const {
    if (!handle) return 0;
    return cswift_nio_bytebuffer_readable_bytes(handle);
}

int32_t CSNIOByteBuffer::writableBytes() const {
    if (!handle) return 0;
    return cswift_nio_bytebuffer_writable_bytes(handle);
}

int32_t CSNIOByteBuffer::capacity() const {
    if (!handle) return 0;
    return cswift_nio_bytebuffer_capacity(handle);
}

void CSNIOByteBuffer::clear() {
    if (!handle) return;
    cswift_nio_bytebuffer_clear(handle);
}

int32_t CSNIOByteBuffer::readerIndex() const {
    if (!handle) return 0;
    return cswift_nio_bytebuffer_reader_index(handle);
}

int32_t CSNIOByteBuffer::writerIndex() const {
    if (!handle) return 0;
    return cswift_nio_bytebuffer_writer_index(handle);
}

void CSNIOByteBuffer::moveReaderIndex(int32_t offset) {
    if (!handle) return;
    cswift_nio_bytebuffer_move_reader_index(handle, offset);
}

void CSNIOByteBuffer::moveWriterIndex(int32_t offset) {
    if (!handle) return;
    cswift_nio_bytebuffer_move_writer_index(handle, offset);
}

void CSNIOByteBuffer::discardReadBytes() {
    if (!handle) return;
    cswift_nio_bytebuffer_discard_read_bytes(handle);
}

void CSNIOByteBuffer::writeBytes(const void* data, size_t length) {
    if (!handle) {
        throw CSException(CS_ERROR_NULL_POINTER, "Failed to write bytes to buffer");
    }
    CSError error = cswift_nio_bytebuffer_write_bytes(handle, data, length);
    if (error != CS_SUCCESS) {
        throw CSException(error, "Failed to write bytes to buffer");
    }
}

void CSNIOByteBuffer::writeBytes(const std::vector<uint8_t>& data) {
    writeBytes(data.data(), data.size());
}

void CSNIOByteBuffer::writeString(const std::string& str) {
    writeBytes(str.data(), str.size());
}

void CSNIOByteBuffer::readBytes(void* data, size_t length) {
    if (!handle) {
        throw CSException(CS_ERROR_NULL_POINTER, "Failed to read bytes from buffer");
    }
    CSError error = cswift_nio_bytebuffer_read_bytes(handle, data, length);
    if (error != CS_SUCCESS) {
        throw CSException(error, "Failed to read bytes from buffer");
    }
}

size_t CSNIOByteBuffer::readBytes(std::vector<uint8_t>& data) {
    size_t toRead = std::min(data.size(), static_cast<size_t>(readableBytes()));
    readBytes(data.data(), toRead);
    return toRead;
}

void CSNIOByteBuffer::getBytes(int32_t index, void* data, size_t length) const {
    if (!handle) {
        throw CSException(CS_ERROR_NULL_POINTER, "Failed to get bytes from buffer");
    }
    CSError error = cswift_nio_bytebuffer_get_bytes(handle, index, data, length);
    if (error != CS_SUCCESS) {
        throw CSException(error, "Failed to get bytes from buffer");
    }
}

void CSNIOByteBuffer::setBytes(int32_t index, const void* data, size_t length) {
    if (!handle) {
        throw CSException(CS_ERROR_NULL_POINTER, "Failed to set bytes in buffer");
    }
    CSError error = cswift_nio_bytebuffer_set_bytes(handle, index, data, length);
    if (error != CS_SUCCESS) {
        throw CSException(error, "Failed to set bytes in buffer");
    }
}

std::pair<const void*, size_t> CSNIOByteBuffer::readPointer() const {
    if (!handle) return {nullptr, 0};
    size_t length = 0;
    const void* ptr = cswift_nio_bytebuffer_read_pointer(handle, &length);
    return {ptr, length};
}

std::pair<void*, size_t> CSNIOByteBuffer::writePointer() {
    if (!handle) return {nullptr, 0};
    size_t length = 0;
    void* ptr = cswift_nio_bytebuffer_write_pointer(handle, &length);
    return {ptr, length};
}



} // namespace cswift