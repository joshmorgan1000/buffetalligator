/**
 * @file 05_buffer_optimization.cpp
 * @brief Buffer optimization techniques demonstration
 * 
 * This example shows optimization strategies:
 * - Memory alignment for SIMD operations
 * - Buffer pooling and reuse
 * - Zero-copy techniques
 * - DMA-friendly allocation
 * - Performance measurement
 */

#include <alligator.hpp>
#include <iostream>
#include <chrono>
#include <vector>
#include <cstring>
#ifdef __ARM_NEON
#include <arm_neon.h>  // For ARM NEON SIMD intrinsics
#endif

using namespace alligator;
using namespace std::chrono;

class PerformanceTimer {
private:
    high_resolution_clock::time_point start_;
    std::string name_;

public:
    PerformanceTimer(const std::string& name) : name_(name) {
        start_ = high_resolution_clock::now();
    }
    
    ~PerformanceTimer() {
        auto end = high_resolution_clock::now();
        auto duration = duration_cast<microseconds>(end - start_).count();
        std::cout << "   " << name_ << " took: " << duration << " µs" << std::endl;
    }
};

void alignment_optimization_example(BuffetAlligator& buffet) {
    std::cout << "1. Memory Alignment Optimization" << std::endl;
    
    // Create buffers with different alignments
    auto* unaligned_buf = buffet.allocate_buffer(1024, BFType::HEAP);
    auto* aligned_buf = buffet.allocate_buffer(1024, BFType::HEAP);
    
    // Check alignment
    uintptr_t unaligned_addr = reinterpret_cast<uintptr_t>(unaligned_buf->data());
    uintptr_t aligned_addr = reinterpret_cast<uintptr_t>(aligned_buf->data());
    
    std::cout << "   Unaligned buffer address: 0x" << std::hex << unaligned_addr 
              << " (alignment: " << std::dec << (unaligned_addr & 63) << ")" << std::endl;
    std::cout << "   Aligned buffer address: 0x" << std::hex << aligned_addr 
              << " (alignment: " << std::dec << (aligned_addr & 63) << ")" << std::endl;
    
    // Performance test with SIMD operations
    const int iterations = 100000;
    float* unaligned_data = reinterpret_cast<float*>(unaligned_buf->data());
    float* aligned_data = reinterpret_cast<float*>(aligned_buf->data());
    
    // Initialize data
    for (int i = 0; i < 256; ++i) {
        unaligned_data[i] = i * 0.1f;
        aligned_data[i] = i * 0.1f;
    }
    
    // SIMD operations on unaligned data
    {
        PerformanceTimer timer("Unaligned SIMD operations");
        float sum = 0.0f;
        for (int iter = 0; iter < iterations; ++iter) {
#ifdef __ARM_NEON
            for (int i = 0; i < 256; i += 4) {
                // Load 4 floats at once (128-bit NEON)
                float32x4_t vec = vld1q_f32(&unaligned_data[i]);
                float32x4_t result = vmulq_f32(vec, vec);  // Square each element
                
                // Sum the results
                float temp[4];
                vst1q_f32(temp, result);
                for (int j = 0; j < 4; ++j) {
                    sum += temp[j];
                }
            }
#else
            // Fallback scalar code
            for (int i = 0; i < 256; ++i) {
                sum += unaligned_data[i] * unaligned_data[i];
            }
#endif
        }
        std::cout << "   Unaligned sum: " << sum << std::endl;
    }
    
    // SIMD operations on aligned data
    {
        PerformanceTimer timer("Aligned SIMD operations");
        float sum = 0.0f;
        for (int iter = 0; iter < iterations; ++iter) {
#ifdef __ARM_NEON
            for (int i = 0; i < 256; i += 4) {
                // ARM NEON doesn't distinguish between aligned/unaligned loads on modern ARM
                float32x4_t vec = vld1q_f32(&aligned_data[i]);
                float32x4_t result = vmulq_f32(vec, vec);
                
                float temp[4];
                vst1q_f32(temp, result);
                for (int j = 0; j < 4; ++j) {
                    sum += temp[j];
                }
            }
#else
            // Fallback scalar code
            for (int i = 0; i < 256; ++i) {
                sum += aligned_data[i] * aligned_data[i];
            }
#endif
        }
        std::cout << "   Aligned sum: " << sum << std::endl;
    }
}

void buffer_pool_example(BuffetAlligator& buffet) {
    std::cout << "\n2. Buffer Pool Optimization" << std::endl;
    
    // Simulate a buffer pool by reusing buffers
    std::vector<ChainBuffer*> buffer_pool;
    const size_t pool_size = 10;
    const size_t buffer_size = 4096;
    
    // Pre-allocate buffers
    {
        PerformanceTimer timer("Pre-allocating buffer pool");
        for (size_t i = 0; i < pool_size; ++i) {
            buffer_pool.push_back(buffet.allocate_buffer(buffer_size, BFType::HEAP));
            // BuffetAlligator manages lifecycle
        }
    }
    std::cout << "   Created pool of " << pool_size << " buffers" << std::endl;
    
    // Simulate workload using pooled buffers
    {
        PerformanceTimer timer("Processing with buffer pool");
        for (int i = 0; i < 1000; ++i) {
            // Get buffer from pool
            ChainBuffer* buf = buffer_pool[i % pool_size];
            
            // Use buffer
            std::memset(buf->data(), i & 0xFF, 100);
            
            // Buffer automatically returns to pool (no deallocation)
        }
    }
    
    // Compare with continuous allocation
    {
        PerformanceTimer timer("Processing with new allocations");
        for (int i = 0; i < 1000; ++i) {
            // Allocate new buffer each time
            ChainBuffer* buf = buffet.allocate_buffer(buffer_size, BFType::HEAP);
            
            // Use buffer
            std::memset(buf->data(), i & 0xFF, 100);
            
            // Buffer will be garbage collected
        }
    }
    
    // BuffetAlligator will clean up pool
}

void zero_copy_optimization_example(BuffetAlligator& buffet) {
    std::cout << "\n3. Zero-Copy Optimization" << std::endl;
    
    // Create a file-backed buffer for zero-copy file I/O
    auto* file_buf = dynamic_cast<FileBackedBuffer*>(
        buffet.allocate_buffer(1024 * 1024, BFType::FILE_BACKED)  // 1MB
    );
    
    if (!file_buf) {
        std::cout << "   File-backed buffer not available" << std::endl;
        return;
    }
    
    std::cout << "   Created 1MB file-backed buffer (memory-mapped)" << std::endl;
    
    // Write pattern with zero-copy
    {
        PerformanceTimer timer("Zero-copy write");
        uint64_t* data = reinterpret_cast<uint64_t*>(file_buf->data());
        for (int i = 0; i < 1024 * 1024 / 8; ++i) {
            data[i] = i;  // Direct write to mapped memory
        }
        file_buf->sync();  // Ensure written to disk
    }
    
    // Read pattern with zero-copy
    {
        PerformanceTimer timer("Zero-copy read");
        uint64_t* data = reinterpret_cast<uint64_t*>(file_buf->data());
        uint64_t sum = 0;
        for (int i = 0; i < 1024 * 1024 / 8; ++i) {
            sum += data[i];  // Direct read from mapped memory
        }
        std::cout << "   Checksum: " << sum << std::endl;
    }
    
    // Network zero-copy example
    auto* nic_buf = dynamic_cast<NICBuffer*>(
        buffet.allocate_buffer(65536, BFType::TCP)  // 64KB
    );
    
    if (nic_buf) {
        std::cout << "   Created network buffer for zero-copy I/O" << std::endl;
        
        // Check NIC features
        uint32_t features = nic_buf->get_features();
        if (features & static_cast<uint32_t>(NICFeatures::ZERO_COPY)) {
            std::cout << "   NIC supports zero-copy transfers!" << std::endl;
            
            // Simulate zero-copy receive
            const char* test_data = "Zero-copy network data";
            std::memcpy(nic_buf->data(), test_data, strlen(test_data));
            
            // Create a claim for zero-copy access
            BufferClaim claim{nic_buf, 0, strlen(test_data), 0};
            
            // Process directly without copying
            std::string_view zero_copy_view(
                reinterpret_cast<const char*>(claim.buffer->data() + claim.offset), 
                claim.size
            );
            std::cout << "   Processed via zero-copy: " << zero_copy_view << std::endl;
        }
    }
}

void dma_friendly_allocation_example(BuffetAlligator& buffet) {
    std::cout << "\n4. DMA-Friendly Allocation" << std::endl;
    
    // Allocate buffers suitable for DMA operations
    auto* dma_buf = buffet.allocate_buffer(8192, BFType::HEAP);  // 8KB
    
    // Check if buffer is page-aligned (typical DMA requirement)
    uintptr_t addr = reinterpret_cast<uintptr_t>(dma_buf->data());
    size_t page_size = 4096;  // Common page size
    bool is_page_aligned = (addr % page_size) == 0;
    
    std::cout << "   Buffer address: 0x" << std::hex << addr << std::dec << std::endl;
    std::cout << "   Page-aligned: " << (is_page_aligned ? "YES" : "NO") << std::endl;
    std::cout << "   Page offset: " << (addr % page_size) << " bytes" << std::endl;
    
    // Thunderbolt DMA buffer example
    try {
        auto* tb_buf = dynamic_cast<ThunderboltDMABuffer*>(
            buffet.allocate_buffer(16384, BFType::THUNDERBOLT_DMA)  // 16KB
        );
        
        if (tb_buf) {
            std::cout << "   Allocated Thunderbolt DMA buffer" << std::endl;
            std::cout << "   Optimized for PCIe DMA transfers via cswift" << std::endl;
            
            // Simulate DMA transfer
            uint32_t* dma_data = reinterpret_cast<uint32_t*>(tb_buf->data());
            for (int i = 0; i < 1024; ++i) {
                dma_data[i] = i * 0x1234;  // Write pattern
            }
            
            std::cout << "   DMA buffer prepared for high-speed transfer" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "   Thunderbolt DMA not available: " << e.what() << std::endl;
    }
}

void cache_optimization_example(BuffetAlligator& buffet) {
    std::cout << "\n5. Cache Optimization" << std::endl;
    
    // Demonstrate cache-friendly vs cache-unfriendly access patterns
    const size_t array_size = 1024 * 1024;  // 1M elements
    auto* buffer = buffet.allocate_buffer(array_size * sizeof(int), BFType::HEAP);
    int* data = reinterpret_cast<int*>(buffer->data());
    
    // Initialize
    for (size_t i = 0; i < array_size; ++i) {
        data[i] = i;
    }
    
    // Sequential access (cache-friendly)
    {
        PerformanceTimer timer("Sequential access");
        int sum = 0;
        for (size_t i = 0; i < array_size; ++i) {
            sum += data[i];
        }
        std::cout << "   Sequential sum: " << sum << std::endl;
    }
    
    // Strided access (less cache-friendly)
    {
        PerformanceTimer timer("Strided access (stride=16)");
        int sum = 0;
        for (size_t i = 0; i < array_size; i += 16) {
            sum += data[i];
        }
        std::cout << "   Strided sum: " << sum << std::endl;
    }
    
    // Random access (cache-unfriendly)
    {
        // Create random indices
        std::vector<size_t> indices(array_size / 16);
        for (size_t i = 0; i < indices.size(); ++i) {
            indices[i] = (i * 7919) % array_size;  // Pseudo-random
        }
        
        PerformanceTimer timer("Random access");
        int sum = 0;
        for (size_t idx : indices) {
            sum += data[idx];
        }
        std::cout << "   Random sum: " << sum << std::endl;
    }
}

void gpu_optimization_example(BuffetAlligator& buffet) {
    std::cout << "\n6. GPU Memory Optimization" << std::endl;
    
    try {
        // Allocate GPU buffer
        auto* gpu_buf = dynamic_cast<GPUBuffer*>(
            buffet.allocate_buffer(4 * 1024 * 1024, BFType::GPU)  // 4MB
        );
        
        if (!gpu_buf) {
            std::cout << "   GPU buffer not available" << std::endl;
            return;
        }
        
        std::cout << "   Allocated 4MB GPU buffer" << std::endl;
        
        // Pinned memory transfer (faster than pageable)
        std::vector<float> pinned_data(1024 * 1024);  // 4MB of floats
        for (size_t i = 0; i < pinned_data.size(); ++i) {
            pinned_data[i] = i * 0.001f;
        }
        
        {
            PerformanceTimer timer("Upload to GPU");
            gpu_buf->upload(pinned_data.data(), pinned_data.size() * sizeof(float));
        }
        
        // Coalesced memory access pattern
        std::cout << "   GPU memory access patterns:" << std::endl;
        std::cout << "     Coalesced: Adjacent threads access adjacent memory" << std::endl;
        std::cout << "     Strided: Threads access memory with stride" << std::endl;
        std::cout << "     Random: Threads access random locations" << std::endl;
        
        // Download back
        std::vector<float> result(1024 * 1024);
        {
            PerformanceTimer timer("Download from GPU");
            gpu_buf->download(result.data(), result.size() * sizeof(float));
        }
        
        // Verify
        bool correct = true;
        for (size_t i = 0; i < 100; ++i) {
            if (result[i] != pinned_data[i]) {
                correct = false;
                break;
            }
        }
        std::cout << "   Transfer verification: " << (correct ? "PASSED" : "FAILED") << std::endl;
        
    } catch (const std::exception& e) {
        std::cout << "   GPU optimization not available: " << e.what() << std::endl;
    }
}

int main() {
    std::cout << "=== BuffetAlligator Optimization Example ===" << std::endl;
    std::cout << "Performance tuning at the buffet! ⚡" << std::endl << std::endl;

    auto& buffet = get_buffet_alligator();

    alignment_optimization_example(buffet);
    buffer_pool_example(buffet);
    zero_copy_optimization_example(buffet);
    dma_friendly_allocation_example(buffet);
    cache_optimization_example(buffet);
    gpu_optimization_example(buffet);

    std::cout << "\n=== Example Complete ===" << std::endl;
    std::cout << "Optimized buffers: maximum performance, minimum overhead!" << std::endl;
    
    return 0;
}