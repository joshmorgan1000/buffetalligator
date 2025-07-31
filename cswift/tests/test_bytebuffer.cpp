/**
 * @brief Unit tests for CSNIOByteBuffer
 */

#include <catch2/catch_test_macros.hpp>
#include <cswift/cswift.hpp>
#include <cstring>
#include <numeric>

using namespace cswift;

TEST_CASE("CSNIOByteBuffer construction and destruction", "[bytebuffer]") {
    SECTION("Default construction") {
        CSNIOByteBuffer buffer;
        // No isValid() method - buffer is valid if constructed successfully
        REQUIRE(buffer.readableBytes() == 0);
        REQUIRE(buffer.writableBytes() > 0);
    }
    
    SECTION("Construction with custom capacity") {
        CSNIOByteBuffer buffer(4096);
        // No isValid() method - buffer is valid if constructed successfully
        REQUIRE(buffer.writableBytes() >= 4096);
    }
    
    SECTION("Move construction") {
        CSNIOByteBuffer buffer1(2048);
        size_t capacity = buffer1.writableBytes();
        
        CSNIOByteBuffer buffer2(std::move(buffer1));
        // No isValid() method - buffer is valid if constructed successfully
        REQUIRE(buffer2.writableBytes() == capacity);
        // buffer1 is moved from, handle is nullptr
    }
    
    SECTION("Move assignment") {
        CSNIOByteBuffer buffer1(1024);
        CSNIOByteBuffer buffer2(2048);
        
        buffer2 = std::move(buffer1);
        // No isValid() method - buffer is valid if constructed successfully
        // buffer1 is moved from, handle is nullptr
    }
}

TEST_CASE("CSNIOByteBuffer basic read/write operations", "[bytebuffer]") {
    CSNIOByteBuffer buffer;
    
    SECTION("Write and read bytes") {
        const char* testData = "Hello, World!";
        size_t dataLen = strlen(testData);
        
        buffer.writeBytes(testData, dataLen);
        // writeBytes doesn't return a value, it throws on error
        REQUIRE(buffer.readableBytes() == dataLen);
        
        char readBuffer[256] = {0};
        buffer.readBytes(readBuffer, dataLen);
        // readBytes doesn't return a value, it throws on error
        REQUIRE(std::memcmp(readBuffer, testData, dataLen) == 0);
        REQUIRE(buffer.readableBytes() == 0);
    }
    
    SECTION("Write and read with vectors") {
        std::vector<uint8_t> testData(256);
        std::iota(testData.begin(), testData.end(), 0);
        
        buffer.writeBytes(testData);
        // writeBytes doesn't return a value
        
        std::vector<uint8_t> readData(testData.size());
        size_t readSize = buffer.readBytes(readData);
        REQUIRE(readSize == testData.size());
        readData.resize(readSize);
        REQUIRE(readData == testData);
    }
    
    SECTION("Partial reads") {
        std::vector<uint8_t> testData(100, 0x42);
        buffer.writeBytes(testData);
        
        std::vector<uint8_t> partial1(25);
        size_t read1 = buffer.readBytes(partial1);
        REQUIRE(read1 == 25);
        REQUIRE(buffer.readableBytes() == 75);
        
        std::vector<uint8_t> partial2(75);
        size_t read2 = buffer.readBytes(partial2);
        REQUIRE(read2 == 75);
        REQUIRE(buffer.readableBytes() == 0);
    }
}

TEST_CASE("CSNIOByteBuffer zero-copy write operations", "[bytebuffer]") {
    CSNIOByteBuffer buffer(1024);
    
    SECTION("Write with unsafe mutable bytes") {
        const char* testString = "Zero-copy write test";
        size_t testLen = strlen(testString);
        
        size_t written = buffer.writeWithUnsafeMutableBytes(
            [testString, testLen](void* ptr, size_t capacity) {
                REQUIRE(ptr != nullptr);
                REQUIRE(capacity >= testLen);
                // ZERO_COPY_ALLOWED - test data initialization
                std::memcpy(ptr, testString, testLen);
                return testLen;
            }
        );
        
        REQUIRE(written == testLen);
        REQUIRE(buffer.readableBytes() == testLen);
        
        // Verify the data was written correctly
        char readBuffer[256] = {0};
        buffer.readBytes(readBuffer, testLen);
        REQUIRE(std::memcmp(readBuffer, testString, testLen) == 0);
    }
    
    SECTION("Multiple zero-copy writes") {
        size_t totalWritten = 0;
        
        for (int i = 0; i < 10; ++i) {
            size_t written = buffer.writeWithUnsafeMutableBytes(
                [i](void* ptr, size_t capacity) {
                    if (capacity < sizeof(int)) return size_t(0);
                    *static_cast<int*>(ptr) = i;
                    return sizeof(int);
                }
            );
            totalWritten += written;
        }
        
        REQUIRE(totalWritten == 10 * sizeof(int));
        REQUIRE(buffer.readableBytes() == totalWritten);
        
        // Verify all values
        for (int i = 0; i < 10; ++i) {
            int value;
            buffer.readBytes(&value, sizeof(value));
            REQUIRE(value == i);
        }
    }
}

TEST_CASE("CSNIOByteBuffer clear operation", "[bytebuffer]") {
    CSNIOByteBuffer buffer;
    
    SECTION("Clear resets indices") {
        std::vector<uint8_t> data(100, 0xFF);
        buffer.writeBytes(data);
        
        REQUIRE(buffer.readableBytes() == 100);
        size_t writableBefore = buffer.writableBytes();
        
        buffer.clear();
        
        REQUIRE(buffer.readableBytes() == 0);
        REQUIRE(buffer.writableBytes() > writableBefore);
    }
}

TEST_CASE("CSNIOByteBuffer error handling", "[bytebuffer]") {
    CSNIOByteBuffer buffer(16);  // Small buffer for testing limits
    
    SECTION("Read more than available") {
        buffer.writeBytes("test", 4);
        std::vector<uint8_t> data(10);
        size_t bytesRead = buffer.readBytes(data);
        REQUIRE(bytesRead == 4);  // Should only read what's available
    }
    
    SECTION("Zero-copy write respects capacity") {
        bool capacityChecked = false;
        buffer.writeWithUnsafeMutableBytes(
            [&capacityChecked](void* ptr, size_t capacity) {
                REQUIRE(ptr != nullptr);
                REQUIRE(capacity > 0);
                capacityChecked = true;
                // Write up to capacity
                std::memset(ptr, 0xAA, capacity);
                return capacity;
            }
        );
        REQUIRE(capacityChecked);
    }
}

TEST_CASE("CSNIOByteBuffer performance characteristics", "[bytebuffer][performance]") {
    SECTION("Large zero-copy write") {
        CSNIOByteBuffer buffer(1024 * 1024);  // 1MB buffer
        
        size_t written = buffer.writeWithUnsafeMutableBytes(
            [](void* ptr, size_t capacity) {
                // Simulate large data write without actual copy
                // In real usage, this would be direct I/O or similar
                return std::min(capacity, size_t(512 * 1024));
            }
        );
        
        REQUIRE(written == 512 * 1024);
        REQUIRE(buffer.readableBytes() == 512 * 1024);
    }
}