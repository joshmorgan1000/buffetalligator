/**
 * @file 04_chain_buffers.cpp
 * @brief Example demonstrating chain buffers and buffer sequencing
 * 
 * This example shows how to:
 * - Use chain buffers for automatic buffer management
 * - Create buffer chains for pipelined processing
 * - Handle buffer lifecycle with reference counting
 * - Implement producer-consumer patterns
 */

#include <alligator.hpp>
#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <cstring>

using namespace alligator;

void chain_buffer_basics() {
    std::cout << "\n=== Chain Buffer Basics ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    // Allocate a chain buffer
    constexpr size_t buffer_size = 1024 * 1024; // 1MB
    auto* chain_buffer = alligator.allocate_buffer(buffer_size, BFType::HEAP);
    
    if (!chain_buffer) {
        std::cerr << "Failed to allocate chain buffer" << std::endl;
        return;
    }
    
    std::cout << "Chain buffer allocated:" << std::endl;
    std::cout << "- Buffer ID: " << chain_buffer->id_.load() << std::endl;
    std::cout << "- Size: " << chain_buffer->buf_size_ << " bytes" << std::endl;
    std::cout << "- Constructor count: " << chain_buffer->get_constructor_count() << std::endl;
    std::cout << "- Destructor count: " << chain_buffer->get_destructor_count() << std::endl;
    
    // Chain buffers track their lifecycle
    chain_buffer->increment_constructor_count();
    std::cout << "\nAfter incrementing constructor count: " << chain_buffer->get_constructor_count() << std::endl;
    
    // Pin buffer to prevent garbage collection
    chain_buffer->pin();
    std::cout << "Buffer pinned: " << chain_buffer->is_pinned() << std::endl;
    
    // Write some data
    const char* message = "Chain buffers enable efficient memory management!";
    std::memcpy(chain_buffer->data(), message, strlen(message) + 1);
    
    // Create a new buffer in the chain
    auto* next_buffer = chain_buffer->create_new(buffer_size);
    if (next_buffer) {
        std::cout << "\nCreated next buffer in chain:" << std::endl;
        std::cout << "- Next buffer ID: " << next_buffer->id_.load() << std::endl;
        std::cout << "- Same type as original" << std::endl;
    }
    
    // Unpin when done
    chain_buffer->unpin();
    std::cout << "Buffer unpinned, eligible for garbage collection" << std::endl;
}

void producer_consumer_example() {
    std::cout << "\n=== Producer-Consumer with Chain Buffers ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    constexpr size_t buffer_size = 64 * 1024; // 64KB
    constexpr size_t chunk_size = 1024; // 1KB chunks
    
    // Shared state
    std::atomic<bool> done{false};
    std::vector<uint32_t> buffer_ids;
    std::mutex ids_mutex;
    
    // Producer thread
    std::thread producer([&]() {
        std::cout << "[Producer] Starting..." << std::endl;
        
        for (int i = 0; i < 10; ++i) {
            // Allocate buffer for data chunk
            auto* buffer = alligator.allocate_buffer(buffer_size, BFType::HEAP);
            if (!buffer) continue;
            
            // Pin to prevent premature collection
            buffer->pin();
            
            // Fill with data
            auto* data = buffer->get<uint32_t>(0);
            for (size_t j = 0; j < chunk_size / sizeof(uint32_t); ++j) {
                data[j] = i * 1000 + j;
            }
            
            // Mark how much data was written
            buffer->next_offset_.store(chunk_size, std::memory_order_release);
            
            // Add to shared list
            {
                std::lock_guard<std::mutex> lock(ids_mutex);
                buffer_ids.push_back(buffer->id_.load());
            }
            
            std::cout << "[Producer] Created buffer " << buffer->id_.load() 
                      << " with chunk " << i << std::endl;
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        done.store(true);
        std::cout << "[Producer] Done!" << std::endl;
    });
    
    // Consumer thread
    std::thread consumer([&]() {
        std::cout << "[Consumer] Starting..." << std::endl;
        int consumed = 0;
        
        while (!done.load() || !buffer_ids.empty()) {
            uint32_t buffer_id = 0;
            
            // Get next buffer ID
            {
                std::lock_guard<std::mutex> lock(ids_mutex);
                if (!buffer_ids.empty()) {
                    buffer_id = buffer_ids.front();
                    buffer_ids.erase(buffer_ids.begin());
                }
            }
            
            if (buffer_id == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            
            // Get buffer from registry
            auto* buffer = alligator.get_buffer(buffer_id);
            if (!buffer) continue;
            
            // Process data
            size_t data_size = buffer->next_offset_.load(std::memory_order_acquire);
            auto* data = buffer->get<uint32_t>(0);
            
            // Verify first few values
            std::cout << "[Consumer] Processing buffer " << buffer_id 
                      << ", first value: " << data[0] << std::endl;
            
            // Mark as consumed
            buffer->consumed_.store(true, std::memory_order_release);
            buffer->unpin();
            buffer->increment_destructor_count();
            
            consumed++;
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        
        std::cout << "[Consumer] Done! Consumed " << consumed << " buffers" << std::endl;
    });
    
    // Wait for completion
    producer.join();
    consumer.join();
    
    std::cout << "Producer-consumer example completed!" << std::endl;
}

void buffer_pool_example() {
    std::cout << "\n=== Buffer Pool with Chain Buffers ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    constexpr size_t buffer_size = 4096; // 4KB buffers
    constexpr size_t pool_size = 5;
    
    // Pre-allocate a pool of buffers
    std::vector<ChainBuffer*> buffer_pool;
    
    std::cout << "Creating buffer pool with " << pool_size << " buffers..." << std::endl;
    
    for (size_t i = 0; i < pool_size; ++i) {
        auto* buffer = alligator.allocate_buffer(buffer_size, BFType::HEAP);
        if (buffer) {
            buffer->pin(); // Keep in pool
            buffer_pool.push_back(buffer);
            std::cout << "- Added buffer " << buffer->id_.load() << " to pool" << std::endl;
        }
    }
    
    // Simulate using buffers from pool
    std::cout << "\nUsing buffers from pool:" << std::endl;
    
    for (int task = 0; task < 10; ++task) {
        // Find available buffer
        ChainBuffer* available = nullptr;
        
        for (auto* buffer : buffer_pool) {
            if (!buffer->consumed_.load(std::memory_order_acquire)) {
                available = buffer;
                break;
            }
        }
        
        if (available) {
            std::cout << "Task " << task << " using buffer " << available->id_.load() << std::endl;
            
            // Use buffer
            auto* data = available->data();
            sprintf(reinterpret_cast<char*>(data), "Task %d data", task);
            
            // Simulate work
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            
            // Mark as consumed (in real use, would reset for reuse)
            available->consumed_.store(true, std::memory_order_release);
            
            // Reset for reuse
            available->consumed_.store(false, std::memory_order_release);
            available->next_offset_.store(0, std::memory_order_release);
        } else {
            std::cout << "Task " << task << " waiting - no buffers available" << std::endl;
        }
    }
    
    // Cleanup pool
    std::cout << "\nCleaning up buffer pool..." << std::endl;
    for (auto* buffer : buffer_pool) {
        buffer->unpin();
        buffer->increment_destructor_count();
    }
}

void buffer_chaining_example() {
    std::cout << "\n=== Buffer Chaining for Pipeline Processing ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    constexpr size_t buffer_size = 8192; // 8KB
    
    // Create a processing pipeline with chained buffers
    struct PipelineStage {
        ChainBuffer* input;
        ChainBuffer* output;
        std::string name;
    };
    
    std::vector<PipelineStage> pipeline;
    
    // Stage 1: Data ingestion
    auto* stage1_in = alligator.allocate_buffer(buffer_size, BFType::HEAP);
    auto* stage1_out = alligator.allocate_buffer(buffer_size, BFType::HEAP);
    pipeline.push_back({stage1_in, stage1_out, "Ingestion"});
    
    // Stage 2: Processing
    auto* stage2_out = alligator.allocate_buffer(buffer_size, BFType::HEAP);
    pipeline.push_back({stage1_out, stage2_out, "Processing"});
    
    // Stage 3: Output
    auto* stage3_out = alligator.allocate_buffer(buffer_size, BFType::HEAP);
    pipeline.push_back({stage2_out, stage3_out, "Output"});
    
    std::cout << "Created 3-stage pipeline:" << std::endl;
    for (size_t i = 0; i < pipeline.size(); ++i) {
        std::cout << "Stage " << i + 1 << " (" << pipeline[i].name << "):" << std::endl;
        if (pipeline[i].input) {
            std::cout << "  Input buffer: " << pipeline[i].input->id_.load() << std::endl;
        }
        std::cout << "  Output buffer: " << pipeline[i].output->id_.load() << std::endl;
    }
    
    // Process data through pipeline
    std::cout << "\nProcessing data through pipeline:" << std::endl;
    
    // Stage 1: Ingest raw data
    const char* raw_data = "Raw sensor data: 42.5, 98.6, 37.0";
    std::memcpy(stage1_in->data(), raw_data, strlen(raw_data) + 1);
    std::cout << "Stage 1 input: " << raw_data << std::endl;
    
    // Simulate processing
    for (auto& stage : pipeline) {
        if (stage.input) {
            // Process input to output
            auto* input_data = reinterpret_cast<char*>(stage.input->data());
            auto* output_data = reinterpret_cast<char*>(stage.output->data());
            
            // Simple transformation
            sprintf(output_data, "[%s processed: %s]", stage.name.c_str(), input_data);
            
            std::cout << stage.name << " output: " << output_data << std::endl;
        }
    }
    
    std::cout << "\nPipeline processing complete!" << std::endl;
    std::cout << "Final output: " << reinterpret_cast<char*>(stage3_out->data()) << std::endl;
}

void garbage_collection_demo() {
    std::cout << "\n=== Garbage Collection Demo ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    std::cout << "BuffetAlligator has automatic garbage collection:" << std::endl;
    std::cout << "- Runs in background thread" << std::endl;
    std::cout << "- Collects buffers when:" << std::endl;
    std::cout << "  * consumed_ flag is set" << std::endl;
    std::cout << "  * destructor_count >= constructor_count" << std::endl;
    std::cout << "  * buffer is not pinned" << std::endl;
    
    // Demonstrate lifecycle
    auto* buffer = alligator.allocate_buffer(1024, BFType::HEAP);
    if (buffer) {
        uint32_t id = buffer->id_.load();
        std::cout << "\nCreated buffer " << id << std::endl;
        
        buffer->increment_constructor_count();
        buffer->increment_constructor_count();
        std::cout << "Constructor count: " << buffer->get_constructor_count() << std::endl;
        
        buffer->increment_destructor_count();
        std::cout << "Destructor count: " << buffer->get_destructor_count() << std::endl;
        
        std::cout << "Buffer is " << (buffer->is_pinned() ? "pinned" : "not pinned") << std::endl;
        
        // Mark as consumed and match counts
        buffer->consumed_.store(true);
        buffer->increment_destructor_count();
        
        std::cout << "Buffer marked for collection" << std::endl;
        std::cout << "Garbage collector will clean it up automatically" << std::endl;
    }
}

int main() {
    std::cout << "BuffetAlligator Chain Buffer Examples" << std::endl;
    std::cout << "=====================================" << std::endl;
    
    try {
        chain_buffer_basics();
        producer_consumer_example();
        buffer_pool_example();
        buffer_chaining_example();
        garbage_collection_demo();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "\nChain buffer examples completed!" << std::endl;
    return 0;
}