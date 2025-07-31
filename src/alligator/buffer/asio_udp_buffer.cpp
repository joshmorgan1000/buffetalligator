#include <alligator/buffer/asio_udp_buffer.hpp>
#include <alligator/buffer/buffet_alligator.hpp>
#include <cstring>
#include <future>

namespace alligator {

AsioUdpBuffer::AsioUdpBuffer(size_t capacity) 
    : NICBuffer(capacity, NetworkProtocol::UDP) {
    internal_buffer_.resize(capacity);
    nic_features_ = static_cast<uint32_t>(NICFeatures::ZERO_COPY) | 
                    static_cast<uint32_t>(NICFeatures::KERNEL_BYPASS);
}

AsioUdpBuffer::~AsioUdpBuffer() {
    stop_io_service();
}

ChainBuffer* AsioUdpBuffer::create_new(size_t size) {
    return get_buffet_alligator().allocate_buffer(size, BFType::UDP);
}

void AsioUdpBuffer::clear(uint8_t value) {
    std::memset(internal_buffer_.data(), value, internal_buffer_.size());
    next_offset_.store(0, std::memory_order_relaxed);
    rx_offset_.store(0, std::memory_order_relaxed);
    tx_offset_.store(0, std::memory_order_relaxed);
}

void AsioUdpBuffer::deallocate() {
    stop_io_service();
    internal_buffer_.clear();
    internal_buffer_.shrink_to_fit();
}

uint8_t* AsioUdpBuffer::data() {
    return internal_buffer_.data();
}

const uint8_t* AsioUdpBuffer::data() const {
    return internal_buffer_.data();
}

std::span<uint8_t> AsioUdpBuffer::get_span(size_t offset, size_t size) {
    if (offset >= buf_size_) {
        return std::span<uint8_t>();
    }
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : std::min(size, buf_size_ - offset);
    return std::span<uint8_t>(internal_buffer_.data() + offset, actual_size);
}

std::span<const uint8_t> AsioUdpBuffer::get_span(const size_t& offset, const size_t& size) const {
    if (offset >= buf_size_) {
        return std::span<const uint8_t>();
    }
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : std::min(size, buf_size_ - offset);
    return std::span<const uint8_t>(internal_buffer_.data() + offset, actual_size);
}

bool AsioUdpBuffer::bind(const NetworkEndpoint& endpoint) {
    try {
        socket_ = std::make_unique<boost::asio::ip::udp::socket>(io_context_);
        boost::asio::ip::udp::endpoint asio_endpoint(
            boost::asio::ip::make_address(endpoint.address), 
            endpoint.port
        );
        socket_->open(asio_endpoint.protocol());
        socket_->bind(asio_endpoint);
        
        start_io_service();
        
        // Start receive coroutine
        boost::asio::co_spawn(io_context_, receive_coroutine(), boost::asio::detached);
        
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool AsioUdpBuffer::connect(const NetworkEndpoint& endpoint) {
    try {
        if (!socket_) {
            socket_ = std::make_unique<boost::asio::ip::udp::socket>(io_context_);
            socket_->open(boost::asio::ip::udp::v4());
        }
        
        remote_endpoint_ = boost::asio::ip::udp::endpoint(
            boost::asio::ip::make_address(endpoint.address), 
            endpoint.port
        );
        connected_mode_ = true;
        
        start_io_service();
        
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

ssize_t AsioUdpBuffer::send(size_t offset, size_t size) {
    if (!socket_ || !socket_->is_open()) {
        return -1;
    }
    
    if (!connected_mode_) {
        return -1; // Need endpoint for unconnected mode
    }
    
    return send_to(offset, size, remote_endpoint_);
}

ssize_t AsioUdpBuffer::send_to(size_t offset, size_t size, const boost::asio::ip::udp::endpoint& endpoint) {
    if (!socket_ || !socket_->is_open()) {
        return -1;
    }
    
    try {
        // Use a promise/future for synchronous interface
        std::promise<size_t> promise;
        auto future = promise.get_future();
        
        boost::asio::co_spawn(io_context_,
            send_coroutine(internal_buffer_.data() + offset, size, endpoint),
            [&promise](std::exception_ptr e, size_t bytes_sent) {
                if (e) {
                    promise.set_value(0);
                } else {
                    promise.set_value(bytes_sent);
                }
            }
        );
        
        size_t result = future.get();
        return result > 0 ? static_cast<ssize_t>(result) : -1;
    } catch (const std::exception&) {
        return -1;
    }
}

ssize_t AsioUdpBuffer::receive(size_t offset, size_t size) {
    if (!socket_ || !socket_->is_open()) {
        return -1;
    }
    
    try {
        boost::asio::ip::udp::endpoint sender_endpoint;
        boost::system::error_code ec;
        size_t bytes_received = socket_->receive_from(
            boost::asio::buffer(internal_buffer_.data() + offset, size),
            sender_endpoint,
            0,
            ec
        );
        return ec ? -1 : static_cast<ssize_t>(bytes_received);
    } catch (const std::exception&) {
        return -1;
    }
}

ssize_t AsioUdpBuffer::receive_from(size_t offset, size_t size, boost::asio::ip::udp::endpoint& sender_endpoint) {
    if (!socket_ || !socket_->is_open()) {
        return -1;
    }
    
    try {
        boost::system::error_code ec;
        size_t bytes_received = socket_->receive_from(
            boost::asio::buffer(internal_buffer_.data() + offset, size),
            sender_endpoint,
            0,
            ec
        );
        return ec ? -1 : static_cast<ssize_t>(bytes_received);
    } catch (const std::exception&) {
        return -1;
    }
}

ssize_t AsioUdpBuffer::send_from(ICollectiveBuffer* src, size_t size, size_t src_offset) {
    if (!socket_ || !socket_->is_open() || !src) {
        return -1;
    }
    
    if (!connected_mode_) {
        return -1; // Need endpoint for unconnected mode
    }
    
    try {
        boost::system::error_code ec;
        size_t bytes_sent = socket_->send_to(
            boost::asio::buffer(src->data() + src_offset, size),
            remote_endpoint_,
            0,
            ec
        );
        return ec ? -1 : static_cast<ssize_t>(bytes_sent);
    } catch (const std::exception&) {
        return -1;
    }
}

ssize_t AsioUdpBuffer::receive_into(ICollectiveBuffer* dst, size_t size, size_t dst_offset) {
    if (!socket_ || !socket_->is_open() || !dst) {
        return -1;
    }
    
    // Special handling for GPU buffers
    if (auto* gpu_buffer = dynamic_cast<GPUBuffer*>(dst)) {
        // For GPU buffers, we might need to receive into a staging buffer first
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
    try {
        boost::asio::ip::udp::endpoint sender_endpoint;
        boost::system::error_code ec;
        size_t bytes_received = socket_->receive_from(
            boost::asio::buffer(dst->data() + dst_offset, size),
            sender_endpoint,
            0,
            ec
        );
        return ec ? -1 : static_cast<ssize_t>(bytes_received);
    } catch (const std::exception&) {
        return -1;
    }
}

BufferClaim AsioUdpBuffer::get_rx(size_t size) {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    if (rx_queue_.empty()) {
        // No data available
        return BufferClaim{this, 0, 0, 0};
    }
    
    RxPacket packet = rx_queue_.front();
    rx_queue_.pop();
    
    return BufferClaim{this, packet.offset, packet.size, 
                      static_cast<int64_t>(buf_size_ - packet.offset - packet.size)};
}

int AsioUdpBuffer::poll(int timeout_ms) {
    if (!running_) {
        return 0;
    }
    
    // Run the io_context for a limited time
    io_context_.run_for(std::chrono::milliseconds(timeout_ms));
    return 1; // Simplified - return number of events processed
}

AsioUdpBuffer::NetworkStats AsioUdpBuffer::get_stats() const {
    // Simplified stats - would need to track these in production
    return NetworkStats{0, 0, 0, 0, 0, 0};
}

bool AsioUdpBuffer::join_multicast_group(const std::string& multicast_address) {
    if (!socket_ || !socket_->is_open()) {
        return false;
    }
    
    try {
        socket_->set_option(
            boost::asio::ip::multicast::join_group(
                boost::asio::ip::make_address(multicast_address)
            )
        );
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool AsioUdpBuffer::leave_multicast_group(const std::string& multicast_address) {
    if (!socket_ || !socket_->is_open()) {
        return false;
    }
    
    try {
        socket_->set_option(
            boost::asio::ip::multicast::leave_group(
                boost::asio::ip::make_address(multicast_address)
            )
        );
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void AsioUdpBuffer::set_udp_options(bool broadcast, bool reuse_addr) {
    if (!socket_ || !socket_->is_open()) {
        return;
    }
    
    try {
        if (broadcast) {
            socket_->set_option(boost::asio::socket_base::broadcast(true));
        }
        if (reuse_addr) {
            socket_->set_option(boost::asio::socket_base::reuse_address(true));
        }
    } catch (const std::exception&) {
        // Ignore errors
    }
}

void AsioUdpBuffer::start_io_service() {
    if (!running_.exchange(true)) {
        io_thread_ = std::thread([this]() {
            io_context_.run();
        });
    }
}

void AsioUdpBuffer::stop_io_service() {
    if (running_.exchange(false)) {
        io_context_.stop();
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
    }
}

boost::asio::awaitable<void> AsioUdpBuffer::receive_coroutine() {
    try {
        while (socket_ && socket_->is_open() && running_) {
            // Find a free spot in the buffer
            size_t offset = rx_offset_.load(std::memory_order_relaxed);
            size_t available = buf_size_ - offset;
            
            if (available == 0) {
                // Buffer full, wrap around
                offset = 0;
                rx_offset_.store(0, std::memory_order_relaxed);
                available = buf_size_;
            }
            
            boost::asio::ip::udp::endpoint sender_endpoint;
            
            // Use co_await for asynchronous receive
            size_t bytes_received = co_await socket_->async_receive_from(
                boost::asio::buffer(internal_buffer_.data() + offset, available),
                sender_endpoint,
                boost::asio::use_awaitable
            );
            
            if (bytes_received > 0) {
                // Update rx offset
                rx_offset_.fetch_add(bytes_received, std::memory_order_relaxed);
                
                // Add to rx queue
                {
                    std::lock_guard<std::mutex> lock(rx_mutex_);
                    rx_queue_.push(RxPacket{sender_endpoint, offset, bytes_received});
                }
            }
        }
    } catch (const std::exception&) {
        // Socket closed or error occurred
    }
    co_return;
}

boost::asio::awaitable<size_t> AsioUdpBuffer::send_coroutine(const void* data, size_t size, 
                                                             std::optional<boost::asio::ip::udp::endpoint> endpoint) {
    try {
        size_t bytes_sent;
        if (endpoint.has_value()) {
            bytes_sent = co_await socket_->async_send_to(
                boost::asio::buffer(data, size),
                endpoint.value(),
                boost::asio::use_awaitable
            );
        } else if (connected_mode_) {
            bytes_sent = co_await socket_->async_send_to(
                boost::asio::buffer(data, size),
                remote_endpoint_,
                boost::asio::use_awaitable
            );
        } else {
            co_return 0; // No endpoint available
        }
        co_return bytes_sent;
    } catch (const std::exception&) {
        co_return 0;
    }
}

} // namespace alligator