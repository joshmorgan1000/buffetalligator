#ifndef CSWIFT_NETWORK_HPP
#define CSWIFT_NETWORK_HPP

#include <cswift/detail/globals.hpp>

namespace cswift {

// ============================================================================
// Network.framework Integration (Apple platforms)
// ============================================================================

/**
 * @brief Network connection states
 */
enum class CSNWConnectionState : int32_t {
    Invalid = 0,    ///< Invalid state
    Waiting = 1,    ///< Waiting for connection
    Preparing = 2,  ///< Preparing connection
    Ready = 3,      ///< Connection ready
    Failed = 4,     ///< Connection failed
    Cancelled = 5   ///< Connection cancelled
};

/**
 * @brief Network protocol types
 */
enum class CSNWProtocolType : int32_t {
    TCP = 0,        ///< TCP protocol
    UDP = 1,        ///< UDP protocol
    WebSocket = 2,  ///< WebSocket protocol
    Custom = 3      ///< Custom protocol
};

/**
 * @brief Network interface types
 */
enum class CSNWInterfaceType : int32_t {
    Any = 0,            ///< Any interface
    WiFi = 1,           ///< WiFi interface
    Cellular = 2,       ///< Cellular interface
    WiredEthernet = 3,  ///< Wired Ethernet interface
    Thunderbolt = 4     ///< Thunderbolt (optimized for high-bandwidth, low-latency)
};

// Forward declarations
class CSNWProtocolFramer;

extern "C" {
    // NWListener functions
    CSHandle cswift_nw_listener_create(uint16_t port, CSHandle parametersHandle, void** error);
    int32_t cswift_nw_listener_set_new_connection_handler(CSHandle listenerHandle, 
                                                         void (*handler)(CSHandle, void*), void* context);
    int32_t cswift_nw_listener_set_state_update_handler(CSHandle listenerHandle,
                                                        void (*handler)(int32_t, void*), void* context);
    int32_t cswift_nw_listener_start(CSHandle listenerHandle, CSHandle queueHandle);
    void cswift_nw_listener_stop(CSHandle listenerHandle);
    void cswift_nw_listener_destroy(CSHandle listenerHandle);
    
    // NWConnection functions
    CSHandle cswift_nw_connection_create(const char* host, uint16_t port, CSHandle parametersHandle, void** error);
    int32_t cswift_nw_connection_set_state_update_handler(CSHandle connectionHandle,
                                                          void (*handler)(int32_t, void*), void* context);
    int32_t cswift_nw_connection_start(CSHandle connectionHandle, CSHandle queueHandle);
    int32_t cswift_nw_connection_send(CSHandle connectionHandle, const void* data, size_t length,
                                     void (*handler)(int32_t, void*), void* context);
    int32_t cswift_nw_connection_receive(CSHandle connectionHandle, size_t minLength, size_t maxLength,
                                        void (*handler)(const void*, size_t, bool, int32_t, void*), void* context);
    void cswift_nw_connection_cancel(CSHandle connectionHandle);
    void cswift_nw_connection_destroy(CSHandle connectionHandle);
    
    // NWParameters functions
    CSHandle cswift_nw_parameters_create_tcp(void);
    CSHandle cswift_nw_parameters_create_udp(void);
    CSHandle cswift_nw_parameters_create_secure_tcp(void);
    CSHandle cswift_nw_parameters_create_websocket(void);
    CSHandle cswift_nw_parameters_create_high_performance(void);
    int32_t cswift_nw_parameters_set_required_interface_type(CSHandle parametersHandle, int32_t interfaceType);
    int32_t cswift_nw_detect_high_speed_interfaces(int32_t* interfaceTypes, int32_t maxInterfaces, int32_t* actualCount);
    void cswift_nw_parameters_destroy(CSHandle parametersHandle);
    
    // Dispatch Queue functions
    CSHandle cswift_dispatch_queue_create(const char* label, int32_t concurrent);
    CSHandle cswift_dispatch_queue_main(void);
    void cswift_dispatch_queue_destroy(CSHandle queueHandle);
    
    // NWProtocolFramer functions
    CSHandle cswift_nw_protocol_framer_create(const char* identifier);
    int32_t cswift_nw_parameters_add_custom_framer(CSHandle parametersHandle, CSHandle framerHandle);
    void cswift_nw_protocol_framer_destroy(CSHandle framerHandle);
    
    // Shared Buffer functions
    CSHandle cswift_shared_buffer_create(size_t capacity, int32_t alignment);
    void* cswift_shared_buffer_data(CSHandle bufferHandle);
    size_t cswift_shared_buffer_capacity(CSHandle bufferHandle);
    void cswift_shared_buffer_set_size(CSHandle bufferHandle, size_t size);
    size_t cswift_shared_buffer_size(CSHandle bufferHandle);
    int32_t cswift_shared_buffer_write_at(CSHandle bufferHandle, size_t offset, const void* data, size_t length);
    int32_t cswift_shared_buffer_read_at(CSHandle bufferHandle, size_t offset, size_t length, void* outData, size_t* outLength);
    
    // Enhanced zero-copy operations
    int32_t cswift_shared_buffer_alignment(CSHandle bufferHandle);
    void cswift_shared_buffer_retain(CSHandle bufferHandle);
    int32_t cswift_shared_buffer_release(CSHandle bufferHandle);
    int32_t cswift_shared_buffer_ref_count(CSHandle bufferHandle);
    int32_t cswift_shared_buffer_as_data_view(CSHandle bufferHandle, const void** outDataPtr);
    const void* cswift_shared_buffer_pointer_at(CSHandle bufferHandle, size_t offset, size_t length);
    void* cswift_shared_buffer_mutable_pointer_at(CSHandle bufferHandle, size_t offset, size_t length);
    
    void cswift_shared_buffer_destroy(CSHandle bufferHandle);
    
    // ============================================================================
    // Zero-Copy Network Enhancements (Phase 1)
    // ============================================================================
    
    // Zero-copy receive using dispatch_data
    int32_t cswift_nw_connection_receive_message(CSHandle connectionHandle,
                                                 CSHandle* dispatchDataHandle,
                                                 void (*handler)(CSHandle, bool, int32_t, void*), void* context);
    
    // Dispatch data operations for zero-copy
    CSHandle cswift_dispatch_data_create_map(const void* buffer, size_t offset, size_t length,
                                            void (*destructor)(void));
    const void* cswift_dispatch_data_get_contiguous_bytes(CSHandle dispatchDataHandle,
                                                          size_t offset, size_t length);
    int32_t cswift_nw_connection_send_dispatch_data(CSHandle connectionHandle,
                                                    CSHandle dispatchDataHandle,
                                                    void (*handler)(int32_t, void*), void* context);
    size_t cswift_dispatch_data_size(CSHandle dispatchDataHandle);
    void cswift_dispatch_data_destroy(CSHandle dispatchDataHandle);
    
    // ============================================================================
    // Phase 2: Direct Buffer Operations
    // ============================================================================
    
    // Receive directly into pre-allocated buffer
    int32_t cswift_nw_connection_receive_into_buffer(CSHandle connectionHandle,
                                                     CSHandle bufferHandle,
                                                     size_t offset,
                                                     size_t minLength,
                                                     size_t maxLength,
                                                     void (*handler)(size_t, bool, int32_t, void*), void* context);
    
    // Zero-copy protocol framer
    CSHandle cswift_nw_protocol_framer_create_zero_copy(const char* identifier);
    int32_t cswift_shared_buffer_prepare_for_network_io(CSHandle bufferHandle);
    
    // Enhanced connection creation with framer
    CSHandle cswift_nw_connection_create_with_framer(const char* host, uint16_t port,
                                                     CSHandle parametersHandle,
                                                     CSHandle framerHandle,
                                                     void** error);
    
    // Framer buffer management
    int32_t cswift_nw_framer_set_target_buffer(CSHandle framerHandle,
                                               CSHandle bufferHandle,
                                               size_t offset,
                                               size_t maxLength);
    
    // ============================================================================
    // Phase 3: Thunderbolt DMA Support
    // ============================================================================
    
    // DMA flags
    enum CSDMAFlags : int32_t {
        CSDMA_READ = 1,
        CSDMA_WRITE = 2,
        CSDMA_READ_WRITE = 3
    };
    
    // Memory region registration for DMA
    int32_t cswift_nw_connection_register_memory_region(CSHandle connectionHandle,
                                                        CSHandle bufferHandle,
                                                        uint64_t* regionID,
                                                        int32_t flags);
    
    // Map peer's memory into local address space
    int32_t cswift_nw_connection_map_peer_memory(CSHandle connectionHandle,
                                                 uint64_t peerRegionID,
                                                 CSHandle localBuffer);
    
    // One-sided RDMA write
    int32_t cswift_nw_connection_rdma_write(CSHandle connectionHandle,
                                            CSHandle localBuffer,
                                            size_t localOffset,
                                            uint64_t peerRegionID,
                                            size_t peerOffset,
                                            size_t length);
    
    // Unregister memory region
    int32_t cswift_nw_connection_unregister_memory_region(CSHandle connectionHandle,
                                                          uint64_t regionID);
    
    // Check DMA support
    int32_t cswift_nw_connection_supports_dma(CSHandle connectionHandle,
                                             int32_t* supportsDMA);
    
    // Thunderbolt device enumeration
    int32_t cswift_thunderbolt_get_device_info(int32_t deviceIndex,
                                               uint32_t* vendorID,
                                               uint32_t* deviceID,
                                               int32_t* linkSpeed);
    
    // ============================================================================
    // Accelerate Framework - Advanced SIMD Operations
    // ============================================================================
    
    // Basic vector operations
    int32_t cswift_accelerate_vadd(const float* a, const float* b, float* result, int32_t length, int32_t dataType);
    int32_t cswift_accelerate_vsub(const float* a, const float* b, float* result, int32_t length, int32_t dataType);
    int32_t cswift_accelerate_vmul(const float* a, const float* b, float* result, int32_t length, int32_t dataType);
    int32_t cswift_accelerate_vdiv(const float* a, const float* b, float* result, int32_t length, int32_t dataType);
    int32_t cswift_accelerate_dotprod(const float* a, const float* b, float* result, int32_t length);
    
    // Matrix operations
    int32_t cswift_accelerate_matmul(const float* a, const float* b, float* result, 
                                     int32_t m, int32_t n, int32_t k, int32_t transposeA, int32_t transposeB);
    
    // Advanced SIMD optimizer updates
    int32_t cswift_accelerate_adam_update(float* parameters, const float* gradients,
                                          float* momentum, float* velocity,
                                          float learningRate, float beta1, float beta2,
                                          float epsilon, int32_t timeStep, int32_t length);
    
    int32_t cswift_accelerate_rmsprop_update(float* parameters, const float* gradients,
                                             float* cache, float learningRate,
                                             float decayRate, float epsilon, int32_t length);
    
    int32_t cswift_accelerate_momentum_sgd_update(float* parameters, const float* gradients,
                                                  float* momentum, float learningRate,
                                                  float momentumFactor, int32_t useNesterov, int32_t length);
    
    int32_t cswift_accelerate_sparse_gradient_update(float* parameters, const int32_t* sparseIndices,
                                                     const float* sparseValues, float* momentum,
                                                     float learningRate, float momentumFactor,
                                                     float compressionRatio, int32_t sparseCount, int32_t totalSize);
    
    // FFT operations
    int32_t cswift_accelerate_fft_forward(const float* input, float* output, int32_t length);
    int32_t cswift_accelerate_fft_inverse(const float* input, float* output, int32_t length);
    
    // Convolution
    int32_t cswift_accelerate_conv1d(const float* signal, int32_t signalLength,
                                     const float* kernel, int32_t kernelLength,
                                     float* result, int32_t resultLength);
    
    // ============================================================================
    // Neural Engine Optimization - Ultra-High Performance ML Inference
    // ============================================================================
    
    // Neural Engine model management
    CSHandle cswift_neural_engine_model_create(const char* modelPath, int32_t optimization,
                                               int32_t precision, int32_t maxBatchSize,
                                               void** error);
    void cswift_neural_engine_model_destroy(CSHandle modelHandle);
    
    // Neural Engine inference
    int32_t cswift_neural_engine_predict(CSHandle modelHandle, const float* inputData,
                                         const int32_t* inputShape, int32_t inputShapeCount,
                                         const char* inputName, float* outputData,
                                         int32_t outputSize, void** error);
    
    int32_t cswift_neural_engine_batch_predict(CSHandle modelHandle, const float** batchInputData,
                                               const int32_t* inputShape, int32_t inputShapeCount,
                                               const char* inputName, int32_t batchSize,
                                               float** batchOutputData, int32_t outputSize, void** error);
    
    // Neural Engine model compilation
    int32_t cswift_neural_engine_compile_model(const char* inputPath, const char* outputPath,
                                               int32_t optimization, int32_t precision);
    
    
    // ============================================================================
    // Secure Federated Learning - Enterprise Privacy & Security
    // ============================================================================
    
    // Secure gradient aggregation
    CSHandle cswift_secure_aggregator_create(int32_t privacyLevel, int32_t encryptionMode,
                                             int32_t noiseType, float epsilonPrivacy,
                                             float deltaPrivacy, void** error);
    void cswift_secure_aggregator_destroy(CSHandle aggregatorHandle);
    
    // Differential privacy operations
    int32_t cswift_add_dp_noise(CSHandle aggregatorHandle, float* gradients,
                                int32_t gradientCount, float sensitivity);
    
    // Secure encryption/decryption
    int32_t cswift_encrypt_gradients(CSHandle aggregatorHandle, const float* gradients,
                                    int32_t gradientCount, void** encryptedData, int* encryptedSize);
    int32_t cswift_decrypt_gradients(CSHandle aggregatorHandle, const uint8_t* encryptedData,
                                    int32_t encryptedSize, float* gradients, int32_t maxGradientCount);
    
    // Secure aggregation
    int32_t cswift_secure_aggregate(CSHandle aggregatorHandle, const float** clientGradients,
                                   const int32_t* gradientCounts, int32_t clientCount,
                                   float* aggregatedGradients, int32_t maxOutputSize);
    
    // Privacy-preserving compression
    int32_t cswift_compress_gradients_private(const float* gradients, int32_t gradientCount,
                                             float compressionRatio, float noiseScale,
                                             int32_t* indices, float* values, int32_t maxOutputSize);
}

} // namespace cswift

#endif // CSWIFT_NETWORK_HPP
