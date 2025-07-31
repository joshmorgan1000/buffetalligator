/**
 * @file 02_gpu_buffers.cpp
 * @brief GPU buffer types demonstration
 * 
 * This example demonstrates GPU buffer usage with:
 * - Automatic GPU backend selection (Metal->CUDA->Vulkan)
 * - Explicit GPU buffer types
 * - Data upload/download operations
 * - GPU memory management
 * - Metal buffer integration with cswift
 */

#include <alligator.hpp>
#include <iostream>
#include <vector>
#include <cstring>
#include <numeric>

using namespace alligator;

void print_gpu_info() {
    std::cout << "GPU Backend Availability:" << std::endl;
#ifdef METAL_ENABLED
    std::cout << "   Metal: ENABLED (via cswift)" << std::endl;
#else
    std::cout << "   Metal: DISABLED" << std::endl;
#endif
#ifdef CUDA_ENABLED
    std::cout << "   CUDA: ENABLED" << std::endl;
    if (CUDABuffer::is_cuda_available()) {
        std::cout << "   CUDA Devices: " << CUDABuffer::get_device_count() << std::endl;
    }
#else
    std::cout << "   CUDA: DISABLED" << std::endl;
#endif
#ifdef VULKAN_ENABLED
    std::cout << "   Vulkan: ENABLED" << std::endl;
#else
    std::cout << "   Vulkan: DISABLED" << std::endl;
#endif
}

int main() {
    std::cout << "=== BuffetAlligator GPU Buffers Example ===" << std::endl;
    std::cout << "GPU acceleration served fresh! 🚀" << std::endl << std::endl;

    print_gpu_info();
    std::cout << std::endl;

    auto& buffet = get_buffet_alligator();

    // Example 1: Auto-selected GPU buffer
    std::cout << "1. Auto-Selected GPU Buffer" << std::endl;
    try {
        // This will automatically select the best available GPU backend
        auto* gpu_buf = dynamic_cast<GPUBuffer*>(
            buffet.allocate_buffer(1024 * 1024, BFType::GPU)  // 1MB
        );
        
        if (gpu_buf) {
            std::cout << "   Allocated GPU buffer with capacity: " << gpu_buf->buf_size_ << " bytes" << std::endl;
            std::cout << "   Memory type: " << (gpu_buf->get_memory_type() == GPUMemoryType::DEVICE_LOCAL ? "Device Local" : 
                                                gpu_buf->get_memory_type() == GPUMemoryType::HOST_VISIBLE ? "Host Visible" : "Unified") << std::endl;
            
            // Prepare some data
            std::vector<float> host_data(256, 0.0f);
            std::iota(host_data.begin(), host_data.end(), 0.0f);  // Fill with 0, 1, 2, ...
            
            // Upload data to GPU
            bool uploaded = gpu_buf->upload(host_data.data(), host_data.size() * sizeof(float));
            std::cout << "   Data upload: " << (uploaded ? "SUCCESS" : "FAILED") << std::endl;
            
            // Synchronize to ensure upload is complete
            gpu_buf->sync();
            
            // Download data back
            std::vector<float> downloaded_data(256);
            bool downloaded = gpu_buf->download(downloaded_data.data(), downloaded_data.size() * sizeof(float));
            std::cout << "   Data download: " << (downloaded ? "SUCCESS" : "FAILED") << std::endl;
            
            // Verify first few elements
            if (downloaded) {
                std::cout << "   First 5 elements: ";
                for (int i = 0; i < 5; ++i) {
                    std::cout << downloaded_data[i] << " ";
                }
                std::cout << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cout << "   No GPU backend available: " << e.what() << std::endl;
    }

#ifdef METAL_ENABLED
    // Example 2: Metal-specific buffer (using cswift)
    std::cout << "\n2. Metal Buffer (via cswift)" << std::endl;
    try {
        auto* metal_buf = dynamic_cast<MetalBuffer*>(
            buffet.allocate_buffer(512 * 1024, BFType::METAL)  // 512KB
        );
        
        if (metal_buf) {
            std::cout << "   Allocated Metal buffer with capacity: " << metal_buf->buf_size_ << " bytes" << std::endl;
            std::cout << "   This buffer uses cswift's Metal wrappers" << std::endl;
            std::cout << "   Ready for Neural Engine operations!" << std::endl;
            
            // Get native handle for direct Metal operations
            void* mtl_buffer = metal_buf->get_native_handle();
            std::cout << "   Native MTLBuffer handle: " << (mtl_buffer ? "Valid" : "Invalid") << std::endl;
            
            // Map the buffer for CPU access
            void* mapped = metal_buf->map();
            if (mapped) {
                // Write some data
                float* data = static_cast<float*>(mapped);
                for (int i = 0; i < 10; ++i) {
                    data[i] = i * 3.14f;
                }
                metal_buf->unmap();
                std::cout << "   Wrote data to Metal buffer" << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cout << "   Metal not available: " << e.what() << std::endl;
    }
#endif

#ifdef CUDA_ENABLED
    // Example 3: CUDA-specific buffer
    std::cout << "\n3. CUDA Buffer" << std::endl;
    try {
        auto* cuda_buf = dynamic_cast<CUDABuffer*>(
            buffet.allocate_buffer(2 * 1024 * 1024, BFType::CUDA)  // 2MB
        );
        
        if (cuda_buf && CUDABuffer::is_cuda_available()) {
            std::cout << "   Allocated CUDA buffer with capacity: " << cuda_buf->buf_size_ << " bytes" << std::endl;
            std::cout << "   CUDA unified memory (accessible from CPU and GPU)" << std::endl;
            
            // Direct data access (unified memory)
            float* unified_data = reinterpret_cast<float*>(cuda_buf->data());
            
            // Fill with data on CPU
            for (int i = 0; i < 100; ++i) {
                unified_data[i] = i * 2.0f;
            }
            std::cout << "   Wrote data directly to unified memory" << std::endl;
            
            // Prefetch to GPU for better performance
            if (cuda_buf->prefetch_async(0)) {  // Device 0
                std::cout << "   Prefetched data to GPU device 0" << std::endl;
            }
            
            // In a real application, you would launch CUDA kernels here
            // to process the data on GPU
            
            // Sync to ensure GPU operations complete
            cuda_buf->sync();
            
            // Read results (already in unified memory)
            std::cout << "   First 5 elements after GPU processing: ";
            for (int i = 0; i < 5; ++i) {
                std::cout << unified_data[i] << " ";
            }
            std::cout << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "   CUDA not available: " << e.what() << std::endl;
    }
#endif

    // Example 4: GPU buffer copy operations
    std::cout << "\n4. GPU Buffer Copy Operations" << std::endl;
    try {
        // Create two GPU buffers
        auto* src_gpu = dynamic_cast<GPUBuffer*>(
            buffet.allocate_buffer(256 * sizeof(float), BFType::GPU)
        );
        auto* dst_gpu = dynamic_cast<GPUBuffer*>(
            buffet.allocate_buffer(256 * sizeof(float), BFType::GPU)
        );
        
        if (src_gpu && dst_gpu) {
            // Fill source with data
            std::vector<float> src_data(256);
            for (int i = 0; i < 256; ++i) {
                src_data[i] = i * 0.5f;
            }
            
            src_gpu->upload(src_data.data(), src_data.size() * sizeof(float));
            std::cout << "   Uploaded data to source GPU buffer" << std::endl;
            
            // Copy from source to destination on GPU
            bool copied = dst_gpu->copy_from(src_gpu, src_data.size() * sizeof(float));
            std::cout << "   GPU-to-GPU copy: " << (copied ? "SUCCESS" : "FAILED") << std::endl;
            
            // Verify by downloading from destination
            if (copied) {
                std::vector<float> verify_data(256);
                dst_gpu->download(verify_data.data(), verify_data.size() * sizeof(float));
                
                bool match = true;
                for (int i = 0; i < 256; ++i) {
                    if (verify_data[i] != src_data[i]) {
                        match = false;
                        break;
                    }
                }
                std::cout << "   Copy verification: " << (match ? "PASSED" : "FAILED") << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cout << "   GPU operations failed: " << e.what() << std::endl;
    }

    // Example 5: GPU buffer as part of chain
    std::cout << "\n5. GPU Buffer in Chain" << std::endl;
    try {
        // Start with a heap buffer
        auto* cpu_buf = buffet.allocate_buffer(1024, BFType::HEAP);
        
        // Fill with data
        float* cpu_data = reinterpret_cast<float*>(cpu_buf->data());
        for (int i = 0; i < 256; ++i) {
            cpu_data[i] = i * 1.5f;
        }
        std::cout << "   Created CPU buffer with data" << std::endl;
        
        // Chain to a GPU buffer for processing
        auto* gpu_chain = cpu_buf->create_new(1024);
        if (auto* gpu_buf = dynamic_cast<GPUBuffer*>(gpu_chain)) {
            // Upload from CPU buffer to GPU buffer
            gpu_buf->upload(cpu_data, 256 * sizeof(float));
            std::cout << "   Chained to GPU buffer and uploaded data" << std::endl;
            
            // Process on GPU (in real app, run kernels here)
            
            // Chain back to CPU for results
            auto* result_buf = gpu_buf->create_new(1024);
            std::cout << "   Chained back to CPU buffer for results" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "   Chain operation failed: " << e.what() << std::endl;
    }

    std::cout << "\n=== Example Complete ===" << std::endl;
    std::cout << "GPU buffers accelerate your data processing!" << std::endl;
    
    return 0;
}