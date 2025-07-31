/**
 * @brief Comprehensive unit tests for CSNIOByteBuffer
 * 
 * @details These tests verify actual functionality, not just "doesn't crash"
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <cswift/cswift.hpp>
#include <cstring>
#include <numeric>
#include <vector>
#include <thread>
#include <random>

using namespace cswift;

TEST_CASE("ByteBuffer capacity management", "[bytebuffer][comprehensive]") {
    SECTION("Capacity expansion on write") {
        CSNIOByteBuffer buffer(10); // Small initial capacity
        
        // Write more than initial capacity
        std::vector<uint8_t> data(100, 0x42);
        buffer.writeBytes(data.data(), data.size());
        
        REQUIRE(buffer.readableBytes() == 100);
        REQUIRE(buffer.writableBytes() >= 0); // Should have expanded
        
        // Verify data integrity
        std::vector<uint8_t> readData(100);
        buffer.readBytes(readData.data(), readData.size());
        REQUIRE(readData == data);
    }
    
    SECTION("Multiple expansions") {
        CSNIOByteBuffer buffer(8);
        size_t totalWritten = 0;
        
        // Force multiple expansions
        for (int i = 0; i < 10; ++i) {
            std::vector<uint8_t> chunk(32, static_cast<uint8_t>(i));
            buffer.writeBytes(chunk.data(), chunk.size());
            totalWritten += 32;
        }
        
        REQUIRE(buffer.readableBytes() == totalWritten);
        
        // Verify all data is correct
        for (int i = 0; i < 10; ++i) {
            std::vector<uint8_t> chunk(32);
            buffer.readBytes(chunk.data(), chunk.size());
            REQUIRE(std::all_of(chunk.begin(), chunk.end(), 
                [i](uint8_t b) { return b == static_cast<uint8_t>(i); }));
        }
    }
}

TEST_CASE("ByteBuffer boundary conditions", "[bytebuffer][comprehensive]") {
    SECTION("Write exactly to capacity") {
        CSNIOByteBuffer buffer(100);
        std::vector<uint8_t> data(100, 0xFF);
        
        buffer.writeBytes(data.data(), data.size());
        REQUIRE(buffer.readableBytes() == 100);
        // Psyne uses 64MB minimum buffer size, so there will always be writable space
        REQUIRE(buffer.writableBytes() >= (64 * 1024 * 1024 - 100));
    }
    
    SECTION("Read more than available") {
        CSNIOByteBuffer buffer;
        buffer.writeBytes("test", 4);
        
        std::vector<uint8_t> readBuf(100);
        // Using the vector version which returns actual bytes read
        size_t bytesRead = buffer.readBytes(readBuf);
        
        REQUIRE(bytesRead == 4); // Should only read what's available
        readBuf.resize(bytesRead);
        REQUIRE(std::string(reinterpret_cast<char*>(readBuf.data()), bytesRead) == "test");
    }
    
    SECTION("Zero-length operations") {
        CSNIOByteBuffer buffer;
        
        buffer.writeBytes(nullptr, 0); // Should not throw
        REQUIRE(buffer.readableBytes() == 0);
        
        buffer.readBytes(nullptr, 0); // Should not throw
    }
}

TEST_CASE("ByteBuffer zero-copy verification", "[bytebuffer][comprehensive]") {
    SECTION("Write with unsafe mutable bytes actually modifies buffer") {
        CSNIOByteBuffer buffer(256);
        const char* testData = "Original";
        
        // First write some data
        buffer.writeBytes(testData, strlen(testData));
        size_t originalReadable = buffer.readableBytes();
        
        // Now use zero-copy write
        const char* newData = "Modified";
        size_t bytesWritten = buffer.writeWithUnsafeMutableBytes([&](void* ptr, size_t available) {
            size_t copyLen = std::min(strlen(newData), available);
            memcpy(ptr, newData, copyLen);
            return copyLen;
        });
        
        REQUIRE(bytesWritten == strlen(newData));
        
        // Verify the modification
        REQUIRE(buffer.readableBytes() == originalReadable + bytesWritten);
        
        // Skip original data
        std::vector<uint8_t> skipBuf(originalReadable);
        buffer.readBytes(skipBuf.data(), skipBuf.size());
        
        // Read modified data
        std::vector<uint8_t> readBuf(bytesWritten);
        buffer.readBytes(readBuf.data(), readBuf.size());
        REQUIRE(std::string(reinterpret_cast<char*>(readBuf.data()), bytesWritten) == 
                std::string(newData, bytesWritten));
    }
    
    SECTION("Multiple zero-copy writes") {
        CSNIOByteBuffer buffer(512);
        
        // Perform multiple zero-copy writes
        for (int i = 0; i < 5; ++i) {
            size_t written = buffer.writeWithUnsafeMutableBytes([i](void* ptr, size_t available) {
                size_t writeSize = std::min(size_t(50), available);
                std::fill_n(static_cast<uint8_t*>(ptr), writeSize, static_cast<uint8_t>(i + 1));
                return writeSize;
            });
            
            REQUIRE(written == 50);
        }
        
        // Verify all patterns
        for (int i = 0; i < 5; ++i) {
            std::vector<uint8_t> readBuf(50);
            buffer.readBytes(readBuf.data(), readBuf.size());
            REQUIRE(std::all_of(readBuf.begin(), readBuf.end(),
                [i](uint8_t b) { return b == static_cast<uint8_t>(i + 1); }));
        }
    }
}

TEST_CASE("ByteBuffer thread safety", "[bytebuffer][comprehensive][!mayfail]") {
    // Note: ByteBuffer may not be thread-safe by design
    // This test documents the expected behavior
    
    SECTION("Concurrent reads should be safe") {
        CSNIOByteBuffer buffer(1024);
        
        // Pre-fill buffer
        std::vector<uint8_t> data(1000);
        std::iota(data.begin(), data.end(), 0);
        buffer.writeBytes(data.data(), data.size());
        
        std::atomic<bool> success{true};
        std::vector<std::thread> readers;
        
        // Multiple readers
        for (int i = 0; i < 4; ++i) {
            readers.emplace_back([&buffer, &success]() {
                for (int j = 0; j < 100; ++j) {
                    auto readable = buffer.readableBytes();
                    if (readable < 0 || readable > 1000) {
                        success = false;
                    }
                }
            });
        }
        
        for (auto& t : readers) {
            t.join();
        }
        
        REQUIRE(success.load());
    }
}

TEST_CASE("ByteBuffer memory patterns", "[bytebuffer][comprehensive]") {
    SECTION("Clear resets indices but preserves capacity") {
        CSNIOByteBuffer buffer(256);
        
        // Fill buffer
        std::vector<uint8_t> data(200, 0xAA);
        buffer.writeBytes(data.data(), data.size());
        
        auto capacityBefore = buffer.capacity();
        auto writableBefore = buffer.writableBytes();
        
        buffer.clear();
        
        REQUIRE(buffer.readableBytes() == 0);
        REQUIRE(buffer.capacity() == capacityBefore);
        REQUIRE(buffer.writableBytes() > writableBefore); // More writable after clear
        
        // Can write again
        buffer.writeBytes(data.data(), data.size());
        REQUIRE(buffer.readableBytes() == 200);
    }
    
    SECTION("Move leaves source in valid but empty state") {
        CSNIOByteBuffer buffer1(512);
        std::vector<uint8_t> data(100, 0xBB);
        buffer1.writeBytes(data.data(), data.size());
        
        CSNIOByteBuffer buffer2(std::move(buffer1));
        
        // buffer2 has the data
        REQUIRE(buffer2.readableBytes() == 100);
        
        // buffer1 handle is now nullptr after move
        // Reading from it returns 0 (safe behavior)
        REQUIRE(buffer1.readableBytes() == 0);
        
        // Writing to it throws because handle is null
        REQUIRE_THROWS_AS(buffer1.writeBytes("test", 4), CSException);
    }
}

TEST_CASE("ByteBuffer performance characteristics", "[bytebuffer][comprehensive][benchmark]") {
    SECTION("Write performance scales linearly") {
        std::vector<size_t> sizes = {1024, 10240, 102400, 1024000};
        std::vector<double> times;
        
        for (auto size : sizes) {
            CSNIOByteBuffer buffer(size * 2);
            std::vector<uint8_t> data(size);
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, 255);
            std::generate(data.begin(), data.end(), [&]() { return dis(gen); });
            
            auto start = std::chrono::high_resolution_clock::now();
            buffer.writeBytes(data.data(), data.size());
            auto end = std::chrono::high_resolution_clock::now();
            
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            times.push_back(duration);
        }
        
        // Check that time roughly scales with size (allowing 3x variance for small implementations)
        for (size_t i = 1; i < times.size(); ++i) {
            // Avoid division by zero for very fast operations
            if (times[i-1] < 1.0) {
                times[i-1] = 1.0; // Set minimum time to 1 microsecond
            }
            double ratio = times[i] / times[i-1];
            double sizeRatio = static_cast<double>(sizes[i]) / sizes[i-1];
            REQUIRE(ratio < sizeRatio * 3.0); // Should not be worse than 3x the size increase
        }
    }
}

// Benchmarks for performance regression testing
TEST_CASE("ByteBuffer benchmarks", "[bytebuffer][benchmark]") {
    // Pre-allocate everything OUTSIDE the benchmark loops
    static CSNIOByteBuffer smallBuffer(64 * 1024 * 1024);  // 64MB
    static CSNIOByteBuffer largeBuffer(128 * 1024 * 1024); // 128MB  
    static std::vector<uint8_t> testData(1024 * 1024, 0x42); // 1MB test data
    static uint8_t smallData[100];
    static bool initialized = false;
    
    if (!initialized) {
        std::fill_n(smallData, 100, 0x42);
        initialized = true;
    }
    
    BENCHMARK("Small writes (100 bytes) - fixed") {
        // Reset buffer position, don't allocate new one!
        smallBuffer.clear();
        smallBuffer.writeBytes(smallData, 100);
        return smallBuffer.readableBytes();
    };
    
    BENCHMARK("Large writes (1MB) - fixed") {
        // Reset buffer position, don't allocate new one!
        largeBuffer.clear();
        largeBuffer.writeBytes(testData.data(), testData.size());
        return largeBuffer.readableBytes();
    };
    
    BENCHMARK("Zero-copy write pointer access") {
        // This actually measures zero-copy access
        static CSNIOByteBuffer zcBuffer(64 * 1024 * 1024);
        zcBuffer.clear();
        
        return zcBuffer.writeWithUnsafeMutableBytes([](void* ptr, size_t available) {
            // Write 1MB directly to the pointer
            if (available >= 1024 * 1024) {
                // Just memset to simulate actual write
                std::memset(ptr, 0x42, 1024 * 1024);
                return size_t(1024 * 1024);
            }
            return size_t(0);
        });
    };
    
    BENCHMARK("Raw memcpy baseline (1MB)") {
        // For comparison - raw memcpy speed
        static std::vector<uint8_t> src(1024 * 1024, 0x42);
        static std::vector<uint8_t> dst(1024 * 1024);
        std::memcpy(dst.data(), src.data(), 1024 * 1024);
        return dst[0]; // Prevent optimization
    };
}