/**
 * @file 07_error_handling.cpp
 * @brief Example demonstrating error handling and fallback strategies
 * 
 * This example shows how to:
 * - Handle allocation failures gracefully
 * - Implement fallback strategies
 * - Deal with platform-specific limitations
 * - Create robust buffer management code
 */

#include <alligator.hpp>
#include <iostream>
#include <vector>
#include <exception>
#include <cstring>

using namespace alligator;

void allocation_failure_handling() {
    std::cout << "\n=== Handling Allocation Failures ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    // Try to allocate an extremely large buffer
    constexpr size_t huge_size = 100ULL * 1024 * 1024 * 1024; // 100GB
    
    std::cout << "Attempting to allocate " << huge_size / (1024.0 * 1024 * 1024) 
              << " GB buffer..." << std::endl;
    
    try {
        auto* huge_buffer = alligator.allocate_buffer(huge_size, BFType::HEAP);
        if (huge_buffer) {
            std::cout << "Surprisingly, allocation succeeded!" << std::endl;
            // Clean up
            huge_buffer->deallocate();
        }
    } catch (const std::bad_alloc& e) {
        std::cout << "Caught std::bad_alloc: " << e.what() << std::endl;
        std::cout << "This is expected for such a large allocation" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "Caught exception: " << e.what() << std::endl;
    }
    
    // Implement a retry strategy with smaller sizes
    std::cout << "\nImplementing fallback strategy:" << std::endl;
    
    size_t sizes[] = {1024 * 1024 * 1024, 512 * 1024 * 1024, 256 * 1024 * 1024, 
                      128 * 1024 * 1024, 64 * 1024 * 1024};
    
    ChainBuffer* buffer = nullptr;
    for (size_t size : sizes) {
        try {
            buffer = alligator.allocate_buffer(size, BFType::HEAP);
            if (buffer) {
                std::cout << "✓ Successfully allocated " << size / (1024.0 * 1024) 
                          << " MB buffer" << std::endl;
                break;
            }
        } catch (const std::exception& e) {
            std::cout << "✗ Failed to allocate " << size / (1024.0 * 1024) 
                      << " MB: " << e.what() << std::endl;
        }
    }
    
    if (!buffer) {
        std::cout << "All allocation attempts failed!" << std::endl;
    }
}

void platform_specific_errors() {
    std::cout << "\n=== Platform-Specific Error Handling ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    constexpr size_t buffer_size = 1024 * 1024; // 1MB
    
    // Try to allocate platform-specific buffers
    struct BufferTest {
        BFType type;
        const char* name;
        const char* platform;
    };
    
    BufferTest tests[] = {
        {BFType::CUDA, "CUDA", "NVIDIA GPU"},
        {BFType::METAL, "Metal", "Apple platforms"},
        {BFType::VULKAN, "Vulkan", "Cross-platform GPU"},
        {BFType::FOUNDATION_TCP, "Foundation TCP", "Apple platforms"},
        {BFType::ASIO_TCP, "ASIO TCP", "Cross-platform"}
    };
    
    for (const auto& test : tests) {
        std::cout << "\nTrying " << test.name << " (" << test.platform << "):" << std::endl;
        
        try {
            auto* buffer = alligator.allocate_buffer(buffer_size, test.type);
            if (buffer) {
                std::cout << "✓ " << test.name << " buffer allocated successfully" << std::endl;
                std::cout << "  Buffer ID: " << buffer->id_.load() << std::endl;
            }
        } catch (const std::runtime_error& e) {
            std::cout << "✗ " << test.name << " not available: " << e.what() << std::endl;
            std::cout << "  This is expected if not on " << test.platform << std::endl;
        } catch (const std::exception& e) {
            std::cout << "✗ Unexpected error: " << e.what() << std::endl;
        }
    }
}

void automatic_fallback_demo() {
    std::cout << "\n=== Automatic Fallback Demonstration ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    constexpr size_t buffer_size = 4 * 1024 * 1024; // 4MB
    
    // GPU buffer with automatic fallback
    std::cout << "Requesting GPU buffer (automatic selection):" << std::endl;
    
    try {
        auto* gpu_buffer = alligator.allocate_buffer(buffer_size, BFType::GPU);
        if (gpu_buffer) {
            std::cout << "✓ GPU buffer allocated successfully" << std::endl;
            
            #ifdef __APPLE__
                std::cout << "  Expected: Metal implementation" << std::endl;
            #else
                std::cout << "  Expected: CUDA or Vulkan implementation" << std::endl;
            #endif
        }
    } catch (const std::runtime_error& e) {
        std::cout << "✗ No GPU support available: " << e.what() << std::endl;
        std::cout << "  Would need to fall back to CPU processing" << std::endl;
        
        // Manual fallback to heap buffer
        std::cout << "\nFalling back to heap buffer:" << std::endl;
        auto* cpu_buffer = alligator.allocate_buffer(buffer_size, BFType::HEAP);
        if (cpu_buffer) {
            std::cout << "✓ Heap buffer allocated as fallback" << std::endl;
        }
    }
    
    // Network buffer with automatic fallback
    std::cout << "\nRequesting TCP buffer (automatic selection):" << std::endl;
    
    try {
        auto* tcp_buffer = alligator.allocate_buffer(buffer_size, BFType::TCP);
        if (tcp_buffer) {
            std::cout << "✓ TCP buffer allocated successfully" << std::endl;
            
            #ifdef __APPLE__
                std::cout << "  Using: Foundation Network.framework" << std::endl;
            #else
                std::cout << "  Using: ASIO implementation" << std::endl;
            #endif
        }
    } catch (const std::runtime_error& e) {
        std::cout << "✗ No network support available: " << e.what() << std::endl;
    }
}

void robust_buffer_allocation() {
    std::cout << "\n=== Robust Buffer Allocation Pattern ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    // Define a robust allocation function
    auto allocate_with_fallback = [&alligator](
        size_t size, 
        const std::vector<BFType>& preferences
    ) -> ChainBuffer* {
        
        for (BFType type : preferences) {
            try {
                auto* buffer = alligator.allocate_buffer(size, type);
                if (buffer) {
                    std::cout << "✓ Allocated buffer type: " << static_cast<int>(type) << std::endl;
                    return buffer;
                }
            } catch (const std::exception& e) {
                std::cout << "  Failed to allocate type " << static_cast<int>(type) 
                          << ": " << e.what() << std::endl;
            }
        }
        
        return nullptr;
    };
    
    // Example 1: GPU compute with fallback
    std::cout << "\nGPU compute allocation with fallback:" << std::endl;
    std::vector<BFType> gpu_preferences = {BFType::GPU, BFType::HEAP};
    
    auto* compute_buffer = allocate_with_fallback(1024 * 1024, gpu_preferences);
    if (compute_buffer) {
        std::cout << "Compute buffer ready, ID: " << compute_buffer->id_.load() << std::endl;
    } else {
        std::cout << "Failed to allocate compute buffer!" << std::endl;
    }
    
    // Example 2: Network with preferences
    std::cout << "\nNetwork allocation with preferences:" << std::endl;
    std::vector<BFType> network_preferences = {
        BFType::QUIC,           // Prefer QUIC
        BFType::TCP,            // Fall back to TCP
        BFType::UDP,            // Last resort UDP
        BFType::HEAP            // Ultimate fallback
    };
    
    auto* network_buffer = allocate_with_fallback(65536, network_preferences);
    if (network_buffer) {
        std::cout << "Network buffer ready, ID: " << network_buffer->id_.load() << std::endl;
    }
}

void error_recovery_strategies() {
    std::cout << "\n=== Error Recovery Strategies ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    // Strategy 1: Exponential backoff for allocation
    std::cout << "\n1. Exponential backoff allocation:" << std::endl;
    
    auto allocate_with_backoff = [&alligator](size_t size, BFType type, int max_retries = 3) {
        int retry = 0;
        int backoff_ms = 100;
        
        while (retry < max_retries) {
            try {
                auto* buffer = alligator.allocate_buffer(size, type);
                if (buffer) {
                    std::cout << "  Success after " << retry << " retries" << std::endl;
                    return buffer;
                }
            } catch (const std::exception& e) {
                std::cout << "  Retry " << retry << " failed: " << e.what() << std::endl;
                
                if (retry < max_retries - 1) {
                    std::cout << "  Waiting " << backoff_ms << "ms before retry..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
                    backoff_ms *= 2; // Exponential backoff
                }
            }
            retry++;
        }
        
        return static_cast<ChainBuffer*>(nullptr);
    };
    
    // Test with a challenging allocation
    auto* buffer = allocate_with_backoff(100 * 1024 * 1024, BFType::GPU, 3);
    if (!buffer) {
        std::cout << "  All retries exhausted" << std::endl;
    }
    
    // Strategy 2: Progressive size reduction
    std::cout << "\n2. Progressive size reduction:" << std::endl;
    
    auto allocate_with_reduction = [&alligator](size_t initial_size, BFType type) {
        size_t size = initial_size;
        const size_t min_size = 1024 * 1024; // 1MB minimum
        
        while (size >= min_size) {
            try {
                auto* buffer = alligator.allocate_buffer(size, type);
                if (buffer) {
                    std::cout << "  Allocated " << size / (1024.0 * 1024) << " MB "
                              << "(reduced from " << initial_size / (1024.0 * 1024) << " MB)" << std::endl;
                    return buffer;
                }
            } catch (const std::exception& e) {
                std::cout << "  Failed at " << size / (1024.0 * 1024) << " MB" << std::endl;
                size /= 2; // Reduce by half
            }
        }
        
        return static_cast<ChainBuffer*>(nullptr);
    };
    
    auto* reduced_buffer = allocate_with_reduction(1024 * 1024 * 1024, BFType::HEAP);
    if (!reduced_buffer) {
        std::cout << "  Could not allocate even minimum size" << std::endl;
    }
}

void buffer_validation() {
    std::cout << "\n=== Buffer Validation and Safety ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    // Safe buffer wrapper with validation
    class SafeBuffer {
    private:
        ChainBuffer* buffer_;
        bool validated_;
        
    public:
        SafeBuffer(ChainBuffer* buf) : buffer_(buf), validated_(false) {
            validate();
        }
        
        bool validate() {
            if (!buffer_) {
                std::cout << "Validation failed: null buffer" << std::endl;
                return false;
            }
            
            if (buffer_->buf_size_ == 0) {
                std::cout << "Validation failed: zero size" << std::endl;
                return false;
            }
            
            if (!buffer_->is_registered()) {
                std::cout << "Validation failed: not registered" << std::endl;
                return false;
            }
            
            validated_ = true;
            std::cout << "Buffer validated successfully" << std::endl;
            return true;
        }
        
        bool write(const void* data, size_t size, size_t offset = 0) {
            if (!validated_) {
                std::cout << "Write failed: buffer not validated" << std::endl;
                return false;
            }
            
            if (offset + size > buffer_->buf_size_) {
                std::cout << "Write failed: would exceed buffer size" << std::endl;
                return false;
            }
            
            std::memcpy(buffer_->data() + offset, data, size);
            return true;
        }
        
        ChainBuffer* get() { return validated_ ? buffer_ : nullptr; }
    };
    
    // Test safe buffer
    auto* raw_buffer = alligator.allocate_buffer(1024, BFType::HEAP);
    SafeBuffer safe_buffer(raw_buffer);
    
    const char* test_data = "Safe write test";
    if (safe_buffer.write(test_data, strlen(test_data))) {
        std::cout << "Safe write succeeded" << std::endl;
    }
    
    // Test write beyond bounds
    if (!safe_buffer.write(test_data, 2000)) {
        std::cout << "Correctly prevented buffer overflow" << std::endl;
    }
}

void best_practices() {
    std::cout << "\n=== Error Handling Best Practices ===" << std::endl;
    
    std::cout << "\n1. Always check allocation results:" << std::endl;
    std::cout << "   if (!buffer) { /* handle error */ }" << std::endl;
    
    std::cout << "\n2. Use try-catch for explicit types:" << std::endl;
    std::cout << "   try { buffer = allocate(size, BFType::CUDA); }" << std::endl;
    std::cout << "   catch (const std::runtime_error& e) { /* handle */ }" << std::endl;
    
    std::cout << "\n3. Implement fallback chains:" << std::endl;
    std::cout << "   GPU -> CPU, Foundation -> ASIO, QUIC -> TCP -> UDP" << std::endl;
    
    std::cout << "\n4. Validate buffer state:" << std::endl;
    std::cout << "   Check size, registration, accessibility" << std::endl;
    
    std::cout << "\n5. Clean up on errors:" << std::endl;
    std::cout << "   Use RAII, pin/unpin correctly, handle partial initialization" << std::endl;
    
    std::cout << "\n6. Log errors appropriately:" << std::endl;
    std::cout << "   Include context, buffer type, size, error message" << std::endl;
    
    std::cout << "\n7. Test error paths:" << std::endl;
    std::cout << "   Force allocation failures, test on different platforms" << std::endl;
}

int main() {
    std::cout << "BuffetAlligator Error Handling Examples" << std::endl;
    std::cout << "=======================================" << std::endl;
    
    try {
        allocation_failure_handling();
        platform_specific_errors();
        automatic_fallback_demo();
        robust_buffer_allocation();
        error_recovery_strategies();
        buffer_validation();
        best_practices();
    } catch (const std::exception& e) {
        std::cerr << "Unhandled error in main: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "\nError handling examples completed!" << std::endl;
    return 0;
}