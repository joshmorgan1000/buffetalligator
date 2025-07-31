#include <cswift/cswift.hpp>
#include <iostream>
#include <thread>
#include <chrono>

using namespace cswift;

/**
 * @brief Example demonstrating Network.framework-based networking
 * 
 * @details This example shows how to use NIOTSConnectionChannel to create
 * high-performance network connections using Apple's Network.framework.
 * 
 * @note This example requires macOS/iOS with Network.framework
 */

int main() {
    std::cout << "Network Client Example\n" << std::endl;
    
    try {
        // Example 1: Basic TCP connection
        std::cout << "=== Example 1: Basic TCP Connection ===" << std::endl;
        {
            CSNIOTSConnectionChannel channel("example.com", 80, false);
            
            std::cout << "Connecting to example.com:80..." << std::endl;
            channel.connect();
            
            // Check if connected
            if (channel.isActive()) {
                std::cout << "Successfully connected!" << std::endl;
                
                // Send HTTP request
                std::string request = "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n";
                size_t written = channel.write(request);
                std::cout << "Sent HTTP request (" << written << " bytes)" << std::endl;
                
                // Read response
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                std::string response = channel.read(1024);
                
                std::cout << "\nReceived response (" << response.size() << " bytes):" << std::endl;
                std::cout << response.substr(0, 200) << "..." << std::endl;
                
                // Close connection
                channel.close();
                std::cout << "Connection closed" << std::endl;
            } else {
                std::cout << "Failed to connect" << std::endl;
            }
        }
        
        // Example 2: Secure TLS connection
        std::cout << "\n=== Example 2: Secure TLS Connection ===" << std::endl;
        {
            CSNIOTSConnectionChannel secureChannel("example.com", 443, true);
            
            std::cout << "Connecting to example.com:443 with TLS..." << std::endl;
            secureChannel.connect();
            
            if (secureChannel.isActive()) {
                std::cout << "Secure connection established!" << std::endl;
                
                // Send HTTPS request
                std::string request = "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n";
                size_t written = secureChannel.write(request);
                std::cout << "Sent HTTPS request (" << written << " bytes)" << std::endl;
                
                // Read response
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                std::string response = secureChannel.read(1024);
                
                std::cout << "\nReceived secure response (" << response.size() << " bytes):" << std::endl;
                std::cout << response.substr(0, 200) << "..." << std::endl;
                
                secureChannel.close();
                std::cout << "Secure connection closed" << std::endl;
            } else {
                std::cout << "Failed to establish secure connection" << std::endl;
            }
        }
        
        // Example 3: Using ByteBuffer for efficient data handling
        std::cout << "\n=== Example 3: Using ByteBuffer with Network Channel ===" << std::endl;
        {
            CSNIOByteBuffer buffer(4096);
            CSNIOTSConnectionChannel channel("httpbin.org", 80, false);
            
            std::cout << "Connecting to httpbin.org:80..." << std::endl;
            channel.connect();
            
            if (channel.isActive()) {
                // Prepare request in ByteBuffer
                std::string request = "GET /get HTTP/1.1\r\nHost: httpbin.org\r\nConnection: close\r\n\r\n";
                buffer.writeBytes(request.data(), request.size());
                
                // Send from buffer
                std::vector<uint8_t> sendData(buffer.readableBytes());
                buffer.readBytes(sendData.data(), sendData.size());
                size_t written = channel.write(sendData);
                std::cout << "Sent request (" << written << " bytes)" << std::endl;
                
                // Read response into buffer
                buffer.clear();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                
                uint8_t tempBuffer[1024];
                size_t bytesRead = channel.read(tempBuffer, sizeof(tempBuffer));
                buffer.writeBytes(tempBuffer, bytesRead);
                
                std::cout << "Received " << bytesRead << " bytes into ByteBuffer" << std::endl;
                std::cout << "Buffer readable bytes: " << buffer.readableBytes() << std::endl;
                
                // Read response from buffer
                std::vector<uint8_t> responseData(buffer.readableBytes());
                buffer.readBytes(responseData.data(), responseData.size());
                std::string response(responseData.begin(), responseData.end());
                
                std::cout << "\nResponse preview:" << std::endl;
                std::cout << response.substr(0, 200) << "..." << std::endl;
                
                channel.close();
            }
        }
        
        // Example 4: Multiple connections
        std::cout << "\n=== Example 4: Multiple Connections ===" << std::endl;
        {
            std::vector<std::unique_ptr<CSNIOTSConnectionChannel>> channels;
            
            // Create multiple connections
            std::vector<std::pair<std::string, uint16_t>> endpoints = {
                {"example.com", 80},
                {"httpbin.org", 80},
                {"google.com", 80}
            };
            
            for (const auto& [host, port] : endpoints) {
                auto channel = std::make_unique<CSNIOTSConnectionChannel>(host, port, false);
                std::cout << "Connecting to " << host << ":" << port << "..." << std::endl;
                
                try {
                    channel->connect();
                    if (channel->isActive()) {
                        std::cout << "  ✓ Connected to " << host << std::endl;
                        channels.push_back(std::move(channel));
                    }
                } catch (const CSException& e) {
                    std::cout << "  ✗ Failed to connect to " << host << ": " << e.what() << std::endl;
                }
            }
            
            std::cout << "\nSuccessfully connected to " << channels.size() << " endpoints" << std::endl;
            
            // Close all connections
            for (auto& channel : channels) {
                channel->close();
            }
            std::cout << "All connections closed" << std::endl;
        }
        
        std::cout << "\nNetwork client example completed successfully!" << std::endl;
        
    } catch (const CSException& e) {
        std::cerr << "Network error: " << e.what() << " (code: " << e.code() << ")" << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}