/**
 * @file network_server_example.cpp
 * @brief Example demonstrating Network.framework server functionality
 *
 * This example shows how to use the cswift library to create a high-performance
 * network server using Apple's Network.framework through C++ bindings.
 * 
 * Features demonstrated:
 * - Creating a TCP server with NWListener
 * - Handling incoming connections
 * - Asynchronous send/receive operations
 * - Proper resource management with RAII
 */

#include <cswift/cswift.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

using namespace cswift;

/**
 * @brief Example echo server using Network.framework
 */
class EchoServer {
private:
    std::unique_ptr<CSNWListener> listener;
    std::shared_ptr<CSDispatchQueue> queue;
    bool running = false;
    
public:
    /**
     * @brief Create echo server on specified port
     * 
     * @param port Port to listen on
     */
    explicit EchoServer(uint16_t port) {
        try {
            // Create TCP parameters for the server
            auto parameters = CSNWParameters::tcp();
            
            // Create listener
            listener = std::make_unique<CSNWListener>(port, *parameters);
            
            // Create dedicated queue for networking
            queue = std::make_shared<CSDispatchQueue>("com.cswift.echo-server", true);
            
            std::cout << "✅ Echo server created on port " << port << std::endl;
            
        } catch (const CSException& e) {
            std::cerr << "❌ Failed to create server: " << e.what() << std::endl;
            throw;
        }
    }
    
    /**
     * @brief Start the server
     */
    void start() {
        if (running) {
            std::cout << "⚠️  Server already running" << std::endl;
            return;
        }
        
        // Set up new connection handler
        listener->setNewConnectionHandler([this](std::unique_ptr<CSNWConnection> connection) {
            std::cout << "🔗 New client connected" << std::endl;
            handleClient(std::move(connection));
        });
        
        // Set up state handler  
        listener->setStateUpdateHandler([](CSNWConnectionState state) {
            switch (state) {
            case CSNWConnectionState::Ready:
                std::cout << "🟢 Server ready and listening" << std::endl;
                break;
            case CSNWConnectionState::Failed:
                std::cerr << "🔴 Server failed" << std::endl;
                break;
            case CSNWConnectionState::Cancelled:
                std::cout << "🟡 Server cancelled" << std::endl;
                break;
            default:
                std::cout << "🔵 Server state: " << static_cast<int>(state) << std::endl;
                break;
            }
        });
        
        // Start listening
        listener->start(*queue);
        running = true;
        
        std::cout << "🚀 Echo server started" << std::endl;
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
        
        std::cout << "🛑 Echo server stopped" << std::endl;
    }
    
    /**
     * @brief Check if server is running
     */
    bool isRunning() const {
        return running;
    }
    
private:
    /**
     * @brief Handle a client connection
     * 
     * @param connection Client connection
     */
    void handleClient(std::unique_ptr<CSNWConnection> connection) {
        // Keep connection alive by moving to heap
        auto* clientConnection = connection.release();
        
        // Set up connection state handler
        clientConnection->setStateUpdateHandler([clientConnection](CSNWConnectionState state) {
            switch (state) {
            case CSNWConnectionState::Ready:
                std::cout << "📱 Client connection ready" << std::endl;
                break;
            case CSNWConnectionState::Failed:
                std::cout << "📱❌ Client connection failed" << std::endl;
                delete clientConnection;
                break;
            case CSNWConnectionState::Cancelled:
                std::cout << "📱🟡 Client connection cancelled" << std::endl;
                delete clientConnection;
                break;
            default:
                break;
            }
        });
        
        // Start the connection
        clientConnection->start(*queue);
        
        // Start receiving data
        receiveFromClient(clientConnection);
    }
    
    /**
     * @brief Receive data from client and echo it back
     * 
     * @param connection Client connection
     */
    void receiveFromClient(CSNWConnection* connection) {
        connection->receive(1, 8192, [this, connection](
            std::vector<uint8_t> data, bool isComplete, bool success) {
            
            if (!success) {
                std::cout << "📱❌ Receive failed" << std::endl;
                delete connection;
                return;
            }
            
            if (!data.empty()) {
                std::string message(data.begin(), data.end());
                std::cout << "📱📥 Received: " << message << std::endl;
                
                // Echo the message back
                std::string echo = "Echo: " + message;
                std::vector<uint8_t> echoData(echo.begin(), echo.end());
                
                connection->send(echoData, [connection](bool sendSuccess) {
                    if (sendSuccess) {
                        std::cout << "📱📤 Echo sent successfully" << std::endl;
                    } else {
                        std::cout << "📱❌ Echo send failed" << std::endl;
                        delete connection;
                    }
                });
            }
            
            if (isComplete) {
                std::cout << "📱👋 Client disconnected" << std::endl;
                delete connection;
            } else {
                // Continue receiving
                receiveFromClient(connection);
            }
        });
    }
};

/**
 * @brief Example client for testing the server
 */
class EchoClient {
private:
    std::unique_ptr<CSNWConnection> connection;
    std::shared_ptr<CSDispatchQueue> queue;
    
public:
    /**
     * @brief Create client connected to server
     * 
     * @param host Server hostname
     * @param port Server port
     */
    EchoClient(const std::string& host, uint16_t port) {
        try {
            // Create client connection
            connection = std::make_unique<CSNWConnection>(host, port);
            
            // Create client queue
            queue = std::make_shared<CSDispatchQueue>("com.cswift.echo-client");
            
            std::cout << "📲 Client created for " << host << ":" << port << std::endl;
            
        } catch (const CSException& e) {
            std::cerr << "❌ Failed to create client: " << e.what() << std::endl;
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
                std::cout << "📲🟢 Connected to server" << std::endl;
                break;
            case CSNWConnectionState::Failed:
                std::cout << "📲🔴 Connection failed" << std::endl;
                break;
            case CSNWConnectionState::Cancelled:
                std::cout << "📲🟡 Connection cancelled" << std::endl;
                break;
            default:
                std::cout << "📲🔵 Connection state: " << static_cast<int>(state) << std::endl;
                break;
            }
        });
        
        // Start connection
        connection->start(*queue);
    }
    
    /**
     * @brief Send message to server
     * 
     * @param message Message to send
     */
    void sendMessage(const std::string& message) {
        std::vector<uint8_t> data(message.begin(), message.end());
        
        connection->send(data, [message](bool success) {
            if (success) {
                std::cout << "📲📤 Sent: " << message << std::endl;
            } else {
                std::cout << "📲❌ Send failed: " << message << std::endl;
            }
        });
    }
    
    /**
     * @brief Receive response from server
     */
    void receiveResponse() {
        connection->receive(1, 8192, [this](
            std::vector<uint8_t> data, bool isComplete, bool success) {
            
            if (success && !data.empty()) {
                std::string response(data.begin(), data.end());
                std::cout << "📲📥 Received: " << response << std::endl;
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
        std::cout << "📲👋 Disconnected from server" << std::endl;
    }
};

/**
 * @brief Main example function
 */
int main() {
    std::cout << "🌐 Network.framework Server Example" << std::endl;
    std::cout << "====================================" << std::endl;
    
#ifdef __APPLE__
    try {
        const uint16_t port = 8080;
        
        // Create and start server
        EchoServer server(port);
        server.start();
        
        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // Create client and connect
        EchoClient client("localhost", port);
        client.connect();
        
        // Give client time to connect
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // Send test messages
        client.receiveResponse();  // Start receiving
        
        client.sendMessage("Hello, Server!");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        client.sendMessage("How are you?");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        client.sendMessage("This is a test message with more content to verify the echo functionality.");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Let messages process
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        // Clean shutdown
        client.disconnect();
        server.stop();
        
        std::cout << "✨ Example completed successfully" << std::endl;
        
    } catch (const CSException& e) {
        std::cerr << "💥 Example failed: " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "💥 Unexpected error: " << e.what() << std::endl;
        return 1;
    }
#else
    std::cout << "⚠️  This example requires Apple platforms with Network.framework" << std::endl;
    std::cout << "📝 On other platforms, the cswift library provides stub implementations" << std::endl;
    std::cout << "   for compatibility but without actual networking functionality." << std::endl;
#endif
    
    return 0;
}