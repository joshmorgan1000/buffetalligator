/** --------------------------------------------------------------------------------------------------------- Generational Reference Counter
 * @file genrefcounter.cpp
 * @brief Implementation of the generational reference counter.
 */
#include <alligator.hpp>
#include <core/genrefcounter.hpp>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- On Zero Registry
 * @brief Implementation of the OnZero registry for generational reference counters.
 * @return Reference to the OnZero registry array.
 */
std::array<OnZero, 0x4000>& GenRefCounter::on_zero_registry() {
    static std::array<OnZero, 0x4000> registry;
    return registry;
}
/** --------------------------------------------------------------------------------------------------------- Next On Zero Index
 * @brief Implementation of the next OnZero index for generational reference counters.
 * @return Reference to the next OnZero index atomic variable.
 */
std::atomic<uint16_t>& GenRefCounter::next_on_zero_index() {
    static std::atomic<uint16_t> index{0};
    return index;
}
/** --------------------------------------------------------------------------------------------------------- Add On Zero
 * @brief Adds a callback to the OnZero registry.
 * @param callback The callback function to be added.
 */
void GenRefCounter::add_on_zero(void (*callback)(void*)) {
    uint16_t index = next_on_zero_index().fetch_add(1, std::memory_order_relaxed);
    if (index >= on_zero_registry().size()) THROW("OnZero registry overflow");
    std::atomic_ref<uint64_t>(
        reinterpret_cast<uint64_t&>(on_zero_registry()[index])
    ).store(reinterpret_cast<uint64_t>(callback));
    {
        std::unique_lock lock(OnZero::registry_mutex());
        OnZero::registry()[callback] = index;
    }
}
/** --------------------------------------------------------------------------------------------------------- Disarm Override
 * @brief Disarms the generational reference counter without triggering the decrement callback.
 */
void GenRefCounter::disarm_override() { id_ = 0xFFFFFFFFu; }
/** --------------------------------------------------------------------------------------------------------- Increment Override
 * @brief Increments the generational reference counter without triggering the increment callback.
 */
void GenRefCounter::increment() {
    if (id_ != 0xFFFFFFFFu) gen_threads()[id_ & 1].queue.enqueue({0, id_ >> 1});
}
/** --------------------------------------------------------------------------------------------------------- Decrement
 * @brief Decrements the generational reference counter.
 */
void GenRefCounter::decrement() {
    if (id_ != 0xFFFFFFFFu) gen_threads()[id_ & 1].queue.enqueue({1, id_ >> 1});
}
/** --------------------------------------------------------------------------------------------------------- GenThread
 * @struct GenThread
 * @brief Represents a thread handling generational reference counting.
 */
struct GenRefCounter::GenThread {
    /** ------------------------------------------------------------------------------------------- Queue Item
     * @struct QueueItem
     * @brief Represents an item in the queue for the generational reference counting thread.
     */
    struct QueueItem {
        uint32_t decrement :  1;
        uint32_t index     : 24;
    };
    /// @brief The queue for handling reference count operations.
    moodycamel::BlockingConcurrentQueue<QueueItem> queue;
    /// @brief Pointer to the thread-specific data.
    void* data = nullptr;
    /// @brief Pointer to the occupancy bitmap for the thread-specific data.
    void* occupancy = nullptr;
    /// @brief Capacity of the thread-specific data.
    size_t capacity = 0;
    /// @brief Next available index in the thread-specific data.
    uint32_t next_index = 0;
    /// @brief Mutex for synchronizing resize operations.
    std::shared_mutex resize_mutex;
    /// @brief The worker thread for handling queue items.
    std::thread worker_thread;
    /// @brief Indicates if this thread is associated with the even entry base.
    bool is_even;
    /** ------------------------------------------------------------------------------------------- Resize
     * @brief Resizes the thread-specific data and occupancy bitmap.
     * @param new_capacity The new capacity for the thread-specific data.
     * @param copy_old Indicates whether to copy the old data to the new allocation.
     */
    void resize(size_t new_capacity, bool copy_old = true) {
        size_t old_capacity = capacity;
        void* old_data = data;
        void* old_occupancy = occupancy;
        capacity = (new_capacity + 64) & ~64;
        data = std::aligned_alloc(64, capacity * sizeof(Entry));
        std::memset(data, 0, capacity * sizeof(Entry));
        occupancy = std::aligned_alloc(8, capacity >> 3);
        std::memset(occupancy, 0, capacity >> 3);
        if (copy_old && old_data && old_occupancy) {
            std::unique_lock<std::shared_mutex> lock(resize_mutex);
            std::memcpy(data, old_data, old_capacity * sizeof(Entry));
            std::memcpy(occupancy, old_occupancy, old_capacity >> 3);
        }
        if (old_data) std::free(old_data);
        if (old_occupancy) std::free(old_occupancy);
        if (is_even) Entry::bases()[0] = reinterpret_cast<Entry*>(data);
        else Entry::bases()[1] = reinterpret_cast<Entry*>(data);
    }
    /** ------------------------------------------------------------------------------------------- Try Set
     * @brief Attempts to set the occupancy bit for the given index.
     * @param index The index to set in the occupancy bitmap.
     * @return True if the bit was successfully set, false if it was already set.
     */
    bool try_set(size_t index) {
        std::shared_lock<std::shared_mutex> lock(resize_mutex);
        if (index >= capacity) {
            size_t new_capacity = capacity;
            while (index >= new_capacity) new_capacity <<= 1;
            resize(new_capacity, true);
        }
        uint32_t* occupancy_word = reinterpret_cast<uint32_t*>(occupancy) + (index >> 5);
        uint32_t mask = 1u << (index & 31);
        return ((std::atomic_ref<uint32_t>(*occupancy_word)
            .fetch_or(mask, std::memory_order_acquire)) & mask) == 0;
    }
    /** ------------------------------------------------------------------------------------------- Get Entry
     * @brief Retrieves the entry at the given index.
     * @param index The index of the entry to retrieve.
     * @return Pointer to the entry at the specified index.
     */
    Entry* get_entry(size_t index) { return &reinterpret_cast<Entry*>(data)[index]; }
    /** ------------------------------------------------------------------------------------------- Get Entry (Const)
     * @brief Retrieves the entry at the given index (const version).
     * @param index The index of the entry to retrieve.
     * @return Pointer to the entry at the specified index.
     */
    const Entry* get_entry(size_t index) const { return &reinterpret_cast<Entry*>(data)[index]; }
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs a GenThread with the specified even/odd designation and on_zero callback.
     * @param even Indicates if this thread is associated with the even entry base.
     * @param on_zero Pointer to the on_zero callback for this thread.
     */
    GenThread(bool even, OnZero* on_zero) : is_even(even) {
        worker_thread = std::thread([this]() {
            while (true) {
                QueueItem item;
                if (queue.wait_dequeue_timed(item, std::chrono::milliseconds(100))) {
                    uint32_t index = item.index & 0xFFFFFF;
                    if (item.decrement) get_entry(index)->dec();
                    else get_entry(index)->inc();
                }
            }
        });
        Entry::on_zero_bases()[is_even ? 0 : 1] = on_zero;
    }
};
/** --------------------------------------------------------------------------------------------------------- Gen Threads
 * @brief Retrieves the array of GenThread instances.
 * @return Reference to the array of GenThread instances.
 */
std::array<GenRefCounter::GenThread, 2>& GenRefCounter::gen_threads() {
    static std::array<GenRefCounter::GenThread, 2> threads{
        {
            GenRefCounter::GenThread(true, on_zero_registry().data()),
            GenRefCounter::GenThread(false, on_zero_registry().data())
        }
    };
    return threads;
};
/** --------------------------------------------------------------------------------------------------------- GenRefCounter Constructor
 * @brief Constructs a GenRefCounter with the specified on_zero callback and context.
 * @param on_zero Pointer to the on_zero callback function.
 * @param context Pointer to the context to be passed to the on_zero callback.
 */
GenRefCounter::GenRefCounter(
    void (*on_zero)(void*),
    void* context
) {
    uint32_t index = 0xFFFFFFFFu;
    if (on_zero) {
        std::shared_lock<std::shared_mutex> lock(OnZero::registry_mutex());
        if (OnZero::registry().find(on_zero) != OnZero::registry().end()) {
            index = OnZero::registry()[on_zero];
        }
    } else ALLIGATOR_THROW("GenRefCounter: on_zero callback is required");
}
} // namespace buffetalligator