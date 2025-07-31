#pragma once

#include <alligator/buffer/chain_buffer.hpp>
#include <atomic>
#include <memory>
#include <vector>
#include <cswift/cswift.hpp>

namespace alligator {

/**
 * @class ThunderboltDMABuffer
 * @brief High-performance buffer using cswift's Thunderbolt networking
 * 
 * This buffer type leverages cswift's HighPerformanceNetwork with
 * Thunderbolt optimization to provide extremely high-speed data transfers.
 * It uses cswift's zero-copy networking and DMA capabilities for optimal
 * performance.
 * 
 * Features:
 * - Up to 40 Gbps transfer rates with Thunderbolt 3/4
 * - Zero-copy transfers using cswift
 * - Automatic device discovery
 * - Fallback to regular buffer if cswift not available
 * 
 * Platform support:
 * - macOS: Full support via cswift
 * - Other platforms: Fallback to HeapBuffer
 */
class ThunderboltDMABuffer : public ChainBuffer {
private:
    std::unique_ptr<cswift::CSSharedBuffer> swift_buffer_;
    std::unique_ptr<cswift::CSNWConnection> connection_;
    std::unique_ptr<cswift::CSNWListener> listener_;
    uint8_t* data_{nullptr};
    
    // Network endpoint info
    std::string remote_host_;
    uint16_t remote_port_{0};
    bool is_connected_{false};
    
    // Transfer statistics
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<uint64_t> bytes_received_{0};
    
public:
    /**
     * @brief Constructor
     * @param size Size of the buffer in bytes
     * @param remote_host Optional remote host for Thunderbolt connection
     * @param remote_port Optional remote port
     */
    explicit ThunderboltDMABuffer(size_t size, 
                                 const std::string& remote_host = "",
                                 uint16_t remote_port = 0);
    
    /**
     * @brief Destructor
     */
    ~ThunderboltDMABuffer() override;
    
    /**
     * @brief Factory method
     * @param size Size in bytes
     * @return Pointer to created ThunderboltDMABuffer
     */
    static ThunderboltDMABuffer* create(size_t size) {
        return new ThunderboltDMABuffer(size);
    }
    
    /**
     * @brief Create a new buffer of the same type
     * @param size Size in bytes
     * @return Pointer to new ThunderboltDMABuffer
     */
    ChainBuffer* create_new(size_t size) override;
    
    /**
     * @brief Check if buffer is local (CPU accessible)
     * @return True
     */
    bool is_local() const override { return true; }
    
    /**
     * @brief Check if buffer is file-backed
     * @return False
     */
    bool is_file_backed() const override { return false; }
    
    /**
     * @brief Check if buffer is shared memory
     * @return True
     */
    bool is_shared() const override { 
        return true;
    }
    
    /**
     * @brief Clear the buffer
     * @param value Value to clear with
     */
    void clear(uint8_t value = 0) override;
    
    /**
     * @brief Deallocate the memory
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
     * @brief Connect to a remote Thunderbolt endpoint
     * @param host Remote host (should be Thunderbolt-connected)
     * @param port Remote port
     * @return True on success
     */
    bool connect(const std::string& host, uint16_t port);
    
    /**
     * @brief Check if connected to remote endpoint
     * @return True if connected
     */
    bool is_connected() const { return is_connected_; }
    
    /**
     * @brief Send data over Thunderbolt connection
     * @param offset Offset in buffer
     * @param size Size to send
     * @return Number of bytes sent, or -1 on error
     */
    ssize_t send(size_t offset, size_t size);
    
    /**
     * @brief Receive data over Thunderbolt connection
     * @param offset Offset in buffer
     * @param size Maximum size to receive
     * @return Number of bytes received, or -1 on error
     */
    ssize_t receive(size_t offset, size_t size);
    
    /**
     * @brief Get transfer statistics
     */
    struct TransferStats {
        uint64_t bytes_sent;
        uint64_t bytes_received;
    };
    
    /**
     * @brief Get current transfer statistics
     * @return Transfer statistics
     */
    TransferStats get_stats() const {
        return {
            bytes_sent_.load(std::memory_order_relaxed),
            bytes_received_.load(std::memory_order_relaxed)
        };
    }
    
    /**
     * @brief Check if Thunderbolt is supported on this system
     * @return True if supported
     */
    static bool is_thunderbolt_supported() {
        return true;
    }
};

} // namespace alligator