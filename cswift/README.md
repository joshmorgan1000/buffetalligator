# cswift

C++20 bindings for Swift frameworks including SwiftNIO, Metal Performance Shaders, and CoreML.

## Features

- Zero-copy I/O operations with SwiftNIO ByteBuffer
- RAII-based C++ wrappers for Swift objects
- Cross-platform support (macOS, Linux, Windows planned)
- Modern C++20 design with move semantics

## Building

```bash
# Quick build
./scripts/build.sh

# Debug build
./scripts/build.sh --debug

# Custom install prefix
./scripts/build.sh --prefix /opt/cswift
```

## Usage

```cpp
#include <cswift/cswift.hpp>

// Zero-copy ByteBuffer operations
cswift::CSNIOByteBuffer buffer(4096);

// Write directly to buffer memory
size_t written = buffer.writeWithUnsafeMutableBytes([](void* ptr, size_t capacity) {
    // Write directly to ptr without copying
    return generateData(ptr, capacity);
});

// Read data
auto data = buffer.readBytes(written);
```

## Requirements

- CMake 3.20+
- C++20 compiler (Clang 12+, GCC 11+, MSVC 2022)
- Swift 5.5+
- macOS: Xcode 13+
- Linux: Swift toolchain

### SwiftNIO Integration

This library provides SwiftNIO-compatible APIs with dual implementation modes:

**Default Mode (Current)**: Uses stub implementations that provide full API compatibility. Perfect for:
- Development and testing
- Basic networking needs (using Apple's Network.framework on macOS)
- Examples and prototyping

**Full SwiftNIO Mode**: Requires integration into a Swift Package Manager project. Recommended for:
- Production Swift servers
- High-performance async networking
- Integration with existing SwiftNIO ecosystems

**To Enable Full SwiftNIO (Advanced)**:
1. Integrate this library into a Swift package with SwiftNIO dependencies
2. SwiftNIO is not available as a system package - it's designed for SPM usage
3. For most C++ projects, the default mode provides sufficient networking capabilities

**Current Status Check**:
- ✅ If you see "Package 'swift-nio' not found" - this is normal and expected
- ✅ The library builds and works fully without SwiftNIO installed
- ✅ All examples and networking features work via Apple's Network.framework

## License

See LICENSE file for details.