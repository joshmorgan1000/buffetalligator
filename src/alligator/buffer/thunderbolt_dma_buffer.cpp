#include <alligator/buffer/thunderbolt_dma_buffer.hpp>
#include <alligator/buffer/buffet_alligator.hpp>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <chrono>

namespace alligator {

ThunderboltDMABuffer::ThunderboltDMABuffer(size_t size, 
                                         const std::string& remote_host,
                                         uint16_t remote_port)
    : remote_host_(remote_host), remote_port_(remote_port) {
    
    buf_size_ = size;
    
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
}

ThunderboltDMABuffer::~ThunderboltDMABuffer() {
    deallocate();
}

ChainBuffer* ThunderboltDMABuffer::create_new(size_t size) {
    return get_buffet_alligator().allocate_buffer(size, BFType::THUNDERBOLT_DMA);
}

void ThunderboltDMABuffer::clear(uint8_t value) {
    if (data_) {
        std::memset(data_, value, buf_size_);
    }
    next_offset_.store(0, std::memory_order_relaxed);
}

void ThunderboltDMABuffer::deallocate() {
    if (connection_) {
        connection_.reset();
    }
    if (listener_) {
        listener_.reset();
    }
    swift_buffer_.reset();
    data_ = nullptr;
    is_connected_ = false;
}

uint8_t* ThunderboltDMABuffer::data() {
    return data_;
}

const uint8_t* ThunderboltDMABuffer::data() const {
    return data_;
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
    try {
        // Create Thunderbolt-optimized connection using cswift
        // Create high-performance parameters for Thunderbolt
        auto params = cswift::CSNWParameters::highPerformance();
        params->setRequiredInterfaceType(cswift::CSNWInterfaceType::Thunderbolt);
        
        connection_ = std::make_unique<cswift::CSNWConnection>(host, port, *params);
        
        // Set up connection handler
        connection_->setStateUpdateHandler([this](cswift::CSNWConnectionState state) {
            if (state == cswift::CSNWConnectionState::Ready) {
                is_connected_ = true;
            } else if (state == cswift::CSNWConnectionState::Failed) {
                is_connected_ = false;
            }
        });
        
        connection_->start();
        
        // Wait for connection
        for (int i = 0; i < 50 && !is_connected_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (is_connected_) {
            remote_host_ = host;
            remote_port_ = port;
            return true;
        }
    } catch (const std::exception& e) {
        // Connection failed
    }
    return false;
}

ssize_t ThunderboltDMABuffer::send(size_t offset, size_t size) {
    if (!is_connected_ || !connection_) {
        return -1;
    }
    
    if (offset + size > buf_size_) {
        size = buf_size_ - offset;
    }
    
    try {
        // Use cswift's send
        std::atomic<bool> send_complete{false};
        std::atomic<bool> send_success{false};
        
        connection_->send(data_ + offset, size, [&send_complete, &send_success](bool success) {
            send_success.store(success);
            send_complete.store(true);
        });
        
        // Wait for send to complete
        while (!send_complete.load()) {
            std::this_thread::yield();
        }
        
        if (send_success.load()) {
            bytes_sent_.fetch_add(size, std::memory_order_relaxed);
            return static_cast<ssize_t>(size);
        }
        return -1;
    } catch (const std::exception& e) {
        return -1;
    }
}

ssize_t ThunderboltDMABuffer::receive(size_t offset, size_t size) {
    if (!is_connected_ || !connection_) {
        return -1;
    }
    
    if (offset + size > buf_size_) {
        size = buf_size_ - offset;
    }
    
    try {
        // Use cswift's receive
        std::atomic<bool> receive_complete{false};
        std::atomic<ssize_t> bytes_received_local{0};
        
        connection_->receive(1, size, [this, offset, &receive_complete, &bytes_received_local](std::vector<uint8_t> data, bool is_complete, bool success) {
            if (success && !data.empty()) {
                size_t copy_size = std::min(data.size(), buf_size_ - offset);
                std::memcpy(data_ + offset, data.data(), copy_size);
                bytes_received_local.store(copy_size);
            }
            receive_complete.store(true);
        });
        
        // Wait for receive to complete
        while (!receive_complete.load()) {
            std::this_thread::yield();
        }
        
        ssize_t received = bytes_received_local.load();
        if (received > 0) {
            bytes_received_.fetch_add(received, std::memory_order_relaxed);
        }
        return received;
    } catch (const std::exception& e) {
        return -1;
    }
}

} // namespace alligator