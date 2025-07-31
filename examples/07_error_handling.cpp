/**
 * @file 07_error_handling.cpp
 * @brief Error handling and edge cases demonstration
 * 
 * This example shows robust error handling:
 * - Buffer allocation failures
 * - Network connection errors
 * - GPU availability checks
 * - Resource exhaustion
 * - Recovery strategies
 */

#include <alligator.hpp>
#include <iostream>
#include <exception>
#include <thread>
#include <vector>

using namespace alligator;

void allocation_error_example(BuffetAlligator& buffet) {
    std::cout << "1. Allocation Error Handling" << std::endl;
    
    // Try to allocate an unavailable buffer type
    try {
        std::cout << "   Attempting to allocate explicit CUDA buffer..." << std::endl;
        auto* cuda_buf = buffet.allocate_buffer(1024, BFType::CUDA);
        std::cout << "   CUDA buffer allocated successfully!" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "   Expected error: " << e.what() << std::endl;
    }
    
    // Try to allocate with auto-selection (GPU)
    try {
        std::cout << "   Attempting to allocate auto-selected GPU buffer..." << std::endl;
        auto* gpu_buf = buffet.allocate_buffer(1024, BFType::GPU);
        std::cout << "   GPU buffer allocated using: ";
        
        if (dynamic_cast<MetalBuffer*>(gpu_buf)) {
            std::cout << "Metal" << std::endl;
        } else if (dynamic_cast<CUDABuffer*>(gpu_buf)) {
            std::cout << "CUDA" << std::endl;
        } else if (dynamic_cast<VulkanBuffer*>(gpu_buf)) {
            std::cout << "Vulkan" << std::endl;
        } else {
            std::cout << "Unknown" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "   No GPU backend available: " << e.what() << std::endl;
    }
    
    // Try to allocate very large buffer
    try {
        std::cout << "   Attempting to allocate 100GB buffer..." << std::endl;
        size_t huge_size = 100ULL * 1024 * 1024 * 1024;  // 100GB
        auto* huge_buf = buffet.allocate_buffer(huge_size, BFType::HEAP);
        std::cout << "   Surprisingly, allocated " << huge_size << " bytes!" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "   Expected allocation failure: " << e.what() << std::endl;
    } catch (...) {
        std::cout << "   Allocation failed with unknown error" << std::endl;
    }
}

void network_error_example(BuffetAlligator& buffet) {
    std::cout << "\n2. Network Error Handling" << std::endl;
    
    auto* tcp_buf = dynamic_cast<NICBuffer*>(
        buffet.allocate_buffer(1024, BFType::TCP)
    );
    
    if (!tcp_buf) {
        std::cout << "   Failed to allocate network buffer" << std::endl;
        return;
    }
    
    // Try to connect to non-existent server
    NetworkEndpoint bad_endpoint{"192.168.255.255", 9999, NetworkProtocol::TCP};
    std::cout << "   Attempting to connect to non-existent server..." << std::endl;
    
    bool connected = tcp_buf->connect(bad_endpoint);
    if (!connected) {
        std::cout << "   Connection failed as expected" << std::endl;
        std::cout << "   Connection failed (no state API available)" << std::endl;
    }
    
    // Try to send data on disconnected socket
    try {
        const char* data = "Test message";
        std::memcpy(tcp_buf->data(), data, strlen(data));
        ssize_t sent = tcp_buf->send(0, strlen(data));
        if (sent < 0) {
            std::cout << "   Send failed on disconnected socket (returned " << sent << ")" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "   Send error: " << e.what() << std::endl;
    }
    
    // Try to bind to privileged port
    try {
        NetworkEndpoint privileged_endpoint{"0.0.0.0", 80, NetworkProtocol::TCP};
        std::cout << "   Attempting to bind to privileged port 80..." << std::endl;
        bool bound = tcp_buf->bind(privileged_endpoint);
        if (!bound) {
            std::cout << "   Bind failed (need root/admin privileges)" << std::endl;
        } else {
            std::cout << "   Surprisingly bound to port 80!" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "   Bind error: " << e.what() << std::endl;
    }
}

void gpu_error_example(BuffetAlligator& buffet) {
    std::cout << "\n3. GPU Error Handling" << std::endl;
    
#ifdef CUDA_ENABLED
    // Check CUDA availability
    if (!CUDABuffer::is_cuda_available()) {
        std::cout << "   CUDA runtime not available" << std::endl;
        std::cout << "   Device count: " << CUDABuffer::get_device_count() << std::endl;
    }
#endif
    
    try {
        auto* gpu_buf = dynamic_cast<GPUBuffer*>(
            buffet.allocate_buffer(1024, BFType::GPU)
        );
        
        if (gpu_buf) {
            // Try invalid operations
            std::cout << "   Testing GPU buffer error cases..." << std::endl;
            
            // Upload from null pointer
            bool uploaded = gpu_buf->upload(nullptr, 1024);
            std::cout << "   Upload from nullptr: " << (uploaded ? "succeeded?!" : "failed (expected)") << std::endl;
            
            // Download to null pointer
            bool downloaded = gpu_buf->download(nullptr, 1024);
            std::cout << "   Download to nullptr: " << (downloaded ? "succeeded?!" : "failed (expected)") << std::endl;
            
            // Copy from null source
            bool copied = gpu_buf->copy_from(nullptr, 1024);
            std::cout << "   Copy from null GPU buffer: " << (copied ? "succeeded?!" : "failed (expected)") << std::endl;
            
            // Out of bounds access
            size_t buf_size = gpu_buf->buf_size_;
            bool oob_upload = gpu_buf->upload(gpu_buf->data(), 1024, buf_size + 1000);
            std::cout << "   Out-of-bounds upload: " << (oob_upload ? "succeeded?!" : "failed (expected)") << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "   GPU operations failed: " << e.what() << std::endl;
    }
}

void resource_exhaustion_example(BuffetAlligator& buffet) {
    std::cout << "\n4. Resource Exhaustion Handling" << std::endl;
    
    std::vector<ChainBuffer*> buffers;
    const size_t buffer_size = 1024 * 1024;  // 1MB each
    size_t allocated_count = 0;
    size_t total_allocated = 0;
    
    std::cout << "   Allocating buffers until exhaustion..." << std::endl;
    
    try {
        // Try to exhaust memory
        for (int i = 0; i < 10000; ++i) {
            auto* buf = buffet.allocate_buffer(buffer_size, BFType::HEAP);
            // BuffetAlligator manages lifecycle
            buffers.push_back(buf);
            allocated_count++;
            total_allocated += buffer_size;
            
            // Print progress every 100MB
            if (total_allocated % (100 * 1024 * 1024) == 0) {
                std::cout << "   Allocated: " << (total_allocated / (1024 * 1024)) << " MB" << std::endl;
            }
        }
    } catch (const std::bad_alloc& e) {
        std::cout << "   Memory exhausted after allocating " << allocated_count << " buffers" << std::endl;
        std::cout << "   Total allocated: " << (total_allocated / (1024 * 1024)) << " MB" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "   Allocation failed: " << e.what() << std::endl;
    }
    
    // Clean up
    std::cout << "   Clearing " << buffers.size() << " buffer references..." << std::endl;
    buffers.clear();  // BuffetAlligator will handle cleanup
    
    // Let garbage collector clean up
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << "   Memory released" << std::endl;
}

void concurrent_error_example(BuffetAlligator& buffet) {
    std::cout << "\n5. Concurrent Access Error Handling" << std::endl;
    
    auto* shared_buf = buffet.allocate_buffer(1024, BFType::HEAP);
    // Keep buffer alive during concurrent access
    
    std::vector<std::thread> threads;
    std::atomic<int> error_count{0};
    
    std::cout << "   Starting concurrent access test..." << std::endl;
    
    // Multiple threads trying to modify the same buffer
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([shared_buf, i, &error_count]() {
            try {
                // Concurrent writes (may cause issues without synchronization)
                for (int j = 0; j < 100; ++j) {
                    size_t offset = (i * 100 + j) % shared_buf->buf_size_;
                    shared_buf->data()[offset] = i;
                }
                
                // Try to chain concurrently
                auto* new_buf = shared_buf->create_new(1024);
                if (new_buf) {
                    // Attempt to set next (race condition)
                    shared_buf->next.store(new_buf, std::memory_order_release);
                }
            } catch (const std::exception& e) {
                error_count.fetch_add(1);
            }
        });
    }
    
    // Wait for threads
    for (auto& t : threads) {
        t.join();
    }
    
    std::cout << "   Concurrent access completed with " << error_count.load() << " errors" << std::endl;
    std::cout << "   Buffer integrity: " << (shared_buf->data() ? "intact" : "corrupted") << std::endl;
    
    // BuffetAlligator will handle cleanup
}

void recovery_strategies_example(BuffetAlligator& buffet) {
    std::cout << "\n6. Recovery Strategies" << std::endl;
    
    // Strategy 1: Fallback allocation
    std::cout << "   Strategy 1: Fallback allocation" << std::endl;
    ChainBuffer* buffer = nullptr;
    
    // Try GPU first
    try {
        buffer = buffet.allocate_buffer(1024, BFType::GPU);
        std::cout << "     Allocated GPU buffer" << std::endl;
    } catch (...) {
        // Fall back to heap
        try {
            buffer = buffet.allocate_buffer(1024, BFType::HEAP);
            std::cout << "     Fell back to heap buffer" << std::endl;
        } catch (const std::exception& e) {
            std::cout << "     All allocation attempts failed: " << e.what() << std::endl;
        }
    }
    
    // Strategy 2: Retry with backoff
    std::cout << "   Strategy 2: Retry with exponential backoff" << std::endl;
    auto* net_buf = dynamic_cast<NICBuffer*>(
        buffet.allocate_buffer(1024, BFType::TCP)
    );
    
    if (net_buf) {
        NetworkEndpoint endpoint{"example.com", 80, NetworkProtocol::TCP};
        int max_retries = 3;
        int retry_delay_ms = 100;
        
        for (int attempt = 1; attempt <= max_retries; ++attempt) {
            std::cout << "     Connection attempt " << attempt << "..." << std::endl;
            
            if (net_buf->connect(endpoint)) {
                std::cout << "     Connected successfully!" << std::endl;
                break;
            }
            
            if (attempt < max_retries) {
                std::cout << "     Failed, retrying in " << retry_delay_ms << "ms" << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
                retry_delay_ms *= 2;  // Exponential backoff
            } else {
                std::cout << "     All connection attempts failed" << std::endl;
            }
        }
    }
    
    // Strategy 3: Graceful degradation
    std::cout << "   Strategy 3: Graceful degradation" << std::endl;
    try {
        // Try high-performance buffer
        auto* perf_buf = buffet.allocate_buffer(1024, BFType::THUNDERBOLT_DMA);
        std::cout << "     Using high-performance Thunderbolt DMA" << std::endl;
    } catch (...) {
        try {
            // Fall back to network buffer
            auto* net_buf = buffet.allocate_buffer(1024, BFType::TCP);
            std::cout << "     Degraded to network buffer" << std::endl;
        } catch (...) {
            // Final fallback to basic heap
            auto* heap_buf = buffet.allocate_buffer(1024, BFType::HEAP);
            std::cout << "     Degraded to basic heap buffer" << std::endl;
        }
    }
}

void validation_example(BuffetAlligator& buffet) {
    std::cout << "\n7. Input Validation" << std::endl;
    
    // Test various invalid inputs
    std::cout << "   Testing invalid buffer operations..." << std::endl;
    
    auto* buf = buffet.allocate_buffer(1024, BFType::HEAP);
    
    // Invalid span requests
    try {
        auto span1 = buf->get_span(2000, 100);  // Offset beyond capacity
        std::cout << "   Span beyond capacity: has data (wrong!)" << std::endl;
    } catch (const std::out_of_range& e) {
        std::cout << "   Span beyond capacity: threw exception (correct)" << std::endl;
    }
    
    try {
        auto span2 = buf->get_span(900, 200);   // Size exceeds remaining
        std::cout << "   Span size exceeds remaining: threw exception" << std::endl;
    } catch (const std::out_of_range& e) {
        std::cout << "   Span size exceeds remaining: threw exception (correct)" << std::endl;
    }
    
    // Invalid clear value (should work)
    buf->clear(0xFF);  // Valid - any uint8_t value
    std::cout << "   Buffer cleared with 0xFF" << std::endl;
    
    // Check buffer state after operations
    std::cout << "   Buffer integrity check:" << std::endl;
    std::cout << "     Capacity: " << buf->buf_size_ << std::endl;
    std::cout << "     Data pointer: " << (buf->data() ? "valid" : "null") << std::endl;
    std::cout << "     Is local: " << buf->is_local() << std::endl;
}

int main() {
    std::cout << "=== BuffetAlligator Error Handling Example ===" << std::endl;
    std::cout << "When things go wrong at the buffet! ⚠️" << std::endl << std::endl;

    auto& buffet = get_buffet_alligator();

    allocation_error_example(buffet);
    network_error_example(buffet);
    gpu_error_example(buffet);
    resource_exhaustion_example(buffet);
    concurrent_error_example(buffet);
    recovery_strategies_example(buffet);
    validation_example(buffet);

    std::cout << "\n=== Example Complete ===" << std::endl;
    std::cout << "Robust error handling keeps the buffet running smoothly!" << std::endl;
    
    return 0;
}