#pragma once
/**
 * @file alligator.hpp
 * @brief Main header for BuffetAlligator memory buffer implementations
 * 
 * This header includes all buffer types available in the BuffetAlligator
 * memory management library. Users should include this single header to
 * access all buffer functionality.
 */

// The cswift library provides high-performance Swift/C++ interoperability
#include <cswift/cswift.hpp>
// Core buffer interfaces
#include <alligator/buffer/buffer.hpp>
#include <alligator/buffer/chain_buffer.hpp>
#include <alligator/buffer/buffet_alligator.hpp>
// Specialized buffer base classes
#include <alligator/buffer/gpu_buffer.hpp>
#include <alligator/buffer/nic_buffer.hpp>
// Basic buffer implementations
#include <alligator/buffer/heap_buffer.hpp>
#include <alligator/buffer/file_backed_buffer.hpp>
#include <alligator/buffer/shared_buffer.hpp>
#include <alligator/buffer/thunderbolt_dma_buffer.hpp>
// GPU buffer implementations
#include <alligator/buffer/vulkan_buffer.hpp>
#include <alligator/buffer/cuda_buffer.hpp>
#include <alligator/buffer/metal_buffer.hpp>
// Network buffer implementations
#include <alligator/buffer/asio_tcp_buffer.hpp>
#include <alligator/buffer/asio_udp_buffer.hpp>
#include <alligator/buffer/asio_quic_buffer.hpp>
#include <alligator/buffer/swift_tcp_buffer.hpp>
#include <alligator/buffer/swift_udp_buffer.hpp>
#include <alligator/buffer/swift_quic_buffer.hpp>
// Swift interop buffer
#include <alligator/buffer/swift_buffer.hpp>
