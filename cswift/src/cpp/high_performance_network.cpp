/**
 * @file high_performance_network.cpp
 * @brief Implementation of high-performance networking classes
 * 
 * Implements zero-copy networking with Swift Network.framework and Linux io_uring
 */

#include <cswift/detail/high_performance_network.hpp>
#include <cswift/cswift.hpp>
#include <cstring>
#include <memory>

namespace cswift {

// HighPerformanceNetwork implementation

HighPerformanceNetwork::HighPerformanceNetwork(const NetworkEndpoint& endpoint) 
    : endpoint_(endpoint) {
    handle_ = cswift_network_connection_create(
        endpoint.host.c_str(),
        endpoint.port,
        static_cast<int32_t>(endpoint.transport),
        static_cast<int32_t>(endpoint.optimization)
    );
    
    if (!handle_) {
        throw CSException(CS_ERROR_OUT_OF_MEMORY, "Failed to create network connection");
    }
}

void HighPerformanceNetwork::connect() {
    int32_t result = cswift_network_connection_connect(handle_);
    if (result != CS_SUCCESS) {
        throw CSException(static_cast<CSError>(result), "Failed to connect to network endpoint");
    }
}

// Methods removed - duplicates of the ones below

size_t HighPerformanceNetwork::sendZeroCopy(CSNIOByteBuffer& buffer) {
    int32_t result = cswift_network_send_zero_copy(handle_, buffer.getHandle());
    if (result < 0) {
        throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to send data");
    }
    return static_cast<size_t>(result);
}

size_t HighPerformanceNetwork::receiveZeroCopy(CSNIOByteBuffer& buffer, size_t maxBytes) {
    int32_t max = maxBytes > 0 ? static_cast<int32_t>(maxBytes) : buffer.writableBytes();
    int32_t result = cswift_network_receive_zero_copy(handle_, buffer.getHandle(), max);
    if (result < 0) {
        throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to receive data");
    }
    return static_cast<size_t>(result);
}

size_t HighPerformanceNetwork::sendFromGPUBuffer(void* metalBuffer, size_t offset, size_t length) {
    int32_t result = cswift_network_send_gpu_buffer(
        handle_, 
        static_cast<CSHandle>(metalBuffer),
        static_cast<int32_t>(offset),
        static_cast<int32_t>(length)
    );
    if (result < 0) {
        throw CSException(CS_ERROR_OPERATION_FAILED, "Failed to send GPU buffer");
    }
    return static_cast<size_t>(result);
}

bool HighPerformanceNetwork::isActive() const {
    return cswift_network_is_active(handle_) != 0;
}

// NetworkConnectionFactory implementation - methods are inline in header

// NetworkConnectionFactory static factory methods

std::unique_ptr<HighPerformanceNetwork> 
NetworkConnectionFactory::createNeuralEngineOptimized(const std::string& host, uint16_t port) {
    NetworkEndpoint endpoint(host, port, NetworkTransport::TCP, NetworkOptimization::NeuralEngine);
    return std::make_unique<HighPerformanceNetwork>(endpoint);
}

std::unique_ptr<HighPerformanceNetwork> 
NetworkConnectionFactory::createThunderboltOptimized(const std::string& host, uint16_t port) {
    NetworkEndpoint endpoint(host, port, NetworkTransport::Thunderbolt, NetworkOptimization::Throughput);
    return std::make_unique<HighPerformanceNetwork>(endpoint);
}

std::unique_ptr<HighPerformanceNetwork> 
NetworkConnectionFactory::createLatencyOptimized(const std::string& host, uint16_t port) {
    NetworkEndpoint endpoint(host, port, NetworkTransport::TCP, NetworkOptimization::Latency);
    return std::make_unique<HighPerformanceNetwork>(endpoint);
}

} // namespace cswift