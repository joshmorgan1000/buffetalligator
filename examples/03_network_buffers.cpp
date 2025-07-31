/**
 * @file 03_network_buffers.cpp
 * @brief Network buffer types demonstration
 * 
 * This example shows network buffer usage with:
 * - Auto-selection between Swift (via cswift) and ASIO
 * - TCP, UDP, and QUIC protocols
 * - Zero-copy networking
 * - Buffer claims for efficient I/O
 * - Network statistics
 */

#include <alligator.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>

using namespace alligator;
using namespace std::chrono_literals;

void print_network_stats(const NICBuffer::NetworkStats& stats) {
    std::cout << "   Network Statistics:" << std::endl;
    std::cout << "     Bytes sent: " << stats.bytes_sent << std::endl;
    std::cout << "     Bytes received: " << stats.bytes_received << std::endl;
    std::cout << "     Packets sent: " << stats.packets_sent << std::endl;
    std::cout << "     Packets received: " << stats.packets_received << std::endl;
    std::cout << "     Errors: " << stats.errors << std::endl;
    std::cout << "     Drops: " << stats.drops << std::endl;
}

void tcp_example(BuffetAlligator& buffet) {
    std::cout << "1. TCP Buffer Example" << std::endl;
    
    // Auto-selects Swift on Apple platforms (via cswift), ASIO otherwise
    auto* server_buf = dynamic_cast<NICBuffer*>(
        buffet.allocate_buffer(4096, BFType::TCP)
    );
    auto* client_buf = dynamic_cast<NICBuffer*>(
        buffet.allocate_buffer(4096, BFType::TCP)
    );
    
    if (!server_buf || !client_buf) {
        std::cout << "   Failed to allocate TCP buffers" << std::endl;
        return;
    }
    
    std::cout << "   Allocated TCP buffers (auto-selected implementation)" << std::endl;
    
    // Server setup
    NetworkEndpoint server_endpoint{"127.0.0.1", 8080, NetworkProtocol::TCP};
    if (server_buf->bind(server_endpoint)) {
        std::cout << "   Server bound to " << server_endpoint.address << ":" << server_endpoint.port << std::endl;
    } else {
        std::cout << "   Failed to bind server" << std::endl;
        return;
    }
    
    // Start server in background thread
    std::thread server_thread([server_buf]() {
        // Wait for data
        auto claim = server_buf->get_rx(0);  // Get any available data
        if (claim.buffer && claim.size > 0) {
            std::cout << "   Server received: " << std::string(
                reinterpret_cast<const char*>(claim.buffer->data() + claim.offset), claim.size
            ) << std::endl;
            
            // Echo back
            const char* response = "Hello from server!";
            std::memcpy(server_buf->data(), response, strlen(response));
            server_buf->send(0, strlen(response));
        }
    });
    
    // Give server time to start
    std::this_thread::sleep_for(100ms);
    
    // Client connection
    if (client_buf->connect(server_endpoint)) {
        std::cout << "   Client connected to server" << std::endl;
        
        // Send data
        const char* message = "Hello from client!";
        std::memcpy(client_buf->data(), message, strlen(message));
        ssize_t sent = client_buf->send(0, strlen(message));
        std::cout << "   Client sent " << sent << " bytes" << std::endl;
        
        // Wait for response
        std::this_thread::sleep_for(100ms);
        auto response_claim = client_buf->get_rx(0);
        if (response_claim.buffer && response_claim.size > 0) {
            std::cout << "   Client received: " << std::string(
                reinterpret_cast<const char*>(response_claim.buffer->data() + response_claim.offset), response_claim.size
            ) << std::endl;
        }
    } else {
        std::cout << "   Client failed to connect" << std::endl;
    }
    
    server_thread.join();
    
    // Print statistics
    print_network_stats(client_buf->get_stats());
}

void udp_example(BuffetAlligator& buffet) {
    std::cout << "\n2. UDP Buffer Example" << std::endl;
    
    // Create UDP buffers
    auto* sender_buf = dynamic_cast<NICBuffer*>(
        buffet.allocate_buffer(2048, BFType::UDP)
    );
    auto* receiver_buf = dynamic_cast<NICBuffer*>(
        buffet.allocate_buffer(2048, BFType::UDP)
    );
    
    if (!sender_buf || !receiver_buf) {
        std::cout << "   Failed to allocate UDP buffers" << std::endl;
        return;
    }
    
    std::cout << "   Allocated UDP buffers" << std::endl;
    
    // Receiver setup
    NetworkEndpoint receiver_endpoint{"127.0.0.1", 8081, NetworkProtocol::UDP};
    if (receiver_buf->bind(receiver_endpoint)) {
        std::cout << "   Receiver bound to " << receiver_endpoint.address << ":" << receiver_endpoint.port << std::endl;
    } else {
        std::cout << "   Failed to bind receiver" << std::endl;
        return;
    }
    
    // Send datagram
    const char* datagram = "UDP datagram message";
    std::memcpy(sender_buf->data(), datagram, strlen(datagram));
    
    // For UDP, we need to "connect" to set the destination
    sender_buf->connect(receiver_endpoint);
    ssize_t sent = sender_buf->send(0, strlen(datagram));
    std::cout << "   Sent UDP datagram: " << sent << " bytes" << std::endl;
    
    // Receive datagram
    std::this_thread::sleep_for(50ms);
    auto claim = receiver_buf->get_rx(0);
    if (claim.buffer && claim.size > 0) {
        std::cout << "   Received: " << std::string(
            reinterpret_cast<const char*>(claim.buffer->data() + claim.offset), claim.size
        ) << std::endl;
    }
    
    // Test multicast (if supported)
    if (auto* udp_receiver = dynamic_cast<SwiftUdpBuffer*>(receiver_buf)) {
        // Swift UDP via cswift might support multicast
        std::cout << "   Using Swift UDP implementation via cswift" << std::endl;
    } else if (auto* udp_receiver = dynamic_cast<AsioUdpBuffer*>(receiver_buf)) {
        // ASIO UDP supports multicast
        NetworkEndpoint multicast_addr{"239.255.0.1", 8082, NetworkProtocol::UDP};
        if (udp_receiver->join_multicast_group(multicast_addr.address)) {
            std::cout << "   Joined multicast group: " << multicast_addr.address << std::endl;
        }
    }
}

void quic_example(BuffetAlligator& buffet) {
    std::cout << "\n3. QUIC Buffer Example" << std::endl;
    
    // QUIC provides multiplexed streams over UDP
    auto* quic_server = dynamic_cast<SwiftQuicBuffer*>(
        buffet.allocate_buffer(8192, BFType::SWIFT_QUIC)
    );
    auto* quic_client = dynamic_cast<SwiftQuicBuffer*>(
        buffet.allocate_buffer(8192, BFType::SWIFT_QUIC)
    );
    
    if (!quic_server || !quic_client) {
        std::cout << "   QUIC buffers not available (requires Swift/cswift)" << std::endl;
        return;
    }
    
    std::cout << "   Allocated QUIC buffers via cswift" << std::endl;
    
    // Server setup
    NetworkEndpoint server_endpoint{"127.0.0.1", 8443, NetworkProtocol::QUIC};
    if (quic_server->bind(server_endpoint)) {
        std::cout << "   QUIC server bound to " << server_endpoint.address << ":" << server_endpoint.port << std::endl;
    }
    
    // Client connection
    if (quic_client->connect(server_endpoint)) {
        std::cout << "   QUIC client connected" << std::endl;
        
        // Create multiple streams
        uint64_t stream1 = quic_client->create_stream(false);  // Bidirectional
        uint64_t stream2 = quic_client->create_stream(true);   // Unidirectional
        
        std::cout << "   Created stream " << stream1 << " (bidirectional)" << std::endl;
        std::cout << "   Created stream " << stream2 << " (unidirectional)" << std::endl;
        
        // Send data on different streams
        const char* msg1 = "Message on stream 1";
        const char* msg2 = "Message on stream 2";
        
        std::memcpy(quic_client->data(), msg1, strlen(msg1));
        quic_client->send_stream(stream1, 0, strlen(msg1));
        
        std::memcpy(quic_client->data() + 100, msg2, strlen(msg2));
        quic_client->send_stream(stream2, 100, strlen(msg2));
        
        std::cout << "   Sent data on multiple streams" << std::endl;
        
        // Get stream info
        auto info = quic_client->get_stream_info(stream1);
        if (info) {
            std::cout << "   Stream " << info->stream_id << " stats:" << std::endl;
            std::cout << "     Bytes sent: " << info->bytes_sent << std::endl;
            std::cout << "     Is open: " << info->is_open << std::endl;
        }
    }
}

void swift_network_example(BuffetAlligator& buffet) {
    std::cout << "\n4. Swift Network Buffer Example (via cswift)" << std::endl;
    
    // Explicitly use Swift TCP buffer
    auto* swift_tcp = dynamic_cast<SwiftTcpBuffer*>(
        buffet.allocate_buffer(4096, BFType::SWIFT_TCP)
    );
    
    if (!swift_tcp) {
        std::cout << "   Swift TCP buffer not available" << std::endl;
        return;
    }
    
    std::cout << "   Allocated Swift TCP buffer using cswift's Network.framework wrappers" << std::endl;
    std::cout << "   This leverages Apple's modern networking stack" << std::endl;
    
    // The Swift buffer uses cswift's CSNWConnection and CSNWListener
    NetworkEndpoint endpoint{"example.com", 80, NetworkProtocol::TCP};
    if (swift_tcp->connect(endpoint)) {
        std::cout << "   Connected to " << endpoint.address << " using Swift networking" << std::endl;
        
        // Send HTTP request
        const char* http_request = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
        std::memcpy(swift_tcp->data(), http_request, strlen(http_request));
        swift_tcp->send(0, strlen(http_request));
        
        std::cout << "   Sent HTTP request" << std::endl;
        
        // Wait for response
        std::this_thread::sleep_for(1s);
        auto response = swift_tcp->get_rx(0);
        if (response.buffer && response.size > 0) {
            std::cout << "   Received " << response.size << " bytes of response" << std::endl;
            // Print first line of response
            std::string first_line(reinterpret_cast<const char*>(response.buffer->data() + response.offset), 
                                 std::min(response.size, size_t(50)));
            std::cout << "   Response: " << first_line << "..." << std::endl;
        }
    }
}

void zero_copy_example(BuffetAlligator& buffet) {
    std::cout << "\n5. Zero-Copy Networking Example" << std::endl;
    
    // Demonstrate zero-copy with buffer claims
    auto* nic_buf = dynamic_cast<NICBuffer*>(
        buffet.allocate_buffer(16384, BFType::TCP)  // 16KB buffer
    );
    
    if (!nic_buf) {
        std::cout << "   Failed to allocate network buffer" << std::endl;
        return;
    }
    
    std::cout << "   Allocated 16KB network buffer for zero-copy operations" << std::endl;
    
    // Check NIC features
    uint32_t features = nic_buf->get_features();
    std::cout << "   NIC Features:" << std::endl;
    std::cout << "     Zero-copy: " << ((features & static_cast<uint32_t>(NICFeatures::ZERO_COPY)) ? "YES" : "NO") << std::endl;
    std::cout << "     Checksum offload: " << ((features & static_cast<uint32_t>(NICFeatures::CHECKSUM_OFFLOAD)) ? "YES" : "NO") << std::endl;
    std::cout << "     Kernel bypass: " << ((features & static_cast<uint32_t>(NICFeatures::KERNEL_BYPASS)) ? "YES" : "NO") << std::endl;
    
    // Simulate receiving data with zero-copy
    // In real scenario, NIC would DMA directly into our buffer
    const char* incoming = "Zero-copy data transfer!";
    std::memcpy(nic_buf->data() + 1000, incoming, strlen(incoming));
    
    // Claim the received data (zero-copy)
    BufferClaim claim{nic_buf, 1000, strlen(incoming), 0};
    std::cout << "   Created zero-copy claim at offset " << claim.offset << ", size " << claim.size << std::endl;
    
    // Process data without copying
    std::string received(reinterpret_cast<const char*>(claim.buffer->data() + claim.offset), claim.size);
    std::cout << "   Processed: " << received << std::endl;
    
    // The claim automatically manages the buffer lifecycle
    std::cout << "   Zero-copy operation complete!" << std::endl;
}

int main() {
    std::cout << "=== BuffetAlligator Network Buffers Example ===" << std::endl;
    std::cout << "High-speed networking on the menu! 🌐" << std::endl << std::endl;

    auto& buffet = get_buffet_alligator();

    try {
        tcp_example(buffet);
    } catch (const std::exception& e) {
        std::cout << "TCP example error: " << e.what() << std::endl;
    }
    
    try {
        udp_example(buffet);
    } catch (const std::exception& e) {
        std::cout << "UDP example error: " << e.what() << std::endl;
    }
    
    try {
        quic_example(buffet);
    } catch (const std::exception& e) {
        std::cout << "QUIC example error: " << e.what() << std::endl;
    }
    
    try {
        swift_network_example(buffet);
    } catch (const std::exception& e) {
        std::cout << "Swift network example error: " << e.what() << std::endl;
    }
    
    try {
        zero_copy_example(buffet);
    } catch (const std::exception& e) {
        std::cout << "Zero-copy example error: " << e.what() << std::endl;
    }

    std::cout << "\n=== Example Complete ===" << std::endl;
    std::cout << "Network buffers deliver data at the speed of light!" << std::endl;
    
    return 0;
}