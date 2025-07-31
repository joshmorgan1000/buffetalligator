/**
 * @file 04_chain_buffers.cpp
 * @brief Chain buffer mechanism demonstration
 * 
 * This example shows the chain buffer system:
 * - Automatic buffer chaining when full
 * - Reference counting and lifecycle management
 * - Chain traversal and data aggregation
 * - Garbage collection by BuffetAlligator
 * - Mixed-type chains (CPU->GPU->Network)
 */

#include <alligator.hpp>
#include <iostream>
#include <thread>
#include <vector>
#include <cstring>

using namespace alligator;

void basic_chaining_example(BuffetAlligator& buffet) {
    std::cout << "1. Basic Buffer Chaining" << std::endl;
    
    // Create a small buffer to demonstrate chaining
    auto* first_buf = buffet.allocate_buffer(64, BFType::HEAP);
    std::cout << "   Created first buffer (64 bytes), ID: " << (first_buf->id_.load() & BUFFER_ID_MASK) << std::endl;
    std::cout << "   Buffer registered: " << first_buf->is_registered() << std::endl;
    
    // Fill the buffer
    const char* msg1 = "This is the first part of a long message that will span multiple buffers.";
    size_t msg1_len = strlen(msg1);
    
    // Write first chunk
    size_t chunk1_size = std::min(size_t(64), msg1_len);
    std::memcpy(first_buf->data(), msg1, chunk1_size);
    first_buf->next_offset_.store(chunk1_size, std::memory_order_release);
    std::cout << "   Wrote " << chunk1_size << " bytes to first buffer" << std::endl;
    
    // Chain to second buffer for remaining data
    auto* second_buf = first_buf->get_next();
    if (!second_buf && msg1_len > chunk1_size) {
        second_buf = first_buf->create_new(64);
        first_buf->next.store(second_buf, std::memory_order_release);
        std::cout << "   Created chain to second buffer, ID: " << (second_buf->id_.load() & BUFFER_ID_MASK) << std::endl;
        
        // Write remaining data
        size_t remaining = msg1_len - chunk1_size;
        std::memcpy(second_buf->data(), msg1 + chunk1_size, remaining);
        second_buf->next_offset_.store(remaining, std::memory_order_release);
        std::cout << "   Wrote " << remaining << " bytes to second buffer" << std::endl;
    }
    
    // Traverse the chain
    std::cout << "   Chain traversal:" << std::endl;
    ChainBuffer* current = first_buf;
    int buffer_num = 1;
    size_t total_size = 0;
    while (current) {
        size_t curr_size = current->next_offset_.load(std::memory_order_acquire);
        std::cout << "     Buffer " << buffer_num << ": " << curr_size << " bytes" << std::endl;
        total_size += curr_size;
        // Only follow existing chain links, don't create new ones
        current = current->next.load(std::memory_order_acquire);
        buffer_num++;
    }
    std::cout << "   Total chain size: " << total_size << " bytes" << std::endl;
}

void reference_counting_example(BuffetAlligator& buffet) {
    std::cout << "\n2. Reference Counting and Lifecycle" << std::endl;
    
    ChainBuffer* buffer = buffet.allocate_buffer(128, BFType::HEAP);
    std::cout << "   Created buffer, ID: " << (buffer->id_.load() & BUFFER_ID_MASK) << std::endl;
    
    // Note: Reference counting would be handled by smart pointers or custom ref count
    std::cout << "   Buffer is registered: " << buffer->is_registered() << std::endl;
    
    // Create a chain
    auto* next_buffer = buffer->create_new(128);
    buffer->next.store(next_buffer, std::memory_order_release);
    std::cout << "   Created chain, next buffer ID: " << (next_buffer->id_.load() & BUFFER_ID_MASK) << std::endl;
    
    // Simulate multiple references (in practice, use smart pointers)
    std::cout << "   Simulating multiple references to buffer" << std::endl;
    
    // Reference management would be handled by smart pointers
    std::cout << "   Buffer lifecycle managed by BuffetAlligator" << std::endl;
    
    // When ref count reaches 0, BuffetAlligator's garbage collector will reclaim it
    std::cout << "   Buffers will be reclaimed when all references are released" << std::endl;
}

void mixed_type_chain_example(BuffetAlligator& buffet) {
    std::cout << "\n3. Mixed-Type Buffer Chain" << std::endl;
    
    // Start with heap buffer
    auto* cpu_buf = buffet.allocate_buffer(256, BFType::HEAP);
    std::cout << "   Stage 1: CPU buffer for initial data" << std::endl;
    
    // Generate some data
    float* cpu_data = reinterpret_cast<float*>(cpu_buf->data());
    for (int i = 0; i < 64; ++i) {
        cpu_data[i] = i * 0.1f;
    }
    cpu_buf->next_offset_.store(64 * sizeof(float), std::memory_order_release);
    
    // Chain to GPU buffer for processing
    ChainBuffer* gpu_buf = nullptr;
    try {
        gpu_buf = buffet.allocate_buffer(256, BFType::GPU);
        cpu_buf->next.store(gpu_buf, std::memory_order_release);
        std::cout << "   Stage 2: Chained to GPU buffer for processing" << std::endl;
        
        // Copy data to GPU
        if (auto* gpu = dynamic_cast<GPUBuffer*>(gpu_buf)) {
            gpu->upload(cpu_data, cpu_buf->next_offset_.load());
            gpu_buf->next_offset_.store(cpu_buf->next_offset_.load(), std::memory_order_release);
            std::cout << "   Data uploaded to GPU" << std::endl;
        }
    } catch (...) {
        std::cout << "   GPU not available, continuing with CPU" << std::endl;
        gpu_buf = cpu_buf;
    }
    
    // Chain to network buffer for transmission
    auto* net_buf = buffet.allocate_buffer(256, BFType::TCP);
    (gpu_buf ? gpu_buf : cpu_buf)->next.store(net_buf, std::memory_order_release);
    std::cout << "   Stage 3: Chained to network buffer for transmission" << std::endl;
    
    // Traverse the complete chain
    std::cout << "   Complete processing pipeline:" << std::endl;
    ChainBuffer* current = cpu_buf;
    int stage = 1;
    while (current) {
        std::cout << "     Stage " << stage << ": ";
        if (dynamic_cast<HeapBuffer*>(current)) {
            std::cout << "CPU Memory";
        } else if (dynamic_cast<GPUBuffer*>(current)) {
            std::cout << "GPU Memory";
        } else if (dynamic_cast<NICBuffer*>(current)) {
            std::cout << "Network Buffer";
        } else {
            std::cout << "Unknown";
        }
        std::cout << " (" << current->next_offset_.load() << " bytes)" << std::endl;
        current = current->next.load(std::memory_order_acquire);  // Only follow existing links
        stage++;
    }
}

void automatic_chaining_example(BuffetAlligator& buffet) {
    std::cout << "\n4. Automatic Buffer Chaining" << std::endl;
    
    // Create a buffer with specific capacity
    const size_t buffer_capacity = 100;
    auto* buffer = buffet.allocate_buffer(buffer_capacity, BFType::HEAP);
    std::cout << "   Created buffer with capacity: " << buffer->buf_size_ << " bytes" << std::endl;
    
    // Simulate streaming data that exceeds single buffer
    std::vector<std::string> messages = {
        "First message in the stream. ",
        "Second message follows. ",
        "Third message continues the flow. ",
        "Fourth message adds more data. ",
        "Fifth message completes the stream."
    };
    
    ChainBuffer* current = buffer;
    size_t current_offset = 0;
    
    for (const auto& msg : messages) {
        size_t msg_size = msg.size();
        
        // Check if we need a new buffer
        if (current_offset + msg_size > current->buf_size_) {
            std::cout << "   Buffer full at " << current_offset << " bytes, creating chain..." << std::endl;
            auto* new_buffer = current->create_new(buffer_capacity);
            current->next.store(new_buffer, std::memory_order_release);
            current->next_offset_.store(current_offset, std::memory_order_release);  // Finalize current buffer size
            current = new_buffer;
            current_offset = 0;
        }
        
        // Write message to current buffer
        std::memcpy(current->data() + current_offset, msg.c_str(), msg_size);
        current_offset += msg_size;
    }
    current->next_offset_.store(current_offset, std::memory_order_release);  // Set final buffer size
    
    // Read back all data from the chain
    std::cout << "   Reading back chained data:" << std::endl;
    current = buffer;
    std::string reconstructed;
    while (current) {
        size_t size = current->next_offset_.load(std::memory_order_acquire);
        reconstructed.append(reinterpret_cast<const char*>(current->data()), size);
        current = current->next.load(std::memory_order_acquire);  // Only follow existing links
    }
    std::cout << "   Reconstructed: " << reconstructed << std::endl;
}

void concurrent_chain_example(BuffetAlligator& buffet) {
    std::cout << "\n5. Concurrent Chain Access" << std::endl;
    
    // Create a chain that will be accessed by multiple threads
    auto* head = buffet.allocate_buffer(64, BFType::HEAP);
    // Keep buffer alive during concurrent access
    
    // Build a chain
    ChainBuffer* current = head;
    for (int i = 0; i < 3; ++i) {
        auto* next = current->create_new(64);
        current->next.store(next, std::memory_order_release);
        
        // Write identifier to each buffer
        *reinterpret_cast<int*>(current->data()) = i;
        current->next_offset_.store(sizeof(int), std::memory_order_release);
        
        current = next;
    }
    *reinterpret_cast<int*>(current->data()) = 3;
    current->next_offset_.store(sizeof(int), std::memory_order_release);
    
    std::cout << "   Created chain with 4 buffers" << std::endl;
    
    // Multiple readers
    std::vector<std::thread> readers;
    for (int reader_id = 0; reader_id < 3; ++reader_id) {
        readers.emplace_back([head, reader_id]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(reader_id * 10));
            
            // Traverse chain
            ChainBuffer* curr = head;
            int count = 0;
            while (curr) {
                if (curr->next_offset_.load() >= sizeof(int)) {
                    int value = *reinterpret_cast<int*>(curr->data());
                    std::cout << "   Reader " << reader_id << " read value: " << value << std::endl;
                }
                curr = curr->next.load(std::memory_order_acquire);  // Only follow existing links
                count++;
            }
            std::cout << "   Reader " << reader_id << " traversed " << count << " buffers" << std::endl;
        });
    }
    
    // Wait for readers
    for (auto& t : readers) {
        t.join();
    }
    
    // BuffetAlligator will handle cleanup
    std::cout << "   Concurrent access complete" << std::endl;
}

void buffer_claim_chain_example(BuffetAlligator& buffet) {
    std::cout << "\n6. Buffer Claims with Chains" << std::endl;
    
    // Create network buffer that will use claims
    auto* nic_buf = dynamic_cast<NICBuffer*>(
        buffet.allocate_buffer(256, BFType::TCP)
    );
    
    if (!nic_buf) {
        std::cout << "   Network buffer not available" << std::endl;
        return;
    }
    
    // Simulate received data
    const char* packet1 = "First network packet";
    const char* packet2 = "Second network packet";
    
    std::memcpy(nic_buf->data(), packet1, strlen(packet1));
    std::memcpy(nic_buf->data() + 100, packet2, strlen(packet2));
    
    // Create claims for zero-copy access
    BufferClaim claim1{nic_buf, 0, strlen(packet1), static_cast<int64_t>(nic_buf->buf_size_ - strlen(packet1))};
    BufferClaim claim2{nic_buf, 100, strlen(packet2), static_cast<int64_t>(nic_buf->buf_size_ - 100 - strlen(packet2))};
    
    std::cout << "   Created claims:" << std::endl;
    std::cout << "     Claim 1: offset=" << claim1.offset << ", size=" << claim1.size << std::endl;
    std::cout << "     Claim 2: offset=" << claim2.offset << ", size=" << claim2.size << std::endl;
    
    // Process claims
    if (claim1.buffer) {
        std::string data1(reinterpret_cast<const char*>(claim1.buffer->data() + claim1.offset), claim1.size);
        std::cout << "   Processed claim 1: " << data1 << std::endl;
    }
    
    if (claim2.buffer) {
        std::string data2(reinterpret_cast<const char*>(claim2.buffer->data() + claim2.offset), claim2.size);
        std::cout << "   Processed claim 2: " << data2 << std::endl;
    }
    
    // Chain to processing buffer
    auto* process_buf = nic_buf->create_new(256);
    nic_buf->next.store(process_buf, std::memory_order_release);
    std::cout << "   Chained to processing buffer for further handling" << std::endl;
}

int main() {
    std::cout << "=== BuffetAlligator Chain Buffers Example ===" << std::endl;
    std::cout << "Link by link, we build the chain! 🔗" << std::endl << std::endl;

    auto& buffet = get_buffet_alligator();

    basic_chaining_example(buffet);
    reference_counting_example(buffet);
    mixed_type_chain_example(buffet);
    automatic_chaining_example(buffet);
    concurrent_chain_example(buffet);
    buffer_claim_chain_example(buffet);

    std::cout << "\n=== Example Complete ===" << std::endl;
    std::cout << "Chain buffers: linking your data from source to destination!" << std::endl;
    
    return 0;
}