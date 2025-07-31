/**
 * @file 03_network_buffers.cpp
 * @brief Example demonstrating network buffer allocation with automatic selection
 * 
 * This example shows how to:
 * - Use generic TCP, UDP, QUIC types with automatic implementation selection
 * - Explicitly request ASIO or Foundation implementations
 * - Set up client/server communication
 * - Perform zero-copy network operations
 */

#include <alligator.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>

using namespace alligator;

void auto_network_buffer_example() {
    std::cout << "\n=== Automatic Network Buffer Selection ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    constexpr size_t buffer_size = 64 * 1024; // 64KB
    
    // TCP Buffer - auto selects Foundation on macOS, ASIO elsewhere
    std::cout << "\nAllocating TCP buffer (auto-select):" << std::endl;
    try {
        auto* tcp_buffer = alligator.allocate_buffer(buffer_size, BFType::TCP);
        if (tcp_buffer) {
            std::cout << "✓ TCP buffer allocated successfully" << std::endl;
            std::cout << "  Buffer ID: " << tcp_buffer->id_.load() << std::endl;
            std::cout << "  Buffer size: " << tcp_buffer->buf_size_ << " bytes" << std::endl;
            #ifdef __APPLE__
                std::cout << "  Implementation: Foundation Network.framework (macOS)" << std::endl;
            #else
                std::cout << "  Implementation: ASIO" << std::endl;
            #endif
        }
    } catch (const std::runtime_error& e) {
        std::cerr << "Failed to allocate TCP buffer: " << e.what() << std::endl;
    }
    
    // UDP Buffer - auto selects Foundation on macOS, ASIO elsewhere
    std::cout << "\nAllocating UDP buffer (auto-select):" << std::endl;
    try {
        auto* udp_buffer = alligator.allocate_buffer(buffer_size, BFType::UDP);
        if (udp_buffer) {
            std::cout << "✓ UDP buffer allocated successfully" << std::endl;
            std::cout << "  Buffer ID: " << udp_buffer->id_.load() << std::endl;
            #ifdef __APPLE__
                std::cout << "  Implementation: Foundation Network.framework (macOS)" << std::endl;
            #else
                std::cout << "  Implementation: ASIO" << std::endl;
            #endif
        }
    } catch (const std::runtime_error& e) {
        std::cerr << "Failed to allocate UDP buffer: " << e.what() << std::endl;
    }
    
    // QUIC Buffer - auto selects Foundation on macOS, ASIO elsewhere
    std::cout << "\nAllocating QUIC buffer (auto-select):" << std::endl;
    try {
        auto* quic_buffer = alligator.allocate_buffer(buffer_size, BFType::QUIC);
        if (quic_buffer) {
            std::cout << "✓ QUIC buffer allocated successfully" << std::endl;
            std::cout << "  Buffer ID: " << quic_buffer->id_.load() << std::endl;
            std::cout << "  Features: Multiplexed streams, built-in encryption, 0-RTT" << std::endl;
            #ifdef __APPLE__
                std::cout << "  Implementation: Foundation Network.framework (macOS)" << std::endl;
            #else
                std::cout << "  Implementation: ASIO with QUIC support" << std::endl;
            #endif
        }
    } catch (const std::runtime_error& e) {
        std::cerr << "Failed to allocate QUIC buffer: " << e.what() << std::endl;
    }
}

void explicit_network_buffer_example() {
    std::cout << "\n=== Explicit Network Buffer Types ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    constexpr size_t buffer_size = 32 * 1024; // 32KB
    
    // Try explicit ASIO implementations
    std::cout << "\nExplicit ASIO buffers:" << std::endl;
    
    try {
        auto* asio_tcp = alligator.allocate_buffer(buffer_size, BFType::ASIO_TCP);
        if (asio_tcp) {
            std::cout << "✓ ASIO TCP buffer allocated" << std::endl;
        }
    } catch (const std::runtime_error& e) {
        std::cout << "✗ ASIO TCP: " << e.what() << std::endl;
    }
    
    try {
        auto* asio_udp = alligator.allocate_buffer(buffer_size, BFType::ASIO_UDP);
        if (asio_udp) {
            std::cout << "✓ ASIO UDP buffer allocated" << std::endl;
        }
    } catch (const std::runtime_error& e) {
        std::cout << "✗ ASIO UDP: " << e.what() << std::endl;
    }
    
    try {
        auto* asio_quic = alligator.allocate_buffer(buffer_size, BFType::ASIO_QUIC);
        if (asio_quic) {
            std::cout << "✓ ASIO QUIC buffer allocated" << std::endl;
        }
    } catch (const std::runtime_error& e) {
        std::cout << "✗ ASIO QUIC: " << e.what() << std::endl;
    }
    
    // Try explicit Foundation implementations
    std::cout << "\nExplicit Foundation buffers:" << std::endl;
    
    try {
        auto* foundation_tcp = alligator.allocate_buffer(buffer_size, BFType::FOUNDATION_TCP);
        if (foundation_tcp) {
            std::cout << "✓ Foundation TCP buffer allocated" << std::endl;
        }
    } catch (const std::runtime_error& e) {
        std::cout << "✗ Foundation TCP: " << e.what() << std::endl;
    }
    
    try {
        auto* foundation_udp = alligator.allocate_buffer(buffer_size, BFType::FOUNDATION_UDP);
        if (foundation_udp) {
            std::cout << "✓ Foundation UDP buffer allocated" << std::endl;
        }
    } catch (const std::runtime_error& e) {
        std::cout << "✗ Foundation UDP: " << e.what() << std::endl;
    }
    
    try {
        auto* foundation_quic = alligator.allocate_buffer(buffer_size, BFType::FOUNDATION_QUIC);
        if (foundation_quic) {
            std::cout << "✓ Foundation QUIC buffer allocated" << std::endl;
        }
    } catch (const std::runtime_error& e) {
        std::cout << "✗ Foundation QUIC: " << e.what() << std::endl;
    }
}

void tcp_echo_server_example() {
    std::cout << "\n=== TCP Echo Server Example ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    constexpr size_t buffer_size = 4096;
    
    try {
        // Create TCP buffer for server
        auto* server_buffer = dynamic_cast<NICBuffer*>(
            alligator.allocate_buffer(buffer_size, BFType::TCP)
        );
        
        if (!server_buffer) {
            std::cout << "Failed to create TCP server buffer" << std::endl;
            return;
        }
        
        std::cout << "TCP Echo Server setup:" << std::endl;
        std::cout << "1. Would bind to port 8080" << std::endl;
        std::cout << "2. Accept incoming connections" << std::endl;
        std::cout << "3. Echo received data back to client" << std::endl;
        
        // Example of how the server would work:
        NetworkEndpoint endpoint{"0.0.0.0", 8080};
        
        // server_buffer->bind(endpoint);
        std::cout << "Server would bind to " << endpoint.address << ":" << endpoint.port << std::endl;
        
        // Write some test data
        const char* welcome = "Welcome to BuffetAlligator Echo Server!\n";
        std::memcpy(server_buffer->data(), welcome, strlen(welcome));
        
        std::cout << "Server buffer contains: " << welcome;
        
        // In a real implementation:
        // while (running) {
        //     auto claim = server_buffer->get_rx();
        //     if (claim.size > 0) {
        //         server_buffer->send(claim.offset, claim.size);
        //     }
        // }
        
    } catch (const std::exception& e) {
        std::cerr << "TCP server example failed: " << e.what() << std::endl;
    }
}

void udp_broadcast_example() {
    std::cout << "\n=== UDP Broadcast Example ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    constexpr size_t buffer_size = 1500; // Standard MTU
    
    try {
        // Create UDP buffer
        auto* udp_buffer = dynamic_cast<NICBuffer*>(
            alligator.allocate_buffer(buffer_size, BFType::UDP)
        );
        
        if (!udp_buffer) {
            std::cout << "Failed to create UDP buffer" << std::endl;
            return;
        }
        
        std::cout << "UDP Broadcast setup:" << std::endl;
        std::cout << "- No connection establishment needed" << std::endl;
        std::cout << "- Can send to multiple receivers" << std::endl;
        std::cout << "- Lower latency than TCP" << std::endl;
        std::cout << "- No delivery guarantees" << std::endl;
        
        // Example broadcast message
        struct BroadcastMessage {
            uint32_t sequence;
            uint64_t timestamp;
            char payload[256];
        };
        
        auto* msg = udp_buffer->get<BroadcastMessage>(0);
        msg->sequence = 1;
        msg->timestamp = std::chrono::system_clock::now().time_since_epoch().count();
        std::strcpy(msg->payload, "BuffetAlligator UDP broadcast message");
        
        std::cout << "\nBroadcast message prepared:" << std::endl;
        std::cout << "- Sequence: " << msg->sequence << std::endl;
        std::cout << "- Payload: " << msg->payload << std::endl;
        
        // Would broadcast to network:
        // NetworkEndpoint broadcast{"255.255.255.255", 9999};
        // udp_buffer->send(0, sizeof(BroadcastMessage));
        
    } catch (const std::exception& e) {
        std::cerr << "UDP broadcast example failed: " << e.what() << std::endl;
    }
}

void quic_streaming_example() {
    std::cout << "\n=== QUIC Streaming Example ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    constexpr size_t buffer_size = 16 * 1024; // 16KB
    
    try {
        // Create QUIC buffer
        auto* quic_buffer = dynamic_cast<NICBuffer*>(
            alligator.allocate_buffer(buffer_size, BFType::QUIC)
        );
        
        if (!quic_buffer) {
            std::cout << "Failed to create QUIC buffer" << std::endl;
            return;
        }
        
        std::cout << "QUIC Features:" << std::endl;
        std::cout << "✓ Built-in encryption (TLS 1.3)" << std::endl;
        std::cout << "✓ Multiplexed streams" << std::endl;
        std::cout << "✓ Connection migration" << std::endl;
        std::cout << "✓ 0-RTT connection establishment" << std::endl;
        std::cout << "✓ Better performance on lossy networks" << std::endl;
        
        // QUIC is ideal for:
        std::cout << "\nUse cases:" << std::endl;
        std::cout << "- Video streaming" << std::endl;
        std::cout << "- Real-time gaming" << std::endl;
        std::cout << "- Mobile applications" << std::endl;
        std::cout << "- HTTP/3 web traffic" << std::endl;
        
        // Example: Streaming video chunks
        struct VideoChunk {
            uint32_t stream_id;
            uint32_t chunk_id;
            uint32_t flags;
            uint8_t data[4096];
        };
        
        auto* chunk = quic_buffer->get<VideoChunk>(0);
        chunk->stream_id = 1;
        chunk->chunk_id = 42;
        chunk->flags = 0x01; // Key frame
        
        std::cout << "\nVideo streaming setup:" << std::endl;
        std::cout << "- Stream ID: " << chunk->stream_id << std::endl;
        std::cout << "- Chunk ID: " << chunk->chunk_id << std::endl;
        std::cout << "- Multiple streams can be multiplexed" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "QUIC streaming example failed: " << e.what() << std::endl;
    }
}

void network_performance_tips() {
    std::cout << "\n=== Network Buffer Performance Tips ===" << std::endl;
    
    std::cout << "\nFoundation vs ASIO:" << std::endl;
    std::cout << "Foundation Network.framework (macOS):" << std::endl;
    std::cout << "  + Native Apple performance optimizations" << std::endl;
    std::cout << "  + Better battery efficiency on Apple devices" << std::endl;
    std::cout << "  + Integrated with system network policies" << std::endl;
    std::cout << "  - macOS/iOS only" << std::endl;
    
    std::cout << "\nASIO:" << std::endl;
    std::cout << "  + Cross-platform (Windows, Linux, macOS)" << std::endl;
    std::cout << "  + Mature, battle-tested library" << std::endl;
    std::cout << "  + Extensive protocol support" << std::endl;
    std::cout << "  - May have higher overhead on Apple platforms" << std::endl;
    
    std::cout << "\nProtocol Selection:" << std::endl;
    std::cout << "TCP: Reliable, ordered delivery - use for:" << std::endl;
    std::cout << "  - File transfers" << std::endl;
    std::cout << "  - Database connections" << std::endl;
    std::cout << "  - HTTP/HTTPS traffic" << std::endl;
    
    std::cout << "\nUDP: Fast, connectionless - use for:" << std::endl;
    std::cout << "  - Real-time gaming" << std::endl;
    std::cout << "  - VoIP" << std::endl;
    std::cout << "  - Live video streaming" << std::endl;
    
    std::cout << "\nQUIC: Modern, encrypted - use for:" << std::endl;
    std::cout << "  - HTTP/3" << std::endl;
    std::cout << "  - Mobile applications" << std::endl;
    std::cout << "  - Any TCP use case needing better performance" << std::endl;
}

int main() {
    std::cout << "BuffetAlligator Network Buffer Examples" << std::endl;
    std::cout << "=======================================" << std::endl;
    
    try {
        auto_network_buffer_example();
        explicit_network_buffer_example();
        tcp_echo_server_example();
        udp_broadcast_example();
        quic_streaming_example();
        network_performance_tips();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "\nNetwork buffer examples completed!" << std::endl;
    return 0;
}