#include <alligator/buffer/swift_udp_buffer.hpp>
#include <alligator/buffer/buffet_alligator.hpp>
#include <cstring>
#include <cswift/cswift.hpp>
#include <thread>
#include <chrono>

namespace alligator {

SwiftUdpBuffer::SwiftUdpBuffer(size_t capacity)
    : NICBuffer(capacity, NetworkProtocol::UDP) {
    
    // Use cswift for networking
    buffer_ = std::make_unique<cswift::CSSharedBuffer>(capacity, 64);
    data_ = buffer_->data<uint8_t>();
    buf_size_ = capacity;
    
    nic_features_ = static_cast<uint32_t>(NICFeatures::ZERO_COPY) | 
                    static_cast<uint32_t>(NICFeatures::KERNEL_BYPASS);
}

SwiftUdpBuffer::~SwiftUdpBuffer() {
    deallocate();
}

ChainBuffer* SwiftUdpBuffer::create_new(size_t size) {
    return get_buffet_alligator().allocate_buffer(size, BFType::UDP);
}

void SwiftUdpBuffer::clear(uint8_t value) {
    if (data_) {
        std::memset(data_, value, buf_size_);
    }
    next_offset_.store(0, std::memory_order_relaxed);
    rx_offset_.store(0, std::memory_order_relaxed);
    tx_offset_.store(0, std::memory_order_relaxed);
}

void SwiftUdpBuffer::deallocate() {
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

uint8_t* SwiftUdpBuffer::data() {
    return data_;
}

const uint8_t* SwiftUdpBuffer::data() const {
    return data_;
}

std::span<uint8_t> SwiftUdpBuffer::get_span(size_t offset, size_t size) {
    if (offset >= buf_size_) {
        return std::span<uint8_t>();
    }
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : std::min(size, buf_size_ - offset);
    return std::span<uint8_t>(data_ + offset, actual_size);
}

std::span<const uint8_t> SwiftUdpBuffer::get_span(const size_t& offset, const size_t& size) const {
    if (offset >= buf_size_) {
        return std::span<const uint8_t>();
    }
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : std::min(size, buf_size_ - offset);
    return std::span<const uint8_t>(data() + offset, actual_size);
}

bool SwiftUdpBuffer::bind(const NetworkEndpoint& endpoint) {
    try {
        // Create UDP parameters using cswift
        auto params = cswift::CSNWParameters::udp();
        
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

bool SwiftUdpBuffer::connect(const NetworkEndpoint& endpoint) {
    try {
        // Create UDP parameters using cswift
        auto params = cswift::CSNWParameters::udp();
        
        // Create connection using cswift
        connection_ = std::make_unique<cswift::CSNWConnection>(endpoint.address, endpoint.port, *params);
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

ssize_t SwiftUdpBuffer::send(size_t offset, size_t size) {
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

ssize_t SwiftUdpBuffer::sendto(const void* data, size_t size, const NetworkEndpoint& endpoint) {
    // For UDP with cswift, we need to create a connection per endpoint
    // For now, use the existing connection if available
    if (!connection_) {
        return -1;
    }
    
    return send(reinterpret_cast<const uint8_t*>(data) - data_, size);
}

ssize_t SwiftUdpBuffer::receive(size_t offset, size_t size) {
    if (state_.load(std::memory_order_acquire) != State::READY || !connection_) {
        return -1;
    }
    
    // Check if we have data in queue
    std::unique_lock<std::mutex> lock(rx_mutex_);
    if (rx_queue_.empty()) {
        return 0; // No data available
    }
    
    RxData packet = rx_queue_.front();
    rx_queue_.pop();
    lock.unlock();
    
    // Copy data
    size_t to_copy = std::min(size, packet.size);
    if (packet.offset + to_copy <= buf_size_ && offset + to_copy <= buf_size_) {
        std::memcpy(data_ + offset,
                   data_ + packet.offset,
                   to_copy);
    }
    
    // UDP packet handled
    
    return static_cast<ssize_t>(to_copy);
}

ssize_t SwiftUdpBuffer::recvfrom(void* dst_data, size_t size, NetworkEndpoint& sender) {
    if (state_.load(std::memory_order_acquire) != State::READY || !connection_) {
        return -1;
    }
    
    // Check if we have data in queue
    std::unique_lock<std::mutex> lock(rx_mutex_);
    if (rx_queue_.empty()) {
        return 0; // No data available
    }
    
    RxData packet = rx_queue_.front();
    rx_queue_.pop();
    lock.unlock();
    
    // Copy data
    size_t to_copy = std::min(size, packet.size);
    if (packet.offset + to_copy <= buf_size_) {
        std::memcpy(dst_data,
                   data_ + packet.offset,
                   to_copy);
    }
    
    // For UDP with cswift, we don't have sender info in this implementation
    sender = NetworkEndpoint{};
    
    return static_cast<ssize_t>(to_copy);
}

ssize_t SwiftUdpBuffer::send_from(ICollectiveBuffer* src, size_t size, size_t src_offset) {
    if (state_.load(std::memory_order_acquire) != State::READY || !connection_ || !src) {
        return -1;
    }
    
    // For zero-copy, we'd need to create dispatch_data from the source buffer
    // For now, copy to our buffer first
    // Check bounds
    if (size > buf_size_) {
        return -1;
    }
    
    std::memcpy(data_, src->data() + src_offset, size);
    return send(0, size);
}

ssize_t SwiftUdpBuffer::receive_into(ICollectiveBuffer* dst, size_t size, size_t dst_offset) {
    if (state_.load(std::memory_order_acquire) != State::READY || !connection_ || !dst) {
        return -1;
    }
    
    // Try to receive
    ssize_t bytes_received = receive(0, size);
    if (bytes_received > 0) {
        // Check if destination is a GPU buffer
        if (auto gpu_buffer = dynamic_cast<GPUBuffer*>(dst)) {
            // Upload to GPU
            try {
                if (gpu_buffer->upload(data_, bytes_received, dst_offset)) {
                    return bytes_received;
                }
            } catch (const std::exception& e) {
                return -1;
            }
        } else {
            // Regular copy
            if (dst_offset + bytes_received <= dst->buf_size_) {
                std::memcpy(dst->data() + dst_offset, data_, bytes_received);
                return bytes_received;
            }
        }
    }
    
    return bytes_received;
}

BufferClaim SwiftUdpBuffer::get_rx(size_t size) {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    
    if (rx_queue_.empty()) {
        return BufferClaim{this, 0, 0, 0};
    }
    
    RxData& packet = rx_queue_.front();
    if (size == 0 || packet.size == size) {
        BufferClaim claim{this, packet.offset, packet.size, 
                         static_cast<int64_t>(buf_size_ - packet.offset - packet.size)};
        rx_queue_.pop();
        return claim;
    }
    
    return BufferClaim{this, 0, 0, 0};
}

int SwiftUdpBuffer::poll(int timeout_ms) {
    // Network.framework handles events internally
    // This is a simplified implementation
    return rx_queue_.empty() ? 0 : 1;
}

SwiftUdpBuffer::NetworkStats SwiftUdpBuffer::get_stats() const {
    return NetworkStats{
        stats_.bytes_sent.load(std::memory_order_relaxed),
        stats_.bytes_received.load(std::memory_order_relaxed),
        stats_.packets_sent.load(std::memory_order_relaxed),
        stats_.packets_received.load(std::memory_order_relaxed),
        stats_.errors.load(std::memory_order_relaxed),
        stats_.drops.load(std::memory_order_relaxed)
    };
}

// Multicast and broadcast support would require additional implementation

void SwiftUdpBuffer::setup_connection_handlers() {
    connection_->setStateUpdateHandler([this](cswift::CSNWConnectionState new_state) {
        handle_state_change(new_state);
        
        if (new_state == cswift::CSNWConnectionState::Ready) {
            start_receive();
        }
    });
}

void SwiftUdpBuffer::handle_state_change(cswift::CSNWConnectionState new_state) {
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

void SwiftUdpBuffer::start_receive() {
    connection_->receive(1, UINT32_MAX, [this](std::vector<uint8_t> received_data, bool is_complete, bool success) {
        if (success && !received_data.empty()) {
            size_t offset = rx_offset_.load(std::memory_order_relaxed);
            size_t data_size = received_data.size();
            
            if (offset + data_size > buf_size_) {
                offset = 0;
                rx_offset_.store(0, std::memory_order_relaxed);
            }
            
            // Copy data to our buffer
            if (offset + data_size <= buf_size_) {
                std::memcpy(data_ + offset, received_data.data(), data_size);
                
                // Add to receive queue
                {
                    std::lock_guard<std::mutex> lock(rx_mutex_);
                    rx_queue_.push(RxData{offset, data_size, NetworkEndpoint{}});
                }
                
                rx_offset_.fetch_add(data_size, std::memory_order_relaxed);
                
                stats_.bytes_received.fetch_add(data_size, std::memory_order_relaxed);
                stats_.packets_received.fetch_add(1, std::memory_order_relaxed);
            }
        }
        
        // Continue receiving if connection is still ready
        if (state_.load(std::memory_order_acquire) == State::READY && !is_complete) {
            start_receive();
        }
    });
}

} // namespace alligator