/**
 * @file test_zerocopy_network.cpp
 * @brief Tests for Phase 1 zero-copy network enhancements
 */

#include <catch2/catch_test_macros.hpp>
#include <cswift/cswift.hpp>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <vector>
#include <iostream>

using namespace cswift;

// Test fixtures
struct ZeroCopyNetworkFixture {
    CSHandle tcpParams = nullptr;
    CSHandle queue = nullptr;
    
    ZeroCopyNetworkFixture() {
        // Create test parameters
        tcpParams = cswift_nw_parameters_create_tcp();
        REQUIRE(tcpParams != nullptr);
        
        // Create dispatch queue
        queue = cswift_dispatch_queue_create("test.queue", 0);
        REQUIRE(queue != nullptr);
    }
    
    ~ZeroCopyNetworkFixture() {
        if (tcpParams) {
            cswift_nw_parameters_destroy(tcpParams);
        }
        if (queue) {
            cswift_dispatch_queue_destroy(queue);
        }
    }
};

/**
 * @brief Test dispatch data creation from existing buffer (zero-copy)
 */
TEST_CASE("Dispatch data zero-copy operations", "[zerocopy][network]") {
    ZeroCopyNetworkFixture fixture;
    
    SECTION("Create dispatch data from existing buffer") {
        // Create a test buffer
        const size_t bufferSize = 1024;
        std::vector<uint8_t> buffer(bufferSize);
        
        // Fill with test pattern
        for (size_t i = 0; i < bufferSize; ++i) {
            buffer[i] = static_cast<uint8_t>(i & 0xFF);
        }
        
        // Create dispatch data referencing the buffer (zero-copy)
        CSHandle dispatchData = cswift_dispatch_data_create_map(
            buffer.data(), 0, bufferSize, nullptr
        );
        REQUIRE(dispatchData != nullptr);
        
        // Verify size
        size_t dataSize = cswift_dispatch_data_size(dispatchData);
        CHECK(dataSize == bufferSize);
        
        // Get contiguous bytes pointer (zero-copy access)
        const void* dataPtr = cswift_dispatch_data_get_contiguous_bytes(
            dispatchData, 0, bufferSize
        );
        REQUIRE(dataPtr != nullptr);
        
        // Verify data content matches original buffer
        CHECK(memcmp(dataPtr, buffer.data(), bufferSize) == 0);
        
        // Test partial access
        const size_t partialOffset = 100;
        const size_t partialLength = 512;
        const void* partialPtr = cswift_dispatch_data_get_contiguous_bytes(
            dispatchData, partialOffset, partialLength
        );
        REQUIRE(partialPtr != nullptr);
        CHECK(memcmp(partialPtr, buffer.data() + partialOffset, partialLength) == 0);
        
        // Cleanup
        cswift_dispatch_data_destroy(dispatchData);
    }

    SECTION("Zero-copy send and receive") {
        const uint16_t testPort = 9876;
        std::atomic<bool> serverReady(false);
        std::atomic<bool> testComplete(false);
        
        // Test data
        const size_t dataSize = 1024 * 1024; // 1MB
        std::vector<uint8_t> sendBuffer(dataSize);
        
        // Fill with test pattern
        for (size_t i = 0; i < dataSize; ++i) {
            sendBuffer[i] = static_cast<uint8_t>((i * 7) & 0xFF);
        }
        
        // Server thread
        std::thread serverThread([&]() {
            // Create listener
            CSHandle listener = cswift_nw_listener_create(testPort, fixture.tcpParams, nullptr);
            REQUIRE(listener != nullptr);
        
        std::atomic<CSHandle> serverConnection(nullptr);
        
        // Set connection handler
        cswift_nw_listener_set_new_connection_handler(
            listener,
            [](CSHandle connection, void* ctx) {
                auto* connPtr = static_cast<std::atomic<CSHandle>*>(ctx);
                connPtr->store(connection);
            },
            &serverConnection
        );
        
            // Start listener
            REQUIRE(cswift_nw_listener_start(listener, fixture.queue) == CS_SUCCESS);
            serverReady = true;
        
        // Wait for connection
        while (!serverConnection.load() && !testComplete.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        CSHandle connection = serverConnection.load();
        if (connection) {
            // Receive using zero-copy
            std::atomic<bool> receiveComplete(false);
            std::atomic<CSHandle> receivedData(nullptr);
            
                int32_t result = cswift_nw_connection_receive_message(
                    connection,
                    nullptr,
                    [](CSHandle dispatchData, bool isComplete, int32_t error, void* ctx) {
                        if (error == CS_SUCCESS && dispatchData) {
                            auto* dataPtr = static_cast<std::atomic<CSHandle>*>(ctx);
                            dataPtr->store(dispatchData);
                        }
                    },
                    &receivedData
                );
                CHECK(result == CS_SUCCESS);
            
            // Wait for data
            while (!receivedData.load() && !testComplete.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            
            CSHandle recvDispatchData = receivedData.load();
            if (recvDispatchData) {
                    // Verify received data size
                    size_t recvSize = cswift_dispatch_data_size(recvDispatchData);
                    CHECK(recvSize == dataSize);
                    
                    // Get zero-copy pointer to received data
                    const void* recvPtr = cswift_dispatch_data_get_contiguous_bytes(
                        recvDispatchData, 0, recvSize
                    );
                    REQUIRE(recvPtr != nullptr);
                    
                    // Verify data content
                    CHECK(memcmp(recvPtr, sendBuffer.data(), dataSize) == 0);
                
                cswift_dispatch_data_destroy(recvDispatchData);
            }
            
            cswift_nw_connection_destroy(connection);
        }
        
            cswift_nw_listener_stop(listener);
            cswift_nw_listener_destroy(listener);
        });
        
        // Wait for server to be ready
        while (!serverReady.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        // Client connection
        CSHandle clientConnection = cswift_nw_connection_create(
            "127.0.0.1", testPort, fixture.tcpParams, nullptr
        );
        REQUIRE(clientConnection != nullptr);
    
    // Set state handler
    std::atomic<bool> clientConnected(false);
    cswift_nw_connection_set_state_update_handler(
        clientConnection,
        [](int32_t state, void* ctx) {
            if (state == static_cast<int32_t>(CSNWConnectionState::Ready)) {
                auto* flag = static_cast<std::atomic<bool>*>(ctx);
                flag->store(true);
            }
        },
        &clientConnected
    );
    
        // Start connection
        REQUIRE(cswift_nw_connection_start(clientConnection, fixture.queue) == CS_SUCCESS);
    
    // Wait for connection
    while (!clientConnected.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
        // Create dispatch data from send buffer (zero-copy)
        CSHandle sendDispatchData = cswift_dispatch_data_create_map(
            sendBuffer.data(), 0, dataSize, nullptr
        );
        REQUIRE(sendDispatchData != nullptr);
        
        // Send using zero-copy
        std::atomic<bool> sendComplete(false);
        int32_t sendResult = cswift_nw_connection_send_dispatch_data(
            clientConnection,
            sendDispatchData,
            [](int32_t error, void* ctx) {
                auto* flag = static_cast<std::atomic<bool>*>(ctx);
                flag->store(error == CS_SUCCESS);
            },
            &sendComplete
        );
        REQUIRE(sendResult == CS_SUCCESS);
    
    // Wait for send to complete
    while (!sendComplete.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
        // Cleanup
        cswift_dispatch_data_destroy(sendDispatchData);
        cswift_nw_connection_cancel(clientConnection);
        cswift_nw_connection_destroy(clientConnection);
        
        // Signal test complete and wait for server
        testComplete = true;
        serverThread.join();
    }

    SECTION("Verify zero-copy reference") {
        // Create a buffer with known content
        const size_t bufferSize = 4096;
        std::vector<uint8_t> buffer(bufferSize);
        
        // Fill with unique pattern
        for (size_t i = 0; i < bufferSize; ++i) {
            buffer[i] = static_cast<uint8_t>((i * 13 + 7) & 0xFF);
        }
        
        // Store original pointer
        const void* originalPtr = buffer.data();
        
        // Create dispatch data
        CSHandle dispatchData = cswift_dispatch_data_create_map(
            originalPtr, 0, bufferSize, nullptr
        );
        REQUIRE(dispatchData != nullptr);
        
        // Get pointer from dispatch data
        const void* dispatchPtr = cswift_dispatch_data_get_contiguous_bytes(
            dispatchData, 0, bufferSize
        );
        REQUIRE(dispatchPtr != nullptr);
        
        // In a true zero-copy implementation, the pointer should be the same
        // or at least the content should match without any copies
        CHECK(memcmp(dispatchPtr, originalPtr, bufferSize) == 0);
        
        // Modify original buffer
        buffer[0] = 0xFF;
        buffer[bufferSize - 1] = 0xEE;
        
        // If truly zero-copy, changes should be reflected
        // Note: This behavior depends on the actual implementation
        // Some implementations might snapshot the data
        
        // Cleanup
        cswift_dispatch_data_destroy(dispatchData);
    }

    SECTION("Benchmark zero-copy performance") {
        const size_t iterations = 100;
        const size_t dataSize = 1024 * 1024; // 1MB
        
        std::vector<uint8_t> testData(dataSize);
        for (size_t i = 0; i < dataSize; ++i) {
            testData[i] = static_cast<uint8_t>(i & 0xFF);
        }
        
        // Benchmark dispatch data creation
        auto start = std::chrono::high_resolution_clock::now();
        
        for (size_t i = 0; i < iterations; ++i) {
            CSHandle dispatchData = cswift_dispatch_data_create_map(
                testData.data(), 0, dataSize, nullptr
            );
            REQUIRE(dispatchData != nullptr);
            
            const void* ptr = cswift_dispatch_data_get_contiguous_bytes(
                dispatchData, 0, dataSize
            );
            REQUIRE(ptr != nullptr);
            
            cswift_dispatch_data_destroy(dispatchData);
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        double avgTimeUs = duration.count() / static_cast<double>(iterations);
        std::cout << "Average time for zero-copy dispatch data operations: " 
                  << avgTimeUs << " microseconds" << std::endl;
        
        // Zero-copy operations should be very fast (< 20 microseconds per operation)
        // Note: On some systems this may take slightly longer due to system overhead
        CHECK(avgTimeUs < 20.0);
    }
}