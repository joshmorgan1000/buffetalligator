#include <cswift/cswift.hpp>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

using namespace cswift;

/**
 * @brief Example demonstrating in-memory transport using EmbeddedChannel
 * 
 * @details This example shows how to use EmbeddedChannel to create an 
 * efficient in-memory communication channel between two components
 * without the overhead of actual network operations.
 */

class Component {
private:
    std::string name;
    CSNIOEmbeddedChannel& channel;
    
public:
    Component(const std::string& name, CSNIOEmbeddedChannel& channel) 
        : name(name), channel(channel) {}
    
    void sendMessage(const std::string& message) {
        std::string fullMessage = name + ": " + message;
        std::cout << "Sending: " << fullMessage << std::endl;
        
        // Create a ByteBuffer with the message
        CSNIOByteBuffer buffer(static_cast<int32_t>(fullMessage.size() + 16));
        buffer.writeBytes(fullMessage.data(), fullMessage.size());
        
        // Write to inbound channel
        channel.writeInbound(buffer);
    }
    
    void processIncoming() {
        // Read from outbound channel
        auto buffer = channel.readOutbound();
        if (buffer) {
            // Read the message from the buffer
            std::vector<uint8_t> data(buffer->readableBytes());
            buffer->readBytes(data.data(), data.size());
            std::string message(data.begin(), data.end());
            std::cout << name << " received: " << message << std::endl;
        }
    }
};

int main() {
    std::cout << "In-Memory Transport Example\n" << std::endl;
    
    try {
        // Create two embedded channels for in-memory transport
        CSNIOEmbeddedChannel channelA;
        CSNIOEmbeddedChannel channelB;
        
        // Create two components that will communicate
        Component componentA("ComponentA", channelA);
        Component componentB("ComponentB", channelB);
        
        // Example 1: Simple message exchange
        std::cout << "=== Example 1: Simple Message Exchange ===" << std::endl;
        {
            // Component A sends a message
            componentA.sendMessage("Hello from A!");
            
            // Transfer data from A's inbound to B's outbound (simulating network)
            auto dataFromA = channelA.readInbound();
            if (dataFromA) {
                channelB.writeOutbound(*dataFromA);
            }
            
            // Component B receives the message
            componentB.processIncoming();
            
            // Component B sends a reply
            componentB.sendMessage("Hello back from B!");
            
            // Transfer data from B's inbound to A's outbound
            auto dataFromB = channelB.readInbound();
            if (dataFromB) {
                channelA.writeOutbound(*dataFromB);
            }
            
            // Component A receives the reply
            componentA.processIncoming();
        }
        
        // Example 2: Bulk data transfer
        std::cout << "\n=== Example 2: Bulk Data Transfer ===" << std::endl;
        {
            // Create a large message
            std::string largeData(1024, 'X');
            largeData = "Large data block: " + largeData;
            
            CSNIOByteBuffer largeBuffer(static_cast<int32_t>(largeData.size() + 16));
            largeBuffer.writeBytes(largeData.data(), largeData.size());
            
            std::cout << "Sending " << largeData.size() << " bytes..." << std::endl;
            channelA.writeInbound(largeBuffer);
            
            // Transfer
            auto bulkData = channelA.readInbound();
            if (bulkData) {
                std::cout << "Transferring " << bulkData->readableBytes() << " bytes..." << std::endl;
                channelB.writeOutbound(*bulkData);
            }
            
            // Receive
            auto received = channelB.readOutbound();
            if (received) {
                std::cout << "Received " << received->readableBytes() << " bytes" << std::endl;
            }
        }
        
        // Example 3: Multiple message queue
        std::cout << "\n=== Example 3: Message Queue ===" << std::endl;
        {
            // Send multiple messages
            for (int i = 0; i < 5; ++i) {
                std::string msg = "Message " + std::to_string(i);
                CSNIOByteBuffer buffer(64);
                buffer.writeBytes(msg.data(), msg.size());
                channelA.writeInbound(buffer);
            }
            
            // Process all messages
            int count = 0;
            while (auto msg = channelA.readInbound()) {
                std::cout << "Processing message " << count++ << " with " 
                          << msg->readableBytes() << " bytes" << std::endl;
            }
        }
        
        // Example 4: Bidirectional streaming
        std::cout << "\n=== Example 4: Bidirectional Streaming ===" << std::endl;
        {
            auto sendAndReceive = [](CSNIOEmbeddedChannel& sender, 
                                    CSNIOEmbeddedChannel& receiver,
                                    const std::string& message) {
                // Send
                CSNIOByteBuffer buffer(256);
                buffer.writeBytes(message.data(), message.size());
                sender.writeInbound(buffer);
                
                // Transfer
                if (auto data = sender.readInbound()) {
                    receiver.writeOutbound(*data);
                }
                
                // Receive
                if (auto received = receiver.readOutbound()) {
                    std::vector<uint8_t> recvData(received->readableBytes());
                    received->readBytes(recvData.data(), recvData.size());
                    std::string recvMsg(recvData.begin(), recvData.end());
                    std::cout << "Transferred: " << recvMsg << std::endl;
                }
            };
            
            // Simulate conversation
            sendAndReceive(channelA, channelB, "A: How are you?");
            sendAndReceive(channelB, channelA, "B: I'm good, thanks!");
            sendAndReceive(channelA, channelB, "A: Great to hear!");
        }
        
        // Example 5: Zero-copy optimization demonstration
        std::cout << "\n=== Example 5: Zero-Copy Transfer ===" << std::endl;
        {
            // Create a buffer with some data
            CSNIOByteBuffer sourceBuffer(1024);
            std::string testData = "Zero-copy transfer demonstration";
            sourceBuffer.writeBytes(testData.data(), testData.size());
            
            std::cout << "Original buffer: " << sourceBuffer.readableBytes() << " bytes" << std::endl;
            
            // The embedded channel can pass buffers without copying
            channelA.writeInbound(sourceBuffer);
            
            // Read it back - this should be efficient
            if (auto transferred = channelA.readInbound()) {
                std::cout << "Transferred buffer: " << transferred->readableBytes() << " bytes" << std::endl;
                
                // Verify content
                std::vector<uint8_t> verification(transferred->readableBytes());
                transferred->readBytes(verification.data(), verification.size());
                std::string verified(verification.begin(), verification.end());
                std::cout << "Content verified: " << verified << std::endl;
            }
        }
        
        // Finish channels
        channelA.finish();
        channelB.finish();
        
        std::cout << "\nIn-memory transport example completed successfully!" << std::endl;
        
    } catch (const CSException& e) {
        std::cerr << "EmbeddedChannel error: " << e.what() << " (code: " << e.code() << ")" << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}