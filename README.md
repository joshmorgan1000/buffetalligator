<div align="center">
  <img src="buffetalligator_logo.png" alt="Buffet Alligator Logo" width="150"/>


</div>

---

BuffetAlligator is a high-performance, cross-platform memory buffer management library written in C++20. It provides a unified interface for various types of memory buffers including heap memory, GPU memory, network buffers, and specialized hardware-accelerated buffers. The library features seamless Swift interoperability through the cswift submodule and supports zero-copy operations wherever possible.

## What It Does

BuffetAlligator abstracts away the complexity of managing different types of memory buffers across various hardware architectures and use cases. Whether you need standard heap allocation, GPU memory for compute operations, shared memory for inter-process communication, or network buffers for high-throughput data transfer, BuffetAlligator provides a consistent API with automatic lifetime management.

Key capabilities include:
- **Unified Buffer Interface**: Single API for all buffer types with automatic type selection
- **Hardware Acceleration**: Native support for CUDA, Vulkan, Metal, and Thunderbolt DMA
- **Network Integration**: Built-in support for TCP, UDP, and QUIC protocols with both ASIO and Swift Network.framework backends
- **Zero-Copy Operations**: Efficient buffer chaining and sharing without memory copies
- **Swift Interoperability**: Seamless integration with Swift code through the cswift bridge
- **Automatic Memory Management**: Reference-counted buffers with garbage collection

## How It Works

BuffetAlligator uses a registry pattern with a global allocator (the "Buffet") that manages different buffer types. Each buffer type inherits from a common base class and implements specific allocation strategies optimized for its use case. The library automatically selects the most appropriate buffer type based on size, usage patterns, and available hardware.

The architecture consists of:
1. **Core Buffer Classes**: Base abstractions for all buffer types
2. **Specialized Implementations**: Platform-specific optimizations (Metal for macOS, CUDA for NVIDIA GPUs, etc.)
3. **Network Buffers**: Integration with networking frameworks for efficient data transfer
4. **Buffer Chains**: Zero-copy concatenation of multiple buffers
5. **Swift Bridge**: C++ to Swift interoperability layer for Apple platforms

## The Name

The name "BuffetAlligator" came out from a combination of autocorrect and voice-to-text, and the name stuck. ASCII art courtesy of Claude AI.
