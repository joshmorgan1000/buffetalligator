/**
 * @file 08_advanced_buffers.cpp
 * @brief Advanced buffer usage patterns demonstration
 * 
 * This example shows advanced techniques:
 * - Custom buffer implementations
 * - Pipeline architectures
 * - Ring buffer patterns
 * - Memory-mapped I/O
 * - High-frequency trading patterns
 * - Machine learning data pipelines
 */

#include <alligator.hpp>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <iomanip>

using namespace alligator;
using namespace std::chrono;

// Custom ring buffer implementation using ChainBuffer
class RingBuffer {
private:
    std::vector<ChainBuffer*> buffers_;
    size_t num_buffers_;
    std::atomic<size_t> write_index_{0};
    std::atomic<size_t> read_index_{0};
    BuffetAlligator& buffet_;
    
public:
    RingBuffer(BuffetAlligator& buffet, size_t num_buffers, size_t buffer_size, BFType type)
        : num_buffers_(num_buffers), buffet_(buffet) {
        
        // Pre-allocate all buffers
        for (size_t i = 0; i < num_buffers; ++i) {
            auto* buf = buffet.allocate_buffer(buffer_size, type);
            // BuffetAlligator manages lifecycle
            buffers_.push_back(buf);
        }
        
        // Link them in a ring
        for (size_t i = 0; i < num_buffers; ++i) {
            buffers_[i]->next.store(buffers_[(i + 1) % num_buffers], std::memory_order_release);
        }
    }
    
    ~RingBuffer() {
        // BuffetAlligator will handle cleanup
    }
    
    ChainBuffer* get_write_buffer() {
        size_t idx = write_index_.fetch_add(1) % num_buffers_;
        return buffers_[idx];
    }
    
    ChainBuffer* get_read_buffer() {
        size_t write_idx = write_index_.load();
        size_t read_idx = read_index_.load();
        
        if (read_idx < write_idx) {
            size_t idx = read_index_.fetch_add(1) % num_buffers_;
            return buffers_[idx];
        }
        return nullptr;
    }
};

void ring_buffer_example(BuffetAlligator& buffet) {
    std::cout << "1. Ring Buffer Pattern" << std::endl;
    
    RingBuffer ring(buffet, 8, 1024, BFType::HEAP);
    std::cout << "   Created ring buffer with 8 slots of 1KB each" << std::endl;
    
    // Producer thread
    std::thread producer([&ring]() {
        for (int i = 0; i < 20; ++i) {
            auto* buf = ring.get_write_buffer();
            
            // Write data
            std::string msg = "Message " + std::to_string(i);
            std::memcpy(buf->data(), msg.c_str(), msg.size() + 1);
            buf->next_offset_.store(msg.size() + 1, std::memory_order_release);
            
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    
    // Consumer thread
    std::thread consumer([&ring]() {
        int consumed = 0;
        while (consumed < 20) {
            auto* buf = ring.get_read_buffer();
            size_t buf_size = buf ? buf->next_offset_.load(std::memory_order_acquire) : 0;
            if (buf && buf_size > 0) {
                std::string msg(reinterpret_cast<char*>(buf->data()), buf_size - 1);
                std::cout << "   Consumed: " << msg << std::endl;
                consumed++;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    });
    
    producer.join();
    consumer.join();
    std::cout << "   Ring buffer processing complete" << std::endl;
}

void pipeline_architecture_example(BuffetAlligator& buffet) {
    std::cout << "\n2. Pipeline Architecture" << std::endl;
    
    // Create a 4-stage pipeline: Input -> Process -> GPU -> Output
    struct PipelineStage {
        std::string name;
        ChainBuffer* input = nullptr;
        ChainBuffer* output = nullptr;
        std::function<void()> process;
    };
    
    std::vector<PipelineStage> pipeline;
    
    // Stage 1: Data ingestion
    pipeline.push_back({
        "Ingestion",
        nullptr,
        buffet.allocate_buffer(4096, BFType::HEAP),
        []() { std::this_thread::sleep_for(std::chrono::microseconds(100)); }
    });
    
    // Stage 2: CPU preprocessing  
    pipeline.push_back({
        "Preprocessing",
        pipeline[0].output,
        buffet.allocate_buffer(4096, BFType::HEAP),
        []() { std::this_thread::sleep_for(std::chrono::microseconds(200)); }
    });
    
    // Stage 3: GPU processing (if available)
    try {
        pipeline.push_back({
            "GPU Processing",
            pipeline[1].output,
            buffet.allocate_buffer(4096, BFType::GPU),
            []() { std::this_thread::sleep_for(std::chrono::microseconds(50)); }
        });
    } catch (...) {
        // Fallback to CPU
        pipeline.push_back({
            "CPU Processing",
            pipeline[1].output,
            buffet.allocate_buffer(4096, BFType::HEAP),
            []() { std::this_thread::sleep_for(std::chrono::microseconds(300)); }
        });
    }
    
    // Stage 4: Network output
    pipeline.push_back({
        "Network Output",
        pipeline[2].output,
        buffet.allocate_buffer(4096, BFType::TCP),
        []() { std::this_thread::sleep_for(std::chrono::microseconds(150)); }
    });
    
    std::cout << "   Created " << pipeline.size() << "-stage pipeline:" << std::endl;
    for (const auto& stage : pipeline) {
        std::cout << "     -> " << stage.name << std::endl;
    }
    
    // Process data through pipeline
    const int num_items = 10;
    auto start_time = high_resolution_clock::now();
    
    for (int i = 0; i < num_items; ++i) {
        // Move data through each stage
        for (auto& stage : pipeline) {
            stage.process();
        }
    }
    
    auto end_time = high_resolution_clock::now();
    auto duration = duration_cast<microseconds>(end_time - start_time).count();
    
    std::cout << "   Processed " << num_items << " items in " << duration << " µs" << std::endl;
    std::cout << "   Average latency: " << (duration / num_items) << " µs per item" << std::endl;
}

void hft_pattern_example(BuffetAlligator& buffet) {
    std::cout << "\n3. High-Frequency Trading Pattern" << std::endl;
    
    // Create ultra-low latency buffers
    struct MarketData {
        uint64_t timestamp;
        double price;
        uint32_t volume;
        char symbol[8];
    };
    
    // Pre-allocate buffers for market data
    const size_t buffer_size = sizeof(MarketData) * 1000;
    auto* market_buf = buffet.allocate_buffer(buffer_size, BFType::HEAP);
    auto* order_buf = buffet.allocate_buffer(buffer_size, BFType::HEAP);
    
    // Network buffer for market data feed
    NICBuffer* feed_buf = nullptr;
    try {
        feed_buf = dynamic_cast<NICBuffer*>(
            buffet.allocate_buffer(buffer_size, BFType::UDP)
        );
    } catch (...) {
        std::cout << "   Network buffer not available, simulating..." << std::endl;
    }
    
    std::cout << "   HFT buffers allocated:" << std::endl;
    std::cout << "     Market data buffer: " << buffer_size << " bytes" << std::endl;
    std::cout << "     Order buffer: " << buffer_size << " bytes" << std::endl;
    
    // Simulate market data processing
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> price_dist(100.0, 101.0);
    std::uniform_int_distribution<> volume_dist(100, 10000);
    
    const int num_ticks = 1000;
    auto start = high_resolution_clock::now();
    
    MarketData* market_data = reinterpret_cast<MarketData*>(market_buf->data());
    
    for (int i = 0; i < num_ticks; ++i) {
        // Simulate receiving market data
        MarketData& tick = market_data[i % 1000];
        tick.timestamp = duration_cast<nanoseconds>(
            high_resolution_clock::now().time_since_epoch()
        ).count();
        tick.price = price_dist(gen);
        tick.volume = volume_dist(gen);
        std::strcpy(tick.symbol, "AAPL");
        
        // Ultra-fast decision making
        if (tick.price < 100.5 && tick.volume > 5000) {
            // Place order (would go to order buffer)
        }
    }
    
    auto end = high_resolution_clock::now();
    auto latency = duration_cast<nanoseconds>(end - start).count() / num_ticks;
    
    std::cout << "   Processed " << num_ticks << " market ticks" << std::endl;
    std::cout << "   Average latency: " << latency << " ns per tick" << std::endl;
    std::cout << "   Throughput: " << (1000000000.0 / latency) << " ticks/second" << std::endl;
}

void ml_pipeline_example(BuffetAlligator& buffet) {
    std::cout << "\n4. Machine Learning Data Pipeline" << std::endl;
    
    // Create buffers for ML pipeline
    const size_t batch_size = 32;
    const size_t image_size = 224 * 224 * 3;  // ImageNet size
    const size_t batch_buffer_size = batch_size * image_size * sizeof(float);
    
    // Data loading buffer (CPU)
    auto* load_buf = buffet.allocate_buffer(batch_buffer_size, BFType::HEAP);
    
    // Preprocessing buffer (CPU)
    auto* preprocess_buf = buffet.allocate_buffer(batch_buffer_size, BFType::HEAP);
    
    // GPU buffer for training (if available)
    GPUBuffer* gpu_buf = nullptr;
    try {
        gpu_buf = dynamic_cast<GPUBuffer*>(
            buffet.allocate_buffer(batch_buffer_size, BFType::GPU)
        );
        std::cout << "   Allocated GPU buffer for ML training" << std::endl;
    } catch (...) {
        std::cout << "   GPU not available, using CPU" << std::endl;
    }
    
    std::cout << "   ML pipeline buffers:" << std::endl;
    std::cout << "     Batch size: " << batch_size << " images" << std::endl;
    std::cout << "     Image size: 224x224x3" << std::endl;
    std::cout << "     Buffer size: " << (batch_buffer_size / (1024*1024)) << " MB per batch" << std::endl;
    
    // Simulate data augmentation pipeline
    float* load_data = reinterpret_cast<float*>(load_buf->data());
    float* preprocess_data = reinterpret_cast<float*>(preprocess_buf->data());
    
    // Load batch
    std::cout << "   Loading batch..." << std::endl;
    for (size_t i = 0; i < batch_size * image_size; ++i) {
        load_data[i] = (i % 256) / 255.0f;  // Normalized pixel values
    }
    
    // Preprocess (normalize, augment)
    std::cout << "   Preprocessing..." << std::endl;
    for (size_t img = 0; img < batch_size; ++img) {
        size_t offset = img * image_size;
        
        // Simple augmentation: random brightness adjustment
        float brightness = 0.9f + (img % 3) * 0.1f;
        
        for (size_t pixel = 0; pixel < image_size; ++pixel) {
            preprocess_data[offset + pixel] = load_data[offset + pixel] * brightness;
        }
    }
    
    // Transfer to GPU if available
    if (gpu_buf) {
        auto start = high_resolution_clock::now();
        gpu_buf->upload(preprocess_data, batch_buffer_size);
        auto end = high_resolution_clock::now();
        
        auto transfer_time = duration_cast<microseconds>(end - start).count();
        double bandwidth = (batch_buffer_size / 1024.0 / 1024.0) / (transfer_time / 1000000.0);
        
        std::cout << "   Transferred to GPU in " << transfer_time << " µs" << std::endl;
        std::cout << "   Transfer bandwidth: " << std::fixed << std::setprecision(2) 
                  << bandwidth << " MB/s" << std::endl;
    }
    
    // Chain buffers for continuous processing
    load_buf->next.store(preprocess_buf, std::memory_order_release);
    if (gpu_buf) {
        preprocess_buf->next.store(gpu_buf, std::memory_order_release);
    }
    
    std::cout << "   ML pipeline ready for training!" << std::endl;
}

void memory_mapped_io_example(BuffetAlligator& buffet) {
    std::cout << "\n5. Memory-Mapped I/O Pattern" << std::endl;
    
    // Create large file-backed buffer
    const size_t file_size = 100 * 1024 * 1024;  // 100MB
    auto* mmap_buf = dynamic_cast<FileBackedBuffer*>(
        buffet.allocate_buffer(file_size, BFType::FILE_BACKED)
    );
    
    if (!mmap_buf) {
        std::cout << "   File-backed buffer not available" << std::endl;
        return;
    }
    
    std::cout << "   Created 100MB memory-mapped file" << std::endl;
    
    // Simulate database-like access pattern
    struct Record {
        uint64_t id;
        char data[120];
    };
    
    const size_t num_records = file_size / sizeof(Record);
    Record* records = reinterpret_cast<Record*>(mmap_buf->data());
    
    // Write records
    std::cout << "   Writing " << num_records << " records..." << std::endl;
    auto write_start = high_resolution_clock::now();
    
    for (size_t i = 0; i < num_records; ++i) {
        records[i].id = i;
        std::snprintf(records[i].data, sizeof(records[i].data), 
                     "Record %zu data", i);
    }
    
    mmap_buf->sync();  // Ensure written to disk
    
    auto write_end = high_resolution_clock::now();
    auto write_time = duration_cast<milliseconds>(write_end - write_start).count();
    
    std::cout << "   Write time: " << write_time << " ms" << std::endl;
    std::cout << "   Write throughput: " << (file_size / 1024.0 / 1024.0) / (write_time / 1000.0) 
              << " MB/s" << std::endl;
    
    // Random access reads
    std::cout << "   Performing random reads..." << std::endl;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, num_records - 1);
    
    auto read_start = high_resolution_clock::now();
    
    for (int i = 0; i < 10000; ++i) {
        size_t idx = dist(gen);
        volatile uint64_t id = records[idx].id;  // Force read
        (void)id;  // Suppress unused warning
    }
    
    auto read_end = high_resolution_clock::now();
    auto read_time = duration_cast<microseconds>(read_end - read_start).count();
    
    std::cout << "   Random read time: " << read_time << " µs for 10000 reads" << std::endl;
    std::cout << "   Average latency: " << (read_time / 10000.0) << " µs per read" << std::endl;
}

void zero_copy_chain_example(BuffetAlligator& buffet) {
    std::cout << "\n6. Zero-Copy Chain Pattern" << std::endl;
    
    // Create a chain optimized for zero-copy operations
    auto* nic_buf = dynamic_cast<NICBuffer*>(
        buffet.allocate_buffer(65536, BFType::TCP)  // 64KB
    );
    
    if (!nic_buf) {
        std::cout << "   Network buffer not available" << std::endl;
        return;
    }
    
    // Check zero-copy support
    uint32_t features = nic_buf->get_features();
    bool has_zero_copy = features & static_cast<uint32_t>(NICFeatures::ZERO_COPY);
    
    std::cout << "   NIC zero-copy support: " << (has_zero_copy ? "YES" : "NO") << std::endl;
    
    // Simulate receiving multiple packets
    struct Packet {
        uint32_t seq;
        uint16_t len;
        uint16_t flags;
        char payload[1024];
    };
    
    // Place packets in buffer
    Packet* packets = reinterpret_cast<Packet*>(nic_buf->data());
    for (int i = 0; i < 10; ++i) {
        packets[i].seq = i;
        packets[i].len = 100 + i * 10;
        packets[i].flags = 0x0001;
        std::snprintf(packets[i].payload, sizeof(packets[i].payload), 
                     "Packet %d data", i);
    }
    
    // Create claims for zero-copy processing
    std::cout << "   Creating zero-copy claims..." << std::endl;
    std::vector<BufferClaim> claims;
    
    for (int i = 0; i < 10; ++i) {
        size_t offset = i * sizeof(Packet);
        claims.emplace_back(nic_buf, offset, sizeof(Packet), i);
    }
    
    // Process claims without copying
    for (const auto& claim : claims) {
        if (claim.buffer) {
            const Packet* pkt = reinterpret_cast<const Packet*>(claim.buffer->data() + claim.offset);
            std::cout << "   Processing packet " << pkt->seq 
                     << " (len=" << pkt->len << ") zero-copy" << std::endl;
        }
    }
    
    // Chain to processing buffer
    auto* process_buf = nic_buf->create_new(65536);
    nic_buf->next.store(process_buf, std::memory_order_release);
    
    std::cout << "   Chained for further zero-copy processing" << std::endl;
}

int main() {
    std::cout << "=== BuffetAlligator Advanced Patterns Example ===" << std::endl;
    std::cout << "Advanced techniques for the memory connoisseur! 🎓" << std::endl << std::endl;

    auto& buffet = get_buffet_alligator();

    ring_buffer_example(buffet);
    pipeline_architecture_example(buffet);
    hft_pattern_example(buffet);
    ml_pipeline_example(buffet);
    memory_mapped_io_example(buffet);
    zero_copy_chain_example(buffet);

    std::cout << "\n=== Example Complete ===" << std::endl;
    std::cout << "Advanced patterns unlock the full power of BuffetAlligator!" << std::endl;
    
    return 0;
}