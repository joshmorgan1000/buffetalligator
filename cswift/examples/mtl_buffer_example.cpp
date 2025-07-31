#include <cswift/cswift.hpp>
#include <iostream>
#include <vector>
#include <numeric>
#include <iomanip>

using namespace cswift;

/**
 * @brief Example demonstrating MTLBuffer unified memory
 * 
 * @details This example shows how to:
 * - Create Metal buffers with unified memory
 * - Zero-copy data sharing between CPU and GPU
 * - Efficient data transfer patterns
 * 
 * @note This example requires macOS with Metal support
 */

int main() {
    std::cout << "MTLBuffer Unified Memory Example\n" << std::endl;
    
    try {
        // Check if Metal device is available
        if (!CSMTLDevice::isAvailable()) {
            std::cout << "Metal device not available on this platform" << std::endl;
            std::cout << "MTLBuffer requires macOS with Metal support" << std::endl;
            return 0;
        }
        
        // Example 1: Creating unified memory buffer
        std::cout << "=== Example 1: Basic Unified Memory Buffer ===" << std::endl;
        {
            CSMTLDevice device;
            
            // Create a 1MB buffer with shared storage (CPU+GPU accessible)
            size_t bufferSize = 1024 * 1024; // 1MB
            auto buffer = device.createBuffer(bufferSize);
            
            std::cout << "Created unified memory buffer:" << std::endl;
            std::cout << "  Size: " << buffer->size() << " bytes" << std::endl;
            std::cout << "  CPU accessible: Yes (zero-copy)" << std::endl;
            std::cout << "  GPU accessible: Yes" << std::endl;
            
            // Write data from CPU
            float* floatData = buffer->contents<float>();
            size_t floatCount = buffer->count<float>();
            
            for (size_t i = 0; i < floatCount; ++i) {
                floatData[i] = static_cast<float>(i) * 0.1f;
            }
            
            // Mark buffer as modified (important for managed storage)
            buffer->didModify();
            
            std::cout << "Wrote " << floatCount << " floats to buffer from CPU" << std::endl;
        }
        
        // Example 2: Different storage modes
        std::cout << "\n=== Example 2: Storage Modes Comparison ===" << std::endl;
        {
            CSMTLDevice device;
            
            // Shared storage - CPU and GPU can access directly
            auto sharedBuffer = device.createBuffer(4096, CSMTLStorageMode::Shared);
            std::cout << "Shared buffer: CPU+GPU accessible, no copying needed" << std::endl;
            
            // Initialize with pattern
            uint32_t* data = sharedBuffer->contents<uint32_t>();
            for (size_t i = 0; i < sharedBuffer->count<uint32_t>(); ++i) {
                data[i] = static_cast<uint32_t>(i * i);
            }
            
            // Private storage would be GPU-only (not demonstrable without GPU kernel)
            std::cout << "Private buffer: GPU-only, fastest for GPU operations" << std::endl;
            std::cout << "Managed buffer: Requires explicit synchronization" << std::endl;
        }
        
        // Example 3: Zero-copy data processing
        std::cout << "\n=== Example 3: Zero-Copy Data Processing ===" << std::endl;
        {
            CSMTLDevice device;
            
            // Create buffer for image data (simulated 512x512 RGBA image)
            size_t width = 512, height = 512;
            size_t pixelCount = width * height;
            size_t bufferSize = pixelCount * 4; // 4 bytes per pixel (RGBA)
            
            auto imageBuffer = device.createBuffer(bufferSize);
            
            std::cout << "Created image buffer for " << width << "x" << height << " RGBA image" << std::endl;
            std::cout << "Buffer size: " << bufferSize << " bytes" << std::endl;
            
            // Direct CPU manipulation of GPU-accessible memory
            uint8_t* pixels = imageBuffer->contents<uint8_t>();
            
            // Fill with gradient pattern
            for (size_t y = 0; y < height; ++y) {
                for (size_t x = 0; x < width; ++x) {
                    size_t idx = (y * width + x) * 4;
                    pixels[idx + 0] = static_cast<uint8_t>(x % 256);     // R
                    pixels[idx + 1] = static_cast<uint8_t>(y % 256);     // G
                    pixels[idx + 2] = static_cast<uint8_t>((x + y) % 256); // B
                    pixels[idx + 3] = 255;                                // A
                }
            }
            
            // This data is now immediately available to GPU without copying
            imageBuffer->didModify();
            
            std::cout << "Filled image buffer with gradient pattern" << std::endl;
            std::cout << "Data is immediately available to GPU (zero-copy)" << std::endl;
        }
        
        // Example 4: Buffer initialization patterns
        std::cout << "\n=== Example 4: Buffer Initialization ===" << std::endl;
        {
            CSMTLDevice device;
            
            // Method 1: Create empty buffer and fill
            auto buffer1 = device.createBuffer(256);
            double* doubles = buffer1->contents<double>();
            for (size_t i = 0; i < buffer1->count<double>(); ++i) {
                doubles[i] = std::sin(i * 0.1);
            }
            buffer1->didModify();
            
            std::cout << "Method 1: Created and filled buffer with " 
                      << buffer1->count<double>() << " doubles" << std::endl;
            
            // Method 2: Create buffer with initial data
            std::vector<float> initialData(64);
            std::iota(initialData.begin(), initialData.end(), 0.0f);
            
            auto buffer2 = device.createBuffer(initialData.data(), 
                                             initialData.size() * sizeof(float));
            
            std::cout << "Method 2: Created buffer with initial data (" 
                      << buffer2->count<float>() << " floats)" << std::endl;
        }
        
        // Example 5: Structured data in unified memory
        std::cout << "\n=== Example 5: Structured Data ===" << std::endl;
        {
            CSMTLDevice device;
            
            // Define a vertex structure
            struct Vertex {
                float position[3];
                float normal[3];
                float texCoord[2];
            };
            
            // Create buffer for vertices
            size_t vertexCount = 1000;
            auto vertexBuffer = device.createBuffer(vertexCount * sizeof(Vertex));
            
            Vertex* vertices = vertexBuffer->contents<Vertex>();
            
            // Fill with sample data
            for (size_t i = 0; i < vertexCount; ++i) {
                vertices[i].position[0] = static_cast<float>(i);
                vertices[i].position[1] = static_cast<float>(i * 2);
                vertices[i].position[2] = static_cast<float>(i * 3);
                
                vertices[i].normal[0] = 0.0f;
                vertices[i].normal[1] = 1.0f;
                vertices[i].normal[2] = 0.0f;
                
                vertices[i].texCoord[0] = static_cast<float>(i) / vertexCount;
                vertices[i].texCoord[1] = 1.0f - vertices[i].texCoord[0];
            }
            
            vertexBuffer->didModify();
            
            std::cout << "Created vertex buffer with " << vertexCount << " vertices" << std::endl;
            std::cout << "Each vertex: " << sizeof(Vertex) << " bytes" << std::endl;
            std::cout << "Total size: " << vertexBuffer->size() << " bytes" << std::endl;
        }
        
        // Example 6: Read/Write helper methods
        std::cout << "\n=== Example 6: Buffer Read/Write Helpers ===" << std::endl;
        {
            CSMTLDevice device;
            auto buffer = device.createBuffer(1024);
            
            // Write data using helper method
            std::vector<int> sourceData = {1, 2, 3, 4, 5, 6, 7, 8};
            buffer->write(sourceData.data(), sourceData.size() * sizeof(int));
            
            std::cout << "Wrote " << sourceData.size() << " integers to buffer" << std::endl;
            
            // Read back data
            std::vector<int> readData(sourceData.size());
            buffer->read(readData.data(), readData.size() * sizeof(int));
            
            std::cout << "Read back: ";
            for (int val : readData) {
                std::cout << val << " ";
            }
            std::cout << std::endl;
            
            // Partial write at offset
            std::vector<int> moreData = {100, 200, 300};
            size_t offset = 4 * sizeof(int);
            buffer->write(moreData.data(), moreData.size() * sizeof(int), offset);
            
            std::cout << "Wrote 3 more integers at offset " << offset << std::endl;
        }
        
        // Example 7: Command queue for GPU execution
        std::cout << "\n=== Example 7: Command Queue Creation ===" << std::endl;
        {
            CSMTLDevice device;
            auto commandQueue = device.createCommandQueue();
            
            std::cout << "Created command queue for GPU command execution" << std::endl;
            std::cout << "Ready to submit compute or render commands" << std::endl;
            
            // In a real application, you would:
            // 1. Create command buffer from queue
            // 2. Encode commands (compute/render)
            // 3. Commit command buffer
            // 4. Wait for completion
        }
        
        std::cout << "\n=== Summary ===" << std::endl;
        std::cout << "Unified memory advantages:" << std::endl;
        std::cout << "1. Zero-copy data sharing between CPU and GPU" << std::endl;
        std::cout << "2. Simplified memory management" << std::endl;
        std::cout << "3. Reduced memory footprint" << std::endl;
        std::cout << "4. Direct CPU manipulation of GPU data" << std::endl;
        std::cout << "5. Ideal for data streaming and dynamic updates" << std::endl;
        
    } catch (const CSException& e) {
        std::cerr << "Metal error: " << e.what() << " (code: " << e.code() << ")" << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}