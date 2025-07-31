#include <alligator/buffer/asio_tcp_buffer.hpp>
#include <alligator/buffer/buffet_alligator.hpp>
#include <cstring>
#include <future>

namespace alligator {

AsioTcpBuffer::AsioTcpBuffer(size_t capacity) 
    : NICBuffer(capacity, NetworkProtocol::TCP) {
    internal_buffer_.resize(capacity);
    nic_features_ = static_cast<uint32_t>(NICFeatures::ZERO_COPY) | 
                    static_cast<uint32_t>(NICFeatures::KERNEL_BYPASS);
}

AsioTcpBuffer::~AsioTcpBuffer() {
    stop_io_service();
}

ChainBuffer* AsioTcpBuffer::create_new(size_t size) {
    return get_buffet_alligator().allocate_buffer(size, BFType::TCP);
}

void AsioTcpBuffer::clear(uint8_t value) {
    std::memset(internal_buffer_.data(), value, internal_buffer_.size());
    next_offset_.store(0, std::memory_order_relaxed);
    rx_offset_.store(0, std::memory_order_relaxed);
    tx_offset_.store(0, std::memory_order_relaxed);
}

void AsioTcpBuffer::deallocate() {
    stop_io_service();
    internal_buffer_.clear();
    internal_buffer_.shrink_to_fit();
}

uint8_t* AsioTcpBuffer::data() {
    return internal_buffer_.data();
}

const uint8_t* AsioTcpBuffer::data() const {
    return internal_buffer_.data();
}

std::span<uint8_t> AsioTcpBuffer::get_span(size_t offset, size_t size) {
    if (offset >= buf_size_) {
        return std::span<uint8_t>();
    }
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : std::min(size, buf_size_ - offset);
    return std::span<uint8_t>(internal_buffer_.data() + offset, actual_size);
}

std::span<const uint8_t> AsioTcpBuffer::get_span(const size_t& offset, const size_t& size) const {
    if (offset >= buf_size_) {
        return std::span<const uint8_t>();
    }
    size_t actual_size = (size == 0) ? (buf_size_ - offset) : std::min(size, buf_size_ - offset);
    return std::span<const uint8_t>(internal_buffer_.data() + offset, actual_size);
}

bool AsioTcpBuffer::bind(const NetworkEndpoint& endpoint) {
    try {
        acceptor_ = std::make_unique<boost::asio::ip::tcp::acceptor>(io_context_);
        boost::asio::ip::tcp::endpoint asio_endpoint(
            boost::asio::ip::make_address(endpoint.address), 
            endpoint.port
        );
        acceptor_->open(asio_endpoint.protocol());
        acceptor_->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
        acceptor_->bind(asio_endpoint);
        acceptor_->listen();
        start_io_service();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool AsioTcpBuffer::connect(const NetworkEndpoint& endpoint) {
    try {
        socket_ = std::make_unique<boost::asio::ip::tcp::socket>(io_context_);
        boost::asio::ip::tcp::endpoint asio_endpoint(
            boost::asio::ip::make_address(endpoint.address), 
            endpoint.port
        );
        socket_->connect(asio_endpoint);
        start_io_service();
        async_receive();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

ssize_t AsioTcpBuffer::send(size_t offset, size_t size) {
    if (!socket_ || !socket_->is_open()) {
        return -1;
    }
    
    try {
        // Use a promise/future for synchronous interface
        std::promise<size_t> promise;
        auto future = promise.get_future();
        
        boost::asio::co_spawn(io_context_,
            send_coroutine(internal_buffer_.data() + offset, size),
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

boost::asio::awaitable<size_t> AsioTcpBuffer::send_coroutine(const void* data, size_t size) {
    try {
        size_t bytes_sent = co_await boost::asio::async_write(
            *socket_,
            boost::asio::buffer(data, size),
            boost::asio::use_awaitable
        );
        co_return bytes_sent;
    } catch (const std::exception&) {
        co_return 0;
    }
}

ssize_t AsioTcpBuffer::receive(size_t offset, size_t size) {
    if (!socket_ || !socket_->is_open()) {
        return -1;
    }
    
    try {
        boost::system::error_code ec;
        size_t bytes_received = socket_->read_some(
            boost::asio::buffer(internal_buffer_.data() + offset, size),
            ec
        );
        return ec ? -1 : static_cast<ssize_t>(bytes_received);
    } catch (const std::exception&) {
        return -1;
    }
}

ssize_t AsioTcpBuffer::send_from(ICollectiveBuffer* src, size_t size, size_t src_offset) {
    if (!socket_ || !socket_->is_open() || !src) {
        return -1;
    }
    
    try {
        boost::system::error_code ec;
        size_t bytes_sent = boost::asio::write(
            *socket_, 
            boost::asio::buffer(src->data() + src_offset, size),
            ec
        );
        return ec ? -1 : static_cast<ssize_t>(bytes_sent);
    } catch (const std::exception&) {
        return -1;
    }
}

ssize_t AsioTcpBuffer::receive_into(ICollectiveBuffer* dst, size_t size, size_t dst_offset) {
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
        boost::system::error_code ec;
        size_t bytes_received = socket_->read_some(
            boost::asio::buffer(dst->data() + dst_offset, size),
            ec
        );
        return ec ? -1 : static_cast<ssize_t>(bytes_received);
    } catch (const std::exception&) {
        return -1;
    }
}

BufferClaim AsioTcpBuffer::get_rx(size_t size) {
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

int AsioTcpBuffer::poll(int timeout_ms) {
    if (!running_) {
        return 0;
    }
    
    // Run the io_context for a limited time
    io_context_.run_for(std::chrono::milliseconds(timeout_ms));
    return 1; // Simplified - return number of events processed
}

AsioTcpBuffer::NetworkStats AsioTcpBuffer::get_stats() const {
    // Simplified stats - would need to track these in production
    return NetworkStats{0, 0, 0, 0, 0, 0};
}

void AsioTcpBuffer::start_io_service() {
    if (!running_.exchange(true)) {
        io_thread_ = std::thread([this]() {
            io_context_.run();
        });
    }
}

void AsioTcpBuffer::stop_io_service() {
    if (running_.exchange(false)) {
        io_context_.stop();
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
    }
}

void AsioTcpBuffer::async_receive() {
    if (!socket_ || !socket_->is_open()) {
        return;
    }
    
    // Start the receive coroutine
    boost::asio::co_spawn(io_context_, receive_coroutine(), boost::asio::detached);
}

boost::asio::awaitable<void> AsioTcpBuffer::receive_coroutine() {
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
            
            // Use co_await for asynchronous receive
            size_t bytes_received = co_await socket_->async_read_some(
                boost::asio::buffer(internal_buffer_.data() + offset, available),
                boost::asio::use_awaitable
            );
            
            if (bytes_received > 0) {
                // Update rx offset
                rx_offset_.fetch_add(bytes_received, std::memory_order_relaxed);
                
                // Add to rx queue
                {
                    std::lock_guard<std::mutex> lock(rx_mutex_);
                    rx_queue_.push(RxPacket{offset, bytes_received});
                }
            }
        }
    } catch (const std::exception&) {
        // Connection closed or error occurred
    }
    co_return;
}

} // namespace alligator