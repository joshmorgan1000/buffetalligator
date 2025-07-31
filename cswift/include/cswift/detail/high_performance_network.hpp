/**
 * @file high_performance_network.hpp
 * @brief High-performance networking substrate for bufferalligator
 * 
 * Provides zero-copy networking with optimizations for:
 * - GPU memory coordination
 * - Neural Engine data flows  
 * - Inter-host streaming (Thunderbolt, 10G+)
 * - Cross-platform Swift networking
 */

#ifndef CSWIFT_HIGH_PERFORMANCE_NETWORK_HPP
#define CSWIFT_HIGH_PERFORMANCE_NETWORK_HPP

#include <cswift/detail/globals.hpp>
#include <cswift/detail/nio.hpp>
#include <string>
#include <memory>

namespace cswift {

/**
 * @brief Network transport types
 */
enum class NetworkTransport : int32_t {
    TCP = 0,
    UDP = 1,
    Thunderbolt = 2,      ///< IP-over-Thunderbolt for 40+ Gbps
    RDMA = 3,             ///< Future: RDMA-style operations
    LocalGPU = 4          ///< Local GPU memory coordination
};

/**
 * @brief Network optimization profiles
 */
enum class NetworkOptimization : int32_t {
    Throughput = 0,       ///< Optimize for maximum bandwidth
    Latency = 1,          ///< Optimize for minimum latency
    GPUPipeline = 2,      ///< Optimize for GPU data flows
    NeuralEngine = 3      ///< Optimize for Neural Engine coordination
};

/**
 * @brief Network endpoint configuration
 */
struct NetworkEndpoint {
    std::string host;
    uint16_t port;
    NetworkTransport transport;
    NetworkOptimization optimization;
    
    NetworkEndpoint(const std::string& h, uint16_t p, 
                   NetworkTransport t = NetworkTransport::TCP,
                   NetworkOptimization opt = NetworkOptimization::Throughput)
        : host(h), port(p), transport(t), optimization(opt) {}
};

// Forward declarations
extern "C" {
    CSHandle cswift_network_connection_create(const char* host, uint16_t port, 
                                             int32_t transport, int32_t optimization);
    void cswift_network_connection_destroy(CSHandle handle);
    int32_t cswift_network_connection_connect(CSHandle handle);
    int32_t cswift_network_send_zero_copy(CSHandle handle, CSHandle buffer);
    int32_t cswift_network_receive_zero_copy(CSHandle handle, CSHandle buffer, int32_t maxBytes);
    int32_t cswift_network_send_gpu_buffer(CSHandle handle, CSHandle metalBuffer, 
                                          int32_t offset, int32_t length);
    int32_t cswift_network_is_active(CSHandle handle);
}

/**
 * @brief High-performance network connection for bufferalligator substrates
 * 
 * This class provides zero-copy networking optimized for different use cases:
 * - GPU memory streaming between hosts
 * - Neural Engine data coordination
 * - High-bandwidth inter-host communication
 * - Low-latency local GPU coordination
 * 
 * @note Thread-safety: Individual connections are not thread-safe.
 *       Use separate connections per thread or external synchronization.
 */
class HighPerformanceNetwork {
private:
    CSHandle handle_;
    NetworkEndpoint endpoint_;
    
public:
    /**
     * @brief Create a high-performance network connection
     * @param endpoint Network endpoint configuration
     * @throws CSException if connection creation fails
     */
    explicit HighPerformanceNetwork(const NetworkEndpoint& endpoint);
    
    /**
     * @brief Destructor - closes connection and releases resources
     */
    ~HighPerformanceNetwork() {
        if (handle_) {
            cswift_network_connection_destroy(handle_);
        }
    }
    
    // Move constructor
    HighPerformanceNetwork(HighPerformanceNetwork&& other) noexcept 
        : handle_(other.handle_), endpoint_(std::move(other.endpoint_)) {
        other.handle_ = nullptr;
    }
    
    // Move assignment
    HighPerformanceNetwork& operator=(HighPerformanceNetwork&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                cswift_network_connection_destroy(handle_);
            }
            handle_ = other.handle_;
            endpoint_ = std::move(other.endpoint_);
            other.handle_ = nullptr;
        }
        return *this;
    }
    
    // Delete copy operations
    HighPerformanceNetwork(const HighPerformanceNetwork&) = delete;
    HighPerformanceNetwork& operator=(const HighPerformanceNetwork&) = delete;
    
    /**
     * @brief Connect to the remote endpoint
     * @throws CSException if connection fails
     */
    void connect();
    
    /**
     * @brief Send data with zero-copy semantics
     * @param buffer ByteBuffer containing data to send
     * @return Number of bytes sent
     * @throws CSException if send fails
     */
    size_t sendZeroCopy(CSNIOByteBuffer& buffer);
    
    /**
     * @brief Receive data with zero-copy semantics
     * @param buffer ByteBuffer to receive data into
     * @param maxBytes Maximum bytes to receive (0 = buffer size)
     * @return Number of bytes received
     * @throws CSException if receive fails
     */
    size_t receiveZeroCopy(CSNIOByteBuffer& buffer, size_t maxBytes = 0);
    
    /**
     * @brief Send data directly from GPU buffer (macOS only)
     * @param metalBuffer Metal buffer handle
     * @param offset Offset in buffer to start sending from
     * @param length Number of bytes to send (0 = entire buffer)
     * @return Number of bytes sent
     * @throws CSException if send fails or Metal not available
     */
    size_t sendFromGPUBuffer(void* metalBuffer, size_t offset = 0, size_t length = 0);
    
    /**
     * @brief Check if connection is active
     * @return true if connection is active
     */
    bool isActive() const;
    
    /**
     * @brief Get the network endpoint configuration
     * @return Reference to endpoint configuration
     */
    const NetworkEndpoint& getEndpoint() const {
        return endpoint_;
    }
    
    /**
     * @brief Get internal handle (for advanced use)
     * @return Internal handle
     * @note For internal use only
     */
    CSHandle getHandle() const {
        return handle_;
    }
};

/**
 * @brief Network connection factory for different optimization profiles
 */
class NetworkConnectionFactory {
public:
    /**
     * @brief Create connection optimized for GPU data streaming
     * @param host Remote host
     * @param port Remote port
     * @param transport Transport type (default: TCP)
     * @return Unique pointer to optimized connection
     */
    static std::unique_ptr<HighPerformanceNetwork> createGPUOptimized(
        const std::string& host, 
        uint16_t port,
        NetworkTransport transport = NetworkTransport::TCP) {
        
        NetworkEndpoint endpoint(host, port, transport, NetworkOptimization::GPUPipeline);
        return std::make_unique<HighPerformanceNetwork>(endpoint);
    }
    
    /**
     * @brief Create connection optimized for Neural Engine coordination
     * @param host Remote host
     * @param port Remote port
     * @return Unique pointer to optimized connection
     */
    static std::unique_ptr<HighPerformanceNetwork> createNeuralEngineOptimized(
        const std::string& host, 
        uint16_t port);
    
    /**
     * @brief Create Thunderbolt connection for maximum bandwidth
     * @param host Remote host (should be Thunderbolt-connected)
     * @param port Remote port
     * @return Unique pointer to Thunderbolt connection
     */
    static std::unique_ptr<HighPerformanceNetwork> createThunderboltOptimized(
        const std::string& host, 
        uint16_t port);
    
    /**
     * @brief Create connection optimized for minimum latency
     * @param host Remote host
     * @param port Remote port
     * @return Unique pointer to latency-optimized connection
     */
    static std::unique_ptr<HighPerformanceNetwork> createLatencyOptimized(
        const std::string& host, 
        uint16_t port);
};

} // namespace cswift

#endif // CSWIFT_HIGH_PERFORMANCE_NETWORK_HPP