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
#include <map>

namespace alligator {

/**
 * @class AsioQuicBuffer
 * @brief QUIC network buffer implementation using ASIO
 * 
 * This class provides a QUIC network buffer that can efficiently transfer
 * data between the network and other buffer types (especially GPU buffers).
 * 
 * Note: This is a simplified implementation that uses UDP as the underlying
 * transport. For production use, consider integrating with a full QUIC
 * library like ngtcp2, quiche, or msquic.
 * 
 * QUIC features simulated:
 * - Stream multiplexing
 * - Connection migration
 * - 0-RTT connection establishment
 * - Built-in encryption (placeholder)
 */
class AsioQuicBuffer : public NICBuffer {
private:
    boost::asio::io_context io_context_;
    std::unique_ptr<boost::asio::ip::udp::socket> socket_;
    std::vector<uint8_t> internal_buffer_;
    std::thread io_thread_;
    std::atomic<bool> running_{false};
    
    // QUIC stream management
    struct QuicStream {
        uint32_t stream_id;
        std::vector<uint8_t> data;
        size_t offset;
        bool fin_received;
    };
    
    std::map<uint32_t, QuicStream> streams_;
    std::mutex streams_mutex_;
    std::atomic<uint32_t> next_stream_id_{0};
    
    // Connection state
    enum class ConnectionState {
        IDLE,
        HANDSHAKING,
        CONNECTED,
        CLOSING,
        CLOSED
    };
    std::atomic<ConnectionState> connection_state_{ConnectionState::IDLE};
    
    // Remote endpoint
    boost::asio::ip::udp::endpoint remote_endpoint_;
    
    // Packet queue
    struct QuicPacket {
        boost::asio::ip::udp::endpoint sender;
        std::vector<uint8_t> data;
    };
    std::queue<QuicPacket> rx_queue_;
    std::mutex rx_mutex_;

public:
    /**
     * @brief Constructor
     * @param capacity Size of the buffer in bytes
     */
    AsioQuicBuffer(size_t capacity);

    /**
     * @brief Destructor
     */
    ~AsioQuicBuffer() override;

    /**
     * @brief Factory method to create an AsioQuicBuffer
     * @param capacity Size in bytes
     * @return Pointer to the created AsioQuicBuffer
     */
    static AsioQuicBuffer* create(size_t capacity) {
        return new AsioQuicBuffer(capacity);
    }

    /**
     * @brief Implement the create_new method from ChainBuffer
     * @param size Size in bytes
     * @return Pointer to a new AsioQuicBuffer allocated via BuffetAlligator
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
     * @brief Connect to a remote endpoint
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
     * @brief Send data on a specific stream
     * @param stream_id Stream ID
     * @param offset Offset in buffer
     * @param size Size to send
     * @param fin Final flag (end of stream)
     * @return Number of bytes sent, or -1 on error
     */
    ssize_t send_stream(uint32_t stream_id, size_t offset, size_t size, bool fin = false);

    /**
     * @brief Receive data into the buffer
     * @param offset Offset in buffer
     * @param size Maximum size to receive
     * @return Number of bytes received, or -1 on error
     */
    ssize_t receive(size_t offset, size_t size) override;

    /**
     * @brief Receive data from a specific stream
     * @param stream_id Stream ID to receive from
     * @param offset Offset in buffer
     * @param size Maximum size to receive
     * @return Number of bytes received, or -1 on error
     */
    ssize_t receive_stream(uint32_t stream_id, size_t offset, size_t size);

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
     * @brief Create a new stream
     * @return Stream ID for the new stream
     */
    uint32_t create_stream();

    /**
     * @brief Close a stream
     * @param stream_id Stream ID to close
     */
    void close_stream(uint32_t stream_id);

    /**
     * @brief Get connection state
     * @return Current connection state
     */
    ConnectionState get_connection_state() const {
        return connection_state_.load(std::memory_order_acquire);
    }

    /**
     * @brief Enable 0-RTT (Zero Round Trip Time) for connection
     * @param enable true to enable, false to disable
     */
    void set_0rtt(bool enable);

    /**
     * @brief Set congestion control algorithm
     * @param algorithm Algorithm name (e.g., "cubic", "bbr")
     */
    void set_congestion_control(const std::string& algorithm);

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
     * @brief Coroutine for handling connection handshake
     */
    boost::asio::awaitable<void> handshake_coroutine();

    /**
     * @brief Process received QUIC packet
     * @param packet Received packet data
     * @param sender Sender endpoint
     */
    void process_quic_packet(const std::vector<uint8_t>& packet, 
                            const boost::asio::ip::udp::endpoint& sender);

    /**
     * @brief Create a QUIC packet header
     * @param stream_id Stream ID
     * @param offset Offset in stream
     * @param length Data length
     * @param fin Final flag
     * @return Packet header bytes
     */
    std::vector<uint8_t> create_quic_header(uint32_t stream_id, size_t offset, 
                                           size_t length, bool fin);
};

}
