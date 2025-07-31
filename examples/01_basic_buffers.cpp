/**
 * @file 01_basic_buffers.cpp
 * @brief Basic buffer types demonstration
 * 
 * This example shows the fundamental buffer types in BuffetAlligator:
 * - HeapBuffer: Standard heap allocation
 * - SharedBuffer: Shared memory for IPC
 * - FileBackedBuffer: Memory-mapped file storage
 * 
 * It also introduces the BuffetAlligator global registry and
 * demonstrates buffer type auto-selection.
 */

#include <alligator.hpp>
#include <iostream>
#include <cstring>

using namespace alligator;

int main() {
    std::cout << "=== BuffetAlligator Basic Buffers Example ===" << std::endl;
    std::cout << "Welcome to the memory buffet! 🐊" << std::endl << std::endl;

    // Get the global BuffetAlligator instance
    auto& buffet = get_buffet_alligator();

    // Example 1: HeapBuffer - Classic heap allocation
    std::cout << "1. HeapBuffer Example" << std::endl;
    {
        // Allocate a 1KB heap buffer through BuffetAlligator
        auto* heap_buf = buffet.allocate_buffer(1024, BFType::HEAP);
        std::cout << "   Allocated HeapBuffer with capacity: " << heap_buf->buf_size_ << " bytes" << std::endl;

        // Write some data
        const char* message = "Hello from HeapBuffer!";
        std::memcpy(heap_buf->data(), message, strlen(message) + 1);
        std::cout << "   Wrote: " << reinterpret_cast<const char*>(heap_buf->data()) << std::endl;

        // Get a span view of the data
        auto span = heap_buf->get_span(0, strlen(message));
        std::cout << "   Span size: " << span.size() << " bytes" << std::endl;

        // Clear the buffer
        heap_buf->clear(0);
        std::cout << "   Buffer cleared" << std::endl;

        // Note: BuffetAlligator manages the buffer lifecycle
        // The garbage collector will clean it up when reference count drops to 0
    }

    // Example 2: SharedBuffer - Shared memory for IPC
    std::cout << "\n2. SharedBuffer Example" << std::endl;
    {
        // Create a shared buffer with a specific name
        auto* shared_buf = dynamic_cast<SharedBuffer*>(
            buffet.allocate_buffer(2048, BFType::SHARED)
        );
        
        if (shared_buf) {
            std::cout << "   Allocated SharedBuffer with capacity: " << shared_buf->buf_size_ << " bytes" << std::endl;
            
            // Write data that could be read by another process
            const char* ipc_message = "IPC message from process A";
            std::memcpy(shared_buf->data(), ipc_message, strlen(ipc_message) + 1);
            std::cout << "   Wrote IPC message: " << ipc_message << std::endl;
            
            // In a real scenario, another process could attach to this buffer
            // using the same name and read the data
            std::cout << "   Buffer ready for inter-process communication" << std::endl;
        }
    }

    // Example 3: FileBackedBuffer - Memory-mapped file
    std::cout << "\n3. FileBackedBuffer Example" << std::endl;
    {
        auto* file_buf = dynamic_cast<FileBackedBuffer*>(
            buffet.allocate_buffer(4096, BFType::FILE_BACKED)
        );
        
        if (file_buf) {
            std::cout << "   Allocated FileBackedBuffer with capacity: " << file_buf->buf_size_ << " bytes" << std::endl;
            
            // Write data that will be persisted to disk
            const char* persistent_data = "This data persists across program runs!";
            std::memcpy(file_buf->data(), persistent_data, strlen(persistent_data) + 1);
            std::cout << "   Wrote persistent data: " << persistent_data << std::endl;
            
            // The data is automatically synced to disk
            file_buf->sync();
            std::cout << "   Data synced to disk" << std::endl;
        }
    }

    // Example 4: Buffer chaining demonstration
    std::cout << "\n4. Buffer Chaining Example" << std::endl;
    {
        // Allocate a buffer and demonstrate chaining
        auto* buf1 = buffet.allocate_buffer(512, BFType::HEAP);
        std::cout << "   First buffer allocated, ID: " << (buf1->id_.load() & BUFFER_ID_MASK) << std::endl;
        
        // Write some data
        const char* data1 = "First chunk of data";
        std::memcpy(buf1->data(), data1, strlen(data1) + 1);
        
        // When buffer is full, it automatically chains to a new buffer
        // For demonstration, let's manually create a chain
        auto* buf2 = buf1->create_new(512);
        std::cout << "   Chained to new buffer, ID: " << (buf2->id_.load() & BUFFER_ID_MASK) << std::endl;
        
        const char* data2 = "Second chunk of data";
        std::memcpy(buf2->data(), data2, strlen(data2) + 1);
        
        // The BuffetAlligator tracks all buffers and their chains
        std::cout << "   Chain created successfully" << std::endl;
    }

    // Example 5: Auto-selection of buffer types
    std::cout << "\n5. Auto-Selection Example" << std::endl;
    {
        // TCP buffer auto-selects between Swift (on Apple) and ASIO
        auto* tcp_buf = buffet.allocate_buffer(1024, BFType::TCP);
        std::cout << "   TCP buffer allocated (auto-selected implementation)" << std::endl;
        std::cout << "   Is network buffer: " << (dynamic_cast<NICBuffer*>(tcp_buf) != nullptr) << std::endl;
        
        // GPU buffer auto-selects best available: Metal->CUDA->Vulkan
        try {
            auto* gpu_buf = buffet.allocate_buffer(1024, BFType::GPU);
            std::cout << "   GPU buffer allocated (auto-selected backend)" << std::endl;
            std::cout << "   Is GPU buffer: " << (dynamic_cast<GPUBuffer*>(gpu_buf) != nullptr) << std::endl;
        } catch (const std::exception& e) {
            std::cout << "   No GPU backend available: " << e.what() << std::endl;
        }
    }

    std::cout << "\n=== Example Complete ===" << std::endl;
    std::cout << "The BuffetAlligator's garbage collector will clean up all buffers automatically!" << std::endl;
    
    return 0;
}