#include <alligator/buffer/thunderbolt_dma_buffer.hpp>
#include <alligator/buffer/buffet_alligator.hpp>
#include <cstring>
#include <stdexcept>

namespace alligator {

ThunderboltDMABuffer::ThunderboltDMABuffer(size_t size, 
                                         const std::string& remote_host,
                                         uint16_t remote_port)
    : remote_host_(remote_host), remote_port_(remote_port) {
    
    buf_size_ = size;
    
#ifdef PSYNE_SWIFT_ENABLED
    try {
        // Create a CSSharedBuffer for the data storage
        swift_buffer_ = std::make_unique<cswift::CSSharedBuffer>(size, 64); // 64-byte aligned
        data_ = swift_buffer_->data<uint8_t>();
        
        // If remote endpoint specified, create Thunderbolt connection
        if (!remote_host.empty() && remote_port > 0) {
            connect(remote_host, remote_port);
        }
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Failed to create ThunderboltDMABuffer: ") + e.what());
    }
#else
    // Fallback to regular buffer
    fallback_buffer_.resize(size);
#endif
}

ThunderboltDMABuffer::~ThunderboltDMABuffer() {
    deallocate();
}

ChainBuffer* ThunderboltDMABuffer::create_new(size_t size) {
    return get_buffet_alligator().allocate_buffer(size, BFType::THUNDERBOLT_DMA);
}

void ThunderboltDMABuffer::clear(uint8_t value) {
#ifdef PSYNE_SWIFT_ENABLED
    if (data_) {
        std::memset(data_, value, buf_size_);
    }
#else
    std::fill(fallback_buffer_.begin(), fallback_buffer_.end(), value);
#endif
    next_offset_.store(0, std::memory_order_relaxed);
}

void ThunderboltDMABuffer::deallocate() {
#ifdef PSYNE_SWIFT_ENABLED
    network_.reset();
    swift_buffer_.reset();
    data_ = nullptr;
#else
    fallback_buffer_.clear();
    fallback_buffer_.shrink_to_fit();
#endif
    is_connected_ = false;
}

uint8_t* ThunderboltDMABuffer::data() {
#ifdef PSYNE_SWIFT_ENABLED
    return data_;
#else
    return fallback_buffer_.data();
#endif
}

const uint8_t* ThunderboltDMABuffer::data() const {
#ifdef PSYNE_SWIFT_ENABLED
    return data_;
#else
    return fallback_buffer_.data();
#endif
}

std::span<uint8_t> ThunderboltDMABuffer::get_span(size_t offset, size_t size) {
    if (offset >= buf_size_) {
        return std::span<uint8_t>();
    }
    
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : std::min(size, buf_size_ - offset);
    return std::span<uint8_t>(data() + offset, actual_size);
}

std::span<const uint8_t> ThunderboltDMABuffer::get_span(const size_t& offset, const size_t& size) const {
    if (offset >= buf_size_) {
        return std::span<const uint8_t>();
    }
    
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : std::min(size, buf_size_ - offset);
    return std::span<const uint8_t>(data() + offset, actual_size);
}

bool ThunderboltDMABuffer::connect(const std::string& host, uint16_t port) {
#ifdef PSYNE_SWIFT_ENABLED
    try {
        // Create Thunderbolt-optimized connection using cswift
        network_ = cswift::HighPerformanceNetwork::createThunderboltOptimized(host, port);
        if (network_) {
            is_connected_ = true;
            remote_host_ = host;
            remote_port_ = port;
            return true;
        }
    } catch (const std::exception& e) {
        // Connection failed
    }
#endif
    return false;
}

ssize_t ThunderboltDMABuffer::send(size_t offset, size_t size) {
#ifdef PSYNE_SWIFT_ENABLED
    if (!is_connected_ || !network_) {
        return -1;
    }
    
    if (offset + size > buf_size_) {
        size = buf_size_ - offset;
    }
    
    try {
        // Use cswift's zero-copy send
        size_t sent = network_->send(data_ + offset, size);
        bytes_sent_.fetch_add(sent, std::memory_order_relaxed);
        return static_cast<ssize_t>(sent);
    } catch (const std::exception& e) {
        return -1;
    }
#else
    return -1;
#endif
}

ssize_t ThunderboltDMABuffer::receive(size_t offset, size_t size) {
#ifdef PSYNE_SWIFT_ENABLED
    if (!is_connected_ || !network_) {
        return -1;
    }
    
    if (offset + size > buf_size_) {
        size = buf_size_ - offset;
    }
    
    try {
        // Use cswift's zero-copy receive
        size_t received = network_->receive(data_ + offset, size);
        bytes_received_.fetch_add(received, std::memory_order_relaxed);
        return static_cast<ssize_t>(received);
    } catch (const std::exception& e) {
        return -1;
    }
#else
    return -1;
#endif
}

} // namespace alligator