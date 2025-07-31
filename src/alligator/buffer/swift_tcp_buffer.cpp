#include <alligator/buffer/swift_tcp_buffer.hpp>
#include <alligator/buffer/buffet_alligator.hpp>
#include <cstring>
#include <thread>
#include <chrono>

namespace alligator {

SwiftTcpBuffer::SwiftTcpBuffer(size_t capacity)
    : NICBuffer(capacity, NetworkProtocol::TCP) {
    
    // Use cswift for networking
    buffer_ = std::make_unique<cswift::CSSharedBuffer>(capacity, 64);
    data_ = buffer_->data<uint8_t>();
    buf_size_ = capacity;
    
    nic_features_ = static_cast<uint32_t>(NICFeatures::ZERO_COPY) | 
                    static_cast<uint32_t>(NICFeatures::KERNEL_BYPASS);
}

SwiftTcpBuffer::~SwiftTcpBuffer() {
    deallocate();
}

ChainBuffer* SwiftTcpBuffer::create_new(size_t size) {
    return get_buffet_alligator().allocate_buffer(size, BFType::TCP);
}

void SwiftTcpBuffer::clear(uint8_t value) {
    if (data_) {
        std::memset(data_, value, buf_size_);
    }
    next_offset_.store(0, std::memory_order_relaxed);
    rx_offset_.store(0, std::memory_order_relaxed);
    tx_offset_.store(0, std::memory_order_relaxed);
    
    // Clear receive queue
    std::lock_guard<std::mutex> lock(rx_mutex_);
    while (!rx_queue_.empty()) {
        rx_queue_.pop();
    }
}

void SwiftTcpBuffer::deallocate() {
    if (connection_) {
        connection_.reset();
    }
    
    if (listener_) {
        listener_.reset();
    }
    
    buffer_.reset();
    data_ = nullptr;
    
    state_.store(State::CANCELLED, std::memory_order_release);
}

uint8_t* SwiftTcpBuffer::data() {
    return data_;
}

const uint8_t* SwiftTcpBuffer::data() const {
    return data_;
}

std::span<uint8_t> SwiftTcpBuffer::get_span(size_t offset, size_t size) {
    if (offset >= buf_size_) {
        return std::span<uint8_t>();
    }
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : std::min(size, buf_size_ - offset);
    return std::span<uint8_t>(data() + offset, actual_size);
}

std::span<const uint8_t> SwiftTcpBuffer::get_span(const size_t& offset, const size_t& size) const {
    if (offset >= buf_size_) {
        return std::span<const uint8_t>();
    }
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : std::min(size, buf_size_ - offset);
    return std::span<const uint8_t>(data() + offset, actual_size);
}

bool SwiftTcpBuffer::bind(const NetworkEndpoint& endpoint) {
    try {
        // Create TCP parameters using cswift
        auto params = cswift::CSNWParameters::tcp();
        
        // Create listener using cswift
        listener_ = std::make_unique<cswift::CSNWListener>(endpoint.port, *params);
        
        // Set new connection handler
        listener_->setNewConnectionHandler([this](std::unique_ptr<cswift::CSNWConnection> new_connection) {
            if (!connection_) {
                connection_ = std::move(new_connection);
                setup_connection_handlers();
                connection_->start();
            }
        });
        
        // Set state handler
        listener_->setStateUpdateHandler([this](cswift::CSNWConnectionState state) {
            if (state == cswift::CSNWConnectionState::Ready) {
                state_.store(State::READY, std::memory_order_release);
            } else if (state == cswift::CSNWConnectionState::Failed) {
                state_.store(State::FAILED, std::memory_order_release);
            }
        });
        
        listener_->start();
        return true;
        
    } catch (const std::exception& e) {
        return false;
    }
}

bool SwiftTcpBuffer::connect(const NetworkEndpoint& endpoint) {
    try {
        // Create connection using cswift
        connection_ = std::make_unique<cswift::CSNWConnection>(endpoint.address, endpoint.port);
        state_.store(State::CONNECTING, std::memory_order_release);
        
        setup_connection_handlers();
        connection_->start();
        
        // Wait for connection to be ready (with timeout)
        auto start = std::chrono::steady_clock::now();
        while (state_.load(std::memory_order_acquire) == State::CONNECTING) {
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
                return false; // Timeout
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        return state_.load(std::memory_order_acquire) == State::READY;
        
    } catch (const std::exception& e) {
        return false;
    }
}

void SwiftTcpBuffer::setup_connection_handlers() {
    connection_->setStateUpdateHandler([this](cswift::CSNWConnectionState new_state) {
        handle_state_change(new_state);
        
        if (new_state == cswift::CSNWConnectionState::Ready) {
            start_receive();
        }
    });
}

void SwiftTcpBuffer::handle_state_change(cswift::CSNWConnectionState new_state) {
    switch (new_state) {
        case cswift::CSNWConnectionState::Preparing:
            state_.store(State::CONNECTING, std::memory_order_release);
            break;
        case cswift::CSNWConnectionState::Ready:
            state_.store(State::READY, std::memory_order_release);
            break;
        case cswift::CSNWConnectionState::Failed:
            state_.store(State::FAILED, std::memory_order_release);
            break;
        case cswift::CSNWConnectionState::Cancelled:
            state_.store(State::CANCELLED, std::memory_order_release);
            break;
        default:
            break;
    }
}

void SwiftTcpBuffer::start_receive() {
    connection_->receive(1, UINT32_MAX, [this](std::vector<uint8_t> received_data, bool is_complete, bool success) {
        if (success && !received_data.empty()) {
            size_t rx_off = rx_offset_.load(std::memory_order_acquire);
            size_t available = buf_size_ - rx_off;
            
            if (received_data.size() <= available) {
                // Copy data to internal buffer
                std::memcpy(data_ + rx_off, received_data.data(), received_data.size());
                
                rx_offset_.fetch_add(received_data.size(), std::memory_order_release);
                
                // Add to receive queue
                {
                    std::lock_guard<std::mutex> lock(rx_mutex_);
                    rx_queue_.push({rx_off, received_data.size()});
                }
                
                stats_.bytes_received.fetch_add(received_data.size(), std::memory_order_relaxed);
                stats_.packets_received.fetch_add(1, std::memory_order_relaxed);
            }
        }
        
        // Continue receiving if connection is still ready
        if (state_.load(std::memory_order_acquire) == State::READY && !is_complete) {
            start_receive();
        }
    });
}

ssize_t SwiftTcpBuffer::send(size_t offset, size_t size) {
    if (state_.load(std::memory_order_acquire) != State::READY || !connection_) {
        return -1;
    }
    
    if (offset + size > buf_size_) {
        size = buf_size_ - offset;
    }
    
    try {
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
            stats_.bytes_sent.fetch_add(size, std::memory_order_relaxed);
            stats_.packets_sent.fetch_add(1, std::memory_order_relaxed);
            return size;
        }
        return -1;
    } catch (const std::exception& e) {
        return -1;
    }
}

ssize_t SwiftTcpBuffer::receive(size_t offset, size_t size) {
    if (state_.load(std::memory_order_acquire) != State::READY || !connection_) {
        return -1;
    }
    
    // Check if we have data in the receive queue
    std::lock_guard<std::mutex> lock(rx_mutex_);
    if (rx_queue_.empty()) {
        return 0; // No data available
    }
    
    RxData rx_data = rx_queue_.front();
    rx_queue_.pop();
    
    // Copy to the specified offset
    size_t copy_size = std::min(size, rx_data.size);
    if (offset + copy_size <= buf_size_) {
        std::memcpy(data() + offset, data() + rx_data.offset, copy_size);
        return copy_size;
    }
    
    return -1;
}

ssize_t SwiftTcpBuffer::send_from(ICollectiveBuffer* src, size_t size, size_t src_offset) {
    if (!src || state_.load(std::memory_order_acquire) != State::READY || !connection_) {
        return -1;
    }
    
    try {
        // Zero-copy send using cswift
        const uint8_t* src_data = src->data() + src_offset;
        
        std::atomic<bool> send_complete{false};
        std::atomic<bool> send_success{false};
        
        connection_->send(src_data, size, [&send_complete, &send_success](bool success) {
            send_success.store(success);
            send_complete.store(true);
        });
        
        // Wait for send to complete
        while (!send_complete.load()) {
            std::this_thread::yield();
        }
        
        if (send_success.load()) {
            stats_.bytes_sent.fetch_add(size, std::memory_order_relaxed);
            stats_.packets_sent.fetch_add(1, std::memory_order_relaxed);
            return size;
        }
        return -1;
    } catch (const std::exception& e) {
        return -1;
    }
}

ssize_t SwiftTcpBuffer::receive_into(ICollectiveBuffer* dst, size_t size, size_t dst_offset) {
    if (!dst || state_.load(std::memory_order_acquire) != State::READY) {
        return -1;
    }
    
    // Check if we have data in the receive queue
    std::lock_guard<std::mutex> lock(rx_mutex_);
    if (rx_queue_.empty()) {
        return 0; // No data available
    }
    
    RxData rx_data = rx_queue_.front();
    rx_queue_.pop();
    
    // Zero-copy transfer to destination buffer
    size_t copy_size = std::min(size, rx_data.size);
    uint8_t* dst_data = dst->data() + dst_offset;
    
    if (dst_offset + copy_size <= dst->buf_size_) {
        std::memcpy(dst_data, data() + rx_data.offset, copy_size);
        return copy_size;
    }
    
    return -1;
}

BufferClaim SwiftTcpBuffer::get_rx(size_t size) {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    
    if (rx_queue_.empty()) {
        return BufferClaim{this, 0, 0, 0};
    }
    
    RxData rx_data = rx_queue_.front();
    
    if (size == 0 || rx_data.size == size) {
        rx_queue_.pop();
        return BufferClaim{this, rx_data.offset, rx_data.size, 0};
    }
    
    return BufferClaim{this, 0, 0, 0};
}

int SwiftTcpBuffer::poll(int timeout_ms) {
    // cswift handles events asynchronously
    // This is a no-op for compatibility
    return 0;
}

NICBuffer::NetworkStats SwiftTcpBuffer::get_stats() const {
    return NICBuffer::NetworkStats{
        stats_.bytes_sent.load(std::memory_order_relaxed),
        stats_.bytes_received.load(std::memory_order_relaxed),
        stats_.packets_sent.load(std::memory_order_relaxed),
        stats_.packets_received.load(std::memory_order_relaxed),
        stats_.errors.load(std::memory_order_relaxed),
        stats_.drops.load(std::memory_order_relaxed)
    };
}

} // namespace alligator
