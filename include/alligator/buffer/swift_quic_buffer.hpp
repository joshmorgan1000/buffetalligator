#pragma once

#include <alligator/buffer/nic_buffer.hpp>
#include <cstdint>
#include <atomic>
#include <span>
#include <memory>
#include <queue>
#include <mutex>
#include <map>
#include <optional>
#include <chrono>
#include <cswift/cswift.hpp>

namespace alligator {

/**
 * @class SwiftQuicBuffer
 * @brief QUIC network buffer using cswift's Network.framework bindings
 * 
 * This class provides a QUIC network buffer that leverages cswift's
 * Network.framework bindings for high-performance QUIC networking on macOS/iOS.
 * 
 * QUIC features supported:
 * - Multiple streams
 * - 0-RTT connection establishment
 * - Built-in encryption with TLS 1.3
 * - Connection migration
 * - Congestion control
 * 
 * Note: This buffer is only available when cswift is enabled.
 */
class SwiftQuicBuffer : public NICBuffer {
private:
    std::unique_ptr<cswift::CSSharedBuffer> buffer_;
    std::unique_ptr<cswift::CSNWConnection> connection_;
    std::unique_ptr<cswift::CSNWListener> listener_;
    uint8_t* data_{nullptr};
    
    // Connection state
    enum class State {
        IDLE,
        CONNECTING,
        READY,
        FAILED,
        CANCELLED
    };
    std::atomic<State> state_{State::IDLE};
    
    // Stream management
    struct QuicStream {
        uint64_t stream_id;
        std::vector<uint8_t> data;
        size_t offset{0};
        size_t bytes_sent{0};
        bool is_unidirectional{false};
        bool fin_received{false};
        bool fin_sent{false};
        uint8_t priority{128}; // Middle priority
        std::chrono::steady_clock::time_point last_activity;
        
        enum class StreamState {
            IDLE,
            OPEN,
            SEND_CLOSE,
            RECV_CLOSE,
            CLOSED
        } state{StreamState::IDLE};
        
        enum class StreamType {
            BIDIRECTIONAL,
            UNIDIRECTIONAL_SEND,
            UNIDIRECTIONAL_RECV
        } type{StreamType::BIDIRECTIONAL};
    };
    std::map<uint64_t, QuicStream> streams_;
    std::mutex streams_mutex_;
    std::atomic<uint64_t> next_stream_id_{0};
    
    // Receive queue
    struct RxData {
        size_t offset;
        size_t size;
        uint64_t stream_id;
    };
    std::queue<RxData> rx_queue_;
    std::mutex rx_mutex_;
    
    // Network statistics
    mutable struct {
        std::atomic<uint64_t> bytes_sent{0};
        std::atomic<uint64_t> bytes_received{0};
        std::atomic<uint64_t> packets_sent{0};
        std::atomic<uint64_t> packets_received{0};
        std::atomic<uint64_t> errors{0};
        std::atomic<uint64_t> drops{0};
    } stats_;

public:
    /**
     * @brief Constructor
     * @param capacity Size of the buffer in bytes
     */
    SwiftQuicBuffer(size_t capacity);

    /**
     * @brief Destructor
     */
    ~SwiftQuicBuffer() override;

    /**
     * @brief Factory method to create a SwiftQuicBuffer
     * @param capacity Size in bytes
     * @return Pointer to the created SwiftQuicBuffer
     */
    static SwiftQuicBuffer* create(size_t capacity) {
        return new SwiftQuicBuffer(capacity);
    }

    /**
     * @brief Implement the create_new method from ChainBuffer
     * @param size Size in bytes
     * @return Pointer to a new buffer
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

    // Public types
    using StreamType = QuicStream::StreamType;
    
    /**
     * @brief QUIC stream information
     */
    struct QuicStreamInfo {
        uint64_t stream_id;
        size_t bytes_sent;
        size_t bytes_received;
        bool is_open;
        bool fin_sent;
        bool fin_received;
    };
    
    /**
     * @brief Create a new QUIC stream
     * @param is_unidirectional True for unidirectional stream
     * @return Stream ID, or -1 on error
     */
    int64_t create_stream(bool is_unidirectional = false);
    
    /**
     * @brief Create a new QUIC stream with type
     * @param type Stream type
     * @return Stream ID
     */
    uint64_t create_stream(StreamType type);

    /**
     * @brief Send data on a specific stream
     * @param stream_id Stream to send on
     * @param data Data to send
     * @param size Size of data
     * @param fin True to close the stream after sending
     * @return Number of bytes sent, or -1 on error
     */
    ssize_t send_on_stream(uint64_t stream_id, const void* data, size_t size, bool fin = false);

    /**
     * @brief Receive data from a specific stream
     * @param stream_id Stream to receive from
     * @param data Buffer to receive into
     * @param size Maximum size to receive
     * @return Number of bytes received, or -1 on error
     */
    ssize_t receive_from_stream(uint64_t stream_id, void* data, size_t size);

    /**
     * @brief Close a stream
     * @param stream_id Stream to close
     * @return true on success
     */
    bool close_stream(uint64_t stream_id);

    /**
     * @brief Get active stream IDs
     * @return Vector of active stream IDs
     */
    std::vector<uint64_t> get_active_streams() const;
    
    /**
     * @brief Send data on a stream (internal buffer)
     * @param stream_id Stream to send on
     * @param offset Offset in buffer
     * @param size Size to send
     * @return Number of bytes sent, or -1 on error
     */
    ssize_t send_stream(uint64_t stream_id, size_t offset, size_t size);
    
    /**
     * @brief Receive data from a stream (internal buffer)
     * @param stream_id Stream to receive from
     * @param offset Offset in buffer
     * @param size Maximum size to receive
     * @return Number of bytes received, or -1 on error
     */
    ssize_t receive_stream(uint64_t stream_id, size_t offset, size_t size);
    
    /**
     * @brief Get stream information
     * @param stream_id Stream ID
     * @return Stream info or nullopt if not found
     */
    std::optional<QuicStreamInfo> get_stream_info(uint64_t stream_id) const;
    
    /**
     * @brief Set stream priority
     * @param stream_id Stream ID
     * @param priority Priority (0-255, lower is higher priority)
     * @return true on success
     */
    bool set_stream_priority(uint64_t stream_id, uint8_t priority);
    
    /**
     * @brief Enable 0-RTT
     * @return true on success
     */
    bool enable_0rtt();
    
    /**
     * @brief Migrate connection to new endpoint
     * @param new_endpoint New endpoint
     * @return true on success
     */
    bool migrate_connection(const NetworkEndpoint& new_endpoint);

private:
    /**
     * @brief Setup connection state handler
     */
    void setup_connection_handlers();

    /**
     * @brief Start receiving data
     */
    void start_receive();

    /**
     * @brief Handle connection state changes
     * @param new_state New connection state
     */
    void handle_state_change(cswift::CSNWConnectionState new_state);

    /**
     * @brief Handle stream events
     * @param stream_id Stream ID
     * @param event Stream event type
     */
    void handle_stream_event(uint64_t stream_id, int event);
};

} // namespace alligator
