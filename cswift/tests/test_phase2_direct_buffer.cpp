/**
 * @file test_phase2_direct_buffer.cpp
 * @brief Tests for Phase 2 direct buffer operations
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

// Test fixture for Phase 2
struct Phase2DirectBufferFixture {
    CSHandle tcpParams = nullptr;
    CSHandle queue = nullptr;
    CSHandle sharedBuffer = nullptr;
    const size_t bufferSize = 4 * 1024 * 1024; // 4MB
    
    Phase2DirectBufferFixture() {
        // Create test parameters
        tcpParams = cswift_nw_parameters_create_high_performance();
        REQUIRE(tcpParams != nullptr);
        
        // Create dispatch queue
        queue = cswift_dispatch_queue_create("phase2.test.queue", 0);
        REQUIRE(queue != nullptr);
        
        // Create shared buffer for direct operations
        sharedBuffer = cswift_shared_buffer_create(bufferSize, 64);
        REQUIRE(sharedBuffer != nullptr);
    }
    
    ~Phase2DirectBufferFixture() {
        if (sharedBuffer) {
            cswift_shared_buffer_destroy(sharedBuffer);
        }
        if (tcpParams) {
            cswift_nw_parameters_destroy(tcpParams);
        }
        if (queue) {
            cswift_dispatch_queue_destroy(queue);
        }
    }
};

TEST_CASE("Phase 2: Direct buffer operations", "[phase2][zerocopy][network]") {
    Phase2DirectBufferFixture fixture;
    
    SECTION("Prepare buffer for network I/O") {
        // Prepare buffer for network operations
        int32_t result = cswift_shared_buffer_prepare_for_network_io(fixture.sharedBuffer);
        CHECK(result == CS_SUCCESS);
        
        // Verify buffer is still accessible
        void* bufferPtr = cswift_shared_buffer_data(fixture.sharedBuffer);
        REQUIRE(bufferPtr != nullptr);
        
        // Verify alignment is maintained
        int32_t alignment = cswift_shared_buffer_alignment(fixture.sharedBuffer);
        CHECK(alignment >= 8);
    }
    
    SECTION("Create zero-copy protocol framer") {
        // Create a zero-copy framer
        CSHandle framer = cswift_nw_protocol_framer_create_zero_copy("bufferalligator.zerocopy.v1");
        REQUIRE(framer != nullptr);
        
        // Set target buffer for the framer
        int32_t result = cswift_nw_framer_set_target_buffer(
            framer, fixture.sharedBuffer, 0, fixture.bufferSize
        );
        CHECK(result == CS_SUCCESS);
        
        // Cleanup
        cswift_nw_protocol_framer_destroy(framer);
    }
    
    SECTION("Receive directly into buffer") {
        const uint16_t testPort = 9877;
        std::atomic<bool> serverReady(false);
        std::atomic<bool> testComplete(false);
        
        // Test data pattern
        const size_t testDataSize = 1024 * 1024; // 1MB
        std::vector<uint8_t> testPattern(testDataSize);
        for (size_t i = 0; i < testDataSize; ++i) {
            testPattern[i] = static_cast<uint8_t>((i * 31 + 17) & 0xFF);
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
                // Set up state handler to wait for connection ready
                std::atomic<bool> serverConnReady(false);
                cswift_nw_connection_set_state_update_handler(
                    connection,
                    [](int32_t state, void* ctx) {
                        if (state == static_cast<int32_t>(CSNWConnectionState::Ready)) {
                            auto* flag = static_cast<std::atomic<bool>*>(ctx);
                            flag->store(true);
                        }
                    },
                    &serverConnReady
                );
                
                // Start the connection
                cswift_nw_connection_start(connection, fixture.queue);
                
                // Wait for connection to be ready
                auto connStartTime = std::chrono::steady_clock::now();
                while (!serverConnReady.load() && !testComplete.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    if (std::chrono::steady_clock::now() - connStartTime > std::chrono::seconds(2)) {
                        break; // Timeout
                    }
                }
                
                if (serverConnReady.load()) {
                    // Send test pattern with completion handler
                    std::atomic<bool> sendComplete(false);
                    cswift_nw_connection_send(
                        connection,
                        testPattern.data(),
                        testPattern.size(),
                        [](int32_t error, void* ctx) {
                            auto* flag = static_cast<std::atomic<bool>*>(ctx);
                            flag->store(true);
                        },
                        &sendComplete
                    );
                    
                    // Wait for send to complete
                    while (!sendComplete.load() && !testComplete.load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
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
        
        // Prepare buffer for network I/O
        REQUIRE(cswift_shared_buffer_prepare_for_network_io(fixture.sharedBuffer) == CS_SUCCESS);
        
        // Receive directly into shared buffer
        struct ReceiveContext {
            std::atomic<size_t> bytesReceived{0};
            std::atomic<bool> isComplete{false};
        } receiveCtx;
        
        int32_t result = cswift_nw_connection_receive_into_buffer(
            clientConnection,
            fixture.sharedBuffer,
            0,                    // offset
            1,                    // minLength
            testDataSize,         // maxLength
            [](size_t bytes, bool isComplete, int32_t error, void* ctx) {
                if (error == CS_SUCCESS) {
                    auto* receiveCtx = static_cast<ReceiveContext*>(ctx);
                    receiveCtx->bytesReceived.store(bytes);
                    receiveCtx->isComplete.store(isComplete);
                }
            },
            &receiveCtx
        );
        REQUIRE(result == CS_SUCCESS);
        
        // Wait for receive to complete
        auto startTime = std::chrono::steady_clock::now();
        while (!receiveCtx.isComplete.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (std::chrono::steady_clock::now() - startTime > std::chrono::seconds(5)) {
                FAIL("Receive timeout");
                break;
            }
        }
        
        // Verify received data
        size_t receivedSize = receiveCtx.bytesReceived.load();
        CHECK(receivedSize == testDataSize);
        
        // Get pointer to received data in buffer
        const void* bufferData = cswift_shared_buffer_data(fixture.sharedBuffer);
        REQUIRE(bufferData != nullptr);
        
        // Verify data matches test pattern
        CHECK(memcmp(bufferData, testPattern.data(), receivedSize) == 0);
        
        // Cleanup
        cswift_nw_connection_cancel(clientConnection);
        cswift_nw_connection_destroy(clientConnection);
        
        testComplete = true;
        serverThread.join();
    }
    
    SECTION("Zero-copy framer with direct buffer") {
        // Create zero-copy framer
        CSHandle framer = cswift_nw_protocol_framer_create_zero_copy("test.direct.v1");
        REQUIRE(framer != nullptr);
        
        // Create connection with framer
        CSHandle connection = cswift_nw_connection_create_with_framer(
            "127.0.0.1", 9999, fixture.tcpParams, framer, nullptr
        );
        REQUIRE(connection != nullptr);
        
        // Cleanup
        cswift_nw_connection_destroy(connection);
        cswift_nw_protocol_framer_destroy(framer);
    }
    
    SECTION("Performance: Direct buffer vs traditional receive") {
        const size_t iterations = 100;
        const size_t dataSize = 64 * 1024; // 64KB
        
        // Prepare buffer
        REQUIRE(cswift_shared_buffer_prepare_for_network_io(fixture.sharedBuffer) == CS_SUCCESS);
        
        // Benchmark direct buffer operations
        auto start = std::chrono::high_resolution_clock::now();
        
        for (size_t i = 0; i < iterations; ++i) {
            // Simulate direct buffer write (what would happen in receive)
            void* bufferPtr = cswift_shared_buffer_mutable_pointer_at(
                fixture.sharedBuffer, 0, dataSize
            );
            REQUIRE(bufferPtr != nullptr);
            
            // Touch the memory to simulate write
            memset(bufferPtr, static_cast<int>(i & 0xFF), dataSize);
            cswift_shared_buffer_set_size(fixture.sharedBuffer, dataSize);
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        double avgTimeUs = duration.count() / static_cast<double>(iterations);
        double throughputMBps = (dataSize * iterations) / (duration.count() / 1e6) / (1024.0 * 1024.0);
        
        std::cout << "Direct buffer operations:" << std::endl;
        std::cout << "  Average time: " << avgTimeUs << " μs" << std::endl;
        std::cout << "  Throughput: " << throughputMBps << " MB/s" << std::endl;
        
        // Direct buffer operations should be very efficient
        CHECK(avgTimeUs < 100.0); // Less than 100 microseconds per 64KB operation
    }
}

TEST_CASE("Phase 2: Buffer lifecycle and memory management", "[phase2][zerocopy]") {
    SECTION("Multiple buffers with network I/O preparation") {
        const size_t numBuffers = 10;
        std::vector<CSHandle> buffers;
        
        // Create and prepare multiple buffers
        for (size_t i = 0; i < numBuffers; ++i) {
            CSHandle buffer = cswift_shared_buffer_create(1024 * 1024, 64);
            REQUIRE(buffer != nullptr);
            
            int32_t result = cswift_shared_buffer_prepare_for_network_io(buffer);
            CHECK(result == CS_SUCCESS);
            
            buffers.push_back(buffer);
        }
        
        // Verify all buffers are independent
        for (size_t i = 0; i < numBuffers; ++i) {
            void* ptr = cswift_shared_buffer_data(buffers[i]);
            REQUIRE(ptr != nullptr);
            
            // Write unique pattern
            memset(ptr, static_cast<int>(i), 1024);
        }
        
        // Verify patterns are maintained
        for (size_t i = 0; i < numBuffers; ++i) {
            void* ptr = cswift_shared_buffer_data(buffers[i]);
            uint8_t* bytePtr = static_cast<uint8_t*>(ptr);
            CHECK(bytePtr[0] == static_cast<uint8_t>(i));
        }
        
        // Cleanup
        for (auto buffer : buffers) {
            cswift_shared_buffer_destroy(buffer);
        }
    }
    
    SECTION("Buffer alignment verification") {
        // Test various alignments
        std::vector<int32_t> alignments = {8, 16, 32, 64, 128};
        
        for (auto alignment : alignments) {
            CSHandle buffer = cswift_shared_buffer_create(4096, alignment);
            REQUIRE(buffer != nullptr);
            
            // Prepare for network I/O
            int32_t result = cswift_shared_buffer_prepare_for_network_io(buffer);
            CHECK(result == CS_SUCCESS);
            
            // Verify alignment is maintained
            void* ptr = cswift_shared_buffer_data(buffer);
            uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
            CHECK((addr % alignment) == 0);
            
            // Verify reported alignment
            CHECK(cswift_shared_buffer_alignment(buffer) == alignment);
            
            cswift_shared_buffer_destroy(buffer);
        }
    }
}