#pragma once

#include <alligator/buffer/nic_buffer.hpp>
#include <cstdint>
#include <atomic>
#include <span>
#include <memory>
#include <queue>
#include <mutex>
#include <cswift/cswift.hpp>

namespace alligator {

/**
 * @class SwiftTcpBuffer
 * @brief TCP network buffer using cswift's Network.framework bindings
 * 
 * This class provides a TCP network buffer that leverages cswift's
 * Network.framework bindings for high-performance networking on macOS/iOS.
 * It extends NICBuffer to provide protocol-specific functionality
 * while maintaining compatibility with the buffer chaining system.
 * 
 * Note: This buffer is only available when cswift is enabled.
 */
class SwiftTcpBuffer : public NICBuffer {
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
    
    // Receive queue
    struct RxData {
        size_t offset;
        size_t size;
    };
    std::queue<RxData> rx_queue_;
    std::mutex rx_mutex_;
    
    // Network statistics with atomic counters
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
    SwiftTcpBuffer(size_t capacity);

    /**
     * @brief Destructor
     */
    ~SwiftTcpBuffer() override;

    /**
     * @brief Factory method to create a SwiftTcpBuffer
     * @param capacity Size in bytes
     * @return Pointer to the created SwiftTcpBuffer
     */
    static SwiftTcpBuffer* create(size_t capacity) {
        return new SwiftTcpBuffer(capacity);
    }

    /**
     * @brief Implement the create_new method from ChainBuffer
     * @param size Size in bytes
     * @return Pointer to a new FoundationTcpBuffer
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
};

} // namespace alligator
