/**
 * @file 05_buffer_optimization.cpp
 * @brief Example demonstrating buffer usage patterns for performance
 * 
 * This example shows how to:
 * - Allocate buffers suitable for zero-copy I/O operations
 * - Use buffer spans for efficient access patterns
 * - Prepare buffers for DMA and kernel bypass
 * - Structure data for cache-friendly access
 */

#include <alligator.hpp>
#include <iostream>
#include <chrono>
#include <cstring>
#include <vector>
#include <numeric>

using namespace alligator;

void efficient_access_patterns() {
    std::cout << "\n=== Efficient Buffer Access Patterns ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    constexpr size_t buffer_size = 4 * 1024 * 1024; // 4MB
    
    // Allocate a buffer
    auto* buffer = alligator.allocate_buffer(buffer_size, BFType::HEAP);
    if (!buffer) {
        std::cerr << "Failed to allocate buffer" << std::endl;
        return;
    }
    
    std::cout << "Buffer allocated at address: " << static_cast<void*>(buffer->data()) << std::endl;
    std::cout << "Size: " << buffer->buf_size_ << " bytes" << std::endl;
    
    // Example 1: Using spans for safe access
    std::cout << "\n1. Span-based access (no pointer arithmetic needed):" << std::endl;
    {
        auto span = buffer->get_span(0, 1024);
        std::cout << "   Span size: " << span.size() << " bytes" << std::endl;
        std::cout << "   Safe to iterate without bounds checking" << std::endl;
        
        // Fill with pattern
        for (size_t i = 0; i < span.size(); ++i) {
            span[i] = static_cast<uint8_t>(i % 256);
        }
    }
    
    // Example 2: Typed access for structured data
    std::cout << "\n2. Typed access for structured data:" << std::endl;
    {
        struct Packet {
            uint32_t sequence;
            uint32_t timestamp;
            uint8_t payload[1024];
        };
        
        auto* packets = buffer->get<Packet>(0);
        size_t num_packets = buffer->buf_size_ / sizeof(Packet);
        
        std::cout << "   Buffer can hold " << num_packets << " packets" << std::endl;
        std::cout << "   Each packet is " << sizeof(Packet) << " bytes" << std::endl;
        
        // Initialize first packet
        packets[0].sequence = 1;
        packets[0].timestamp = 12345;
        std::memset(packets[0].payload, 0xAB, sizeof(packets[0].payload));
    }
    
    // Example 3: Cache-line aligned access
    std::cout << "\n3. Cache-friendly access patterns:" << std::endl;
    {
        constexpr size_t cache_line_size = 64; // Common cache line size
        
        // Ensure we're working on cache-line boundaries
        size_t aligned_offset = 0;
        while (aligned_offset < buffer_size) {
            auto* cache_line = buffer->data() + aligned_offset;
            // Process one cache line at a time
            // This minimizes cache misses
            
            aligned_offset += cache_line_size;
        }
        
        std::cout << "   Processing in " << cache_line_size << "-byte chunks" << std::endl;
        std::cout << "   Minimizes cache misses and false sharing" << std::endl;
    }
}

void buffer_alignment_demo() {
    std::cout << "\n=== Buffer Alignment for Performance ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    // Different buffer types may have different alignment guarantees
    std::vector<BFType> types = {BFType::HEAP, BFType::FILE_BACKED};
    
    for (auto type : types) {
        try {
            auto* buffer = alligator.allocate_buffer(4096, type);
            if (buffer) {
                uintptr_t addr = reinterpret_cast<uintptr_t>(buffer->data());
                
                std::cout << "\nBuffer type " << static_cast<int>(type) << ":" << std::endl;
                std::cout << "  Address: 0x" << std::hex << addr << std::dec << std::endl;
                std::cout << "  16-byte aligned: " << ((addr % 16) == 0 ? "Yes" : "No") << std::endl;
                std::cout << "  64-byte aligned: " << ((addr % 64) == 0 ? "Yes" : "No") << std::endl;
                std::cout << "  4K page aligned: " << ((addr % 4096) == 0 ? "Yes" : "No") << std::endl;
                
                // Page-aligned buffers are ideal for:
                // - Direct I/O operations
                // - Memory mapping
                // - DMA transfers
            }
        } catch (const std::exception& e) {
            std::cout << "Skipping type " << static_cast<int>(type) << ": " << e.what() << std::endl;
        }
    }
}

void network_buffer_preparation() {
    std::cout << "\n=== Preparing Buffers for Network I/O ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    // Network buffers benefit from specific allocation patterns
    try {
        auto* net_buffer = dynamic_cast<NICBuffer*>(
            alligator.allocate_buffer(65536, BFType::TCP)
        );
        
        if (net_buffer) {
            std::cout << "Network buffer allocated:" << std::endl;
            std::cout << "- Size: " << net_buffer->buf_size_ << " bytes" << std::endl;
            std::cout << "- Features: " << net_buffer->nic_features_ << std::endl;
            
            // Network buffers can be used with zero-copy APIs like:
            std::cout << "\nThis buffer can be used with:" << std::endl;
            std::cout << "- sendfile() for zero-copy file transmission" << std::endl;
            std::cout << "- splice() for zero-copy pipe operations" << std::endl;
            std::cout << "- MSG_ZEROCOPY flag with send()" << std::endl;
            std::cout << "- io_uring for kernel bypass" << std::endl;
            
            // Example: Prepare packet headers at fixed offsets
            struct EthernetFrame {
                uint8_t dst_mac[6];
                uint8_t src_mac[6];
                uint16_t ethertype;
                uint8_t payload[];
            };
            
            auto* frame = net_buffer->get<EthernetFrame>(0);
            std::cout << "\nEthernet frame at buffer start:" << std::endl;
            std::cout << "- Header size: " << sizeof(EthernetFrame) << " bytes" << std::endl;
            std::cout << "- Payload starts at offset " << offsetof(EthernetFrame, payload) << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "Network buffer example skipped: " << e.what() << std::endl;
    }
}

void gpu_buffer_preparation() {
    std::cout << "\n=== Preparing Buffers for GPU Operations ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    try {
        // GPU buffers have special requirements
        auto* gpu_buffer = alligator.allocate_buffer(16 * 1024 * 1024, BFType::GPU);
        
        if (gpu_buffer) {
            std::cout << "GPU buffer allocated:" << std::endl;
            std::cout << "- Size: " << gpu_buffer->buf_size_ << " bytes" << std::endl;
            std::cout << "- CPU accessible: " << (gpu_buffer->is_local() ? "Yes" : "No") << std::endl;
            
            std::cout << "\nGPU buffers are optimized for:" << std::endl;
            std::cout << "- High bandwidth memory access" << std::endl;
            std::cout << "- Coalesced memory patterns" << std::endl;
            std::cout << "- Minimal CPU-GPU transfers" << std::endl;
            
            // If we had a CPU staging buffer
            auto* staging = alligator.allocate_buffer(1024 * 1024, BFType::HEAP);
            if (staging) {
                std::cout << "\nStaging buffer for GPU transfers:" << std::endl;
                std::cout << "- CPU writes to staging buffer" << std::endl;
                std::cout << "- DMA controller transfers to GPU" << std::endl;
                std::cout << "- No CPU cycles used during transfer" << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cout << "GPU buffer example skipped: " << e.what() << std::endl;
    }
}

void scatter_gather_pattern() {
    std::cout << "\n=== Scatter-Gather I/O Pattern ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    // Allocate multiple buffers for scatter-gather operations
    std::vector<ChainBuffer*> buffers;
    constexpr size_t num_buffers = 4;
    constexpr size_t buffer_size = 4096;
    
    std::cout << "Allocating " << num_buffers << " buffers for scatter-gather:" << std::endl;
    
    for (size_t i = 0; i < num_buffers; ++i) {
        auto* buf = alligator.allocate_buffer(buffer_size, BFType::HEAP);
        if (buf) {
            buffers.push_back(buf);
            
            // Fill with test data
            auto* data = buf->get<uint32_t>(0);
            data[0] = i * 1000; // Marker for each buffer
            
            std::cout << "- Buffer " << i << " at address " 
                      << static_cast<void*>(buf->data()) << std::endl;
        }
    }
    
    std::cout << "\nScatter-gather enables:" << std::endl;
    std::cout << "- Single system call for multiple buffers" << std::endl;
    std::cout << "- Efficient handling of fragmented data" << std::endl;
    std::cout << "- Used by readv()/writev() and similar APIs" << std::endl;
    
    // Example: Building iovec array for scatter-gather
    std::cout << "\nExample iovec array setup:" << std::endl;
    std::cout << "```cpp" << std::endl;
    std::cout << "struct iovec iov[" << buffers.size() << "];" << std::endl;
    for (size_t i = 0; i < buffers.size(); ++i) {
        std::cout << "iov[" << i << "].iov_base = buffer[" << i << "]->data();" << std::endl;
        std::cout << "iov[" << i << "].iov_len = buffer[" << i << "]->buf_size_;" << std::endl;
    }
    std::cout << "writev(fd, iov, " << buffers.size() << "); // Single syscall" << std::endl;
    std::cout << "```" << std::endl;
}

void buffer_pooling_for_performance() {
    std::cout << "\n=== Buffer Pooling for Performance ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    // Pre-allocate a pool to avoid allocation overhead
    constexpr size_t pool_size = 10;
    constexpr size_t buffer_size = 8192;
    
    std::cout << "Pre-allocating buffer pool..." << std::endl;
    
    std::vector<ChainBuffer*> pool;
    auto start = std::chrono::high_resolution_clock::now();
    
    for (size_t i = 0; i < pool_size; ++i) {
        auto* buf = alligator.allocate_buffer(buffer_size, BFType::HEAP);
        if (buf) {
            buf->pin(); // Keep in pool
            pool.push_back(buf);
        }
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    std::cout << "Pool allocation took " << duration.count() << " microseconds" << std::endl;
    std::cout << "Pool size: " << pool.size() << " buffers" << std::endl;
    
    std::cout << "\nBenefits of buffer pooling:" << std::endl;
    std::cout << "- Amortize allocation cost" << std::endl;
    std::cout << "- Reduce memory fragmentation" << std::endl;
    std::cout << "- Predictable performance" << std::endl;
    std::cout << "- Better cache locality" << std::endl;
    
    // Cleanup
    for (auto* buf : pool) {
        buf->unpin();
    }
}

void performance_measurement() {
    std::cout << "\n=== Buffer Performance Characteristics ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    constexpr size_t test_size = 1024 * 1024; // 1MB
    
    // Measure allocation time for different buffer types
    struct BufferTypeTest {
        BFType type;
        const char* name;
    };
    
    BufferTypeTest tests[] = {
        {BFType::HEAP, "Heap"},
        {BFType::FILE_BACKED, "File-backed"},
    };
    
    for (const auto& test : tests) {
        try {
            auto start = std::chrono::high_resolution_clock::now();
            auto* buffer = alligator.allocate_buffer(test_size, test.type);
            auto end = std::chrono::high_resolution_clock::now();
            
            if (buffer) {
                auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
                std::cout << test.name << " buffer allocation: " << duration.count() << " µs" << std::endl;
                
                // Measure access time
                start = std::chrono::high_resolution_clock::now();
                auto* data = buffer->data();
                volatile uint8_t sum = 0;
                for (size_t i = 0; i < 1024; ++i) {
                    sum += data[i];
                }
                end = std::chrono::high_resolution_clock::now();
                
                duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
                std::cout << "  First 1KB access: " << duration.count() << " ns" << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << test.name << " buffer: " << e.what() << std::endl;
        }
    }
}

int main() {
    std::cout << "BuffetAlligator Buffer Optimization Examples" << std::endl;
    std::cout << "============================================" << std::endl;
    
    std::cout << "\nBuffetAlligator provides memory allocation optimized for:" << std::endl;
    std::cout << "- Zero-copy I/O operations (sendfile, splice, DMA)" << std::endl;
    std::cout << "- Efficient memory access patterns" << std::endl;
    std::cout << "- Platform-specific optimizations" << std::endl;
    std::cout << "- Minimal allocation overhead" << std::endl;
    
    try {
        efficient_access_patterns();
        buffer_alignment_demo();
        network_buffer_preparation();
        gpu_buffer_preparation();
        scatter_gather_pattern();
        buffer_pooling_for_performance();
        performance_measurement();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "\nBuffer optimization examples completed!" << std::endl;
    return 0;
}