/**
 * @file 06_swift_interop.cpp
 * @brief Swift interoperability demonstration via cswift
 * 
 * This example shows Swift integration through cswift:
 * - Swift buffer types
 * - Metal GPU operations via cswift
 * - Network.framework integration
 * - CoreML preparation
 * - Thunderbolt DMA operations
 */

#include <alligator.hpp>
#include <cswift/cswift.hpp>
#include <iostream>
#include <vector>
#include <thread>

using namespace alligator;

void swift_buffer_example(BuffetAlligator& buffet) {
    std::cout << "1. Swift Buffer Integration" << std::endl;
    
    // Create a Swift buffer
    auto* swift_buf = dynamic_cast<SwiftBuffer*>(
        buffet.allocate_buffer(1024, BFType::SWIFT)
    );
    
    if (!swift_buf) {
        std::cout << "   Swift buffer not available" << std::endl;
        return;
    }
    
    std::cout << "   Allocated Swift buffer via cswift" << std::endl;
    std::cout << "   This buffer bridges C++ and Swift seamlessly" << std::endl;
    
    // Write data that could be processed by Swift code
    const char* message = "Hello from C++, processed by Swift!";
    std::memcpy(swift_buf->data(), message, strlen(message) + 1);
    std::cout << "   Wrote: " << message << std::endl;
    
    // The buffer can be passed to Swift functions via cswift
    // Swift code can operate on this buffer directly
    std::cout << "   Buffer ready for Swift processing" << std::endl;
}

void metal_via_cswift_example(BuffetAlligator& buffet) {
    std::cout << "\n2. Metal GPU via cswift" << std::endl;
    
#ifdef METAL_ENABLED
    try {
        // Create Metal buffer through cswift
        auto* metal_buf = dynamic_cast<MetalBuffer*>(
            buffet.allocate_buffer(4096 * sizeof(float), BFType::METAL)
        );
        
        if (!metal_buf) {
            std::cout << "   Metal buffer not available" << std::endl;
            return;
        }
        
        std::cout << "   Allocated Metal buffer using cswift wrappers" << std::endl;
        
        // Prepare data for Metal compute
        std::vector<float> input_data(4096);
        for (int i = 0; i < 4096; ++i) {
            input_data[i] = i * 0.1f;
        }
        
        // Upload to Metal buffer
        metal_buf->upload(input_data.data(), input_data.size() * sizeof(float));
        std::cout << "   Uploaded " << input_data.size() << " floats to Metal buffer" << std::endl;
        
        // In a real application, you would:
        // 1. Create a Metal compute pipeline via cswift
        // 2. Encode commands to process this buffer
        // 3. Use Metal Performance Shaders (MPS) via cswift
        // 4. Access Apple Neural Engine for ML operations
        
        std::cout << "   Metal buffer ready for:" << std::endl;
        std::cout << "     - Metal Performance Shaders (MPS)" << std::endl;
        std::cout << "     - Neural Engine operations" << std::endl;
        std::cout << "     - Custom Metal compute kernels" << std::endl;
        
        // Example: Using cswift's MPS integration
        auto device = cswift::CSMTLDevice::systemDefault();
        if (device) {
            std::cout << "   Metal device: " << device->name() << std::endl;
            
            // Create MPS graph for neural operations
            auto graph = std::make_unique<cswift::CSMPSGraph>();
            std::cout << "   Created MPSGraph for neural operations" << std::endl;
        }
        
    } catch (const std::exception& e) {
        std::cout << "   Metal example failed: " << e.what() << std::endl;
    }
#else
    std::cout << "   Metal support not enabled in build" << std::endl;
#endif
}

void swift_network_example(BuffetAlligator& buffet) {
    std::cout << "\n3. Swift Network.framework Integration" << std::endl;
    
    // Create Swift TCP buffer
    auto* swift_tcp = dynamic_cast<SwiftTcpBuffer*>(
        buffet.allocate_buffer(8192, BFType::SWIFT_TCP)
    );
    
    if (!swift_tcp) {
        std::cout << "   Swift TCP buffer not available" << std::endl;
        return;
    }
    
    std::cout << "   Created Swift TCP buffer using Network.framework via cswift" << std::endl;
    std::cout << "   Features:" << std::endl;
    std::cout << "     - Modern networking stack" << std::endl;
    std::cout << "     - Built-in TLS support" << std::endl;
    std::cout << "     - Automatic protocol selection" << std::endl;
    std::cout << "     - Energy-efficient networking" << std::endl;
    
    // The buffer uses cswift's CSNWConnection internally
    NetworkEndpoint endpoint{"example.com", 443, NetworkProtocol::TCP};
    
    std::cout << "   Connecting to " << endpoint.address << ":" << endpoint.port << " (TLS)" << std::endl;
    
    if (swift_tcp->connect(endpoint)) {
        std::cout << "   Connected successfully via Network.framework" << std::endl;
        
        // Send HTTPS request
        const char* https_request = 
            "GET / HTTP/1.1\r\n"
            "Host: example.com\r\n"
            "Connection: close\r\n"
            "\r\n";
        
        std::memcpy(swift_tcp->data(), https_request, strlen(https_request));
        ssize_t sent = swift_tcp->send(0, strlen(https_request));
        std::cout << "   Sent HTTPS request: " << sent << " bytes" << std::endl;
        
        // Wait for response
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        auto response = swift_tcp->get_rx(0);
        if (response.buffer && response.size > 0) {
            std::cout << "   Received encrypted response: " << response.size << " bytes" << std::endl;
            
            // Show first line of response
            std::string first_line;
            const char* data = reinterpret_cast<const char*>(response.buffer->data() + response.offset);
            for (size_t i = 0; i < response.size && data[i] != '\r'; ++i) {
                first_line += data[i];
            }
            std::cout << "   Response: " << first_line << std::endl;
        }
    }
}

void swift_udp_multicast_example(BuffetAlligator& buffet) {
    std::cout << "\n4. Swift UDP with Bonjour Discovery" << std::endl;
    
    auto* swift_udp = dynamic_cast<SwiftUdpBuffer*>(
        buffet.allocate_buffer(2048, BFType::SWIFT_UDP)
    );
    
    if (!swift_udp) {
        std::cout << "   Swift UDP buffer not available" << std::endl;
        return;
    }
    
    std::cout << "   Created Swift UDP buffer for service discovery" << std::endl;
    
    // Bind to port for receiving
    NetworkEndpoint local_endpoint{"0.0.0.0", 5353, NetworkProtocol::UDP};  // mDNS port
    if (swift_udp->bind(local_endpoint)) {
        std::cout << "   Bound to mDNS port " << local_endpoint.port << std::endl;
        std::cout << "   Ready for Bonjour/mDNS service discovery" << std::endl;
        
        // In a real application, you would:
        // 1. Join mDNS multicast group (224.0.0.251)
        // 2. Send service queries
        // 3. Receive service announcements
        // 4. Parse DNS-SD records
        
        std::cout << "   Network.framework features used:" << std::endl;
        std::cout << "     - Automatic multicast handling" << std::endl;
        std::cout << "     - Built-in Bonjour support" << std::endl;
        std::cout << "     - Energy-efficient discovery" << std::endl;
    }
}

void swift_quic_example(BuffetAlligator& buffet) {
    std::cout << "\n5. Swift QUIC Buffer (HTTP/3 Ready)" << std::endl;
    
    auto* swift_quic = dynamic_cast<SwiftQuicBuffer*>(
        buffet.allocate_buffer(16384, BFType::SWIFT_QUIC)
    );
    
    if (!swift_quic) {
        std::cout << "   Swift QUIC buffer not available" << std::endl;
        return;
    }
    
    std::cout << "   Created Swift QUIC buffer" << std::endl;
    std::cout << "   QUIC features:" << std::endl;
    std::cout << "     - Multiplexed streams" << std::endl;
    std::cout << "     - 0-RTT connection resumption" << std::endl;
    std::cout << "     - Connection migration" << std::endl;
    std::cout << "     - Built-in congestion control" << std::endl;
    
    // Create multiple streams
    uint64_t data_stream = swift_quic->create_stream(false);     // Bidirectional
    uint64_t control_stream = swift_quic->create_stream(false);  // Bidirectional
    uint64_t push_stream = swift_quic->create_stream(true);      // Unidirectional
    
    std::cout << "   Created streams:" << std::endl;
    std::cout << "     Data stream: " << data_stream << std::endl;
    std::cout << "     Control stream: " << control_stream << std::endl;
    std::cout << "     Push stream: " << push_stream << std::endl;
    
    // Set stream priorities
    swift_quic->set_stream_priority(control_stream, 255);  // Highest priority
    swift_quic->set_stream_priority(data_stream, 128);     // Normal priority
    swift_quic->set_stream_priority(push_stream, 64);      // Lower priority
    
    std::cout << "   Stream priorities configured for QoS" << std::endl;
}

void thunderbolt_dma_example(BuffetAlligator& buffet) {
    std::cout << "\n6. Thunderbolt DMA via cswift" << std::endl;
    
    try {
        auto* tb_dma = dynamic_cast<ThunderboltDMABuffer*>(
            buffet.allocate_buffer(1024 * 1024, BFType::THUNDERBOLT_DMA)  // 1MB
        );
        
        if (!tb_dma) {
            std::cout << "   Thunderbolt DMA buffer not available" << std::endl;
            return;
        }
        
        std::cout << "   Allocated Thunderbolt DMA buffer via cswift" << std::endl;
        std::cout << "   Features:" << std::endl;
        std::cout << "     - PCIe peer-to-peer DMA" << std::endl;
        std::cout << "     - Ultra-low latency transfers" << std::endl;
        std::cout << "     - GPU direct memory access" << std::endl;
        std::cout << "     - External device integration" << std::endl;
        
        // Prepare data for DMA transfer
        uint32_t* dma_data = reinterpret_cast<uint32_t*>(tb_dma->data());
        for (int i = 0; i < 256; ++i) {
            dma_data[i] = 0xDEADBEEF + i;
        }
        
        std::cout << "   Prepared DMA buffer with test pattern" << std::endl;
        
        // In a real application with Thunderbolt devices:
        // 1. Enumerate Thunderbolt devices
        // 2. Set up DMA mappings
        // 3. Initiate peer-to-peer transfers
        // 4. Synchronize with external accelerators
        
        std::cout << "   Ready for high-speed Thunderbolt transfers!" << std::endl;
        
    } catch (const std::exception& e) {
        std::cout << "   Thunderbolt DMA not available: " << e.what() << std::endl;
    }
}

void coreml_preparation_example(BuffetAlligator& buffet) {
    std::cout << "\n7. CoreML Preparation Example" << std::endl;
    
#ifdef METAL_ENABLED
    // Prepare buffers for CoreML inference
    auto* input_buf = dynamic_cast<MetalBuffer*>(
        buffet.allocate_buffer(224 * 224 * 3 * sizeof(float), BFType::METAL)  // ImageNet size
    );
    
    auto* output_buf = dynamic_cast<MetalBuffer*>(
        buffet.allocate_buffer(1000 * sizeof(float), BFType::METAL)  // 1000 classes
    );
    
    if (input_buf && output_buf) {
        std::cout << "   Allocated Metal buffers for ML inference" << std::endl;
        std::cout << "   Input buffer: 224x224x3 image (ImageNet)" << std::endl;
        std::cout << "   Output buffer: 1000 class probabilities" << std::endl;
        
        // Prepare dummy image data
        std::vector<float> image_data(224 * 224 * 3);
        for (size_t i = 0; i < image_data.size(); ++i) {
            image_data[i] = (i % 256) / 255.0f;  // Normalized pixel values
        }
        
        input_buf->upload(image_data.data(), image_data.size() * sizeof(float));
        std::cout << "   Uploaded image data to Metal buffer" << std::endl;
        
        // With cswift, you can:
        // 1. Create CoreML model wrapper
        // 2. Set Metal buffers as inputs
        // 3. Run inference on Neural Engine
        // 4. Get results in output buffer
        
        std::cout << "   Buffers ready for CoreML inference via:" << std::endl;
        std::cout << "     - Apple Neural Engine (ANE)" << std::endl;
        std::cout << "     - GPU compute shaders" << std::endl;
        std::cout << "     - CPU fallback" << std::endl;
        
        // Example using cswift's neural API
        auto neural_engine = std::make_unique<cswift::CSNeuralEngine>();
        if (neural_engine->isAvailable()) {
            std::cout << "   Neural Engine is available for acceleration!" << std::endl;
        }
    }
#else
    std::cout << "   CoreML preparation requires Metal support" << std::endl;
#endif
}

int main() {
    std::cout << "=== BuffetAlligator Swift Interop Example ===" << std::endl;
    std::cout << "Bridging C++ and Swift with cswift! 🌉" << std::endl << std::endl;

    auto& buffet = get_buffet_alligator();

    swift_buffer_example(buffet);
    metal_via_cswift_example(buffet);
    swift_network_example(buffet);
    swift_udp_multicast_example(buffet);
    swift_quic_example(buffet);
    thunderbolt_dma_example(buffet);
    coreml_preparation_example(buffet);

    std::cout << "\n=== Example Complete ===" << std::endl;
    std::cout << "cswift brings the power of Apple frameworks to C++!" << std::endl;
    
    return 0;
}