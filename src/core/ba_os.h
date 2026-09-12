#pragma once
/** --------------------------------------------------------------------------------------------------------- OS Interface
 * @file ba_os.h
 * @brief Defines private operating-system memory and synchronization services.
 */
#include <stdint.h>
#include <stddef.h>
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
/** --------------------------------------------------------------------------------------------------------- System Information
 * @brief Contains a fresh snapshot of machine capacity and process constraints.
 */
typedef struct ba_sysinfo {
    uint64_t page; ///< Native page size.
    uint64_t large_page; ///< Reported usable large-page size.
    uint64_t granule; ///< Slab allocation granule.
    uint32_t cache_line; ///< Hardware data-cache line size.
    uint32_t hw_threads; ///< Available hardware threads.
    uint64_t physical; ///< Physical memory bytes.
    uint64_t available; ///< Available system memory bytes.
    uint64_t limit; ///< Effective process memory limit.
    uint64_t rss; ///< Process resident footprint.
} ba_sysinfo_t;
/** --------------------------------------------------------------------------------------------------------- Pressure
 * @brief Identifies the current memory pressure level.
 */
typedef enum { BA_PRESSURE_NONE, BA_PRESSURE_WARN, BA_PRESSURE_CRITICAL } ba_pressure_t;
/** --------------------------------------------------------------------------------------------------------- Event
 * @brief Provides an opaque auto-reset wake event.
 */
typedef struct ba_event ba_event_t;
/** --------------------------------------------------------------------------------------------------------- Mutex
 * @brief Provides an opaque blocking mutex for infrequent shared mutations.
 */
typedef struct ba_mutex ba_mutex_t;
typedef void (*ba_thread_fn)(void*);
typedef void (*ba_tls_dtor)(void*);
/** --------------------------------------------------------------------------------------------------------- Probe
 * @brief Refreshes every system information field.
 */
void ba_os_probe(ba_sysinfo_t* out);
/** --------------------------------------------------------------------------------------------------------- Map
 * @brief Obtains aligned zero-filled pages or returns NULL on failure.
 */
void* ba_os_map(uint64_t bytes, uint64_t alignment, int want_large_pages);
/** --------------------------------------------------------------------------------------------------------- Reserve
 * @brief Reserves inaccessible page-aligned address space without committing backing storage.
 */
void* ba_os_reserve(uint64_t bytes);
/** --------------------------------------------------------------------------------------------------------- Commit
 * @brief Makes a page-aligned reserved range writable without moving existing pages.
 */
int ba_os_commit(void* base, uint64_t bytes);
/** --------------------------------------------------------------------------------------------------------- Unmap
 * @brief Returns a mapping to the operating system.
 */
void ba_os_unmap(void* base, uint64_t bytes);
/** --------------------------------------------------------------------------------------------------------- Rezero
 * @brief Replaces owned pages with fresh zero-filled pages.
 */
int ba_os_rezero(void* base, uint64_t bytes);
/** --------------------------------------------------------------------------------------------------------- Pressure
 * @brief Samples operating-system memory pressure.
 */
ba_pressure_t ba_os_pressure(void);
/** --------------------------------------------------------------------------------------------------------- Time
 * @brief Returns monotonic nanoseconds.
 */
uint64_t ba_os_now_ns(void);
/** --------------------------------------------------------------------------------------------------------- Start Thread
 * @brief Starts a detached thread invoking the supplied function.
 */
int ba_os_thread_start(ba_thread_fn function, void* argument);
/** --------------------------------------------------------------------------------------------------------- Create Mutex
 * @brief Allocates and initializes a blocking mutex.
 */
ba_mutex_t* ba_mutex_create(void);
/** --------------------------------------------------------------------------------------------------------- Destroy Mutex
 * @brief Releases a mutex after its final user exits.
 */
void ba_mutex_destroy(ba_mutex_t* mutex);
/** --------------------------------------------------------------------------------------------------------- Lock Mutex
 * @brief Acquires exclusive ownership and parks contending threads.
 */
void ba_mutex_lock(ba_mutex_t* mutex);
/** --------------------------------------------------------------------------------------------------------- Unlock Mutex
 * @brief Publishes protected writes and releases exclusive ownership.
 */
void ba_mutex_unlock(ba_mutex_t* mutex);
/** --------------------------------------------------------------------------------------------------------- Create Event
 * @brief Allocates and initializes an opaque event.
 */
ba_event_t* ba_event_create(void);
/** --------------------------------------------------------------------------------------------------------- Destroy Event
 * @brief Destroys an event after its final waiter exits.
 */
void ba_event_destroy(ba_event_t* event);
/** --------------------------------------------------------------------------------------------------------- Initialize Event
 * @brief Initializes event synchronization primitives.
 */
void ba_event_init(ba_event_t* event);
/** --------------------------------------------------------------------------------------------------------- Wait Event
 * @brief Consumes a signal or waits until the relative timeout expires.
 */
void ba_event_wait(ba_event_t* event, uint64_t timeout_ns);
/** --------------------------------------------------------------------------------------------------------- Signal Event
 * @brief Records a persistent signal and wakes a waiter.
 */
void ba_event_signal(ba_event_t* event);
/** --------------------------------------------------------------------------------------------------------- TLS Key
 * @brief Registers a callback for thread-local cleanup.
 */
int ba_os_tls_key(ba_tls_dtor destructor);
/** --------------------------------------------------------------------------------------------------------- TLS Value
 * @brief Registers this thread's destructor argument.
 */
void ba_os_tls_set(void* value);
/** --------------------------------------------------------------------------------------------------------- Yield
 * @brief Yields the current processor to another runnable thread.
 */
void ba_os_yield(void);
