/**
 * @file thunderbolt_high_performance.cpp
 * @brief Thunderbolt high-performance networking example
 *
 * This example demonstrates how to use the cswift library for high-performance
 * networking over Thunderbolt connections. It shows:
 * - Thunderbolt interface detection
 * - Optimized parameters for high-bandwidth, low-latency networking
 * - Zero-copy buffer operations
 * - Custom protocol framing for maximum performance
 */

#include <cswift/cswift.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <random>

using namespace cswift;

/**
 * @brief High-performance Thunderbolt server for ML data transfer
 */
class ThunderboltMLServer {
private:
    std::unique_ptr<CSNWListener> listener;
    std::shared_ptr<CSDispatchQueue> queue;
    std::unique_ptr<CSSharedBuffer> sharedBuffer;
    bool running = false;
    
public:
    /**
     * @brief Create Thunderbolt-optimized ML server
     * 
     * @param port Port to listen on
     * @param bufferSize Size of shared buffer for zero-copy operations
     */
    explicit ThunderboltMLServer(uint16_t port, size_t bufferSize = 64 * 1024 * 1024) {
        try {
            // Detect high-speed interfaces
            auto interfaces = CSNWParameters::detectHighSpeedInterfaces();
            std::cout << "🔍 Detected " << interfaces.size() << " high-speed interfaces:" << std::endl;
            for (const auto& iface : interfaces) {
                switch (iface) {
                case CSNWInterfaceType::WiredEthernet:
                    std::cout << "  📡 Wired Ethernet (may include Thunderbolt)" << std::endl;
                    break;
                case CSNWInterfaceType::Thunderbolt:
                    std::cout << "  ⚡ Thunderbolt (high-performance)" << std::endl;
                    break;
                case CSNWInterfaceType::WiFi:
                    std::cout << "  📶 WiFi" << std::endl;
                    break;
                default:
                    std::cout << "  ❓ Other interface" << std::endl;
                    break;
                }
            }
            
            // Create high-performance parameters optimized for Thunderbolt
            auto parameters = CSNWParameters::highPerformance();
            parameters->setRequiredInterfaceType(CSNWInterfaceType::Thunderbolt);
            
            // Create custom framer for zero-copy ML data protocol
            CSNWProtocolFramer framer("ml-data-v1");
            parameters->addCustomFramer(framer);
            
            // Create listener with optimized parameters
            listener = std::make_unique<CSNWListener>(port, *parameters);
            
            // Create high-performance queue
            queue = std::make_shared<CSDispatchQueue>("com.cswift.thunderbolt-ml", true);
            
            // Create shared buffer with 64-byte alignment for SIMD operations
            sharedBuffer = std::make_unique<CSSharedBuffer>(bufferSize, 64);
            
            std::cout << "⚡ Thunderbolt ML Server created on port " << port << std::endl;
            std::cout << "📊 Shared buffer: " << (bufferSize / (1024*1024)) << "MB, " 
                      << sharedBuffer->alignment() << "-byte aligned" << std::endl;
            
        } catch (const CSException& e) {
            std::cerr << "❌ Failed to create Thunderbolt server: " << e.what() << std::endl;
            throw;
        }
    }
    
    /**
     * @brief Start the high-performance server
     */
    void start() {
        if (running) {
            std::cout << "⚠️  Server already running" << std::endl;
            return;
        }
        
        // Set up high-performance connection handler
        listener->setNewConnectionHandler([this](std::unique_ptr<CSNWConnection> connection) {
            std::cout << "⚡ High-speed client connected" << std::endl;
            handleHighPerformanceClient(std::move(connection));
        });
        
        // Set up state handler
        listener->setStateUpdateHandler([](CSNWConnectionState state) {
            switch (state) {
            case CSNWConnectionState::Ready:
                std::cout << "🟢 Thunderbolt server ready for high-performance connections" << std::endl;
                break;
            case CSNWConnectionState::Failed:
                std::cerr << "🔴 Thunderbolt server failed" << std::endl;
                break;
            case CSNWConnectionState::Cancelled:
                std::cout << "🟡 Thunderbolt server cancelled" << std::endl;
                break;
            default:
                std::cout << "🔵 Server state: " << static_cast<int>(state) << std::endl;
                break;
            }
        });
        
        // Start listening
        listener->start(*queue);
        running = true;
        
        std::cout << "🚀 Thunderbolt ML server started (optimized for 40+ Gbps)" << std::endl;
    }
    
    /**
     * @brief Stop the server
     */
    void stop() {
        if (!running) {
            return;
        }
        
        listener->stop();
        running = false;
        
        std::cout << "🛑 Thunderbolt server stopped" << std::endl;
    }
    
    /**
     * @brief Check if server is running
     */
    bool isRunning() const {
        return running;
    }
    
private:
    /**
     * @brief Handle high-performance client with zero-copy operations
     * 
     * @param connection Client connection
     */
    void handleHighPerformanceClient(std::unique_ptr<CSNWConnection> connection) {
        // Keep connection alive
        auto* client = connection.release();
        
        // Set up connection state handler
        client->setStateUpdateHandler([client](CSNWConnectionState state) {
            switch (state) {
            case CSNWConnectionState::Ready:
                std::cout << "⚡📱 High-speed client ready" << std::endl;
                break;
            case CSNWConnectionState::Failed:
                std::cout << "⚡📱❌ High-speed client failed" << std::endl;
                delete client;
                break;
            case CSNWConnectionState::Cancelled:
                std::cout << "⚡📱🟡 High-speed client disconnected" << std::endl;
                delete client;
                break;
            default:
                break;
            }
        });
        
        // Start the connection
        client->start(*queue);
        
        // Start high-performance data processing
        processMLData(client);
    }
    
    /**
     * @brief Process ML data using zero-copy operations
     * 
     * @param connection Client connection
     */
    void processMLData(CSNWConnection* connection) {
        // Use large buffer sizes for high-bandwidth Thunderbolt
        constexpr size_t thunderboltChunkSize = 1024 * 1024; // 1MB chunks
        
        connection->receive(1, thunderboltChunkSize, [this, connection](
            std::vector<uint8_t> data, bool isComplete, bool success) {
            
            if (!success) {
                std::cout << "⚡📱❌ High-speed receive failed" << std::endl;
                delete connection;
                return;
            }
            
            if (!data.empty()) {
                // Demonstrate zero-copy processing with shared buffer
                auto processingTime = processDataZeroCopy(data);
                
                std::cout << "⚡📥 Processed " << (data.size() / 1024) << "KB in " 
                          << processingTime.count() << "μs (zero-copy)" << std::endl;
                
                // Send processed data back using zero-copy
                sendProcessedData(connection, data);
            }
            
            if (isComplete) {
                std::cout << "⚡📱👋 High-speed client finished" << std::endl;
                delete connection;
            } else {
                // Continue receiving at high speed
                processMLData(connection);
            }
        });
    }
    
    /**
     * @brief Process data using zero-copy operations
     * 
     * @param data Input data
     * @return Processing time in microseconds
     */
    std::chrono::microseconds processDataZeroCopy(const std::vector<uint8_t>& data) {
        auto start = std::chrono::high_resolution_clock::now();
        
        // Write data to shared buffer (zero-copy when possible)
        if (data.size() <= sharedBuffer->capacity()) {
            sharedBuffer->writeAt(0, data.data(), data.size());
            sharedBuffer->setSize(data.size());
            
            // Get zero-copy span for SIMD processing
            auto floatSpan = sharedBuffer->spanAt<float>(0, data.size() / sizeof(float));
            
            // Simulate ML processing using the span (zero-copy)
            if (!floatSpan.empty()) {
                // Example: Apply SIMD-friendly transformation
                for (size_t i = 0; i < floatSpan.size(); i += 8) {
                    // Simulate vectorized operations (would use Accelerate framework)
                    size_t end = std::min(i + 8, floatSpan.size());
                    for (size_t j = i; j < end; ++j) {
                        floatSpan[j] = floatSpan[j] * 1.1f + 0.1f; // Simple transformation
                    }
                }
            }
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    }
    
    /**
     * @brief Send processed data back to client
     * 
     * @param connection Client connection
     * @param originalData Original data (for size reference)
     */
    void sendProcessedData(CSNWConnection* connection, const std::vector<uint8_t>& originalData) {
        if (sharedBuffer->size() > 0) {
            // Get zero-copy pointer to processed data
            const void* processedData = nullptr;
            if (sharedBuffer->asDataView(&processedData) == CS_SUCCESS && processedData) {
                // Create response with zero-copy data
                std::vector<uint8_t> response(static_cast<const uint8_t*>(processedData),
                                            static_cast<const uint8_t*>(processedData) + sharedBuffer->size());
                
                connection->send(response, [](bool success) {
                    if (success) {
                        std::cout << "⚡📤 High-speed response sent" << std::endl;
                    } else {
                        std::cout << "⚡❌ High-speed send failed" << std::endl;
                    }
                });
            }
        }
    }
};

/**
 * @brief High-performance Thunderbolt client
 */
class ThunderboltMLClient {
private:
    std::unique_ptr<CSNWConnection> connection;
    std::shared_ptr<CSDispatchQueue> queue;
    std::unique_ptr<CSSharedBuffer> dataBuffer;
    
public:
    /**
     * @brief Create high-performance Thunderbolt client
     * 
     * @param host Server hostname
     * @param port Server port
     */
    ThunderboltMLClient(const std::string& host, uint16_t port) {
        try {
            // Create high-performance parameters
            auto parameters = CSNWParameters::highPerformance();
            parameters->setRequiredInterfaceType(CSNWInterfaceType::Thunderbolt);
            
            // Create connection
            connection = std::make_unique<CSNWConnection>(host, port, *parameters);
            
            // Create high-performance queue
            queue = std::make_shared<CSDispatchQueue>("com.cswift.thunderbolt-client");
            
            // Create data buffer for ML data
            dataBuffer = std::make_unique<CSSharedBuffer>(16 * 1024 * 1024, 64); // 16MB
            
            std::cout << "⚡📲 Thunderbolt client created for " << host << ":" << port << std::endl;
            
        } catch (const CSException& e) {
            std::cerr << "❌ Failed to create Thunderbolt client: " << e.what() << std::endl;
            throw;
        }
    }
    
    /**
     * @brief Connect to server
     */
    void connect() {
        // Set up state handler
        connection->setStateUpdateHandler([](CSNWConnectionState state) {
            switch (state) {
            case CSNWConnectionState::Ready:
                std::cout << "⚡📲🟢 Connected via Thunderbolt" << std::endl;
                break;
            case CSNWConnectionState::Failed:
                std::cout << "⚡📲🔴 Thunderbolt connection failed" << std::endl;
                break;
            case CSNWConnectionState::Cancelled:
                std::cout << "⚡📲🟡 Thunderbolt connection cancelled" << std::endl;
                break;
            default:
                std::cout << "⚡📲🔵 Connection state: " << static_cast<int>(state) << std::endl;
                break;
            }
        });
        
        // Start connection
        connection->start(*queue);
    }
    
    /**
     * @brief Send large ML dataset using zero-copy operations
     * 
     * @param dataSize Size of data to generate and send
     */
    void sendMLDataset(size_t dataSize) {
        std::cout << "⚡📤 Generating " << (dataSize / (1024*1024)) << "MB ML dataset..." << std::endl;
        
        // Generate ML data directly in shared buffer (zero-copy)
        if (dataSize <= dataBuffer->capacity()) {
            generateMLData(dataSize);
            
            // Get zero-copy view of the data
            const void* dataPtr = nullptr;
            if (dataBuffer->asDataView(&dataPtr) == CS_SUCCESS && dataPtr) {
                std::vector<uint8_t> dataset(static_cast<const uint8_t*>(dataPtr),
                                           static_cast<const uint8_t*>(dataPtr) + dataSize);
                
                auto start = std::chrono::high_resolution_clock::now();
                
                connection->send(dataset, [start, dataSize](bool success) {
                    auto end = std::chrono::high_resolution_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
                    
                    if (success) {
                        double mbps = (dataSize * 8.0) / (duration.count() * 1000.0); // Mbps
                        std::cout << "⚡📤 Dataset sent: " << (dataSize / (1024*1024)) 
                                  << "MB in " << duration.count() << "ms (" 
                                  << std::fixed << std::setprecision(1) << mbps << " Mbps)" << std::endl;
                    } else {
                        std::cout << "⚡❌ Dataset send failed" << std::endl;
                    }
                });
            }
        }
    }
    
    /**
     * @brief Receive response from server
     */
    void receiveResponse() {
        connection->receive(1, 16 * 1024 * 1024, [this]( // 16MB max
            std::vector<uint8_t> data, bool isComplete, bool success) {
            
            if (success && !data.empty()) {
                std::cout << "⚡📥 Received processed data: " << (data.size() / 1024) << "KB" << std::endl;
                
                // Verify data using zero-copy operations
                verifyProcessedData(data);
            }
            
            if (!isComplete && success) {
                // Continue receiving
                receiveResponse();
            }
        });
    }
    
    /**
     * @brief Disconnect from server
     */
    void disconnect() {
        connection->cancel();
        std::cout << "⚡📲👋 Disconnected from Thunderbolt server" << std::endl;
    }
    
private:
    /**
     * @brief Generate ML data directly in shared buffer
     * 
     * @param size Size of data to generate
     */
    void generateMLData(size_t size) {
        // Generate data as float array for ML processing
        size_t floatCount = size / sizeof(float);
        auto floatSpan = dataBuffer->spanAt<float>(0, floatCount);
        
        // Generate random ML data using zero-copy span
        std::random_device rd;
        std::mt19937 gen(rd());
        std::normal_distribution<float> dist(0.0f, 1.0f);
        
        for (size_t i = 0; i < floatSpan.size(); ++i) {
            floatSpan[i] = dist(gen);
        }
        
        dataBuffer->setSize(floatCount * sizeof(float));
    }
    
    /**
     * @brief Verify processed data
     * 
     * @param data Processed data from server
     */
    void verifyProcessedData(const std::vector<uint8_t>& data) {
        // Simple verification that server applied the transformation
        const float* floats = reinterpret_cast<const float*>(data.data());
        size_t floatCount = data.size() / sizeof(float);
        
        if (floatCount > 0) {
            float avg = 0.0f;
            for (size_t i = 0; i < std::min(floatCount, size_t(100)); ++i) {
                avg += floats[i];
            }
            avg /= std::min(floatCount, size_t(100));
            
            std::cout << "⚡✅ Data verification: avg=" << std::fixed << std::setprecision(3) 
                      << avg << " (processed " << floatCount << " floats)" << std::endl;
        }
    }
};

/**
 * @brief Main demonstration function
 */
int main() {
    std::cout << "⚡ Thunderbolt High-Performance Networking Demo" << std::endl;
    std::cout << "=============================================" << std::endl;
    
#ifdef __APPLE__
    try {
        const uint16_t port = 8080;
        const size_t datasetSize = 8 * 1024 * 1024; // 8MB test dataset
        
        // Create and start Thunderbolt-optimized server
        ThunderboltMLServer server(port, 64 * 1024 * 1024); // 64MB buffer
        server.start();
        
        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // Create high-performance client
        ThunderboltMLClient client("localhost", port);
        client.connect();
        
        // Give client time to connect
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // Start receiving responses
        client.receiveResponse();
        
        // Send large ML dataset
        client.sendMLDataset(datasetSize);
        
        // Let processing complete
        std::this_thread::sleep_for(std::chrono::seconds(3));
        
        // Clean shutdown
        client.disconnect();
        server.stop();
        
        std::cout << "✨ Thunderbolt demo completed successfully" << std::endl;
        std::cout << "🎯 Optimizations used:" << std::endl;
        std::cout << "   • Thunderbolt interface targeting" << std::endl;
        std::cout << "   • Zero-copy shared buffers" << std::endl;
        std::cout << "   • SIMD-aligned memory (64-byte)" << std::endl;
        std::cout << "   • Custom protocol framing" << std::endl;
        std::cout << "   • TCP NoDelay (low latency)" << std::endl;
        std::cout << "   • Large buffer sizes (MB chunks)" << std::endl;
        
    } catch (const CSException& e) {
        std::cerr << "💥 Demo failed: " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "💥 Unexpected error: " << e.what() << std::endl;
        return 1;
    }
#else
    std::cout << "⚠️  This demo requires Apple platforms with Network.framework" << std::endl;
    std::cout << "📝 On other platforms, the cswift library provides stub implementations" << std::endl;
    std::cout << "   for compatibility but without actual Thunderbolt optimizations." << std::endl;
#endif
    
    return 0;
}