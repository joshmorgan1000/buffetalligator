#include <alligator/buffer/asio_quic_buffer.hpp>
#include <alligator/buffer/buffet_alligator.hpp>
#include <cstring>
#include <future>

namespace alligator {

// Simple QUIC packet structure (placeholder)
struct QuicHeader {
    uint8_t flags;
    uint32_t stream_id;
    uint64_t offset;
    uint16_t length;
} __attribute__((packed));

AsioQuicBuffer::AsioQuicBuffer(size_t capacity) 
    : NICBuffer(capacity, NetworkProtocol::QUIC) {
    internal_buffer_.resize(capacity);
    nic_features_ = static_cast<uint32_t>(NICFeatures::ZERO_COPY) | 
                    static_cast<uint32_t>(NICFeatures::KERNEL_BYPASS);
}

AsioQuicBuffer::~AsioQuicBuffer() {
    stop_io_service();
}

ChainBuffer* AsioQuicBuffer::create_new(size_t size) {
    return get_buffet_alligator().allocate_buffer(size, BFType::QUIC);
}

void AsioQuicBuffer::clear(uint8_t value) {
    std::memset(internal_buffer_.data(), value, internal_buffer_.size());
    next_offset_.store(0, std::memory_order_relaxed);
    rx_offset_.store(0, std::memory_order_relaxed);
    tx_offset_.store(0, std::memory_order_relaxed);
    
    // Clear streams
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        streams_.clear();
    }
}

void AsioQuicBuffer::deallocate() {
    stop_io_service();
    internal_buffer_.clear();
    internal_buffer_.shrink_to_fit();
}

uint8_t* AsioQuicBuffer::data() {
    return internal_buffer_.data();
}

const uint8_t* AsioQuicBuffer::data() const {
    return internal_buffer_.data();
}

std::span<uint8_t> AsioQuicBuffer::get_span(size_t offset, size_t size) {
    if (offset >= buf_size_) {
        return std::span<uint8_t>();
    }
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : std::min(size, buf_size_ - offset);
    return std::span<uint8_t>(internal_buffer_.data() + offset, actual_size);
}

std::span<const uint8_t> AsioQuicBuffer::get_span(const size_t& offset, const size_t& size) const {
    if (offset >= buf_size_) {
        return std::span<const uint8_t>();
    }
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : std::min(size, buf_size_ - offset);
    return std::span<const uint8_t>(internal_buffer_.data() + offset, actual_size);
}

bool AsioQuicBuffer::bind(const NetworkEndpoint& endpoint) {
    try {
        socket_ = std::make_unique<boost::asio::ip::udp::socket>(io_context_);
        boost::asio::ip::udp::endpoint asio_endpoint(
            boost::asio::ip::make_address(endpoint.address), 
            endpoint.port
        );
        socket_->open(asio_endpoint.protocol());
        socket_->bind(asio_endpoint);
        
        connection_state_.store(ConnectionState::IDLE, std::memory_order_release);
        
        start_io_service();
        
        // Start receive coroutine
        boost::asio::co_spawn(io_context_, receive_coroutine(), boost::asio::detached);
        
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool AsioQuicBuffer::connect(const NetworkEndpoint& endpoint) {
    try {
        if (!socket_) {
            socket_ = std::make_unique<boost::asio::ip::udp::socket>(io_context_);
            socket_->open(boost::asio::ip::udp::v4());
        }
        
        remote_endpoint_ = boost::asio::ip::udp::endpoint(
            boost::asio::ip::make_address(endpoint.address), 
            endpoint.port
        );
        
        connection_state_.store(ConnectionState::HANDSHAKING, std::memory_order_release);
        
        start_io_service();
        
        // Start handshake coroutine
        boost::asio::co_spawn(io_context_, handshake_coroutine(), boost::asio::detached);
        
        // Wait for connection (simplified - real QUIC would be async)
        for (int i = 0; i < 50 && connection_state_ == ConnectionState::HANDSHAKING; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        return connection_state_ == ConnectionState::CONNECTED;
    } catch (const std::exception&) {
        return false;
    }
}

ssize_t AsioQuicBuffer::send(size_t offset, size_t size) {
    // Send on stream 0 by default
    return send_stream(0, offset, size, false);
}

ssize_t AsioQuicBuffer::send_stream(uint32_t stream_id, size_t offset, size_t size, bool fin) {
    if (!socket_ || !socket_->is_open() || connection_state_ != ConnectionState::CONNECTED) {
        return -1;
    }
    
    try {
        // Create QUIC packet with header
        auto header = create_quic_header(stream_id, offset, size, fin);
        
        // Combine header and data
        std::vector<uint8_t> packet;
        packet.reserve(header.size() + size);
        packet.insert(packet.end(), header.begin(), header.end());
        packet.insert(packet.end(), 
                     internal_buffer_.data() + offset, 
                     internal_buffer_.data() + offset + size);
        
        boost::system::error_code ec;
        size_t bytes_sent = socket_->send_to(
            boost::asio::buffer(packet),
            remote_endpoint_,
            0,
            ec
        );
        
        return ec ? -1 : static_cast<ssize_t>(size); // Return data size, not packet size
    } catch (const std::exception&) {
        return -1;
    }
}

ssize_t AsioQuicBuffer::receive(size_t offset, size_t size) {
    // Receive from any stream
    return receive_stream(0, offset, size);
}

ssize_t AsioQuicBuffer::receive_stream(uint32_t stream_id, size_t offset, size_t size) {
    if (!socket_ || !socket_->is_open()) {
        return -1;
    }
    
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end() || it->second.data.empty()) {
        return 0; // No data available for this stream
    }
    
    QuicStream& stream = it->second;
    size_t available = stream.data.size() - stream.offset;
    size_t to_copy = std::min(size, available);
    
    if (to_copy > 0) {
        std::memcpy(internal_buffer_.data() + offset, 
                   stream.data.data() + stream.offset, 
                   to_copy);
        stream.offset += to_copy;
        
        // Clean up if all data consumed
        if (stream.offset >= stream.data.size()) {
            streams_.erase(it);
        }
    }
    
    return static_cast<ssize_t>(to_copy);
}

ssize_t AsioQuicBuffer::send_from(ICollectiveBuffer* src, size_t size, size_t src_offset) {
    if (!socket_ || !socket_->is_open() || !src || connection_state_ != ConnectionState::CONNECTED) {
        return -1;
    }
    
    try {
        // Create QUIC packet with header
        auto header = create_quic_header(0, src_offset, size, false);
        
        // For zero-copy, we'd need scatter-gather IO
        // For now, copy to temporary buffer
        std::vector<uint8_t> packet;
        packet.reserve(header.size() + size);
        packet.insert(packet.end(), header.begin(), header.end());
        packet.insert(packet.end(), 
                     src->data() + src_offset, 
                     src->data() + src_offset + size);
        
        boost::system::error_code ec;
        size_t bytes_sent = socket_->send_to(
            boost::asio::buffer(packet),
            remote_endpoint_,
            0,
            ec
        );
        
        return ec ? -1 : static_cast<ssize_t>(size);
    } catch (const std::exception&) {
        return -1;
    }
}

ssize_t AsioQuicBuffer::receive_into(ICollectiveBuffer* dst, size_t size, size_t dst_offset) {
    if (!socket_ || !socket_->is_open() || !dst) {
        return -1;
    }
    
    // For GPU buffers, receive into staging buffer first
    if (auto* gpu_buffer = dynamic_cast<GPUBuffer*>(dst)) {
        if (!gpu_buffer->is_local()) {
            ssize_t bytes_received = receive(0, size);
            if (bytes_received > 0) {
                if (gpu_buffer->upload(internal_buffer_.data(), bytes_received, dst_offset)) {
                    return bytes_received;
                }
            }
            return -1;
        }
    }
    
    // For local buffers, receive from streams
    return receive_stream(0, dst_offset, size);
}

BufferClaim AsioQuicBuffer::get_rx(size_t size) {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    if (rx_queue_.empty()) {
        return BufferClaim{this, 0, 0, 0};
    }
    
    // Process first packet in queue
    QuicPacket packet = rx_queue_.front();
    rx_queue_.pop();
    
    // Process the packet data
    process_quic_packet(packet.data, packet.sender);
    
    // Return claim for any available stream data
    std::lock_guard<std::mutex> stream_lock(streams_mutex_);
    for (auto& [stream_id, stream] : streams_) {
        if (!stream.data.empty()) {
            size_t available = stream.data.size() - stream.offset;
            if (size == 0 || available >= size) {
                return BufferClaim{this, stream.offset, available, 
                                  static_cast<int64_t>(buf_size_ - stream.offset - available)};
            }
        }
    }
    
    return BufferClaim{this, 0, 0, 0};
}

int AsioQuicBuffer::poll(int timeout_ms) {
    if (!running_) {
        return 0;
    }
    
    io_context_.run_for(std::chrono::milliseconds(timeout_ms));
    return 1;
}

AsioQuicBuffer::NetworkStats AsioQuicBuffer::get_stats() const {
    return NetworkStats{0, 0, 0, 0, 0, 0};
}

uint32_t AsioQuicBuffer::create_stream() {
    uint32_t stream_id = next_stream_id_.fetch_add(4, std::memory_order_relaxed);
    
    std::lock_guard<std::mutex> lock(streams_mutex_);
    streams_[stream_id] = QuicStream{stream_id, {}, 0, false};
    
    return stream_id;
}

void AsioQuicBuffer::close_stream(uint32_t stream_id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    streams_.erase(stream_id);
}

void AsioQuicBuffer::set_0rtt(bool enable) {
    // Placeholder for 0-RTT configuration
}

void AsioQuicBuffer::set_congestion_control(const std::string& algorithm) {
    // Placeholder for congestion control configuration
}

void AsioQuicBuffer::start_io_service() {
    if (!running_.exchange(true)) {
        io_thread_ = std::thread([this]() {
            io_context_.run();
        });
    }
}

void AsioQuicBuffer::stop_io_service() {
    if (running_.exchange(false)) {
        connection_state_.store(ConnectionState::CLOSED, std::memory_order_release);
        io_context_.stop();
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
    }
}

boost::asio::awaitable<void> AsioQuicBuffer::receive_coroutine() {
    try {
        std::vector<uint8_t> recv_buffer(65535); // Max UDP packet size
        
        while (socket_ && socket_->is_open() && running_) {
            boost::asio::ip::udp::endpoint sender_endpoint;
            
            size_t bytes_received = co_await socket_->async_receive_from(
                boost::asio::buffer(recv_buffer),
                sender_endpoint,
                boost::asio::use_awaitable
            );
            
            if (bytes_received > 0) {
                // Queue the packet for processing
                std::lock_guard<std::mutex> lock(rx_mutex_);
                rx_queue_.push(QuicPacket{
                    sender_endpoint,
                    std::vector<uint8_t>(recv_buffer.begin(), recv_buffer.begin() + bytes_received)
                });
            }
        }
    } catch (const std::exception&) {
        // Socket closed or error occurred
    }
    co_return;
}

boost::asio::awaitable<void> AsioQuicBuffer::handshake_coroutine() {
    try {
        // Simplified handshake - just send an initial packet
        std::vector<uint8_t> handshake_packet = {0x01, 0x00, 0x00, 0x00}; // Version 1
        
        co_await socket_->async_send_to(
            boost::asio::buffer(handshake_packet),
            remote_endpoint_,
            boost::asio::use_awaitable
        );
        
        // In real QUIC, we'd wait for handshake response
        // For now, just mark as connected
        connection_state_.store(ConnectionState::CONNECTED, std::memory_order_release);
        
    } catch (const std::exception&) {
        connection_state_.store(ConnectionState::CLOSED, std::memory_order_release);
    }
    co_return;
}

void AsioQuicBuffer::process_quic_packet(const std::vector<uint8_t>& packet, 
                                        const boost::asio::ip::udp::endpoint& sender) {
    if (packet.size() < sizeof(QuicHeader)) {
        return; // Invalid packet
    }
    
    const QuicHeader* header = reinterpret_cast<const QuicHeader*>(packet.data());
    size_t data_offset = sizeof(QuicHeader);
    
    if (packet.size() < data_offset + header->length) {
        return; // Invalid packet length
    }
    
    // Store stream data
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto& stream = streams_[header->stream_id];
    stream.stream_id = header->stream_id;
    
    // Append data to stream buffer
    stream.data.insert(stream.data.end(),
                      packet.begin() + data_offset,
                      packet.begin() + data_offset + header->length);
    
    if (header->flags & 0x01) { // FIN flag
        stream.fin_received = true;
    }
}

std::vector<uint8_t> AsioQuicBuffer::create_quic_header(uint32_t stream_id, size_t offset, 
                                                       size_t length, bool fin) {
    std::vector<uint8_t> header(sizeof(QuicHeader));
    QuicHeader* h = reinterpret_cast<QuicHeader*>(header.data());
    
    h->flags = fin ? 0x01 : 0x00;
    h->stream_id = stream_id;
    h->offset = offset;
    h->length = static_cast<uint16_t>(length);
    
    return header;
}

} // namespace alligator
