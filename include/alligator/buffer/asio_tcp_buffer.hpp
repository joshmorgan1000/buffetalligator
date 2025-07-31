#pragma once

#include <alligator/buffer/nic_buffer.hpp>
#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <memory>
#include <vector>
#include <queue>
#include <mutex>
#include <coroutine>

namespace alligator {

/**
 * @class AsioTcpBuffer
 * @brief TCP network buffer implementation using ASIO
 * 
 * This class provides a TCP network buffer that can efficiently transfer
 * data between the network and other buffer types (especially GPU buffers)
 * using ASIO for cross-platform networking. It extends NICBuffer which
 * extends ChainBuffer for automatic chaining support.
 */
class AsioTcpBuffer : public NICBuffer {
private:
    boost::asio::io_context io_context_;
    std::unique_ptr<boost::asio::ip::tcp::socket> socket_;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    std::vector<uint8_t> internal_buffer_;
    std::thread io_thread_;
    std::atomic<bool> running_{false};
    
    // Receive queue for incoming data
    struct RxPacket {
        size_t offset;
        size_t size;
    };
    std::queue<RxPacket> rx_queue_;
    std::mutex rx_mutex_;

public:
    /**
     * @brief Constructor
     * @param capacity Size of the buffer in bytes
     */
    AsioTcpBuffer(size_t capacity);

    /**
     * @brief Destructor
     */
    ~AsioTcpBuffer() override;

    /**
     * @brief Factory method to create an AsioTcpBuffer
     * @param capacity Size in bytes
     * @return Pointer to the created AsioTcpBuffer
     */
    static AsioTcpBuffer* create(size_t capacity) {
        return new AsioTcpBuffer(capacity);
    }

    /**
     * @brief Implement the create_new method from ChainBuffer
     * @param size Size in bytes
     * @return Pointer to a new AsioTcpBuffer allocated via BuffetAlligator
     */
    ChainBuffer* create_new(size_t size) override;

    /**
     * @brief Clear the buffer
     * @param value Value to clear with
     */
    void clear(uint8_t value = 0) override;

    /**
     * @brief Deallocate the buffer
     */
    void deallocate() override;

    /**
     * @brief Get raw data pointer
     * @return Pointer to buffer data
     */
    uint8_t* data() override;
    const uint8_t* data() const override;

    /**
     * @brief Get span view of buffer
     */
    std::span<uint8_t> get_span(size_t offset = 0, size_t size = 0) override;
    std::span<const uint8_t> get_span(const size_t& offset = 0, const size_t& size = 0) const override;

    /**
     * @brief Bind to a network endpoint for receiving
     * @param endpoint Endpoint to bind to
     * @return true on success, false on failure
     */
    bool bind(const NetworkEndpoint& endpoint) override;

    /**
     * @brief Connect to a remote endpoint for sending
     * @param endpoint Endpoint to connect to
     * @return true on success, false on failure
     */
    bool connect(const NetworkEndpoint& endpoint) override;

    /**
     * @brief Send data from the buffer
     * @param offset Offset in buffer
     * @param size Size to send
     * @return Number of bytes sent, or -1 on error
     */
    ssize_t send(size_t offset, size_t size) override;

    /**
     * @brief Receive data into the buffer
     * @param offset Offset in buffer
     * @param size Maximum size to receive
     * @return Number of bytes received, or -1 on error
     */
    ssize_t receive(size_t offset, size_t size) override;

    /**
     * @brief Zero-copy send from another buffer
     * @param src Source buffer
     * @param size Size to send
     * @param src_offset Offset in source buffer
     * @return Number of bytes sent, or -1 on error
     */
    ssize_t send_from(ICollectiveBuffer* src, size_t size, size_t src_offset = 0) override;

    /**
     * @brief Zero-copy receive into another buffer
     * @param dst Destination buffer
     * @param size Maximum size to receive
     * @param dst_offset Offset in destination buffer
     * @return Number of bytes received, or -1 on error
     */
    ssize_t receive_into(ICollectiveBuffer* dst, size_t size, size_t dst_offset = 0) override;

    /**
     * @brief Get the next received data region
     * @param size Expected size (0 to get any available data)
     * @return BufferClaim with the received data region
     */
    BufferClaim get_rx(size_t size = 0) override;

    /**
     * @brief Poll for network events
     * @param timeout_ms Timeout in milliseconds
     * @return Number of events processed
     */
    int poll(int timeout_ms = 0) override;

    /**
     * @brief Get current network statistics
     * @return Network statistics
     */
    NetworkStats get_stats() const override;

private:
    /**
     * @brief Start the IO service thread
     */
    void start_io_service();

    /**
     * @brief Stop the IO service thread
     */
    void stop_io_service();

    /**
     * @brief Handle incoming data asynchronously
     */
    void async_receive();
    
    /**
     * @brief Coroutine for handling receive operations
     */
    boost::asio::awaitable<void> receive_coroutine();
    
    /**
     * @brief Coroutine for handling send operations
     * @param data Data to send
     * @param size Size of data
     * @return Number of bytes sent
     */
    boost::asio::awaitable<size_t> send_coroutine(const void* data, size_t size);
};

}
