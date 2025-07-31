/**
 * @file 01_basic_buffers.cpp
 * @brief Example demonstrating basic buffer types: Heap, File-backed, and Shared memory
 * 
 * This example shows how to:
 * - Allocate different types of basic buffers
 * - Read and write data to buffers
 * - Use the BuffetAlligator registry
 * - Work with buffer spans and typed access
 */

#include <alligator.hpp>
#include <iostream>
#include <cstring>
#include <vector>

using namespace alligator;

void heap_buffer_example() {
    std::cout << "\n=== Heap Buffer Example ===" << std::endl;
    
    // Get the global buffer allocator
    auto& alligator = get_buffet_alligator();
    
    // Allocate a 1MB heap buffer
    constexpr size_t buffer_size = 1024 * 1024;
    auto* buffer = alligator.allocate_buffer(buffer_size, BFType::HEAP);
    
    if (!buffer) {
        std::cerr << "Failed to allocate heap buffer" << std::endl;
        return;
    }
    
    std::cout << "Allocated heap buffer with ID: " << buffer->id_.load() << std::endl;
    std::cout << "Buffer size: " << buffer->buf_size_ << " bytes" << std::endl;
    std::cout << "Is local (CPU accessible): " << buffer->is_local() << std::endl;
    
    // Write some data
    const char* message = "Hello from Heap Buffer!";
    std::memcpy(buffer->data(), message, strlen(message) + 1);
    
    // Read data back
    std::cout << "Buffer contents: " << reinterpret_cast<const char*>(buffer->data()) << std::endl;
    
    // Use span interface
    auto span = buffer->get_span(0, 100);
    std::cout << "Span size: " << span.size() << " bytes" << std::endl;
    
    // Write structured data
    struct DataPoint {
        float x, y, z;
        uint32_t timestamp;
    };
    
    // Get typed pointer
    auto* data_array = buffer->get<DataPoint>(0);
    for (int i = 0; i < 10; ++i) {
        data_array[i] = {
            static_cast<float>(i * 1.0f),
            static_cast<float>(i * 2.0f),
            static_cast<float>(i * 3.0f),
            static_cast<uint32_t>(i * 1000)
        };
    }
    
    // Read back
    std::cout << "First data point: x=" << data_array[0].x 
              << ", y=" << data_array[0].y 
              << ", z=" << data_array[0].z 
              << ", timestamp=" << data_array[0].timestamp << std::endl;
    
    // Clear buffer
    buffer->clear(0xFF);
    std::cout << "Buffer cleared with 0xFF" << std::endl;
}

void file_backed_buffer_example() {
    std::cout << "\n=== File-Backed Buffer Example ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    // Allocate a 10MB file-backed buffer
    constexpr size_t buffer_size = 10 * 1024 * 1024;
    auto* buffer = alligator.allocate_buffer(buffer_size, BFType::FILE_BACKED);
    
    if (!buffer) {
        std::cerr << "Failed to allocate file-backed buffer" << std::endl;
        return;
    }
    
    std::cout << "Allocated file-backed buffer with ID: " << buffer->id_.load() << std::endl;
    std::cout << "Buffer size: " << buffer->buf_size_ << " bytes" << std::endl;
    std::cout << "Is file-backed: " << buffer->is_file_backed() << std::endl;
    
    // File-backed buffers are useful for large datasets that might not fit in RAM
    // They provide virtual memory mapping to files
    
    // Write a large dataset
    auto* float_data = buffer->get<float>(0);
    size_t num_floats = buffer_size / sizeof(float);
    
    std::cout << "Writing " << num_floats << " float values..." << std::endl;
    for (size_t i = 0; i < num_floats; ++i) {
        float_data[i] = static_cast<float>(i) * 0.1f;
    }
    
    // Read back some samples
    std::cout << "Sample values: ";
    for (size_t i = 0; i < 5; ++i) {
        std::cout << float_data[i * 1000] << " ";
    }
    std::cout << std::endl;
    
    // File-backed buffers persist data even after program restarts (if not cleaned up)
    std::cout << "Data persisted to disk via memory mapping" << std::endl;
}

void shared_memory_buffer_example() {
    std::cout << "\n=== Shared Memory Buffer Example ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    // Note: SHARED buffer type is not fully implemented yet
    // This will fall back to heap allocation
    constexpr size_t buffer_size = 64 * 1024; // 64KB
    auto* buffer = alligator.allocate_buffer(buffer_size, BFType::SHARED);
    
    if (!buffer) {
        std::cerr << "Failed to allocate shared memory buffer" << std::endl;
        return;
    }
    
    std::cout << "Allocated buffer with ID: " << buffer->id_.load() << std::endl;
    std::cout << "Is shared: " << buffer->is_shared() << std::endl;
    std::cout << "(Note: Shared memory implementation pending, using heap fallback)" << std::endl;
    
    // Shared memory buffers would allow zero-copy IPC between processes
    // For now, demonstrate the API
    
    struct SharedData {
        std::atomic<uint32_t> counter;
        char message[256];
    };
    
    auto* shared_data = buffer->get<SharedData>(0);
    shared_data->counter.store(42);
    std::strcpy(shared_data->message, "This would be visible across processes");
    
    std::cout << "Shared counter: " << shared_data->counter.load() << std::endl;
    std::cout << "Shared message: " << shared_data->message << std::endl;
}

void buffer_registry_example() {
    std::cout << "\n=== Buffer Registry Example ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    // Allocate multiple buffers
    std::vector<uint32_t> buffer_ids;
    
    for (int i = 0; i < 5; ++i) {
        size_t size = (i + 1) * 1024; // 1KB, 2KB, 3KB, etc.
        auto* buffer = alligator.allocate_buffer(size, BFType::HEAP);
        
        if (buffer) {
            buffer_ids.push_back(buffer->id_.load());
            std::cout << "Allocated buffer " << i << " with ID: " << buffer->id_.load()
                      << ", size: " << size << " bytes" << std::endl;
        }
    }
    
    // Access buffers by ID
    std::cout << "\nAccessing buffers by ID:" << std::endl;
    for (auto id : buffer_ids) {
        auto* buffer = alligator.get_buffer(id);
        if (buffer) {
            std::cout << "Retrieved buffer ID " << id << ", size: " << buffer->buf_size_ << std::endl;
        }
    }
    
    // Clear a specific buffer
    if (!buffer_ids.empty()) {
        uint32_t id_to_clear = buffer_ids[0];
        alligator.clear_buffer(id_to_clear);
        std::cout << "\nCleared buffer with ID: " << id_to_clear << std::endl;
    }
}

int main() {
    std::cout << "BuffetAlligator Basic Buffer Examples" << std::endl;
    std::cout << "=====================================" << std::endl;
    
    try {
        heap_buffer_example();
        file_backed_buffer_example();
        shared_memory_buffer_example();
        buffer_registry_example();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "\nAll examples completed successfully!" << std::endl;
    return 0;
}