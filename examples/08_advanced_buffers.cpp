/**
 * @file 08_advanced_buffers.cpp
 * @brief Example demonstrating shared memory and Thunderbolt DMA buffers
 * 
 * This example shows how to:
 * - Use shared memory buffers for inter-process communication
 * - Allocate Thunderbolt DMA buffers for high-speed device transfers
 * - Handle platform-specific features and limitations
 * - Implement IPC patterns with zero-copy shared memory
 */

#include <alligator.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <atomic>

using namespace alligator;

void shared_memory_ipc_example() {
    std::cout << "\n=== Shared Memory IPC Example ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    // Create a named shared memory buffer
    constexpr size_t buffer_size = 64 * 1024; // 64KB
    const std::string shm_name = "/buffetalligator_example_shm";
    
    std::cout << "Creating shared memory buffer: " << shm_name << std::endl;
    
    auto* shared_buffer = dynamic_cast<SharedBuffer*>(
        alligator.allocate_buffer(buffer_size, BFType::SHARED)
    );
    
    if (!shared_buffer) {
        std::cerr << "Failed to allocate shared memory buffer" << std::endl;
        return;
    }
    
    std::cout << "Shared buffer allocated:" << std::endl;
    std::cout << "- Buffer ID: " << shared_buffer->id_.load() << std::endl;
    std::cout << "- Size: " << shared_buffer->buf_size_ << " bytes" << std::endl;
    std::cout << "- SHM name: " << shared_buffer->get_shm_name() << std::endl;
    std::cout << "- Reference count: " << shared_buffer->get_ref_count() << std::endl;
    
    // Define shared data structure
    struct SharedState {
        std::atomic<uint32_t> sequence{0};
        std::atomic<bool> ready{false};
        char message[256];
        std::atomic<uint64_t> timestamp{0};
        float data[1024];
    };
    
    // Initialize shared state
    auto* state = shared_buffer->get<SharedState>(0);
    state->sequence.store(0);
    state->ready.store(false);
    std::strcpy(state->message, "Hello from parent process!");
    
    std::cout << "\nShared memory initialized with message: " << state->message << std::endl;
    
    // Simulate child process behavior
    std::cout << "\nSimulating child process access:" << std::endl;
    std::thread child_process([shm_name, buffer_size]() {
        // In a real scenario, this would be a separate process
        // Child would attach to existing shared memory
        
        // Simulate attachment delay
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        std::cout << "[Child] Attempting to attach to shared memory..." << std::endl;
        
        // In real implementation:
        // auto* child_buffer = SharedBuffer::attach(shm_name, buffer_size);
        
        std::cout << "[Child] Would read parent's message and respond" << std::endl;
        std::cout << "[Child] Would increment sequence counter" << std::endl;
        std::cout << "[Child] Would set ready flag" << std::endl;
    });
    
    // Parent process continues
    std::cout << "[Parent] Waiting for child to respond..." << std::endl;
    
    // In real implementation, would wait for state->ready
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Update shared state
    state->sequence.fetch_add(1);
    state->timestamp.store(
        std::chrono::system_clock::now().time_since_epoch().count()
    );
    
    std::cout << "[Parent] Updated sequence: " << state->sequence.load() << std::endl;
    
    child_process.join();
    
    std::cout << "\nShared memory IPC benefits:" << std::endl;
    std::cout << "- Zero-copy data sharing between processes" << std::endl;
    std::cout << "- Extremely low latency" << std::endl;
    std::cout << "- No serialization overhead" << std::endl;
    std::cout << "- Supports complex data structures" << std::endl;
}

void shared_memory_ring_buffer() {
    std::cout << "\n=== Shared Memory Ring Buffer ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    // Create shared memory for ring buffer
    constexpr size_t ring_size = 1024 * 1024; // 1MB
    auto* ring_buffer = alligator.allocate_buffer(ring_size, BFType::SHARED);
    
    if (!ring_buffer) {
        std::cerr << "Failed to allocate ring buffer" << std::endl;
        return;
    }
    
    // Ring buffer header
    struct RingHeader {
        std::atomic<uint64_t> write_pos{0};
        std::atomic<uint64_t> read_pos{0};
        std::atomic<uint32_t> element_size{0};
        std::atomic<uint32_t> capacity{0};
    };
    
    constexpr size_t header_size = sizeof(RingHeader);
    constexpr size_t data_size = ring_size - header_size;
    
    auto* header = ring_buffer->get<RingHeader>(0);
    uint8_t* data_start = ring_buffer->data() + header_size;
    
    // Initialize ring buffer
    header->write_pos.store(0);
    header->read_pos.store(0);
    header->element_size.store(64); // 64-byte elements
    header->capacity.store(data_size / 64);
    
    std::cout << "Ring buffer created:" << std::endl;
    std::cout << "- Total size: " << ring_size << " bytes" << std::endl;
    std::cout << "- Element size: " << header->element_size.load() << " bytes" << std::endl;
    std::cout << "- Capacity: " << header->capacity.load() << " elements" << std::endl;
    
    // Producer thread
    std::thread producer([header, data_start]() {
        for (int i = 0; i < 10; ++i) {
            uint64_t write_pos = header->write_pos.load();
            uint64_t next_pos = (write_pos + 1) % header->capacity.load();
            
            // Wait if full
            while (next_pos == header->read_pos.load()) {
                std::this_thread::yield();
            }
            
            // Write data
            uint64_t offset = write_pos * header->element_size.load();
            auto* element = reinterpret_cast<uint32_t*>(data_start + offset);
            element[0] = i;
            element[1] = i * 100;
            
            // Update write position
            header->write_pos.store(next_pos);
            
            std::cout << "[Producer] Wrote element " << i << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    
    // Consumer thread
    std::thread consumer([header, data_start]() {
        for (int i = 0; i < 10; ++i) {
            // Wait for data
            while (header->read_pos.load() == header->write_pos.load()) {
                std::this_thread::yield();
            }
            
            // Read data
            uint64_t read_pos = header->read_pos.load();
            uint64_t offset = read_pos * header->element_size.load();
            auto* element = reinterpret_cast<uint32_t*>(data_start + offset);
            
            std::cout << "[Consumer] Read element: " << element[0] 
                      << ", value: " << element[1] << std::endl;
            
            // Update read position
            header->read_pos.store((read_pos + 1) % header->capacity.load());
            
            std::this_thread::sleep_for(std::chrono::milliseconds(75));
        }
    });
    
    producer.join();
    consumer.join();
    
    std::cout << "\nShared memory ring buffer is ideal for:" << std::endl;
    std::cout << "- High-frequency trading systems" << std::endl;
    std::cout << "- Audio/video streaming between processes" << std::endl;
    std::cout << "- Logging systems" << std::endl;
    std::cout << "- Real-time data processing pipelines" << std::endl;
}

void thunderbolt_dma_example() {
    std::cout << "\n=== Thunderbolt DMA Buffer Example ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    // Allocate DMA buffer for Thunderbolt device
    constexpr size_t dma_size = 16 * 1024 * 1024; // 16MB
    
    std::cout << "Allocating Thunderbolt DMA buffer..." << std::endl;
    
    auto* dma_buffer = dynamic_cast<ThunderboltDMABuffer*>(
        alligator.allocate_buffer(dma_size, BFType::THUNDERBOLT_DMA)
    );
    
    if (!dma_buffer) {
        std::cout << "Note: Thunderbolt DMA allocation fell back to regular heap buffer" << std::endl;
        std::cout << "This is expected if Thunderbolt hardware is not available" << std::endl;
        
        // Use regular buffer for demonstration
        auto* regular_buffer = alligator.allocate_buffer(dma_size, BFType::HEAP);
        if (!regular_buffer) {
            return;
        }
        
        std::cout << "\nUsing regular buffer for demonstration:" << std::endl;
        std::cout << "- Size: " << regular_buffer->buf_size_ << " bytes" << std::endl;
        std::cout << "- In real Thunderbolt DMA, this would be pinned memory" << std::endl;
        return;
    }
    
    std::cout << "Thunderbolt DMA buffer allocated:" << std::endl;
    std::cout << "- Buffer ID: " << dma_buffer->id_.load() << std::endl;
    std::cout << "- Size: " << dma_buffer->buf_size_ << " bytes" << std::endl;
    std::cout << "- Physical address: 0x" << std::hex 
              << dma_buffer->get_physical_address() << std::dec << std::endl;
    std::cout << "- Link speed: " << dma_buffer->get_link_speed() << " Gbps" << std::endl;
    
    // Prepare data for DMA transfer
    struct DMAPacket {
        uint32_t command;
        uint32_t length;
        uint64_t device_address;
        uint8_t payload[4088]; // 4KB total packet
    };
    
    auto* packet = dma_buffer->get<DMAPacket>(0);
    packet->command = 0x1234; // Example command
    packet->length = sizeof(packet->payload);
    packet->device_address = 0x100000; // Example device address
    
    // Fill payload with test pattern
    for (size_t i = 0; i < sizeof(packet->payload); ++i) {
        packet->payload[i] = static_cast<uint8_t>(i & 0xFF);
    }
    
    std::cout << "\nDMA packet prepared:" << std::endl;
    std::cout << "- Command: 0x" << std::hex << packet->command << std::dec << std::endl;
    std::cout << "- Length: " << packet->length << " bytes" << std::endl;
    
    // Simulate DMA operations
    std::cout << "\nThunderbolt DMA operations:" << std::endl;
    
    // DMA to device
    std::cout << "1. Initiating DMA transfer to device..." << std::endl;
    bool success = dma_buffer->dma_to_device(packet->device_address, sizeof(DMAPacket));
    std::cout << "   Result: " << (success ? "Would succeed" : "Not implemented") << std::endl;
    
    // DMA from device
    std::cout << "2. Initiating DMA transfer from device..." << std::endl;
    success = dma_buffer->dma_from_device(packet->device_address, sizeof(DMAPacket));
    std::cout << "   Result: " << (success ? "Would succeed" : "Not implemented") << std::endl;
    
    // Wait for completion
    std::cout << "3. Waiting for DMA completion..." << std::endl;
    bool completed = dma_buffer->wait_for_dma(1000); // 1 second timeout
    std::cout << "   Status: " << (completed ? "Complete" : "Timeout") << std::endl;
}

void thunderbolt_use_cases() {
    std::cout << "\n=== Thunderbolt DMA Use Cases ===" << std::endl;
    
    std::cout << "\n1. External GPU (eGPU):" << std::endl;
    std::cout << "   - Direct memory access for GPU compute" << std::endl;
    std::cout << "   - Up to 40 Gbps transfer rate" << std::endl;
    std::cout << "   - Minimal CPU overhead" << std::endl;
    
    std::cout << "\n2. High-Speed Storage:" << std::endl;
    std::cout << "   - NVMe RAID arrays" << std::endl;
    std::cout << "   - Video editing workstations" << std::endl;
    std::cout << "   - Real-time 8K video capture" << std::endl;
    
    std::cout << "\n3. Network Adapters:" << std::endl;
    std::cout << "   - 10/40/100 GbE adapters" << std::endl;
    std::cout << "   - RDMA over Converged Ethernet" << std::endl;
    std::cout << "   - High-frequency trading" << std::endl;
    
    std::cout << "\n4. Scientific Instruments:" << std::endl;
    std::cout << "   - High-speed cameras" << std::endl;
    std::cout << "   - Oscilloscopes" << std::endl;
    std::cout << "   - Data acquisition systems" << std::endl;
    
    std::cout << "\n5. Audio/Video Production:" << std::endl;
    std::cout << "   - Multi-channel audio interfaces" << std::endl;
    std::cout << "   - Video capture cards" << std::endl;
    std::cout << "   - Real-time effects processing" << std::endl;
}

void platform_support_info() {
    std::cout << "\n=== Platform Support Information ===" << std::endl;
    
    std::cout << "\nShared Memory Buffers:" << std::endl;
    std::cout << "✓ Linux: POSIX shared memory (shm_open/mmap)" << std::endl;
    std::cout << "✓ macOS: POSIX shared memory" << std::endl;
    std::cout << "✓ Windows: Named shared memory (CreateFileMapping)" << std::endl;
    
    std::cout << "\nThunderbolt DMA Buffers:" << std::endl;
    std::cout << "◐ macOS: Partial support via IOKit (requires kernel extension)" << std::endl;
    std::cout << "◐ Linux: Limited support (requires custom kernel module)" << std::endl;
    std::cout << "◐ Windows: Limited support (requires WDF driver)" << std::endl;
    std::cout << "Note: Falls back to pinned heap memory when not available" << std::endl;
    
    std::cout << "\nPerformance Characteristics:" << std::endl;
    std::cout << "Shared Memory: ~100ns latency for IPC" << std::endl;
    std::cout << "Thunderbolt 3/4: 40 Gbps bidirectional" << std::endl;
    std::cout << "Thunderbolt 2: 20 Gbps bidirectional" << std::endl;
}

int main() {
    std::cout << "BuffetAlligator Advanced Buffer Examples" << std::endl;
    std::cout << "========================================" << std::endl;
    
    try {
        shared_memory_ipc_example();
        shared_memory_ring_buffer();
        thunderbolt_dma_example();
        thunderbolt_use_cases();
        platform_support_info();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "\nAdvanced buffer examples completed!" << std::endl;
    return 0;
}