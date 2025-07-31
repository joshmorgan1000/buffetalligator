# BuffetAlligator Examples

This directory contains comprehensive examples demonstrating the BuffetAlligator memory management library.

## Examples Overview

### 01_basic_buffers
Demonstrates basic buffer types including heap, file-backed, and shared memory buffers.

### 02_gpu_buffers  
Shows GPU buffer allocation with automatic selection between Metal, CUDA, and Vulkan.

### 03_network_buffers
Illustrates network buffer usage with automatic selection between Foundation Network and ASIO.

### 04_chain_buffers
Explains chain buffer concepts, buffer lifecycle, and the registry system.

### 05_buffer_optimization
Demonstrates buffer allocation patterns and usage optimized for performance, including preparation for zero-copy I/O.

### 06_swift_interop
Shows Swift/C++ interoperability using Swift buffers and CSwift integration.

### 07_error_handling
Covers error handling strategies, fallback mechanisms, and robust allocation patterns.

## Building the Examples

The examples are built automatically when you build the main project:

```bash
mkdir build
cd build
cmake ..
make examples  # Build all examples
```

## Running the Examples

Each example can be run independently:

```bash
./01_basic_buffers
./02_gpu_buffers
# etc...
```

## Platform Notes

- GPU examples will use Metal on macOS, CUDA/Vulkan on Linux/Windows
- Network examples will use Foundation on macOS, ASIO elsewhere  
- Swift interop examples require Swift support to be enabled
