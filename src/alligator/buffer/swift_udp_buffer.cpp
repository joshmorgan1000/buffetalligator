#include <alligator/buffer/swift_udp_buffer.hpp>
#include <alligator/buffer/buffet_alligator.hpp>
#include <cstring>
#include <cswift/cswift.hpp>

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
    return std::span<uint8_t>(internal_buffer_.data() + offset, actual_size);
}

std::span<const uint8_t> SwiftUdpBuffer::get_span(const size_t& offset, const size_t& size) const {
    if (offset >= buf_size_) {
        return std::span<const uint8_t>();
    }
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : std::min(size, buf_size_ - offset);
    return std::span<const uint8_t>(internal_buffer_.data() + offset, actual_size);
}

bool SwiftUdpBuffer::bind(const NetworkEndpoint& endpoint) {
    // Create UDP parameters
    nw_parameters_t parameters = create_udp_parameters();
    
    // Create endpoint
    const char* host = endpoint.address.c_str();
    std::string port_str = std::to_string(endpoint.port);
    const char* port = port_str.c_str();
    nw_endpoint_t nw_endpoint = nw_endpoint_create_host(host, port);
    
    // For UDP, we don't use a listener the same way as TCP
    // Instead, create a connection in server mode
    connection_ = nw_connection_create(nw_endpoint, parameters);
    if (!connection_) {
        nw_release(nw_endpoint);
        return false;
    }
    nw_release(nw_endpoint);
    nw_release(parameters);
    
    // Set up connection handlers for UDP
    setup_connection_handlers();
    
    // Start connection
    nw_connection_set_queue(connection_, queue_);
    nw_connection_start(connection_);
    
    // Wait for ready state
    for (int i = 0; i < 50 && !ready_.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    return ready_.load(std::memory_order_acquire);
}

bool SwiftUdpBuffer::connect(const NetworkEndpoint& endpoint) {
    // Create UDP parameters
    nw_parameters_t parameters = create_udp_parameters();
    
    // Create endpoint
    const char* host = endpoint.address.c_str();
    std::string port_str = std::to_string(endpoint.port);
    const char* port = port_str.c_str();
    remote_endpoint_ = nw_endpoint_create_host(host, port);
    
    // Create connection
    connection_ = nw_connection_create(remote_endpoint_, parameters);
    nw_release(parameters);
    
    if (!connection_) {
        return false;
    }
    
    connected_mode_ = true;
    setup_connection_handlers();
    
    // Start connection
    nw_connection_set_queue(connection_, queue_);
    nw_connection_start(connection_);
    
    // Wait for ready state
    for (int i = 0; i < 50 && !ready_.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    return ready_.load(std::memory_order_acquire);
}

ssize_t SwiftUdpBuffer::send(size_t offset, size_t size) {
    if (!connection_ || !ready_.load(std::memory_order_acquire)) {
        return -1;
    }
    
    if (!connected_mode_) {
        return -1; // Need endpoint for unconnected mode
    }
    
    // Create dispatch data
    dispatch_data_t data = create_dispatch_data(offset, size);
    if (!data) {
        return -1;
    }
    
    __block ssize_t result = -1;
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    
    nw_connection_send(connection_, data, NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, 
                      true, ^(nw_error_t error) {
        if (!error) {
            result = static_cast<ssize_t>(size);
        }
        dispatch_semaphore_signal(semaphore);
    });
    
    dispatch_release(data);
    dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER);
    dispatch_release(semaphore);
    
    return result;
}

ssize_t SwiftUdpBuffer::send_to(size_t offset, size_t size, nw_endpoint_t endpoint) {
    // For simplicity, we'll use the connected mode
    // A full implementation would create a new connection for each endpoint
    return send(offset, size);
}

ssize_t SwiftUdpBuffer::receive(size_t offset, size_t size) {
    if (!connection_ || !ready_.load(std::memory_order_acquire)) {
        return -1;
    }
    
    // Check if we have data in queue
    std::unique_lock<std::mutex> lock(rx_mutex_);
    if (rx_queue_.empty()) {
        return 0; // No data available
    }
    
    RxPacket packet = rx_queue_.front();
    rx_queue_.pop();
    lock.unlock();
    
    // Copy data
    size_t to_copy = std::min(size, packet.size);
    if (packet.offset + to_copy <= buf_size_ && offset + to_copy <= buf_size_) {
        std::memcpy(internal_buffer_.data() + offset,
                   internal_buffer_.data() + packet.offset,
                   to_copy);
    }
    
    if (packet.sender) {
        nw_release(packet.sender);
    }
    
    return static_cast<ssize_t>(to_copy);
}

ssize_t SwiftUdpBuffer::receive_from(size_t offset, size_t size, nw_endpoint_t& sender) {
    if (!connection_ || !ready_.load(std::memory_order_acquire)) {
        return -1;
    }
    
    // Check if we have data in queue
    std::unique_lock<std::mutex> lock(rx_mutex_);
    if (rx_queue_.empty()) {
        return 0; // No data available
    }
    
    RxPacket packet = rx_queue_.front();
    rx_queue_.pop();
    lock.unlock();
    
    // Copy data
    size_t to_copy = std::min(size, packet.size);
    if (packet.offset + to_copy <= buf_size_ && offset + to_copy <= buf_size_) {
        std::memcpy(internal_buffer_.data() + offset,
                   internal_buffer_.data() + packet.offset,
                   to_copy);
    }
    
    sender = packet.sender;
    // Don't release sender here - caller owns it now
    
    return static_cast<ssize_t>(to_copy);
}

ssize_t SwiftUdpBuffer::send_from(ICollectiveBuffer* src, size_t size, size_t src_offset) {
    if (!connection_ || !ready_.load(std::memory_order_acquire) || !src) {
        return -1;
    }
    
    // For zero-copy, we'd need to create dispatch_data from the source buffer
    // For now, copy to our buffer first
    // Check bounds
    if (size > buf_size_) {
        return -1;
    }
    
    std::memcpy(internal_buffer_.data(), src->data() + src_offset, size);
    return send(0, size);
}

ssize_t SwiftUdpBuffer::receive_into(ICollectiveBuffer* dst, size_t size, size_t dst_offset) {
    if (!connection_ || !ready_.load(std::memory_order_acquire) || !dst) {
        return -1;
    }
    
    // Special handling for GPU buffers
    if (auto* gpu_buffer = dynamic_cast<GPUBuffer*>(dst)) {
        if (!gpu_buffer->is_local()) {
            // Receive into our internal buffer first
            ssize_t bytes_received = receive(0, size);
            if (bytes_received > 0) {
                // Then upload to GPU
                if (gpu_buffer->upload(internal_buffer_.data(), bytes_received, dst_offset)) {
                    return bytes_received;
                }
            }
            return -1;
        }
    }
    
    // For local buffers, receive directly
    ssize_t bytes_received = receive(0, size);
    if (bytes_received > 0) {
        std::memcpy(dst->data() + dst_offset, internal_buffer_.data(), bytes_received);
    }
    
    return bytes_received;
}

BufferClaim SwiftUdpBuffer::get_rx(size_t size) {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    if (rx_queue_.empty()) {
        return BufferClaim{this, 0, 0, 0};
    }
    
    RxPacket& packet = rx_queue_.front();
    return BufferClaim{this, packet.offset, packet.size, 
                      static_cast<int64_t>(buf_size_ - packet.offset - packet.size)};
}

int SwiftUdpBuffer::poll(int timeout_ms) {
    // Network.framework handles events internally
    // This is a simplified implementation
    return rx_queue_.empty() ? 0 : 1;
}

SwiftUdpBuffer::NetworkStats SwiftUdpBuffer::get_stats() const {
    // Simplified stats
    return NetworkStats{0, 0, 0, 0, 0, 0};
}

bool SwiftUdpBuffer::join_multicast_group(const std::string& multicast_address) {
    // Network.framework handles multicast differently
    // This would require creating appropriate parameters
    return false;
}

bool SwiftUdpBuffer::leave_multicast_group(const std::string& multicast_address) {
    return false;
}

void SwiftUdpBuffer::set_broadcast(bool enable) {
    // Would need to be set in parameters during creation
}

nw_parameters_t SwiftUdpBuffer::create_udp_parameters() {
    nw_parameters_t parameters = nw_parameters_create_secure_udp(
        NW_PARAMETERS_DISABLE_PROTOCOL,
        NW_PARAMETERS_DEFAULT_CONFIGURATION
    );
    
    // Configure for UDP
    nw_protocol_stack_t protocol_stack = nw_parameters_copy_default_protocol_stack(parameters);
    nw_protocol_options_t udp_options = nw_protocol_stack_copy_transport_protocol(protocol_stack);
    
    // Set UDP options if needed
    
    nw_release(protocol_stack);
    
    return parameters;
}

void SwiftUdpBuffer::setup_connection_handlers() {
    if (!connection_) {
        return;
    }
    
    nw_connection_set_state_changed_handler(connection_, ^(nw_connection_state_t state, nw_error_t error) {
        if (state == nw_connection_state_ready) {
            ready_.store(true, std::memory_order_release);
            start_receive();
        } else if (state == nw_connection_state_failed || state == nw_connection_state_cancelled) {
            ready_.store(false, std::memory_order_release);
        }
    });
}

void SwiftUdpBuffer::start_receive() {
    if (!connection_ || !ready_.load(std::memory_order_acquire)) {
        return;
    }
    
    nw_connection_receive(connection_, 1, UINT32_MAX, 
        ^(dispatch_data_t content, nw_content_context_t context, bool is_complete, nw_error_t error) {
        if (content && !error) {
            // Find a free spot in the buffer
            size_t offset = rx_offset_.load(std::memory_order_relaxed);
            size_t data_size = dispatch_data_get_size(content);
            
            if (offset + data_size > buf_size_) {
                offset = 0;
                rx_offset_.store(0, std::memory_order_relaxed);
            }
            
            // Copy data to our buffer
            dispatch_data_apply(content, ^bool(dispatch_data_t region, size_t region_offset, const void* buffer, size_t size) {
                if (offset + region_offset + size <= buf_size_) {
                    std::memcpy(internal_buffer_.data() + offset + region_offset, buffer, size);
                }
                return true; // Continue iteration
            });
            
            // Update rx offset
            rx_offset_.fetch_add(data_size, std::memory_order_relaxed);
            
            // Get sender endpoint if available
            nw_endpoint_t sender = nullptr;
            if (context) {
                // Note: Getting sender from context is more complex in practice
                // This is a simplified version
            }
            
            // Add to rx queue
            {
                std::lock_guard<std::mutex> lock(rx_mutex_);
                rx_queue_.push(RxPacket{sender, offset, data_size});
            }
        }
        
        // Continue receiving
        if (!error && ready_.load(std::memory_order_acquire)) {
            start_receive();
        }
    });
}

dispatch_data_t SwiftUdpBuffer::create_dispatch_data(size_t offset, size_t size) {
    if (offset + size > buf_size_) {
        return nullptr;
    }
    
    return dispatch_data_create(internal_buffer_.data() + offset, size, 
                               queue_, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
}

} // namespace alligator
