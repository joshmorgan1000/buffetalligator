cmake_minimum_required(VERSION 3.20)

# Find required packages
find_package(Threads REQUIRED)

# Common include directories for all examples
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/cswift/include
)

# List of example programs
set(EXAMPLES
    01_basic_buffers
    02_gpu_buffers
    03_network_buffers
    04_chain_buffers
    05_buffer_optimization
    06_swift_interop
    07_error_handling
    08_advanced_buffers
)

# Create executables for each example
foreach(EXAMPLE ${EXAMPLES})
    add_executable(${EXAMPLE} ${EXAMPLE}.cpp)
    
    # Link against the buffers library
    target_link_libraries(${EXAMPLE} 
        PRIVATE 
            buffet_alligator
            Threads::Threads
    )
    
    # Set C++ standard
    set_target_properties(${EXAMPLE} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    
    # Add compile definitions
    target_compile_definitions(${EXAMPLE} PRIVATE
        $<$<CONFIG:Debug>:DEBUG>
    )
endforeach()

# Special handling for Swift interop example
if(TARGET cswift)
    target_link_libraries(06_swift_interop PRIVATE cswift)
endif()

# Create a convenience target to build all examples
add_custom_target(examples DEPENDS ${EXAMPLES})

# Install examples
install(TARGETS ${EXAMPLES}
    RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/bin/examples
)

# Install example source files for reference
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    DESTINATION ${CMAKE_INSTALL_PREFIX}/share/buffetalligator/examples
)

# Add README for examples
set(EXAMPLES_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
if(NOT EXISTS ${EXAMPLES_README})
    file(WRITE ${EXAMPLES_README} "# BuffetAlligator Examples

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

### 08_advanced_buffers
Demonstrates shared memory buffers for IPC and Thunderbolt DMA buffers for high-speed device communication.

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
")
endif()

install(FILES ${EXAMPLES_README}
    DESTINATION ${CMAKE_INSTALL_PREFIX}/share/buffetalligator/examples
)