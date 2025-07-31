#pragma once

#include <alligator/buffer/buffer.hpp>
#include <thread>
#include <functional>
#include <memory>
#include <cstring>
#include <algorithm>
#include <vector>
#include <chrono>
#ifdef __APPLE__
#include <pthread.h>
#include <sched.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace alligator {

// Forward declarations
class ChainBuffer;
class HeapBuffer;

/**
 * @class BuffetAlligator  (ASCII art attributed to Anthropic's Claude AI)
 * @brief The Buffet Alligator - all-you-can-allocate memory buffet!
 * 
 *         🍽️ BUFFET ALLIGATOR 🍽️
 *    ╔═══════════════════════════╗
 *    ║  ALL-YOU-CAN-ALLOCATE!   ║
 *    ╚═══════════════════════════╝
 *          .-._   _ _ _ _ _ _ _ _
 *       .-'   `-.|_|_|_|_|_|_|_|_|
 *      /     \   🥘 HEAP SPECIAL |
 *     |  👁 👁 |  🍜 CUDA DELUXE  |
 *     |   <   |  🍕 METAL SUPREME|
 *      \  ~~~ /  🥗 ZERO-COPY SALAD
 *       `-..-'   🍰 SHARED DESSERT
 *        | |     ________________|
 *       /| |\    
 *      🍴| |🍴   "Come hungry,
 *     '  |_|  `   leave allocated!"
 *        
 *    🐊 BUFFET ALLIGATOR 🐊
 *   ~~~~~~~~~~~~~~~~~~~~~~
 *   < Step right up to the >
 *   < memory smorgasbord! >
 *   ~~~~~~~~~~~~~~~~~~~~~~
 *          \
 *           \    .--.
 *            \  |👁_👁|
 *               |:_/ |  🍳
 *              //   \ \
 *             (|  🥄 | )
 *            /'\_   _/`\
 *            \___)=(___/
 * 
 * The Buffet Alligator serves up a delicious spread of memory buffers!
 * From hearty HEAP allocations to exotic CUDA delicacies, we've got 
 * all your memory needs covered. No reservations needed - just grab
 * a plate and dig in!
 * 
 * Today's Specials:
 * - HEAP Buffer: Classic comfort food, always satisfying
 * - CUDA Buffer: GPU-accelerated for those with refined tastes  
 * - Metal Buffer: Apple's signature dish, smooth as silk
 * - Shared Memory: Perfect for sharing with friends
 * - Zero-Copy Salad: Light, efficient, and oh-so-fast
 * 
 * ⚠️ WARNING: May cause extreme performance satisfaction
 */
enum class BFType : uint32_t{
    HEAP                 = 1,   ///< Heap-allocated buffer
    CUDA                 = 2,   ///< CUDA buffer (explicit, will error if not available)
    METAL                = 3,   ///< Metal buffer (explicit, will error if not available)
    VULKAN               = 4,   ///< Vulkan buffer (explicit, will error if not available)
    SHARED               = 5,   ///< Shared memory buffer
    FILE_BACKED          = 6,   ///< File-backed buffer
    SWIFT                = 7,   ///< Swift buffer
    THUNDERBOLT_DMA      = 9,   ///< Thunderbolt DMA buffer
    TCP                  = 10,  ///< TCP buffer (auto-selects: Swift on Apple, ASIO otherwise)
    UDP                  = 11,  ///< UDP buffer (auto-selects: Swift on Apple, ASIO otherwise)
    QUIC                 = 12,  ///< QUIC buffer (auto-selects: Swift on Apple, ASIO otherwise)
    GPU                  = 13,  ///< GPU buffer (auto-selects: Metal on Apple, CUDA if available, else Vulkan)
    
    // Explicit network buffer types
    ASIO_TCP             = 14,  ///< ASIO TCP buffer (explicit)
    ASIO_UDP             = 15,  ///< ASIO UDP buffer (explicit)
    ASIO_QUIC            = 16,  ///< ASIO QUIC buffer (explicit)
    SWIFT_TCP            = 17,  ///< Swift TCP buffer
    SWIFT_UDP            = 18,  ///< Swift UDP buffer
    SWIFT_QUIC           = 19   ///< Swift QUIC buffer
};
class BuffetAlligator {
private:
    static constexpr size_t INITIAL_CAPACITY = 1024;
    std::atomic<uint64_t> registry_size_{INITIAL_CAPACITY};  ///< Current size of the buffer registry
    std::atomic<bool> size_growing_{false};  ///< Gate for adjusting the size of the registry
    std::atomic<uint64_t> next_index_{0};  ///< Next available index for registration
    std::unique_ptr<std::atomic<ChainBuffer*>[]> buffers_{};
    std::unique_ptr<std::thread> garbage_collector_thread_{nullptr};  ///< Thread for garbage collection
    BuffetAlligator();
    std::atomic<bool> stop_{false};
    void garbage_collector();
public:
    ~BuffetAlligator();
    static BuffetAlligator& instance() {
        static BuffetAlligator registry;
        return registry;
    }
    ChainBuffer* allocate_buffer(uint32_t size, BFType type);
    ChainBuffer* get_buffer(uint32_t id);
    void clear_buffer(uint32_t id);
};

inline BuffetAlligator& get_buffet_alligator() {
    return BuffetAlligator::instance();
};

} // namespace alligator
