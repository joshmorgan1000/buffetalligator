#include <iostream>
#include <vector>
#include <chrono>
#include <cstring>
#include <numeric>
#include <iomanip>
#include <thread>
#include <cstdlib>
#include <span>
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

void benchmark_raw_buffer_spans() {
    // Pre-allocate buffer - make it static like Swift buffer for fairness
    static uint8_t* buffer = (uint8_t*)aligned_alloc(64, MESSAGE_SIZE * NUM_MESSAGES);
    static bool initialized = false;
    if (!initialized) {
        std::memset(buffer, 0, MESSAGE_SIZE * NUM_MESSAGES); // Touch memory
        initialized = true;
    }
    
    // Warmup - write to different locations
    for (size_t i = 0; i < WARMUP_MESSAGES; ++i) {
        std::span<uint8_t> msg_span(buffer + (i % 1000) * MESSAGE_SIZE, MESSAGE_SIZE);
        Message* msg = reinterpret_cast<Message*>(msg_span.data());
        msg->id = i;
        msg->type = i % 10;
        msg->timestamp = i * 1000;
        for (int j = 0; j < 252; ++j) {
            msg->data[j] = i * 0.1f + j;
        }
        g_sink = msg->data[0];
    }
    
    // Actual benchmark - get span for each message and write
    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < NUM_MESSAGES; ++i) {
        std::span<uint8_t> msg_span(buffer + i * MESSAGE_SIZE, MESSAGE_SIZE);
        Message* msg = reinterpret_cast<Message*>(msg_span.data());
        msg->id = i;
        msg->type = i % 10;
        msg->timestamp = i * 1000;
        for (int j = 0; j < 252; ++j) {
            msg->data[j] = i * 0.1f + j;
        }
    }
    auto end = std::chrono::high_resolution_clock::now();
    
    g_sink = buffer[1000]; // Prevent optimization
    
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double avg_ns = duration / (double)NUM_MESSAGES;
    double throughput_gb = (MESSAGE_SIZE * NUM_MESSAGES) / (duration / 1.0); // GB/s total
    double total_mb = (MESSAGE_SIZE * NUM_MESSAGES) / (1024.0 * 1024.0);
    double total_ms = duration / 1000000.0;
    
    std::cout << "Raw buffer spans:      " << std::fixed << std::setprecision(3) 
              << avg_ns << " ns/msg, " 
              << throughput_gb << " GB/s, "
              << total_mb << " MB in " << total_ms << " ms" << std::endl;
    
    // Don't free static buffer
}

void benchmark_vector_spans() {
    // Pre-allocate vector - make it static for fairness
    static std::vector<uint8_t> buffer(MESSAGE_SIZE * NUM_MESSAGES);
    static bool initialized = false;
    if (!initialized) {
        std::memset(buffer.data(), 0, MESSAGE_SIZE * NUM_MESSAGES); // Touch memory
        initialized = true;
    }
    
    // Warmup - write to different locations
    for (size_t i = 0; i < WARMUP_MESSAGES; ++i) {
        std::span<uint8_t> msg_span(buffer.data() + (i % 1000) * MESSAGE_SIZE, MESSAGE_SIZE);
        Message* msg = reinterpret_cast<Message*>(msg_span.data());
        msg->id = i;
        msg->type = i % 10;
        msg->timestamp = i * 1000;
        for (int j = 0; j < 252; ++j) {
            msg->data[j] = i * 0.1f + j;
        }
        g_sink = msg->data[0];
    }
    
    // Actual benchmark - get span for each message and write
    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < NUM_MESSAGES; ++i) {
        std::span<uint8_t> msg_span(buffer.data() + i * MESSAGE_SIZE, MESSAGE_SIZE);
        Message* msg = reinterpret_cast<Message*>(msg_span.data());
        msg->id = i;
        msg->type = i % 10;
        msg->timestamp = i * 1000;
        for (int j = 0; j < 252; ++j) {
            msg->data[j] = i * 0.1f + j;
        }
    }
    auto end = std::chrono::high_resolution_clock::now();
    
    g_sink = buffer[1000]; // Prevent optimization
    
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double avg_ns = duration / (double)NUM_MESSAGES;
    double throughput_gb = (MESSAGE_SIZE * NUM_MESSAGES) / (duration / 1.0); // GB/s total
    double total_mb = (MESSAGE_SIZE * NUM_MESSAGES) / (1024.0 * 1024.0);
    double total_ms = duration / 1000000.0;
    
    std::cout << "Vector spans:          " << std::fixed << std::setprecision(3) 
              << avg_ns << " ns/msg, " 
              << throughput_gb << " GB/s, "
              << total_mb << " MB in " << total_ms << " ms" << std::endl;
}

void benchmark_swift_batch_spans() {
    // Pre-allocate Swift buffer
    static cswift::CSNIOByteBuffer swiftBuffer(std::max(MESSAGE_SIZE * NUM_MESSAGES, size_t(64*1024*1024)));
    
    // Warmup
    swiftBuffer.clear();
    swiftBuffer.writeWithUnsafeMutableBytes([](void* ptr, size_t available) {
        uint8_t* buffer = static_cast<uint8_t*>(ptr);
        size_t messages_to_write = std::min(WARMUP_MESSAGES, available / MESSAGE_SIZE);
        
        for (size_t i = 0; i < messages_to_write; ++i) {
            std::span<uint8_t> msg_span(buffer + i * MESSAGE_SIZE, MESSAGE_SIZE);
            Message* msg = reinterpret_cast<Message*>(msg_span.data());
            msg->id = i;
            msg->type = i % 10;
            msg->timestamp = i * 1000;
            for (int j = 0; j < 252; ++j) {
                msg->data[j] = i * 0.1f + j;
            }
        }
        g_sink = buffer[0];
        return messages_to_write * MESSAGE_SIZE;
    });
    
    // Actual benchmark - write all messages in one batch
    swiftBuffer.clear();
    auto start = std::chrono::high_resolution_clock::now();
    
    size_t written = swiftBuffer.writeWithUnsafeMutableBytes([](void* ptr, size_t available) {
        uint8_t* buffer = static_cast<uint8_t*>(ptr);
        size_t messages_to_write = std::min(NUM_MESSAGES, available / MESSAGE_SIZE);
        
        for (size_t i = 0; i < messages_to_write; ++i) {
            std::span<uint8_t> msg_span(buffer + i * MESSAGE_SIZE, MESSAGE_SIZE);
            Message* msg = reinterpret_cast<Message*>(msg_span.data());
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
    double throughput_gb = written / (duration / 1.0); // GB/s total
    double total_mb = written / (1024.0 * 1024.0);
    double total_ms = duration / 1000000.0;
    
    std::cout << "Swift batch spans:     " << std::fixed << std::setprecision(3) 
              << avg_ns << " ns/msg, " 
              << throughput_gb << " GB/s, "
              << total_mb << " MB in " << total_ms << " ms"
              << " (" << messages_written << " msgs)" << std::endl;
}

void benchmark_swift_sequential_spans() {
    // Pre-allocate Swift buffer
    static cswift::CSNIOByteBuffer swiftBuffer(std::max(MESSAGE_SIZE * NUM_MESSAGES, size_t(64*1024*1024)));
    
    // Actual benchmark - get new span for each message
    swiftBuffer.clear();
    auto start = std::chrono::high_resolution_clock::now();
    
    size_t total_written = 0;
    for (size_t i = 0; i < NUM_MESSAGES; ++i) {
        size_t written = swiftBuffer.writeWithUnsafeMutableBytes([i, &total_written](void* ptr, size_t available) {
            if (available >= MESSAGE_SIZE) {
                // Get span at current write position
                std::span<uint8_t> msg_span(static_cast<uint8_t*>(ptr), MESSAGE_SIZE);
                Message* msg = reinterpret_cast<Message*>(msg_span.data());
                msg->id = i;
                msg->type = i % 10;
                msg->timestamp = i * 1000;
                for (int j = 0; j < 252; ++j) {
                    msg->data[j] = i * 0.1f + j;
                }
                return MESSAGE_SIZE;
            }
            return size_t(0);
        });
        total_written += written;
        if (written == 0) break; // Buffer full
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    size_t messages_written = total_written / MESSAGE_SIZE;
    double avg_ns = duration / (double)messages_written;
    double throughput_gb = total_written / (duration / 1.0); // GB/s total
    double total_mb = total_written / (1024.0 * 1024.0);
    double total_ms = duration / 1000000.0;
    
    std::cout << "Swift sequential spans: " << std::fixed << std::setprecision(3) 
              << avg_ns << " ns/msg, " 
              << throughput_gb << " GB/s, "
              << total_mb << " MB in " << total_ms << " ms"
              << " (" << messages_written << " msgs)" << std::endl;
}

int main() {
    std::cout << "\nReal Usage Pattern Benchmark (Span-based writes)" << std::endl;
    std::cout << "Message size: " << MESSAGE_SIZE << " bytes" << std::endl;
    std::cout << "Number of messages: " << NUM_MESSAGES << std::endl;
    std::cout << "Total data: " << (MESSAGE_SIZE * NUM_MESSAGES) / (1024.0 * 1024.0) << " MB\n" << std::endl;
    
    // Run each test 3 times
    for (int round = 1; round <= 3; ++round) {
        std::cout << "=== Round " << round << " ===" << std::endl;
        
        benchmark_raw_buffer_spans();
        benchmark_vector_spans();
        benchmark_swift_batch_spans();
        benchmark_swift_sequential_spans();
        
        std::cout << std::endl;
        
        // Small delay between rounds
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    return 0;
}