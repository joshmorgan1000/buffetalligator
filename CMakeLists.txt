cmake_minimum_required(VERSION 3.20)
project(buffet_alligator VERSION 1.0.0 LANGUAGES C CXX ASM)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# ------------------------------------------------------------------------------
# Configuration settings
# ------------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
option(FORCE_CPU_ONLY "Force CPU-only build (disable GPU)" OFF)
option(BUILD_EXAMPLES "Build examples" ON)
option(BUILD_TESTS "Build tests" ON)
option(BUILD_BENCHMARKS "Build benchmarks" ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Detect optimal configuration for current environment
if(NOT FORCE_CPU_ONLY)
    include(DetectGPU)
else()
    message(STATUS "GPU support disabled by user")
endif()

# Detect CPU features
include(CheckCXXCompilerFlag)

# Platform-specific base flags (before any architecture detection)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Base flags without architecture-specific options (C++ only)
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:-Wall>
        $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
        $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-parameter>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-variable>
        $<$<COMPILE_LANGUAGE:CXX>:-ffast-math>
        $<$<COMPILE_LANGUAGE:CXX>:-funroll-loops>
        $<$<COMPILE_LANGUAGE:CXX>:-ftree-vectorize>
    )
    # Release-specific flags
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-O3>)
        add_compile_definitions(NDEBUG)
    else()
        add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-O0> $<$<COMPILE_LANGUAGE:CXX>:-g>)
        # Sanitizers for debug (not on Apple due to issues)
        if(NOT APPLE)
            add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fsanitize=address> $<$<COMPILE_LANGUAGE:CXX>:-fsanitize=undefined>)
            add_link_options(-fsanitize=address -fsanitize=undefined)
        endif()
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    add_compile_options(
        /W4
        /fp:fast
        /Oi  # Enable intrinsic functions
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(/O2 /GL)  # Whole program optimization
        add_link_options(/LTCG)  # Link-time code generation
        add_compile_definitions(NDEBUG)
    else()
        add_compile_options(/Od /Zi)
    endif()
endif()

# Save current compile options
get_directory_property(SAVED_COMPILE_OPTIONS COMPILE_OPTIONS)
# Clear compile options for cswift (Swift doesn't understand C++ flags)
set_directory_properties(PROPERTIES COMPILE_OPTIONS "")
# Set Swift-specific flags before including cswift
set(CMAKE_Swift_FLAGS_RELEASE "-O -wmo")  # No -march=native for Swift!
add_subdirectory(cswift)
# Restore compile options
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${SAVED_COMPILE_OPTIONS}")

include_directories(
    "${CMAKE_PROJECT_SOURCE_DIR}/include"
    "${CMAKE_PROJECT_SOURCE_DIR}/cswift/include"
)

# Add module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ASIO
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/asio/asio/include/asio.hpp")
    message(STATUS "Using bundled ASIO")
    set(ASIO_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/asio/asio/include")
else()
    find_package(Boost REQUIRED COMPONENTS system)
    if(NOT Boost_FOUND)
        message(FATAL_ERROR "Could not find ASIO, and Boost was not found either. You may need to run `git submodule update --init --recursive` to fetch ASIO.")
    endif()
    set(ASIO_INCLUDE_DIR ${Boost_INCLUDE_DIRS})
    add_definitions(-DASIO_STANDALONE)
endif()

# Source files for buffers
set(BUFFER_SOURCES
    src/alligator/buffer/heap_buffer.cpp
    src/alligator/buffer/buffet_alligator.cpp
    src/alligator/buffer/buffer_pin.cpp
    src/alligator/buffer/file_backed_buffer.cpp
    src/alligator/buffer/shared_buffer.cpp
    src/alligator/buffer/thunderbolt_dma_buffer.cpp
    src/alligator/buffer/asio_tcp_buffer.cpp
    src/alligator/buffer/asio_udp_buffer.cpp
    src/alligator/buffer/asio_quic_buffer.cpp
    src/alligator/buffer/vulkan_buffer.cpp
    src/alligator/buffer/metal_buffer.cpp
    src/alligator/buffer/cuda_buffer.cpp
    src/alligator/buffer/swift_buffer.cpp
    src/alligator/buffer/swift_tcp_buffer.cpp
    src/alligator/buffer/swift_udp_buffer.cpp
    src/alligator/buffer/swift_quic_buffer.cpp
)

# Create the buffers library as static
add_library(buffet_alligator STATIC ${BUFFER_SOURCES})

# Include directories
target_include_directories(buffet_alligator PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cswift/include>
    $<INSTALL_INTERFACE:include>
)

# Link libraries
target_link_libraries(buffet_alligator PUBLIC 
    $<$<BOOL:${Vulkan_INCLUDE_DIRS}>:Vulkan::Vulkan>
    $<$<BOOL:${CUDA_ARCHITECTURES_INCLUDE_DIRS}>:cuda>
    $<$<BOOL:${METAL_FRAMEWORK_INCLUDE_DIRS}>:Metal>
    $<$<BOOL:${ACCELERATE_FRAMEWORK_INCLUDE_DIRS}>:Accelerate>
)

add_subdirectory(examples)

# ──────────────────────────────────────────────────────────────
# Print configuration summary
# ──────────────────────────────────────────────────────────────
message(STATUS "____________________________________________________")
message(STATUS "          _  _                      _             ")
message(STATUS "  |_    _|__|_ _ _|_    _  |  |  o (_| _ _|_ _  __")
message(STATUS "  |_)|_| |  | (/_ |_   (_| |  |  | __|(_| |_(_) | ")
message(STATUS "────────────────────────────────────────────────────")
message(STATUS "  Buffet Alligator - C++20 Memory Management Library")
message(STATUS "        Version:        ${PROJECT_VERSION}")
message(STATUS "        C++ Standard:   ${CMAKE_CXX_STANDARD}")
message(STATUS "        CUDA Enabled:   ${CUDA_ENABLED}")
message(STATUS "        Vulkan Enabled: ${VULKAN_ENABLED}")
message(STATUS "        Metal Enabled:  ${METAL_ENABLED}")
message(STATUS "────────────────────────────────────────────────────")
