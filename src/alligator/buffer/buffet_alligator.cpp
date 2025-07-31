#include <alligator/buffer/buffet_alligator.hpp>
#include <alligator/buffer/heap_buffer.hpp>
#include <alligator/buffer/vulkan_buffer.hpp>
#include <alligator/buffer/swift_buffer.hpp>
#include <alligator/buffer/swift_tcp_buffer.hpp>
#include <alligator/buffer/swift_udp_buffer.hpp>
#include <alligator/buffer/swift_quic_buffer.hpp>
#include <alligator/buffer/cuda_buffer.hpp>
#include <alligator/buffer/metal_buffer.hpp>
#include <alligator/buffer/file_backed_buffer.hpp>
#include <alligator/buffer/chain_buffer.hpp>
#include <alligator/buffer/shared_buffer.hpp>
#include <alligator/buffer/thunderbolt_dma_buffer.hpp>
#include <alligator/buffer/asio_tcp_buffer.hpp>
#include <alligator/buffer/asio_udp_buffer.hpp>
#include <alligator/buffer/asio_quic_buffer.hpp>
#include <chrono>
#include <stdexcept>

namespace alligator {

BuffetAlligator::BuffetAlligator() {
    static bool initialized = false;
    if (!initialized) {
        buffers_ = std::make_unique<std::atomic<ChainBuffer*>[]>(INITIAL_CAPACITY);
        for (size_t i = 0; i < INITIAL_CAPACITY; ++i) {
            buffers_[i].store(nullptr, std::memory_order_relaxed);
        }
        // Spin up the garbage collector thread
        stop_.store(false, std::memory_order_release);
        garbage_collector_thread_ = std::make_unique<std::thread>(&BuffetAlligator::garbage_collector, this);
        garbage_collector_thread_->detach();  // Detach to run independently
        // Initialize the registry size and next index
        registry_size_.store(INITIAL_CAPACITY, std::memory_order_release);
        next_index_.store(0, std::memory_order_release);
        size_growing_.store(false, std::memory_order_release);
        initialized = true;
    }
}

BuffetAlligator::~BuffetAlligator() {
    stop_.store(true, std::memory_order_release);
    
    // Wait a bit for garbage collector to finish
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    size_t registry_size = registry_size_.load(std::memory_order_acquire);
    for (size_t i = 0; i < registry_size; ++i) {
        // Use compare_exchange to avoid double-free race with garbage collector
        ChainBuffer* buffer = buffers_[i].load(std::memory_order_acquire);
        ChainBuffer* expected = buffer;
        if (buffer && buffers_[i].compare_exchange_strong(expected, nullptr, 
                                                          std::memory_order_release,
                                                          std::memory_order_acquire)) {
            buffer->deallocate();  // Only deallocate if we successfully nulled the pointer
        }
    }
    buffers_.reset();  // Release the buffer registry
    // Don't log here - logger may already be destroyed
}

void BuffetAlligator::garbage_collector() {
    uint64_t index = 0;
    while (!stop_.load(std::memory_order_acquire)) {
        size_t registry_size = registry_size_.load(std::memory_order_acquire);
        ChainBuffer* buffer = get_buffer(index % registry_size);
        if (buffer && buffer->consumed_.load(std::memory_order_acquire)
                && (buffer->get_destructor_count() >= buffer->get_constructor_count() || !buffer->pinned_.load(std::memory_order_acquire))) {
            buffer->deallocate();
            buffers_[index % registry_size].store(nullptr, std::memory_order_relaxed);
        }
        index++;
        if (index % registry_size == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));  // Sleep every full cycle
        }
    }
}

ChainBuffer* BuffetAlligator::allocate_buffer(uint32_t size, BFType type) {
    if (size == 0) return nullptr;
    // Allocate the buffer first
    ChainBuffer* new_buffer = nullptr;
    switch (type) {
        case BFType::HEAP:
            new_buffer = new HeapBuffer(size);
            break;
        case BFType::VULKAN:
#ifdef VULKAN_ENABLED
            new_buffer = new VulkanBuffer(size);
#else
            throw std::runtime_error("Vulkan support not available on this system");
#endif
            break;
        case BFType::SWIFT:
            new_buffer = new SwiftBuffer(size);
            break;
        case BFType::CUDA:
#ifdef CUDA_ENABLED
            new_buffer = new CUDABuffer(size);
#else
            throw std::runtime_error("CUDA support not available on this system");
#endif
            break;
        case BFType::METAL:
#ifdef METAL_ENABLED
//            LOG_DEBUG_STREAM << "BuffetAlligator: Allocating Metal buffer of size " << size << " bytes";
            new_buffer = new MetalBuffer(size);
#else
            throw std::runtime_error("Metal support not available on this system");
#endif
            break;
        case BFType::FILE_BACKED:
            new_buffer = new FileBackedBuffer(size);
            break;
        case BFType::SHARED:
            new_buffer = new SharedBuffer(size);
            break;
            
        case BFType::THUNDERBOLT_DMA:
            try {
                new_buffer = new ThunderboltDMABuffer(size);
            } catch (const std::exception& e) {
                // Fall back to heap buffer if Thunderbolt DMA not available
                new_buffer = new HeapBuffer(size);
            }
            break;
        case BFType::TCP:
#if defined(__APPLE__)
            new_buffer = new SwiftTcpBuffer(size);
#else
            new_buffer = new AsioTcpBuffer(size);
#endif
            break;
        case BFType::UDP:
#if defined(__APPLE__)
            new_buffer = new SwiftUdpBuffer(size);
#else
            new_buffer = new AsioUdpBuffer(size);
#endif
            break;
        case BFType::QUIC:
#if defined(__APPLE__)
            new_buffer = new SwiftQuicBuffer(size);
#else
            new_buffer = new AsioQuicBuffer(size);
#endif
            break;
        case BFType::GPU:
            // Priority order: Metal (on Apple), CUDA, then Vulkan
#if defined(METAL_ENABLED) && defined(__APPLE__)
            new_buffer = new MetalBuffer(size);
#elif defined(CUDA_ENABLED)
            new_buffer = new CUDABuffer(size);
#elif defined(VULKAN_ENABLED)
            new_buffer = new VulkanBuffer(size);
#else
            throw std::runtime_error("No GPU support available on this system");
#endif
            break;
        
        // Explicit ASIO network types
        case BFType::ASIO_TCP:
            new_buffer = new AsioTcpBuffer(size);
            break;
            
        case BFType::ASIO_UDP:
            new_buffer = new AsioUdpBuffer(size);
            break;
            
        case BFType::ASIO_QUIC:
            new_buffer = new AsioQuicBuffer(size);
            break;
            
        // Explicit Foundation network types (require cswift)
        case BFType::SWIFT_TCP:
            new_buffer = new SwiftTcpBuffer(size);
            break;
            
        case BFType::SWIFT_UDP:
            new_buffer = new SwiftUdpBuffer(size);
            break;
            
        case BFType::SWIFT_QUIC:
            new_buffer = new SwiftQuicBuffer(size);
            break;
            
        default:
            new_buffer = new HeapBuffer(size);
            break;
    }
    // Use CAS loop to find an empty slot and claim it atomically
    size_t registry_size = registry_size_.load(std::memory_order_acquire);
    size_t counter = registry_size;
    while (true) {
        uint64_t index = next_index_.fetch_add(1, std::memory_order_relaxed);
        size_t slot = index % registry_size;
        // Try to claim this slot atomically
        ChainBuffer* expected = nullptr;
        if (buffers_[slot].compare_exchange_strong(expected,
                new_buffer, std::memory_order_release, std::memory_order_relaxed)) {
            // Successfully claimed the slot
            new_buffer->id_.store(static_cast<uint32_t>(slot), std::memory_order_release);
            return new_buffer;
        }
        --counter;
        if (counter == 0) {
            bool expected = true;
            if (!size_growing_.compare_exchange_strong(expected, false,
                    std::memory_order_release, std::memory_order_relaxed)) {
                // Another thread is currently resizing, we just need to wait for them to finish
                while (size_growing_.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                registry_size = registry_size_.load(std::memory_order_acquire);
                counter = registry_size;
            }
            // We got the swap, so time to grow the array
            size_t old_size = registry_size_.load(std::memory_order_acquire);
            size_t new_size = old_size + INITIAL_CAPACITY;
            auto new_buffers = std::make_unique<std::atomic<ChainBuffer*>[]>(new_size);
            // Copy existing pointers
            for (size_t i = 0; i < old_size; ++i) {
                new_buffers[i].store(buffers_[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
            }
            // Initialize new slots
            for (size_t i = old_size; i < new_size; ++i) {
                new_buffers[i].store(nullptr, std::memory_order_relaxed);
            }
            buffers_ = std::move(new_buffers);
            registry_size_.store(new_size, std::memory_order_release);
            next_index_.store(old_size, std::memory_order_release);
            size_growing_.store(false, std::memory_order_release);
            registry_size = new_size;
            counter = registry_size;
        }
    }
}

ChainBuffer* BuffetAlligator::get_buffer(uint32_t id) {
    ChainBuffer* buffer = buffers_[id % registry_size_.load(std::memory_order_acquire)]
                .load(std::memory_order_acquire);
    if (buffer) {
        return buffer;
    }
    return nullptr;  // Not registered or not found
}

void BuffetAlligator::clear_buffer(uint32_t id) {
    buffers_[id % registry_size_.load(std::memory_order_acquire)].store(nullptr, std::memory_order_release);
}

} // namespace alligator
