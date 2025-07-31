#include <alligator/buffer/swift_quic_buffer.hpp>
#include <alligator/buffer/buffet_alligator.hpp>
#include <cswift/cswift.hpp>
#include <cstring>
#include <chrono>
#include <thread>

namespace alligator {

SwiftQuicBuffer::SwiftQuicBuffer(size_t capacity)
    : NICBuffer(capacity, NetworkProtocol::QUIC) {
    
    // Use cswift for networking
    buffer_ = std::make_unique<cswift::CSSharedBuffer>(capacity, 64);
    data_ = buffer_->data<uint8_t>();
    buf_size_ = capacity;
    
    nic_features_ = static_cast<uint32_t>(NICFeatures::ZERO_COPY) | 
                    static_cast<uint32_t>(NICFeatures::KERNEL_BYPASS);
}

SwiftQuicBuffer::~SwiftQuicBuffer() {
    deallocate();
}

ChainBuffer* SwiftQuicBuffer::create_new(size_t size) {
    return get_buffet_alligator().allocate_buffer(size, BFType::QUIC);
}

void SwiftQuicBuffer::clear(uint8_t value) {
    if (data_) {
        std::memset(data_, value, buf_size_);
    }
    next_offset_.store(0, std::memory_order_relaxed);
    rx_offset_.store(0, std::memory_order_relaxed);
    tx_offset_.store(0, std::memory_order_relaxed);
    
    // Clear streams
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        streams_.clear();
    }
}

void SwiftQuicBuffer::deallocate() {
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

uint8_t* SwiftQuicBuffer::data() {
    return data_;
}

const uint8_t* SwiftQuicBuffer::data() const {
    return data_;
}

std::span<uint8_t> SwiftQuicBuffer::get_span(size_t offset, size_t size) {
    if (offset >= buf_size_) {
        return std::span<uint8_t>();
    }
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : std::min(size, buf_size_ - offset);
    return std::span<uint8_t>(data_ + offset, actual_size);
}

std::span<const uint8_t> SwiftQuicBuffer::get_span(const size_t& offset, const size_t& size) const {
    if (offset >= buf_size_) {
        return std::span<const uint8_t>();
    }
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : std::min(size, buf_size_ - offset);
    return std::span<const uint8_t>(data_ + offset, actual_size);
}

bool SwiftQuicBuffer::bind(const NetworkEndpoint& endpoint) {
    try {
        // Note: QUIC support would need to be added to cswift
        // For now, use TCP as a fallback with similar semantics
        auto params = cswift::CSNWParameters::tcp();
        
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
        listener_->setStateUpdateHandler([this](cswift::CSNWListenerState state) {
            if (state == cswift::CSNWListenerState::Ready) {
                state_.store(State::READY, std::memory_order_release);
            } else if (state == cswift::CSNWListenerState::Failed) {
                state_.store(State::FAILED, std::memory_order_release);
            }
        });
        
        listener_->start();
        return true;
        
    } catch (const std::exception& e) {
        return false;
    }
}

bool SwiftQuicBuffer::connect(const NetworkEndpoint& endpoint) {
    try {
        // Note: QUIC support would need to be added to cswift
        // For now, use TCP as a fallback with similar semantics
        auto params = cswift::CSNWParameters::tcp();
        
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

void SwiftQuicBuffer::setup_connection_handlers() {
    connection_->setStateUpdateHandler([this](cswift::CSNWConnectionState new_state) {
        handle_state_change(new_state);
        
        if (new_state == cswift::CSNWConnectionState::Ready) {
            start_receive();
        }
    });
}

void SwiftQuicBuffer::handle_state_change(cswift::CSNWConnectionState new_state) {
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

void SwiftQuicBuffer::start_receive() {
    connection_->receive(1, UINT32_MAX, [this](const uint8_t* data, size_t size, bool is_complete) {
        if (data && size > 0) {
            // For QUIC simulation, treat the first 8 bytes as stream ID
            uint64_t stream_id = 0;
            if (size >= 8) {
                std::memcpy(&stream_id, data, sizeof(uint64_t));
                data += 8;
                size -= 8;
            }
            
            // Store stream data
            {
                std::lock_guard<std::mutex> lock(streams_mutex_);
                auto& stream = streams_[stream_id];
                stream.stream_id = stream_id;
                
                // Append data to stream buffer
                stream.data.insert(stream.data.end(), data, data + size);
                
                if (is_complete) {
                    stream.fin_received = true;
                }
                
                stream.last_activity = std::chrono::steady_clock::now();
            }
            
            stats_.bytes_received.fetch_add(size, std::memory_order_relaxed);
            stats_.packets_received.fetch_add(1, std::memory_order_relaxed);
        }
        
        // Continue receiving if connection is still ready
        if (state_.load(std::memory_order_acquire) == State::READY && !is_complete) {
            start_receive();
        }
    });
}

uint64_t SwiftQuicBuffer::create_stream(StreamType type) {
    static std::atomic<uint64_t> next_stream_id{0};
    uint64_t stream_id = next_stream_id.fetch_add(2); // Even for client-initiated
    
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto& stream = streams_[stream_id];
    stream.stream_id = stream_id;
    stream.type = type;
    stream.state = QuicStream::StreamState::OPEN;
    stream.last_activity = std::chrono::steady_clock::now();
    
    return stream_id;
}

bool SwiftQuicBuffer::close_stream(uint64_t stream_id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto it = streams_.find(stream_id);
    if (it != streams_.end()) {
        it->second.state = QuicStream::StreamState::CLOSED;
        return true;
    }
    return false;
}

ssize_t SwiftQuicBuffer::send_stream(uint64_t stream_id, size_t offset, size_t size) {
    if (state_.load(std::memory_order_acquire) != State::READY || !connection_) {
        return -1;
    }
    
    if (offset + size > buf_size_) {
        size = buf_size_ - offset;
    }
    
    try {
        // Prepend stream ID to data
        std::vector<uint8_t> packet;
        packet.reserve(8 + size);
        
        // Add stream ID
        const uint8_t* stream_id_bytes = reinterpret_cast<const uint8_t*>(&stream_id);
        packet.insert(packet.end(), stream_id_bytes, stream_id_bytes + 8);
        
        // Add data
        packet.insert(packet.end(), data_ + offset, data_ + offset + size);
        
        connection_->send(packet.data(), packet.size(), true);
        
        stats_.bytes_sent.fetch_add(size, std::memory_order_relaxed);
        stats_.packets_sent.fetch_add(1, std::memory_order_relaxed);
        
        return size;
    } catch (const std::exception& e) {
        return -1;
    }
}

ssize_t SwiftQuicBuffer::receive_stream(uint64_t stream_id, size_t offset, size_t size) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end() || it->second.data.empty()) {
        return 0; // No data available
    }
    
    auto& stream = it->second;
    size_t available = stream.data.size();
    size_t copy_size = std::min(size, available);
    
    if (offset + copy_size <= buf_size_) {
        std::memcpy(data_ + offset, stream.data.data(), copy_size);
        
        // Remove consumed data
        stream.data.erase(stream.data.begin(), stream.data.begin() + copy_size);
        stream.last_activity = std::chrono::steady_clock::now();
        
        return copy_size;
    }
    
    return -1;
}

std::optional<QuicStreamInfo> SwiftQuicBuffer::get_stream_info(uint64_t stream_id) const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto it = streams_.find(stream_id);
    if (it != streams_.end()) {
        QuicStreamInfo info;
        info.stream_id = it->second.stream_id;
        info.bytes_sent = it->second.bytes_sent;
        info.bytes_received = it->second.data.size();
        info.is_open = (it->second.state == QuicStream::StreamState::OPEN);
        info.fin_sent = it->second.fin_sent;
        info.fin_received = it->second.fin_received;
        return info;
    }
    return std::nullopt;
}

bool SwiftQuicBuffer::set_stream_priority(uint64_t stream_id, uint8_t priority) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto it = streams_.find(stream_id);
    if (it != streams_.end()) {
        it->second.priority = priority;
        return true;
    }
    return false;
}

bool SwiftQuicBuffer::enable_0rtt() {
    // 0-RTT would be enabled via connection parameters in a real QUIC implementation
    return false;
}

bool SwiftQuicBuffer::migrate_connection(const NetworkEndpoint& new_endpoint) {
    // Connection migration would require QUIC-specific implementation
    return false;
}

ssize_t SwiftQuicBuffer::send(size_t offset, size_t size) {
    // For backward compatibility, send on stream 0
    return send_stream(0, offset, size);
}

ssize_t SwiftQuicBuffer::receive(size_t offset, size_t size) {
    // For backward compatibility, receive from any stream
    std::lock_guard<std::mutex> lock(streams_mutex_);
    for (auto& [stream_id, stream] : streams_) {
        if (!stream.data.empty()) {
            size_t copy_size = std::min(size, stream.data.size());
            if (offset + copy_size <= buf_size_) {
                std::memcpy(data_ + offset, stream.data.data(), copy_size);
                stream.data.erase(stream.data.begin(), stream.data.begin() + copy_size);
                return copy_size;
            }
        }
    }
    return 0;
}

BufferClaim SwiftQuicBuffer::get_rx(size_t size) {
    // Check if any stream has data
    std::lock_guard<std::mutex> lock(streams_mutex_);
    for (auto& [stream_id, stream] : streams_) {
        if (!stream.data.empty() && (size == 0 || stream.data.size() >= size)) {
            size_t claim_size = (size == 0) ? stream.data.size() : size;
            
            // Copy to buffer for claim
            size_t rx_off = rx_offset_.load(std::memory_order_acquire);
            if (rx_off + claim_size <= buf_size_) {
                std::memcpy(data_ + rx_off, stream.data.data(), claim_size);
                stream.data.erase(stream.data.begin(), stream.data.begin() + claim_size);
                
                rx_offset_.fetch_add(claim_size, std::memory_order_release);
                
                return BufferClaim{this, rx_off, claim_size, stream_id};
            }
        }
    }
    
    return BufferClaim{this, 0, 0, 0};
}

int SwiftQuicBuffer::poll(int timeout_ms) {
    // cswift handles events asynchronously
    return 0;
}

SwiftQuicBuffer::NetworkStats SwiftQuicBuffer::get_stats() const {
    return NetworkStats{
        stats_.bytes_sent.load(std::memory_order_relaxed),
        stats_.bytes_received.load(std::memory_order_relaxed),
        stats_.packets_sent.load(std::memory_order_relaxed),
        stats_.packets_received.load(std::memory_order_relaxed),
        stats_.errors.load(std::memory_order_relaxed),
        stats_.drops.load(std::memory_order_relaxed)
    };
}

} // namespace alligator