#include <iostream>
#include <vector>
#include <chrono>
#include <cstring>
#include <numeric>
#include <iomanip>
#include <thread>
#include <cstdlib>
#include <span>
#include <memory>
#include <cswift/cswift.hpp>

constexpr size_t MESSAGE_SIZE = 1024; // 1KB messages
constexpr size_t NUM_MESSAGES = 2000000; // 2 million messages = 2GB total
constexpr size_t WARMUP_MESSAGES = 100000; // 100K warmup messages

// Prevent optimization
volatile float g_sink = 0;

// Simulate a real message structure
struct Message {
    uint32_t id;
    uint32_t type;
    uint64_t timestamp;
    float data[252]; // 252 * 4 = 1008, plus 16 bytes header = 1024 bytes total
};

static_assert(sizeof(Message) == 1024, "Message must be 1024 bytes");

// Test 1: Static global allocation (fastest possible)
void benchmark_static_global() {
    static uint8_t buffer[MESSAGE_SIZE * NUM_MESSAGES];
    static bool initialized = false;
    if (!initialized) {
        std::memset(buffer, 0, sizeof(buffer));
        initialized = true;
    }
    
    // Warmup
    for (size_t i = 0; i < WARMUP_MESSAGES; ++i) {
        Message* msg = reinterpret_cast<Message*>(buffer + (i % 1000) * MESSAGE_SIZE);
        msg->id = i;
        msg->type = i % 10;
        msg->timestamp = i * 1000;
        for (int j = 0; j < 252; ++j) {
            msg->data[j] = i * 0.1f + j;
        }
        g_sink = msg->data[0];
    }
    
    // Actual benchmark
    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < NUM_MESSAGES; ++i) {
        Message* msg = reinterpret_cast<Message*>(buffer + i * MESSAGE_SIZE);
        msg->id = i;
        msg->type = i % 10;
        msg->timestamp = i * 1000;
        for (int j = 0; j < 252; ++j) {
            msg->data[j] = i * 0.1f + j;
        }
    }
    auto end = std::chrono::high_resolution_clock::now();
    
    g_sink = buffer[1000];
    
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double avg_ns = duration / (double)NUM_MESSAGES;
    double throughput_gb = (MESSAGE_SIZE * NUM_MESSAGES) / (duration / 1.0);
    double total_mb = (MESSAGE_SIZE * NUM_MESSAGES) / (1024.0 * 1024.0);
    double total_ms = duration / 1000000.0;
    
    std::cout << "Static global:     " << std::fixed << std::setprecision(3) 
              << avg_ns << " ns/msg, " 
              << throughput_gb << " GB/s, "
              << total_mb << " MB in " << total_ms << " ms" << std::endl;
}

// Test 2: Heap allocation with malloc
void benchmark_malloc() {
    static uint8_t* buffer = nullptr;
    static bool initialized = false;
    if (!initialized) {
        buffer = (uint8_t*)std::malloc(MESSAGE_SIZE * NUM_MESSAGES);
        std::memset(buffer, 0, MESSAGE_SIZE * NUM_MESSAGES);
        initialized = true;
    }
    
    // Warmup
    for (size_t i = 0; i < WARMUP_MESSAGES; ++i) {
        Message* msg = reinterpret_cast<Message*>(buffer + (i % 1000) * MESSAGE_SIZE);
        msg->id = i;
        msg->type = i % 10;
        msg->timestamp = i * 1000;
        for (int j = 0; j < 252; ++j) {
            msg->data[j] = i * 0.1f + j;
        }
        g_sink = msg->data[0];
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < NUM_MESSAGES; ++i) {
        Message* msg = reinterpret_cast<Message*>(buffer + i * MESSAGE_SIZE);
        msg->id = i;
        msg->type = i % 10;
        msg->timestamp = i * 1000;
        for (int j = 0; j < 252; ++j) {
            msg->data[j] = i * 0.1f + j;
        }
    }
    auto end = std::chrono::high_resolution_clock::now();
    
    g_sink = buffer[1000];
    
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double avg_ns = duration / (double)NUM_MESSAGES;
    double throughput_gb = (MESSAGE_SIZE * NUM_MESSAGES) / (duration / 1.0);
    double total_mb = (MESSAGE_SIZE * NUM_MESSAGES) / (1024.0 * 1024.0);
    double total_ms = duration / 1000000.0;
    
    std::cout << "malloc:            " << std::fixed << std::setprecision(3) 
              << avg_ns << " ns/msg, " 
              << throughput_gb << " GB/s, "
              << total_mb << " MB in " << total_ms << " ms" << std::endl;
}

// Test 3: Aligned allocation
void benchmark_aligned_alloc() {
    static uint8_t* buffer = nullptr;
    static bool initialized = false;
    if (!initialized) {
        buffer = (uint8_t*)std::aligned_alloc(64, MESSAGE_SIZE * NUM_MESSAGES);
        std::memset(buffer, 0, MESSAGE_SIZE * NUM_MESSAGES);
        initialized = true;
    }
    
    // Warmup
    for (size_t i = 0; i < WARMUP_MESSAGES; ++i) {
        Message* msg = reinterpret_cast<Message*>(buffer + (i % 1000) * MESSAGE_SIZE);
        msg->id = i;
        msg->type = i % 10;
        msg->timestamp = i * 1000;
        for (int j = 0; j < 252; ++j) {
            msg->data[j] = i * 0.1f + j;
        }
        g_sink = msg->data[0];
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < NUM_MESSAGES; ++i) {
        Message* msg = reinterpret_cast<Message*>(buffer + i * MESSAGE_SIZE);
        msg->id = i;
        msg->type = i % 10;
        msg->timestamp = i * 1000;
        for (int j = 0; j < 252; ++j) {
            msg->data[j] = i * 0.1f + j;
        }
    }
    auto end = std::chrono::high_resolution_clock::now();
    
    g_sink = buffer[1000];
    
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double avg_ns = duration / (double)NUM_MESSAGES;
    double throughput_gb = (MESSAGE_SIZE * NUM_MESSAGES) / (duration / 1.0);
    double total_mb = (MESSAGE_SIZE * NUM_MESSAGES) / (1024.0 * 1024.0);
    double total_ms = duration / 1000000.0;
    
    std::cout << "aligned_alloc:     " << std::fixed << std::setprecision(3) 
              << avg_ns << " ns/msg, " 
              << throughput_gb << " GB/s, "
              << total_mb << " MB in " << total_ms << " ms" << std::endl;
}

// Test 4: new[] allocation
void benchmark_new_array() {
    static uint8_t* buffer = nullptr;
    static bool initialized = false;
    if (!initialized) {
        buffer = new uint8_t[MESSAGE_SIZE * NUM_MESSAGES];
        std::memset(buffer, 0, MESSAGE_SIZE * NUM_MESSAGES);
        initialized = true;
    }
    
    // Warmup
    for (size_t i = 0; i < WARMUP_MESSAGES; ++i) {
        Message* msg = reinterpret_cast<Message*>(buffer + (i % 1000) * MESSAGE_SIZE);
        msg->id = i;
        msg->type = i % 10;
        msg->timestamp = i * 1000;
        for (int j = 0; j < 252; ++j) {
            msg->data[j] = i * 0.1f + j;
        }
        g_sink = msg->data[0];
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < NUM_MESSAGES; ++i) {
        Message* msg = reinterpret_cast<Message*>(buffer + i * MESSAGE_SIZE);
        msg->id = i;
        msg->type = i % 10;
        msg->timestamp = i * 1000;
        for (int j = 0; j < 252; ++j) {
            msg->data[j] = i * 0.1f + j;
        }
    }
    auto end = std::chrono::high_resolution_clock::now();
    
    g_sink = buffer[1000];
    
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double avg_ns = duration / (double)NUM_MESSAGES;
    double throughput_gb = (MESSAGE_SIZE * NUM_MESSAGES) / (duration / 1.0);
    double total_mb = (MESSAGE_SIZE * NUM_MESSAGES) / (1024.0 * 1024.0);
    double total_ms = duration / 1000000.0;
    
    std::cout << "new[]:             " << std::fixed << std::setprecision(3) 
              << avg_ns << " ns/msg, " 
              << throughput_gb << " GB/s, "
              << total_mb << " MB in " << total_ms << " ms" << std::endl;
}

// Test 5: std::vector
void benchmark_vector() {
    static std::vector<uint8_t> buffer;
    static bool initialized = false;
    if (!initialized) {
        buffer.resize(MESSAGE_SIZE * NUM_MESSAGES);
        std::memset(buffer.data(), 0, buffer.size());
        initialized = true;
    }
    
    // Warmup
    for (size_t i = 0; i < WARMUP_MESSAGES; ++i) {
        Message* msg = reinterpret_cast<Message*>(buffer.data() + (i % 1000) * MESSAGE_SIZE);
        msg->id = i;
        msg->type = i % 10;
        msg->timestamp = i * 1000;
        for (int j = 0; j < 252; ++j) {
            msg->data[j] = i * 0.1f + j;
        }
        g_sink = msg->data[0];
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < NUM_MESSAGES; ++i) {
        Message* msg = reinterpret_cast<Message*>(buffer.data() + i * MESSAGE_SIZE);
        msg->id = i;
        msg->type = i % 10;
        msg->timestamp = i * 1000;
        for (int j = 0; j < 252; ++j) {
            msg->data[j] = i * 0.1f + j;
        }
    }
    auto end = std::chrono::high_resolution_clock::now();
    
    g_sink = buffer[1000];
    
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double avg_ns = duration / (double)NUM_MESSAGES;
    double throughput_gb = (MESSAGE_SIZE * NUM_MESSAGES) / (duration / 1.0);
    double total_mb = (MESSAGE_SIZE * NUM_MESSAGES) / (1024.0 * 1024.0);
    double total_ms = duration / 1000000.0;
    
    std::cout << "std::vector:       " << std::fixed << std::setprecision(3) 
              << avg_ns << " ns/msg, " 
              << throughput_gb << " GB/s, "
              << total_mb << " MB in " << total_ms << " ms" << std::endl;
}

// Test 6: std::unique_ptr
void benchmark_unique_ptr() {
    static std::unique_ptr<uint8_t[]> buffer;
    static bool initialized = false;
    if (!initialized) {
        buffer = std::make_unique<uint8_t[]>(MESSAGE_SIZE * NUM_MESSAGES);
        std::memset(buffer.get(), 0, MESSAGE_SIZE * NUM_MESSAGES);
        initialized = true;
    }
    
    // Warmup
    for (size_t i = 0; i < WARMUP_MESSAGES; ++i) {
        Message* msg = reinterpret_cast<Message*>(buffer.get() + (i % 1000) * MESSAGE_SIZE);
        msg->id = i;
        msg->type = i % 10;
        msg->timestamp = i * 1000;
        for (int j = 0; j < 252; ++j) {
            msg->data[j] = i * 0.1f + j;
        }
        g_sink = msg->data[0];
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < NUM_MESSAGES; ++i) {
        Message* msg = reinterpret_cast<Message*>(buffer.get() + i * MESSAGE_SIZE);
        msg->id = i;
        msg->type = i % 10;
        msg->timestamp = i * 1000;
        for (int j = 0; j < 252; ++j) {
            msg->data[j] = i * 0.1f + j;
        }
    }
    auto end = std::chrono::high_resolution_clock::now();
    
    g_sink = buffer[1000];
    
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double avg_ns = duration / (double)NUM_MESSAGES;
    double throughput_gb = (MESSAGE_SIZE * NUM_MESSAGES) / (duration / 1.0);
    double total_mb = (MESSAGE_SIZE * NUM_MESSAGES) / (1024.0 * 1024.0);
    double total_ms = duration / 1000000.0;
    
    std::cout << "std::unique_ptr:   " << std::fixed << std::setprecision(3) 
              << avg_ns << " ns/msg, " 
              << throughput_gb << " GB/s, "
              << total_mb << " MB in " << total_ms << " ms" << std::endl;
}

// Test 7: Swift ByteBuffer
void benchmark_swift_bytebuffer() {
    static cswift::CSNIOByteBuffer buffer(MESSAGE_SIZE * NUM_MESSAGES);
    
    // Warmup
    buffer.clear();
    buffer.writeWithUnsafeMutableBytes([](void* ptr, size_t available) {
        uint8_t* data = static_cast<uint8_t*>(ptr);
        size_t messages_to_write = std::min(WARMUP_MESSAGES, available / MESSAGE_SIZE);
        
        for (size_t i = 0; i < messages_to_write; ++i) {
            Message* msg = reinterpret_cast<Message*>(data + i * MESSAGE_SIZE);
            msg->id = i;
            msg->type = i % 10;
            msg->timestamp = i * 1000;
            for (int j = 0; j < 252; ++j) {
                msg->data[j] = i * 0.1f + j;
            }
        }
        g_sink = data[0];
        return messages_to_write * MESSAGE_SIZE;
    });
    
    // Actual benchmark
    buffer.clear();
    auto start = std::chrono::high_resolution_clock::now();
    
    size_t written = buffer.writeWithUnsafeMutableBytes([](void* ptr, size_t available) {
        uint8_t* data = static_cast<uint8_t*>(ptr);
        size_t messages_to_write = std::min(NUM_MESSAGES, available / MESSAGE_SIZE);
        
        for (size_t i = 0; i < messages_to_write; ++i) {
            Message* msg = reinterpret_cast<Message*>(data + i * MESSAGE_SIZE);
            msg->id = i;
            msg->type = i % 10;
            msg->timestamp = i * 1000;
            for (int j = 0; j < 252; ++j) {
                msg->data[j] = i * 0.1f + j;
            }
        }
        return messages_to_write * MESSAGE_SIZE;
    });
    
    auto end = std::chrono::high_resolution_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    size_t messages_written = written / MESSAGE_SIZE;
    double avg_ns = duration / (double)messages_written;
    double throughput_gb = written / (duration / 1.0);
    double total_mb = written / (1024.0 * 1024.0);
    double total_ms = duration / 1000000.0;
    
    std::cout << "Swift ByteBuffer:  " << std::fixed << std::setprecision(3) 
              << avg_ns << " ns/msg, " 
              << throughput_gb << " GB/s, "
              << total_mb << " MB in " << total_ms << " ms"
              << " (" << messages_written << " msgs)" << std::endl;
}

int main() {
    std::cout << "\nAllocation Method Comparison Benchmark" << std::endl;
    std::cout << "Message size: " << MESSAGE_SIZE << " bytes" << std::endl;
    std::cout << "Number of messages: " << NUM_MESSAGES << std::endl;
    std::cout << "Total data: " << (MESSAGE_SIZE * NUM_MESSAGES) / (1024.0 * 1024.0) << " MB\n" << std::endl;
    
    // Run each test 5 times
    for (int round = 1; round <= 5; ++round) {
        std::cout << "=== Round " << round << " ===" << std::endl;
        
        benchmark_static_global();
        benchmark_malloc();
        benchmark_aligned_alloc();
        benchmark_new_array();
        benchmark_vector();
        benchmark_unique_ptr();
        benchmark_swift_bytebuffer();
        
        std::cout << std::endl;
        
        // Small delay between rounds
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    return 0;
}