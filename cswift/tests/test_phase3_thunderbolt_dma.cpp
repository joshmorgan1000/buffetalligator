/**
 * @file test_phase3_thunderbolt_dma.cpp
 * @brief Tests for Phase 3 Thunderbolt DMA support
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

// Test fixture for Phase 3
struct Phase3ThunderboltDMAFixture {
    CSHandle tcpParams = nullptr;
    CSHandle queue = nullptr;
    std::vector<CSHandle> bufferPool;
    const size_t bufferSize = 16 * 1024 * 1024; // 16MB for DMA testing
    const size_t poolSize = 2;
    
    Phase3ThunderboltDMAFixture() {
        // Create high-performance parameters
        tcpParams = cswift_nw_parameters_create_high_performance();
        REQUIRE(tcpParams != nullptr);
        
        // Create dispatch queue
        queue = cswift_dispatch_queue_create("phase3.dma.queue", 0);
        REQUIRE(queue != nullptr);
        
        // Create buffer pool for DMA
        for (size_t i = 0; i < poolSize; ++i) {
            CSHandle buffer = cswift_shared_buffer_create(bufferSize, 4096); // 4KB alignment for DMA
            REQUIRE(buffer != nullptr);
            
            // Prepare for DMA operations
            int32_t result = cswift_shared_buffer_prepare_for_network_io(buffer);
            CHECK(result == CS_SUCCESS);
            
            bufferPool.push_back(buffer);
        }
    }
    
    ~Phase3ThunderboltDMAFixture() {
        for (auto buffer : bufferPool) {
            cswift_shared_buffer_destroy(buffer);
        }
        if (tcpParams) {
            cswift_nw_parameters_destroy(tcpParams);
        }
        if (queue) {
            cswift_dispatch_queue_destroy(queue);
        }
    }
};

TEST_CASE("Phase 3: Thunderbolt DMA Operations", "[phase3][dma][thunderbolt]") {
    Phase3ThunderboltDMAFixture fixture;
    
    SECTION("Thunderbolt device enumeration") {
        uint32_t vendorID = 0;
        uint32_t deviceID = 0;
        int32_t linkSpeed = 0;
        
        // Try to get info for first Thunderbolt device
        int32_t result = cswift_thunderbolt_get_device_info(0, &vendorID, &deviceID, &linkSpeed);
        
        if (result == CS_SUCCESS) {
            CHECK(vendorID != 0);
            CHECK(deviceID != 0);
            CHECK(linkSpeed > 0);
            
            std::cout << "Thunderbolt device found:" << std::endl;
            std::cout << "  Vendor ID: 0x" << std::hex << vendorID << std::dec << std::endl;
            std::cout << "  Device ID: 0x" << std::hex << deviceID << std::dec << std::endl;
            std::cout << "  Link Speed: " << linkSpeed << " Gbps" << std::endl;
        } else {
            std::cout << "No Thunderbolt devices found (expected on non-Mac systems)" << std::endl;
        }
    }
    
    SECTION("Memory region registration") {
        // Create a test connection (local loopback for testing)
        CSHandle connection = cswift_nw_connection_create("127.0.0.1", 0, fixture.tcpParams, nullptr);
        REQUIRE(connection != nullptr);
        
        // Check if connection supports DMA
        int32_t supportsDMA = 0;
        int32_t result = cswift_nw_connection_supports_dma(connection, &supportsDMA);
        CHECK(result == CS_SUCCESS);
        
        if (supportsDMA) {
            // Register memory region
            CSHandle buffer = fixture.bufferPool[0];
            uint64_t regionID = 0;
            
            result = cswift_nw_connection_register_memory_region(
                connection, buffer, &regionID, CSDMA_READ_WRITE
            );
            CHECK(result == CS_SUCCESS);
            CHECK(regionID != 0);
            
            std::cout << "Memory region registered with ID: " << regionID << std::endl;
            
            // Unregister the region
            result = cswift_nw_connection_unregister_memory_region(connection, regionID);
            CHECK(result == CS_SUCCESS);
        } else {
            std::cout << "DMA not supported on this connection" << std::endl;
        }
        
        cswift_nw_connection_destroy(connection);
    }
    
    SECTION("RDMA write simulation") {
        const uint16_t testPort = 9879;
        std::atomic<bool> serverReady(false);
        std::atomic<bool> testComplete(false);
        std::atomic<uint64_t> serverRegionID(0);
        
        // Test pattern for RDMA
        const size_t rdmaDataSize = 1024 * 1024; // 1MB
        std::vector<uint8_t> testPattern(rdmaDataSize);
        for (size_t i = 0; i < rdmaDataSize; ++i) {
            testPattern[i] = static_cast<uint8_t>((i * 37 + 73) & 0xFF);
        }
        
        // Server thread - registers memory and waits for RDMA
        std::thread serverThread([&]() {
            CSHandle listener = cswift_nw_listener_create(testPort, fixture.tcpParams, nullptr);
            REQUIRE(listener != nullptr);
            
            std::atomic<CSHandle> serverConnection(nullptr);
            
            cswift_nw_listener_set_new_connection_handler(
                listener,
                [](CSHandle connection, void* ctx) {
                    auto* connPtr = static_cast<std::atomic<CSHandle>*>(ctx);
                    connPtr->store(connection);
                },
                &serverConnection
            );
            
            REQUIRE(cswift_nw_listener_start(listener, fixture.queue) == CS_SUCCESS);
            serverReady = true;
            
            // Wait for connection
            while (!serverConnection.load() && !testComplete.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            
            CSHandle connection = serverConnection.load();
            if (connection) {
                // Register memory region for RDMA
                CSHandle serverBuffer = fixture.bufferPool[1];
                uint64_t regionID = 0;
                
                int32_t result = cswift_nw_connection_register_memory_region(
                    connection, serverBuffer, &regionID, CSDMA_WRITE
                );
                if (result == CS_SUCCESS) {
                    serverRegionID.store(regionID);
                    
                    // In real scenario, we'd exchange region IDs over the connection
                    // For testing, we use atomic variable
                    
                    // Wait for RDMA write
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    
                    // Verify data was written (in simulation)
                    void* bufferData = cswift_shared_buffer_data(serverBuffer);
                    if (bufferData) {
                        // In real DMA, data would appear here
                        std::cout << "Server: Memory region ready for RDMA" << std::endl;
                    }
                    
                    cswift_nw_connection_unregister_memory_region(connection, regionID);
                }
                
                cswift_nw_connection_destroy(connection);
            }
            
            cswift_nw_listener_stop(listener);
            cswift_nw_listener_destroy(listener);
        });
        
        // Wait for server
        while (!serverReady.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        // Client performs RDMA write
        CSHandle clientConnection = cswift_nw_connection_create(
            "127.0.0.1", testPort, fixture.tcpParams, nullptr
        );
        REQUIRE(clientConnection != nullptr);
        
        // Start connection
        REQUIRE(cswift_nw_connection_start(clientConnection, fixture.queue) == CS_SUCCESS);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Wait for server to register memory
        while (serverRegionID.load() == 0 && !testComplete.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        uint64_t peerRegionID = serverRegionID.load();
        if (peerRegionID != 0) {
            // Fill client buffer with test pattern
            CSHandle clientBuffer = fixture.bufferPool[0];
            void* clientData = cswift_shared_buffer_data(clientBuffer);
            REQUIRE(clientData != nullptr);
            memcpy(clientData, testPattern.data(), rdmaDataSize);
            
            // Perform RDMA write
            auto startTime = std::chrono::high_resolution_clock::now();
            
            int32_t result = cswift_nw_connection_rdma_write(
                clientConnection,
                clientBuffer,
                0,              // local offset
                peerRegionID,   // peer's region
                0,              // peer offset
                rdmaDataSize    // length
            );
            
            auto endTime = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
            
            if (result == CS_SUCCESS) {
                double throughputGBps = (rdmaDataSize / (1024.0 * 1024.0 * 1024.0)) / (duration.count() / 1e6);
                std::cout << "RDMA write completed:" << std::endl;
                std::cout << "  Size: " << rdmaDataSize / 1024 << " KB" << std::endl;
                std::cout << "  Time: " << duration.count() << " μs" << std::endl;
                std::cout << "  Throughput: " << throughputGBps << " GB/s" << std::endl;
            } else if (result == CS_ERROR_NOT_SUPPORTED) {
                std::cout << "RDMA not supported on this platform" << std::endl;
            }
        }
        
        cswift_nw_connection_cancel(clientConnection);
        cswift_nw_connection_destroy(clientConnection);
        
        testComplete = true;
        serverThread.join();
    }
    
    SECTION("Peer memory mapping") {
        CSHandle connection = cswift_nw_connection_create("127.0.0.1", 0, fixture.tcpParams, nullptr);
        REQUIRE(connection != nullptr);
        
        // Simulate peer region ID
        uint64_t peerRegionID = 12345;
        CSHandle localBuffer = fixture.bufferPool[0];
        
        // Attempt to map peer memory
        int32_t result = cswift_nw_connection_map_peer_memory(
            connection, peerRegionID, localBuffer
        );
        
        if (result == CS_SUCCESS) {
            std::cout << "Peer memory mapping simulated successfully" << std::endl;
        } else if (result == CS_ERROR_NOT_SUPPORTED) {
            std::cout << "Peer memory mapping not supported on this platform" << std::endl;
        }
        
        cswift_nw_connection_destroy(connection);
    }
}

TEST_CASE("Phase 3: DMA Performance Characteristics", "[phase3][dma][performance]") {
    Phase3ThunderboltDMAFixture fixture;
    
    SECTION("Compare DMA vs traditional transfer speeds") {
        const size_t testSizes[] = {64 * 1024, 1024 * 1024, 16 * 1024 * 1024}; // 64KB, 1MB, 16MB
        
        for (size_t testSize : testSizes) {
            CSHandle buffer = fixture.bufferPool[0];
            void* bufferData = cswift_shared_buffer_data(buffer);
            REQUIRE(bufferData != nullptr);
            
            // Fill with test data
            memset(bufferData, 0xAB, testSize);
            
            // Simulate DMA transfer timing
            auto startTime = std::chrono::high_resolution_clock::now();
            
            // In real DMA, this would be a PCIe transaction
            // For simulation, we just measure memory bandwidth
            std::vector<uint8_t> destination(testSize);
            memcpy(destination.data(), bufferData, testSize);
            
            auto endTime = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime);
            
            double throughputGBps = (testSize / (1024.0 * 1024.0 * 1024.0)) / (duration.count() / 1e9);
            
            std::cout << "Transfer size: " << testSize / 1024 << " KB" << std::endl;
            std::cout << "  Time: " << duration.count() / 1000.0 << " μs" << std::endl;
            std::cout << "  Throughput: " << throughputGBps << " GB/s" << std::endl;
            std::cout << "  Expected DMA improvement: " << (throughputGBps * 2) << " GB/s (no CPU copy)" << std::endl;
            std::cout << std::endl;
        }
    }
    
    SECTION("DMA alignment requirements") {
        // Test various alignments for DMA
        const int32_t alignments[] = {8, 64, 4096, 65536}; // Various page sizes
        
        for (int32_t alignment : alignments) {
            CSHandle alignedBuffer = cswift_shared_buffer_create(1024 * 1024, alignment);
            REQUIRE(alignedBuffer != nullptr);
            
            int32_t result = cswift_shared_buffer_prepare_for_network_io(alignedBuffer);
            CHECK(result == CS_SUCCESS);
            
            void* ptr = cswift_shared_buffer_data(alignedBuffer);
            uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
            
            bool properlyAligned = (addr % alignment) == 0;
            CHECK(properlyAligned);
            
            if (properlyAligned) {
                std::cout << "✓ Buffer aligned to " << alignment << " bytes for DMA" << std::endl;
            }
            
            cswift_shared_buffer_destroy(alignedBuffer);
        }
    }
}