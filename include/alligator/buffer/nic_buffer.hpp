#pragma once

#include <alligator/buffer/chain_buffer.hpp>
#include <alligator/buffer/gpu_buffer.hpp>
#include <cstdint>
#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <vector>

namespace alligator {

/**
 * @enum NetworkProtocol
 * @brief Supported network protocols
 */
enum class NetworkProtocol : uint32_t {
    TCP = 1,
    UDP = 2,
    QUIC = 3,
    RDMA = 4,      ///< Remote Direct Memory Access
    DPDK = 5       ///< Data Plane Development Kit
};

/**
 * @enum NICFeatures
 * @brief NIC hardware features (bit flags)
 */
enum class NICFeatures : uint32_t {
    NONE = 0,
    CHECKSUM_OFFLOAD = 1,
    SEGMENTATION_OFFLOAD = 2,
    RECEIVE_SIDE_SCALING = 4,
    JUMBO_FRAMES = 8,
    ZERO_COPY = 16,
    DMA_ENGINE = 32,
    GPU_DIRECT = 64,       ///< GPU Direct RDMA support
    KERNEL_BYPASS = 128    ///< Kernel bypass networking
};

/**
 * @struct NetworkEndpoint
 * @brief Network endpoint information
 */
struct NetworkEndpoint {
    std::string address;
    uint16_t port;
    NetworkProtocol protocol;
};

/**
 * @class NICBuffer
 * @brief Abstract base class for Network Interface Card buffers
 * 
 * This class provides a unified interface for network buffers that can
 * efficiently transfer data between NICs and other buffer types (especially
 * GPU buffers) with minimal copying. It extends ChainBuffer to support
 * automatic chaining for efficient message passing.
 */
class NICBuffer : public ChainBuffer {
protected:
    NetworkProtocol protocol_{NetworkProtocol::TCP};
    uint32_t nic_features_{0};
    std::string interface_name_;
    uint32_t ring_buffer_size_{0};
    std::atomic<size_t> rx_offset_{0};
    std::atomic<size_t> tx_offset_{0};

public:
    /**
     * @brief Constructor
     * @param capacity Size of the buffer in bytes
     * @param protocol Network protocol to use
     */
    NICBuffer(size_t capacity, NetworkProtocol protocol) 
        : protocol_(protocol) {
        buf_size_ = capacity;
    }

    /**
     * @brief Destructor
     */
    virtual ~NICBuffer() = default;

    /**
     * @brief Get the network protocol
     * @return The protocol used by this buffer
     */
    NetworkProtocol get_protocol() const { return protocol_; }

    /**
     * @brief Get supported NIC features
     * @return Bitmask of supported features
     */
    uint32_t get_features() const { return nic_features_; }

    /**
     * @brief Check if a specific feature is supported
     * @param feature Feature to check
     * @return true if supported, false otherwise
     */
    bool has_feature(NICFeatures feature) const {
        return (nic_features_ & static_cast<uint32_t>(feature)) != 0;
    }

    /**
     * @brief Bind to a network endpoint for receiving
     * @param endpoint Endpoint to bind to
     * @return true on success, false on failure
     */
    virtual bool bind(const NetworkEndpoint& endpoint) = 0;

    /**
     * @brief Connect to a remote endpoint for sending
     * @param endpoint Endpoint to connect to
     * @return true on success, false on failure
     */
    virtual bool connect(const NetworkEndpoint& endpoint) = 0;

    /**
     * @brief Send data from the buffer
     * @param offset Offset in buffer
     * @param size Size to send
     * @return Number of bytes sent, or -1 on error
     */
    virtual ssize_t send(size_t offset, size_t size) = 0;

    /**
     * @brief Receive data into the buffer
     * @param offset Offset in buffer
     * @param size Maximum size to receive
     * @return Number of bytes received, or -1 on error
     */
    virtual ssize_t receive(size_t offset, size_t size) = 0;

    /**
     * @brief Zero-copy send from another buffer
     * @param src Source buffer
     * @param size Size to send
     * @param src_offset Offset in source buffer
     * @return Number of bytes sent, or -1 on error
     */
    virtual ssize_t send_from(ICollectiveBuffer* src, size_t size, size_t src_offset = 0) = 0;

    /**
     * @brief Zero-copy receive into another buffer (especially GPU buffers)
     * @param dst Destination buffer
     * @param size Maximum size to receive
     * @param dst_offset Offset in destination buffer
     * @return Number of bytes received, or -1 on error
     */
    virtual ssize_t receive_into(ICollectiveBuffer* dst, size_t size, size_t dst_offset = 0) = 0;

    /**
     * @brief Direct GPU transfer using GPU Direct RDMA (if supported)
     * @param gpu_buffer GPU buffer to transfer to/from
     * @param size Size to transfer
     * @param gpu_offset Offset in GPU buffer
     * @param is_send true for send, false for receive
     * @return Number of bytes transferred, or -1 on error
     */
    virtual ssize_t gpu_direct_transfer(GPUBuffer* gpu_buffer, size_t size, 
                                       size_t gpu_offset, bool is_send) {
        // Default implementation: fallback to regular transfer
        if (is_send) {
            return send_from(gpu_buffer, size, gpu_offset);
        } else {
            return receive_into(gpu_buffer, size, gpu_offset);
        }
    }

    /**
     * @brief Claim memory directly in the NIC buffer for producers
     * @param size Size to claim
     * @return BufferClaim with the claimed region
     */
    BufferClaim claim_tx(size_t size) {
        // Use the base class claim method for TX
        return claim(size);
    }

    /**
     * @brief Get the next received data region
     * @param size Expected size (0 to get any available data)
     * @return BufferClaim with the received data region
     */
    virtual BufferClaim get_rx(size_t size = 0) = 0;

    /**
     * @brief Completion callback for async operations
     */
    using CompletionCallback = std::function<void(ssize_t bytes_transferred)>;

    /**
     * @brief Async send with completion callback
     * @param offset Buffer offset
     * @param size Size to send
     * @param callback Completion callback
     */
    virtual void send_async(size_t offset, size_t size, CompletionCallback callback) {
        // Default implementation: synchronous send then callback
        ssize_t result = send(offset, size);
        if (callback) callback(result);
    }

    /**
     * @brief Async receive with completion callback
     * @param offset Buffer offset
     * @param size Maximum size to receive
     * @param callback Completion callback
     */
    virtual void receive_async(size_t offset, size_t size, CompletionCallback callback) {
        // Default implementation: synchronous receive then callback
        ssize_t result = receive(offset, size);
        if (callback) callback(result);
    }

    /**
     * @brief Poll for network events (non-blocking)
     * @param timeout_ms Timeout in milliseconds (0 for non-blocking)
     * @return Number of events processed
     */
    virtual int poll(int timeout_ms = 0) = 0;

    /**
     * @brief Get network statistics
     */
    struct NetworkStats {
        uint64_t bytes_sent;
        uint64_t bytes_received;
        uint64_t packets_sent;
        uint64_t packets_received;
        uint64_t errors;
        uint64_t drops;
    };

    /**
     * @brief Get current network statistics
     * @return Network statistics
     */
    virtual NetworkStats get_stats() const = 0;

    /**
     * @brief ICollectiveBuffer interface implementation
     */
    bool is_local() const override {
        return true;  // NIC buffers are accessible by CPU
    }

    bool is_file_backed() const override {
        return false;  // NIC buffers are not file-backed
    }

    bool is_shared() const override {
        // Some NIC buffers might use shared memory for zero-copy
        return has_feature(NICFeatures::ZERO_COPY);
    }

    /**
     * @brief Set interrupt coalescing parameters
     * @param usecs Microseconds to wait before interrupt
     * @param packets Number of packets before interrupt
     */
    virtual void set_interrupt_coalescing(uint32_t usecs, uint32_t packets) {
        // Optional: implementation-specific
    }

    /**
     * @brief Enable/disable hardware offload features
     * @param feature Feature to enable/disable
     * @param enable true to enable, false to disable
     */
    virtual void set_hardware_offload(NICFeatures feature, bool enable) {
        // Optional: implementation-specific
    }
};

/**
 * @class NICBufferPool
 * @brief Pool of NIC buffers for efficient allocation
 */
class NICBufferPool {
private:
    std::vector<std::unique_ptr<NICBuffer>> buffers_;
    std::atomic<size_t> next_buffer_{0};
    size_t buffer_size_;
    NetworkProtocol protocol_;

public:
    /**
     * @brief Constructor
     * @param num_buffers Number of buffers in the pool
     * @param buffer_size Size of each buffer
     * @param protocol Network protocol
     */
    NICBufferPool(size_t num_buffers, size_t buffer_size, NetworkProtocol protocol)
        : buffer_size_(buffer_size), protocol_(protocol) {
        buffers_.reserve(num_buffers);
    }

    /**
     * @brief Get the next available buffer from the pool
     * @return Pointer to NICBuffer, or nullptr if none available
     */
    NICBuffer* get_buffer() {
        size_t idx = next_buffer_.fetch_add(1, std::memory_order_relaxed) % buffers_.size();
        return buffers_[idx].get();
    }

    /**
     * @brief Add a buffer to the pool
     * @param buffer Buffer to add
     */
    void add_buffer(std::unique_ptr<NICBuffer> buffer) {
        buffers_.push_back(std::move(buffer));
    }
};

} // namespace alligator
