/**
 * @file 02_gpu_buffers.cpp
 * @brief Example demonstrating GPU buffer allocation with automatic selection
 * 
 * This example shows how to:
 * - Use the generic GPU buffer type for automatic selection
 * - Explicitly request specific GPU implementations (CUDA, Metal, Vulkan)
 * - Handle GPU buffer operations and transfers
 * - Deal with platform-specific GPU features
 */

#include <alligator.hpp>
#include <iostream>
#include <vector>
#include <chrono>
#include <cstring>

using namespace alligator;

void auto_gpu_buffer_example() {
    std::cout << "\n=== Automatic GPU Buffer Selection ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    // Allocate a GPU buffer - system will automatically select the best available
    // On macOS: Metal > Vulkan
    // On Linux: CUDA > Vulkan
    // On Windows: CUDA > Vulkan
    constexpr size_t buffer_size = 16 * 1024 * 1024; // 16MB
    
    try {
        auto* gpu_buffer = alligator.allocate_buffer(buffer_size, BFType::GPU);
        
        if (gpu_buffer) {
            std::cout << "Successfully allocated GPU buffer!" << std::endl;
            std::cout << "Buffer ID: " << gpu_buffer->id_.load() << std::endl;
            std::cout << "Buffer size: " << gpu_buffer->buf_size_ << " bytes" << std::endl;
            std::cout << "Is local (CPU accessible): " << gpu_buffer->is_local() << std::endl;
            
            // Check which implementation was selected
            #ifdef __APPLE__
                std::cout << "Platform: macOS - expecting Metal buffer" << std::endl;
            #elif defined(__linux__)
                std::cout << "Platform: Linux - expecting CUDA or Vulkan buffer" << std::endl;
            #elif defined(_WIN32)
                std::cout << "Platform: Windows - expecting CUDA or Vulkan buffer" << std::endl;
            #endif
            
            // GPU buffers may not be directly CPU accessible
            if (!gpu_buffer->is_local()) {
                std::cout << "Note: GPU buffer is not directly CPU accessible" << std::endl;
                std::cout << "Use transfer operations for data movement" << std::endl;
            }
        }
    } catch (const std::runtime_error& e) {
        std::cerr << "Failed to allocate GPU buffer: " << e.what() << std::endl;
        std::cout << "No GPU support available on this system" << std::endl;
    }
}

void explicit_gpu_buffer_example() {
    std::cout << "\n=== Explicit GPU Buffer Types ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    constexpr size_t buffer_size = 4 * 1024 * 1024; // 4MB
    
    // Try to explicitly allocate each type of GPU buffer
    
    // CUDA Buffer
    std::cout << "\nTrying CUDA buffer:" << std::endl;
    try {
        auto* cuda_buffer = alligator.allocate_buffer(buffer_size, BFType::CUDA);
        if (cuda_buffer) {
            std::cout << "✓ CUDA buffer allocated successfully" << std::endl;
            std::cout << "  Buffer ID: " << cuda_buffer->id_.load() << std::endl;
        }
    } catch (const std::runtime_error& e) {
        std::cout << "✗ CUDA not available: " << e.what() << std::endl;
    }
    
    // Metal Buffer
    std::cout << "\nTrying Metal buffer:" << std::endl;
    try {
        auto* metal_buffer = alligator.allocate_buffer(buffer_size, BFType::METAL);
        if (metal_buffer) {
            std::cout << "✓ Metal buffer allocated successfully" << std::endl;
            std::cout << "  Buffer ID: " << metal_buffer->id_.load() << std::endl;
        }
    } catch (const std::runtime_error& e) {
        std::cout << "✗ Metal not available: " << e.what() << std::endl;
    }
    
    // Vulkan Buffer
    std::cout << "\nTrying Vulkan buffer:" << std::endl;
    try {
        auto* vulkan_buffer = alligator.allocate_buffer(buffer_size, BFType::VULKAN);
        if (vulkan_buffer) {
            std::cout << "✓ Vulkan buffer allocated successfully" << std::endl;
            std::cout << "  Buffer ID: " << vulkan_buffer->id_.load() << std::endl;
        }
    } catch (const std::runtime_error& e) {
        std::cout << "✗ Vulkan not available: " << e.what() << std::endl;
    }
}

void gpu_compute_example() {
    std::cout << "\n=== GPU Compute Example ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    try {
        // Allocate GPU buffer for computation
        constexpr size_t num_elements = 1024 * 1024; // 1M elements
        constexpr size_t buffer_size = num_elements * sizeof(float);
        
        auto* gpu_buffer = alligator.allocate_buffer(buffer_size, BFType::GPU);
        if (!gpu_buffer) {
            std::cout << "Failed to allocate GPU buffer" << std::endl;
            return;
        }
        
        // Allocate CPU buffer for staging
        auto* cpu_buffer = alligator.allocate_buffer(buffer_size, BFType::HEAP);
        if (!cpu_buffer) {
            std::cout << "Failed to allocate CPU buffer" << std::endl;
            return;
        }
        
        // Initialize data on CPU
        float* cpu_data = cpu_buffer->get<float>(0);
        for (size_t i = 0; i < num_elements; ++i) {
            cpu_data[i] = static_cast<float>(i) * 0.1f;
        }
        
        std::cout << "Initialized " << num_elements << " float values on CPU" << std::endl;
        
        // GPU operations would typically involve:
        // 1. Transfer data from CPU to GPU
        // 2. Launch compute kernels
        // 3. Transfer results back to CPU
        
        std::cout << "GPU compute operations would happen here:" << std::endl;
        std::cout << "- Data transfer CPU -> GPU" << std::endl;
        std::cout << "- Kernel execution (e.g., vector operations)" << std::endl;
        std::cout << "- Results transfer GPU -> CPU" << std::endl;
        
        // Simulate computation time
        std::cout << "Simulating GPU computation..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        std::cout << "GPU computation complete!" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "GPU compute example failed: " << e.what() << std::endl;
    }
}

void multi_gpu_example() {
    std::cout << "\n=== Multi-GPU Buffer Management ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    // Simulate multi-GPU scenario
    constexpr size_t buffer_size = 8 * 1024 * 1024; // 8MB per GPU
    constexpr int num_gpus = 2;
    
    std::vector<ChainBuffer*> gpu_buffers;
    
    std::cout << "Attempting to allocate buffers on " << num_gpus << " GPUs..." << std::endl;
    
    for (int gpu_id = 0; gpu_id < num_gpus; ++gpu_id) {
        try {
            auto* buffer = alligator.allocate_buffer(buffer_size, BFType::GPU);
            if (buffer) {
                gpu_buffers.push_back(buffer);
                std::cout << "GPU " << gpu_id << ": Allocated buffer ID " 
                          << buffer->id_.load() << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "GPU " << gpu_id << ": " << e.what() << std::endl;
            break;
        }
    }
    
    if (gpu_buffers.size() > 1) {
        std::cout << "\nMulti-GPU setup ready for:" << std::endl;
        std::cout << "- Model parallelism (split model across GPUs)" << std::endl;
        std::cout << "- Data parallelism (split batch across GPUs)" << std::endl;
        std::cout << "- Pipeline parallelism (different stages on different GPUs)" << std::endl;
    } else if (gpu_buffers.size() == 1) {
        std::cout << "\nSingle GPU available - all computation on one device" << std::endl;
    } else {
        std::cout << "\nNo GPU buffers allocated - CPU fallback required" << std::endl;
    }
}

void gpu_memory_info() {
    std::cout << "\n=== GPU Memory Information ===" << std::endl;
    
    std::cout << "GPU buffer characteristics:" << std::endl;
    std::cout << "- High bandwidth (hundreds of GB/s)" << std::endl;
    std::cout << "- Limited capacity compared to system RAM" << std::endl;
    std::cout << "- Not directly CPU accessible (requires transfers)" << std::endl;
    std::cout << "- Optimized for parallel access patterns" << std::endl;
    
    std::cout << "\nSupported GPU types in BuffetAlligator:" << std::endl;
    std::cout << "- CUDA: NVIDIA GPUs on Linux/Windows" << std::endl;
    std::cout << "- Metal: Apple Silicon and AMD GPUs on macOS" << std::endl;
    std::cout << "- Vulkan: Cross-platform GPU support" << std::endl;
    
    std::cout << "\nBest practices:" << std::endl;
    std::cout << "- Use BFType::GPU for automatic selection" << std::endl;
    std::cout << "- Minimize CPU<->GPU transfers" << std::endl;
    std::cout << "- Batch operations for efficiency" << std::endl;
    std::cout << "- Consider pinned memory for faster transfers" << std::endl;
}

int main() {
    std::cout << "BuffetAlligator GPU Buffer Examples" << std::endl;
    std::cout << "===================================" << std::endl;
    
    try {
        auto_gpu_buffer_example();
        explicit_gpu_buffer_example();
        gpu_compute_example();
        multi_gpu_example();
        gpu_memory_info();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "\nGPU buffer examples completed!" << std::endl;
    return 0;
}