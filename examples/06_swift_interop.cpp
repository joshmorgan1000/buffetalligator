/**
 * @file 06_swift_interop.cpp
 * @brief Example demonstrating Swift/C++ interoperability with buffers
 * 
 * This example shows how to:
 * - Allocate Swift-compatible buffers
 * - Share data between C++ and Swift
 * - Use CSwift for high-performance interop
 * - Handle platform-specific Swift features
 */

#include <alligator.hpp>
#ifdef PSYNE_SWIFT_ENABLED
#include <cswift/cswift.hpp>
#endif
#include <iostream>
#include <vector>
#include <cstring>

using namespace alligator;

void swift_buffer_basics() {
    std::cout << "\n=== Swift Buffer Basics ===" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
#ifdef PSYNE_SWIFT_ENABLED
    std::cout << "Swift support is enabled!" << std::endl;
    
    // Allocate a Swift-compatible buffer
    constexpr size_t buffer_size = 4096;
    auto* swift_buffer = alligator.allocate_buffer(buffer_size, BFType::SWIFT);
    
    if (swift_buffer) {
        std::cout << "Swift buffer allocated successfully:" << std::endl;
        std::cout << "- Buffer ID: " << swift_buffer->id_.load() << std::endl;
        std::cout << "- Size: " << swift_buffer->buf_size_ << " bytes" << std::endl;
        std::cout << "- Can be accessed from both C++ and Swift" << std::endl;
        
        // Write data from C++
        const char* message = "Hello from C++ to Swift!";
        std::memcpy(swift_buffer->data(), message, strlen(message) + 1);
        
        std::cout << "C++ wrote: " << reinterpret_cast<char*>(swift_buffer->data()) << std::endl;
        
        // In Swift, you could access this buffer directly:
        // let buffer = getSwiftBuffer(bufferId)
        // let data = buffer.data
        // print(String(cString: data))
    } else {
        std::cout << "Failed to allocate Swift buffer" << std::endl;
    }
#else
    std::cout << "Swift support not enabled in this build" << std::endl;
    std::cout << "Build with Swift support to use Swift buffers" << std::endl;
#endif
}

void cswift_integration_example() {
    std::cout << "\n=== CSwift Integration ===" << std::endl;
    
#ifdef PSYNE_SWIFT_ENABLED
    std::cout << "CSwift provides high-performance Swift/C++ interop:" << std::endl;
    std::cout << "- Zero-copy data sharing" << std::endl;
    std::cout << "- Type-safe interfaces" << std::endl;
    std::cout << "- Automatic memory management" << std::endl;
    std::cout << "- Support for Swift concurrency" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    // Example: Sharing structured data
    struct MLTensor {
        uint32_t dims[4];      // NCHW dimensions
        float* data;           // Tensor data
        size_t element_count;  // Total elements
    };
    
    // Allocate buffer for tensor
    constexpr size_t tensor_elements = 32 * 3 * 224 * 224; // Batch=32, RGB, 224x224
    constexpr size_t tensor_size = tensor_elements * sizeof(float);
    
    auto* tensor_buffer = alligator.allocate_buffer(
        sizeof(MLTensor) + tensor_size, 
        BFType::SWIFT
    );
    
    if (tensor_buffer) {
        // Setup tensor header
        auto* tensor = tensor_buffer->get<MLTensor>(0);
        tensor->dims[0] = 32;   // N (batch)
        tensor->dims[1] = 3;    // C (channels)
        tensor->dims[2] = 224;  // H (height)
        tensor->dims[3] = 224;  // W (width)
        tensor->element_count = tensor_elements;
        tensor->data = reinterpret_cast<float*>(
            tensor_buffer->data() + sizeof(MLTensor)
        );
        
        // Initialize with test data
        for (size_t i = 0; i < 100; ++i) {
            tensor->data[i] = static_cast<float>(i) * 0.01f;
        }
        
        std::cout << "\nML Tensor prepared for Swift:" << std::endl;
        std::cout << "- Shape: [" << tensor->dims[0] << ", " << tensor->dims[1] 
                  << ", " << tensor->dims[2] << ", " << tensor->dims[3] << "]" << std::endl;
        std::cout << "- Elements: " << tensor->element_count << std::endl;
        std::cout << "- Memory: " << tensor_size / (1024.0f * 1024.0f) << " MB" << std::endl;
        
        std::cout << "\nSwift can process this tensor using:" << std::endl;
        std::cout << "- Metal Performance Shaders" << std::endl;
        std::cout << "- Core ML" << std::endl;
        std::cout << "- Accelerate framework" << std::endl;
    }
#else
    std::cout << "CSwift integration requires Swift support" << std::endl;
#endif
}

void swift_dispatch_example() {
    std::cout << "\n=== Swift Dispatch Queue Integration ===" << std::endl;
    
#ifdef __APPLE__
    std::cout << "On Apple platforms, buffers can integrate with GCD:" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    // Allocate buffer for async processing
    constexpr size_t buffer_size = 1024 * 1024; // 1MB
    auto* async_buffer = alligator.allocate_buffer(buffer_size, BFType::SWIFT);
    
    if (async_buffer) {
        std::cout << "Buffer allocated for async Swift processing" << std::endl;
        
        // Example async pattern:
        std::cout << "\nSwift async pattern:" << std::endl;
        std::cout << "```swift" << std::endl;
        std::cout << "// Get buffer from C++" << std::endl;
        std::cout << "let buffer = BuffetAlligator.getBuffer(id: bufferId)" << std::endl;
        std::cout << "" << std::endl;
        std::cout << "// Process async on dispatch queue" << std::endl;
        std::cout << "DispatchQueue.global(qos: .userInitiated).async {" << std::endl;
        std::cout << "    // Process buffer data" << std::endl;
        std::cout << "    let result = processData(buffer.data, size: buffer.size)" << std::endl;
        std::cout << "    " << std::endl;
        std::cout << "    // Notify completion" << std::endl;
        std::cout << "    DispatchQueue.main.async {" << std::endl;
        std::cout << "        completionHandler(result)" << std::endl;
        std::cout << "    }" << std::endl;
        std::cout << "}" << std::endl;
        std::cout << "```" << std::endl;
    }
#else
    std::cout << "Swift dispatch queues are available on Apple platforms" << std::endl;
#endif
}

void metal_swift_example() {
    std::cout << "\n=== Metal + Swift Buffer Example ===" << std::endl;
    
#if defined(__APPLE__) && defined(PSYNE_SWIFT_ENABLED)
    std::cout << "Metal + Swift provides GPU compute on Apple platforms:" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    // Allocate Metal buffer through Swift interface
    constexpr size_t buffer_size = 16 * 1024 * 1024; // 16MB
    auto* metal_buffer = alligator.allocate_buffer(buffer_size, BFType::METAL);
    
    if (metal_buffer) {
        std::cout << "\nMetal buffer allocated for Swift GPU compute" << std::endl;
        std::cout << "Buffer ID: " << metal_buffer->id_.load() << std::endl;
        
        std::cout << "\nSwift Metal compute example:" << std::endl;
        std::cout << "```swift" << std::endl;
        std::cout << "import Metal" << std::endl;
        std::cout << "import MetalPerformanceShaders" << std::endl;
        std::cout << "" << std::endl;
        std::cout << "// Get Metal buffer from C++" << std::endl;
        std::cout << "let buffer = BuffetAlligator.getMetalBuffer(id: bufferId)" << std::endl;
        std::cout << "" << std::endl;
        std::cout << "// Create MPS kernel" << std::endl;
        std::cout << "let device = MTLCreateSystemDefaultDevice()!" << std::endl;
        std::cout << "let commandQueue = device.makeCommandQueue()!" << std::endl;
        std::cout << "" << std::endl;
        std::cout << "// Example: Matrix multiplication" << std::endl;
        std::cout << "let matrixMultiplication = MPSMatrixMultiplication(" << std::endl;
        std::cout << "    device: device," << std::endl;
        std::cout << "    transposeLeft: false," << std::endl;
        std::cout << "    transposeRight: false," << std::endl;
        std::cout << "    resultRows: 1024," << std::endl;
        std::cout << "    resultColumns: 1024," << std::endl;
        std::cout << "    interiorColumns: 1024," << std::endl;
        std::cout << "    alpha: 1.0," << std::endl;
        std::cout << "    beta: 0.0" << std::endl;
        std::cout << ")" << std::endl;
        std::cout << "```" << std::endl;
    } else {
        std::cout << "Metal buffer allocation failed" << std::endl;
    }
#else
    std::cout << "Metal + Swift requires Apple platform with Swift support" << std::endl;
#endif
}

void foundation_network_swift_example() {
    std::cout << "\n=== Foundation Network + Swift ===" << std::endl;
    
#if defined(__APPLE__) && defined(PSYNE_SWIFT_ENABLED)
    std::cout << "Foundation Network provides native networking on Apple platforms:" << std::endl;
    
    auto& alligator = get_buffet_alligator();
    
    // TCP buffer that works seamlessly with Swift
    auto* tcp_buffer = alligator.allocate_buffer(65536, BFType::TCP);
    
    if (tcp_buffer) {
        std::cout << "\nTCP buffer ready for Swift networking" << std::endl;
        
        std::cout << "\nSwift Network.framework example:" << std::endl;
        std::cout << "```swift" << std::endl;
        std::cout << "import Network" << std::endl;
        std::cout << "" << std::endl;
        std::cout << "// Get buffer from C++" << std::endl;
        std::cout << "let buffer = BuffetAlligator.getNetworkBuffer(id: bufferId)" << std::endl;
        std::cout << "" << std::endl;
        std::cout << "// Create connection" << std::endl;
        std::cout << "let connection = NWConnection(" << std::endl;
        std::cout << "    host: \"example.com\"," << std::endl;
        std::cout << "    port: 443," << std::endl;
        std::cout << "    using: .tls" << std::endl;
        std::cout << ")" << std::endl;
        std::cout << "" << std::endl;
        std::cout << "// Send buffer data" << std::endl;
        std::cout << "connection.send(" << std::endl;
        std::cout << "    content: buffer.data," << std::endl;
        std::cout << "    completion: .contentProcessed { error in" << std::endl;
        std::cout << "        // Handle completion" << std::endl;
        std::cout << "    }" << std::endl;
        std::cout << ")" << std::endl;
        std::cout << "```" << std::endl;
    }
#else
    std::cout << "Foundation Network + Swift requires Apple platform" << std::endl;
#endif
}

void swift_concurrency_example() {
    std::cout << "\n=== Swift Concurrency with Buffers ===" << std::endl;
    
#ifdef PSYNE_SWIFT_ENABLED
    std::cout << "Swift's async/await works great with BuffetAlligator:" << std::endl;
    
    std::cout << "\nSwift async example:" << std::endl;
    std::cout << "```swift" << std::endl;
    std::cout << "// Async buffer processing" << std::endl;
    std::cout << "func processBufferAsync(id: UInt32) async throws -> Result {" << std::endl;
    std::cout << "    // Get buffer from C++" << std::endl;
    std::cout << "    guard let buffer = await BuffetAlligator.getBuffer(id: id) else {" << std::endl;
    std::cout << "        throw BufferError.notFound" << std::endl;
    std::cout << "    }" << std::endl;
    std::cout << "    " << std::endl;
    std::cout << "    // Process in parallel" << std::endl;
    std::cout << "    async let result1 = processChunk(buffer, range: 0..<buffer.size/2)" << std::endl;
    std::cout << "    async let result2 = processChunk(buffer, range: buffer.size/2..<buffer.size)" << std::endl;
    std::cout << "    " << std::endl;
    std::cout << "    // Wait for both" << std::endl;
    std::cout << "    let results = await [result1, result2]" << std::endl;
    std::cout << "    return combine(results)" << std::endl;
    std::cout << "}" << std::endl;
    std::cout << "" << std::endl;
    std::cout << "// Actor for thread-safe buffer access" << std::endl;
    std::cout << "actor BufferManager {" << std::endl;
    std::cout << "    private var buffers: [UInt32: Buffer] = [:]" << std::endl;
    std::cout << "    " << std::endl;
    std::cout << "    func addBuffer(_ buffer: Buffer) {" << std::endl;
    std::cout << "        buffers[buffer.id] = buffer" << std::endl;
    std::cout << "    }" << std::endl;
    std::cout << "}" << std::endl;
    std::cout << "```" << std::endl;
#else
    std::cout << "Swift concurrency requires Swift support" << std::endl;
#endif
}

void performance_tips() {
    std::cout << "\n=== Swift/C++ Performance Tips ===" << std::endl;
    
    std::cout << "\n1. Memory Management:" << std::endl;
    std::cout << "   - Use pinned buffers for long-lived Swift references" << std::endl;
    std::cout << "   - Let BuffetAlligator handle lifecycle" << std::endl;
    std::cout << "   - Avoid unnecessary copies between Swift and C++" << std::endl;
    
    std::cout << "\n2. Type Safety:" << std::endl;
    std::cout << "   - Use typed buffer access (get<T>())" << std::endl;
    std::cout << "   - Define shared structs in both languages" << std::endl;
    std::cout << "   - Use CSwift for automatic bridging" << std::endl;
    
    std::cout << "\n3. Concurrency:" << std::endl;
    std::cout << "   - Swift actors work well with buffer atomics" << std::endl;
    std::cout << "   - Use GCD for async processing" << std::endl;
    std::cout << "   - Leverage Swift's structured concurrency" << std::endl;
    
    std::cout << "\n4. Platform Integration:" << std::endl;
    std::cout << "   - Use Metal buffers for GPU compute" << std::endl;
    std::cout << "   - Use Foundation Network for networking" << std::endl;
    std::cout << "   - Leverage Accelerate framework for SIMD" << std::endl;
}

int main() {
    std::cout << "BuffetAlligator Swift Interop Examples" << std::endl;
    std::cout << "======================================" << std::endl;
    
    try {
        swift_buffer_basics();
        cswift_integration_example();
        swift_dispatch_example();
        metal_swift_example();
        foundation_network_swift_example();
        swift_concurrency_example();
        performance_tips();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "\nSwift interop examples completed!" << std::endl;
    return 0;
}