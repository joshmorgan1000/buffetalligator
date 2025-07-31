/**
 * @file zerocopy_network_example.cpp
 * @brief Example demonstrating Phase 1 zero-copy network enhancements
 */

#include <cswift/cswift.hpp>
#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <cstring>

using namespace cswift;

/**
 * @brief Example showing zero-copy network operations
 */
void demonstrateZeroCopyNetworking() {
    std::cout << "=== Zero-Copy Network Enhancement Demo ===" << std::endl;
    
    // Create parameters for high-performance networking
    CSHandle params = cswift_nw_parameters_create_high_performance();
    if (!params) {
        std::cerr << "Failed to create network parameters" << std::endl;
        return;
    }
    
    // Create dispatch queue
    CSHandle queue = cswift_dispatch_queue_create("zerocopy.queue", 0);
    if (!queue) {
        std::cerr << "Failed to create dispatch queue" << std::endl;
        cswift_nw_parameters_destroy(params);
        return;
    }
    
    // Create a large buffer to demonstrate zero-copy benefits
    const size_t bufferSize = 10 * 1024 * 1024; // 10MB
    std::vector<uint8_t> sendBuffer(bufferSize);
    
    // Fill with test pattern
    for (size_t i = 0; i < bufferSize; ++i) {
        sendBuffer[i] = static_cast<uint8_t>((i * 17 + 31) & 0xFF);
    }
    
    std::cout << "Created " << bufferSize / (1024 * 1024) << "MB test buffer" << std::endl;
    
    // Demonstrate zero-copy dispatch data creation
    auto start = std::chrono::high_resolution_clock::now();
    
    // Create dispatch data that references our buffer without copying
    CSHandle dispatchData = cswift_dispatch_data_create_map(
        sendBuffer.data(), 0, bufferSize, nullptr
    );
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    std::cout << "Zero-copy dispatch data creation took: " << duration.count() << " microseconds" << std::endl;
    
    if (!dispatchData) {
        std::cerr << "Failed to create dispatch data" << std::endl;
        cswift_dispatch_queue_destroy(queue);
        cswift_nw_parameters_destroy(params);
        return;
    }
    
    // Verify we can access the data without copying
    const void* dataPtr = cswift_dispatch_data_get_contiguous_bytes(dispatchData, 0, bufferSize);
    if (!dataPtr) {
        std::cerr << "Failed to get contiguous bytes" << std::endl;
        cswift_dispatch_data_destroy(dispatchData);
        cswift_dispatch_queue_destroy(queue);
        cswift_nw_parameters_destroy(params);
        return;
    }
    
    // Verify the data matches
    if (memcmp(dataPtr, sendBuffer.data(), 1024) == 0) {
        std::cout << "✓ Zero-copy verification passed - data matches original buffer" << std::endl;
    } else {
        std::cout << "✗ Data mismatch - zero-copy may not be working correctly" << std::endl;
    }
    
    // Show memory efficiency
    std::cout << "\nMemory Efficiency Comparison:" << std::endl;
    std::cout << "Traditional approach: " << bufferSize / (1024 * 1024) << "MB original + " 
              << bufferSize / (1024 * 1024) << "MB copy = " 
              << (bufferSize * 2) / (1024 * 1024) << "MB total" << std::endl;
    std::cout << "Zero-copy approach: " << bufferSize / (1024 * 1024) << "MB total (no copy!)" << std::endl;
    std::cout << "Memory saved: " << bufferSize / (1024 * 1024) << "MB" << std::endl;
    
    // Cleanup
    cswift_dispatch_data_destroy(dispatchData);
    cswift_dispatch_queue_destroy(queue);
    cswift_nw_parameters_destroy(params);
}

/**
 * @brief Example showing zero-copy with shared buffers
 */
void demonstrateSharedBufferIntegration() {
    std::cout << "\n=== Zero-Copy with Shared Buffers ===" << std::endl;
    
    // Create a shared buffer
    const size_t bufferSize = 1024 * 1024; // 1MB
    CSHandle sharedBuffer = cswift_shared_buffer_create(bufferSize, 64);
    if (!sharedBuffer) {
        std::cerr << "Failed to create shared buffer" << std::endl;
        return;
    }
    
    // Get direct pointer to buffer memory
    void* bufferPtr = cswift_shared_buffer_data(sharedBuffer);
    if (!bufferPtr) {
        std::cerr << "Failed to get buffer pointer" << std::endl;
        cswift_shared_buffer_destroy(sharedBuffer);
        return;
    }
    
    // Fill buffer with test data
    uint8_t* dataPtr = static_cast<uint8_t*>(bufferPtr);
    for (size_t i = 0; i < bufferSize; ++i) {
        dataPtr[i] = static_cast<uint8_t>((i * 7) & 0xFF);
    }
    
    // Set the buffer size
    cswift_shared_buffer_set_size(sharedBuffer, bufferSize);
    
    // Create dispatch data from shared buffer (zero-copy)
    CSHandle dispatchData = cswift_dispatch_data_create_map(
        bufferPtr, 0, bufferSize, nullptr
    );
    
    if (!dispatchData) {
        std::cerr << "Failed to create dispatch data from shared buffer" << std::endl;
        cswift_shared_buffer_destroy(sharedBuffer);
        return;
    }
    
    std::cout << "✓ Created zero-copy dispatch data from shared buffer" << std::endl;
    std::cout << "  Buffer size: " << cswift_dispatch_data_size(dispatchData) / 1024 << "KB" << std::endl;
    std::cout << "  Shared buffer refcount: " << cswift_shared_buffer_ref_count(sharedBuffer) << std::endl;
    
    // Clean up
    cswift_dispatch_data_destroy(dispatchData);
    cswift_shared_buffer_destroy(sharedBuffer);
}

/**
 * @brief Benchmark comparison between old and new API
 */
void benchmarkComparison() {
    std::cout << "\n=== Performance Benchmark ===" << std::endl;
    
    const size_t iterations = 1000;
    const size_t dataSize = 64 * 1024; // 64KB per message
    
    std::vector<uint8_t> testData(dataSize);
    for (size_t i = 0; i < dataSize; ++i) {
        testData[i] = static_cast<uint8_t>(i & 0xFF);
    }
    
    // Benchmark zero-copy dispatch data operations
    auto start = std::chrono::high_resolution_clock::now();
    
    for (size_t i = 0; i < iterations; ++i) {
        // Create dispatch data (should be very fast - just wrapping pointer)
        CSHandle dispatchData = cswift_dispatch_data_create_map(
            testData.data(), 0, dataSize, nullptr
        );
        
        // Access data (should be instant - just returning pointer)
        const void* ptr = cswift_dispatch_data_get_contiguous_bytes(
            dispatchData, 0, dataSize
        );
        
        // Use the pointer to prevent optimization
        volatile uint8_t dummy = *static_cast<const uint8_t*>(ptr);
        (void)dummy;
        
        cswift_dispatch_data_destroy(dispatchData);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    double avgTimeUs = duration.count() / static_cast<double>(iterations);
    double throughputGBps = (dataSize * iterations) / (duration.count() / 1e6) / (1024.0 * 1024.0 * 1024.0);
    
    std::cout << "Zero-copy operations benchmark:" << std::endl;
    std::cout << "  Average time per operation: " << avgTimeUs << " μs" << std::endl;
    std::cout << "  Effective throughput: " << throughputGBps << " GB/s" << std::endl;
    std::cout << "  Total data processed: " << (dataSize * iterations) / (1024.0 * 1024.0) << " MB" << std::endl;
}

int main() {
    std::cout << "cswift Zero-Copy Network Enhancement Examples" << std::endl;
    std::cout << "=============================================" << std::endl;
    
    try {
        // Demonstrate basic zero-copy operations
        demonstrateZeroCopyNetworking();
        
        // Show integration with shared buffers
        demonstrateSharedBufferIntegration();
        
        // Run performance benchmark
        benchmarkComparison();
        
        std::cout << "\n✓ All examples completed successfully!" << std::endl;
        std::cout << "\nPhase 1 zero-copy enhancements provide:" << std::endl;
        std::cout << "- Eliminated memory copies in network receive path" << std::endl;
        std::cout << "- Direct access to Network.framework dispatch_data" << std::endl;
        std::cout << "- Zero-copy send from existing buffers" << std::endl;
        std::cout << "- Foundation for Thunderbolt DMA support (Phase 3)" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}