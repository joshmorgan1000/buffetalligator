#include <alligator/buffer/heap_buffer.hpp>
#include <alligator/buffer/buffet_alligator.hpp>
#include <algorithm>
#include <stdexcept>

namespace alligator {

HeapBuffer::HeapBuffer(size_t capacity) {
    buf_size_ = capacity;
    try {
        data_ = new uint8_t[buf_size_];
        allocation_counter_.fetch_add(buf_size_, std::memory_order_relaxed);
    } catch (const std::bad_alloc& e) {
        throw;
    }
    clear(0);
}

HeapBuffer::~HeapBuffer() {}

void HeapBuffer::clear(uint8_t value) {
    if (data_) std::fill(data_, data_ + buf_size_, value);
}

void HeapBuffer::deallocate() {
    if (data_) {
        delete[] data_;
        data_ = nullptr;
    }
    deallocation_counter_.fetch_add(buf_size_, std::memory_order_relaxed);
}

uint8_t* HeapBuffer::data() {
    return data_;
}

const uint8_t* HeapBuffer::data() const {
    return data_;
}

std::span<uint8_t> HeapBuffer::get_span(size_t offset, size_t size) {
    if (offset >= buf_size_) {
        throw std::out_of_range("Span offset out of bounds");
    }
    uint32_t span_size = size > 0 ? size : (buf_size_ - offset);
    if (offset + span_size > buf_size_) {
        throw std::out_of_range("Span size out of bounds");
    }
    return std::span<uint8_t>(data_ + offset, span_size);
}

std::span<const uint8_t> HeapBuffer::get_span(const size_t& offset, const size_t& size) const {
    if (offset >= buf_size_) {
        throw std::out_of_range("Span offset out of bounds");
    }
    uint32_t span_size = size > 0 ? size : (buf_size_ - offset);
    if (offset + span_size > buf_size_) {
        throw std::out_of_range("Span size out of bounds");
    }
    return std::span<const uint8_t>(data_ + offset, span_size);
}

ChainBuffer* HeapBuffer::create_new(size_t size) {
    return get_buffet_alligator().allocate_buffer(size, BFType::HEAP);
}

} // namespace alligator