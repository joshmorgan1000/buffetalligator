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
 * @class AsioUdpBuffer
 * @brief UDP network buffer implementation using ASIO
 * 
 * This class provides a UDP network buffer that can efficiently transfer
 * data between the network and other buffer types (especially GPU buffers)
 * using ASIO for cross-platform networking. It extends NICBuffer which
 * extends ChainBuffer for automatic chaining support.
 * 
 * UDP is connectionless, so this implementation handles both unicast
 * and multicast scenarios with efficient packet handling.
 */
class AsioUdpBuffer : public NICBuffer {
private:
    boost::asio::io_context io_context_;
    std::unique_ptr<boost::asio::ip::udp::socket> socket_;
    std::vector<uint8_t> internal_buffer_;
    std::thread io_thread_;
    std::atomic<bool> running_{false};
    
    // Receive queue for incoming packets
    struct RxPacket {
        boost::asio::ip::udp::endpoint sender;
        size_t offset;
        size_t size;
    };
    std::queue<RxPacket> rx_queue_;
    std::mutex rx_mutex_;
    
    // Remote endpoint for connected mode
    boost::asio::ip::udp::endpoint remote_endpoint_;
    bool connected_mode_{false};

public:
    /**
     * @brief Constructor
     * @param capacity Size of the buffer in bytes
     */
    AsioUdpBuffer(size_t capacity);

    /**
     * @brief Destructor
     */
    ~AsioUdpBuffer() override;

    /**
     * @brief Factory method to create an AsioUdpBuffer
     * @param capacity Size in bytes
     * @return Pointer to the created AsioUdpBuffer
     */
    static AsioUdpBuffer* create(size_t capacity) {
        return new AsioUdpBuffer(capacity);
    }

    /**
     * @brief Implement the create_new method from ChainBuffer
     * @param size Size in bytes
     * @return Pointer to a new AsioUdpBuffer allocated via BuffetAlligator
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
     * @brief Connect to a remote endpoint for sending (sets default target)
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
     * @brief Send data to a specific endpoint
     * @param offset Offset in buffer
     * @param size Size to send
     * @param endpoint Target endpoint
     * @return Number of bytes sent, or -1 on error
     */
    ssize_t send_to(size_t offset, size_t size, const boost::asio::ip::udp::endpoint& endpoint);

    /**
     * @brief Receive data into the buffer
     * @param offset Offset in buffer
     * @param size Maximum size to receive
     * @return Number of bytes received, or -1 on error
     */
    ssize_t receive(size_t offset, size_t size) override;

    /**
     * @brief Receive data and get sender endpoint
     * @param offset Offset in buffer
     * @param size Maximum size to receive
     * @param sender_endpoint Output parameter for sender endpoint
     * @return Number of bytes received, or -1 on error
     */
    ssize_t receive_from(size_t offset, size_t size, boost::asio::ip::udp::endpoint& sender_endpoint);

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

    /**
     * @brief Join a multicast group
     * @param multicast_address Multicast group address
     * @return true on success, false on failure
     */
    bool join_multicast_group(const std::string& multicast_address);

    /**
     * @brief Leave a multicast group
     * @param multicast_address Multicast group address
     * @return true on success, false on failure
     */
    bool leave_multicast_group(const std::string& multicast_address);

    /**
     * @brief Set UDP socket options
     * @param broadcast Enable broadcast
     * @param reuse_addr Enable address reuse
     */
    void set_udp_options(bool broadcast = false, bool reuse_addr = true);

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
     * @brief Coroutine for handling receive operations
     */
    boost::asio::awaitable<void> receive_coroutine();

    /**
     * @brief Coroutine for sending data
     * @param data Data to send
     * @param size Size of data
     * @param endpoint Target endpoint (nullopt for connected mode)
     * @return Number of bytes sent
     */
    boost::asio::awaitable<size_t> send_coroutine(const void* data, size_t size, 
                                                   std::optional<boost::asio::ip::udp::endpoint> endpoint = std::nullopt);
};

}
