/** --------------------------------------------------------------------------------------------------------- Buffet Alligator
 * @file alligator.cpp
 * @brief Implementation of the Alligator class.
 */
#include <alligator.hpp>
#include <memory/pressure.hpp>
#include <memory/tracker.hpp>
#include <containers/bitplane.hpp>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Next Slice ID
 * @brief Generate the next available slice ID.
 * @return The next available slice ID.
 */
SliceId Alligator::next_id() {
    while (true) {
        uint32_t current = static_cast<uint32_t>(
            next_slice_.fetch_add(1, std::memory_order_relaxed) & ((1 << POOL_BITS) - 1)
        );
        if (!occupancy_->test_and_set(current)) {
            return static_cast<SliceId>(current);
        }
    }
}
/** ------------------------------------------------------------------------------------------- Worker Thread
 * @struct WorkerThread
 * @brief One pool worker; slots 0..requested-1 hold the live set, and a worker whose slot
 * index is at or beyond the requested count decommissions itself.
 */
struct Alligator::WorkerThread {
    /// @brief Pointer to the task queue from which this worker thread fetches tasks.
    moodycamel::BlockingConcurrentQueue<Task>* task_queue;
    /// @brief Pointer to the atomic variable tracking the requested number of threads.
    std::atomic<size_t>* requested_thread_count;
    /// @brief Pointer to the array of worker threads.
    std::array<std::atomic<WorkerThread*>, 32>* worker_threads_;
    /// @brief Signal to indicate when the worker thread should stop execution.
    std::atomic<bool>* stop_signal{nullptr};
    /// @brief This worker's slot in the array, which is also its decommission order.
    size_t index;
    /// @brief The thread object, started by start() once the worker is published in its slot.
    std::thread thread_;
    /** ----------------------------------------------------------------------------- Work Loop
     * @brief Fills empty slots below the requested count, scales the request from queue depth,
     * runs tasks, and exits once its own slot falls outside the requested count.
     */
    void work() {
        while (!stop_signal->load(std::memory_order_acquire)) {
            const size_t requested = requested_thread_count->load(std::memory_order_acquire);
            if (index >= requested) {
                break;
            }
            for (size_t slot = 0; slot < requested; ++slot) {
                if (worker_threads_->at(slot).load(std::memory_order_acquire) != nullptr) {
                    continue;
                }
                WorkerThread* fresh = new WorkerThread(
                    task_queue, requested_thread_count, worker_threads_, stop_signal, slot);
                WorkerThread* expected = nullptr;
                if (worker_threads_->at(slot).compare_exchange_strong(
                    expected, fresh, std::memory_order_acq_rel, std::memory_order_acquire)
                ) {
                    fresh->start();
                } else {
                    delete fresh;
                }
            }
            const size_t current_tasks = task_queue->size_approx();
            if (current_tasks > 2 * requested && requested < worker_threads_->size()) {
                size_t expected = requested;
                requested_thread_count->compare_exchange_strong(
                    expected, requested + 1, std::memory_order_acq_rel, std::memory_order_relaxed);
            } else if (current_tasks == 0 && requested > 1) {
                size_t expected = requested;
                requested_thread_count->compare_exchange_strong(
                    expected, requested - 1, std::memory_order_acq_rel, std::memory_order_relaxed);
            }
            Task task;
            if (task_queue->try_dequeue(task)) {
                task.execute();
                if (task.after_this_task != nullptr) {
                    task_queue->enqueue(std::move(*task.after_this_task));
                }
            } else {
                std::this_thread::yield();
            }
        }
        WorkerThread* self = this;
        worker_threads_->at(index).compare_exchange_strong(
            self, nullptr, std::memory_order_acq_rel, std::memory_order_relaxed);
    }
    /** ----------------------------------------------------------------------------- Start
     * @brief Starts the thread; called only after the worker is published in its slot.
     */
    void start() {
        thread_ = std::thread(&WorkerThread::work, this);
    }
    /** ----------------------------------------------------------------------------- Constructor
     * @brief Constructs an unstarted worker bound to one slot.
     * @param task_queue Pointer to the task queue.
     * @param requested_thread_count Pointer to the atomic requested thread count.
     * @param worker_threads Pointer to the array of worker threads.
     * @param stop The pool-wide stop signal.
     * @param slot This worker's index in the array.
     */
    WorkerThread(
        moodycamel::BlockingConcurrentQueue<Task>* task_queue,
        std::atomic<size_t>* requested_thread_count,
        std::array<std::atomic<WorkerThread*>, 32>* worker_threads,
        std::atomic<bool>* stop,
        size_t slot
    ) : task_queue(task_queue)
    , requested_thread_count(requested_thread_count)
    , worker_threads_(worker_threads)
    , stop_signal(stop)
    , index(slot) {}
    /** ----------------------------------------------------------------------------- Destructor
     * @brief Destroys the WorkerThread object and stops the thread.
     */
    ~WorkerThread() = default;
    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;
};
/** --------------------------------------------------------------------------------------------------------- GatorBuf
 * @struct Alligator::GatorBuf
 * @brief Internal buffer structure used by the Alligator class.
 */
struct Alligator::GatorBuf {
    /// @brief Pointer to the allocated buffer.
    void* data;
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs a GatorBuf with the specified size, allocating aligned memory.
     * @param size The size of the buffer to allocate.
     */
    GatorBuf(size_t size) : data(std::aligned_alloc(64, size)) {}
    GatorBuf() : data(nullptr) {}
    ~GatorBuf() { std::free(data); }
};
/** --------------------------------------------------------------------------------------------------------- Alligator Constructor
 * @brief Constructs a new Alligator object and initializes its resources.
 */
Alligator::Alligator() {
    plates_ = new GatorBuf((1 << POOL_BITS) * sizeof(Placemat::Plate*));
    host_ptrs_ = new GatorBuf((1 << POOL_BITS) * sizeof(HostPtr));
    const Placemat* table_placement = VulkanKernel::table_placement();
    const size_t table_bytes = (1 << POOL_BITS) * sizeof(GPUBuf);
    auto [table_host, table_substrate] = table_placement->allocate()(
        table_bytes, table_placement->get_context()());
    gpubufs_ = new Placemat::Plate(
        const_cast<Placemat*>(table_placement), table_bytes, table_host, table_substrate, true);
    occupancy_ = std::make_unique<ConcurrentBitplane>(1 << POOL_BITS);
    WorkerThread* worker = new WorkerThread(
        &task_queue_,
        &requested_thread_count_,
        &worker_threads_,
        &stop_signal_,
        0
    );
    worker_threads_[0].store(worker, std::memory_order_release);
    worker->start();
}
/** --------------------------------------------------------------------------------------------------------- Claim
 * @brief Claims a slice from the occupancy bitplane.
 * @return The ID of the claimed slice.
 */
Placemat::Plate*& Alligator::plate(SliceId slice_id) {
    return *reinterpret_cast<Placemat::Plate**>(static_cast<char*>(plates_->data) + slice_id.id_ * sizeof(Placemat::Plate*));
}
/** --------------------------------------------------------------------------------------------------------- Plate (const)
 * @brief Retrieves the plate associated with the given slice ID.
 * @param slice_id The ID of the slice.
 * @return A pointer to the plate if it exists, nullptr otherwise.
 */
const Placemat::Plate*& Alligator::plate(const SliceId& slice_id) const {
    return const_cast<const Placemat::Plate*&>(
        *reinterpret_cast<const Placemat::Plate* const*>(
            static_cast<char*>(plates_->data) + slice_id.id_ * sizeof(Placemat::Plate*)
        )
    );
}
/** --------------------------------------------------------------------------------------------------------- HostPtr
 * @brief Retrieves the host pointer entry stored for the given slice ID.
 * @param slice_id The ID of the slice.
 * @return The host pointer entry's slot in the value table.
 */
HostPtr* Alligator::host_ptr(SliceId slice_id) {
    return reinterpret_cast<HostPtr*>(static_cast<char*>(host_ptrs_->data) + slice_id.id_ * sizeof(HostPtr));
}
/** --------------------------------------------------------------------------------------------------------- HostPtr (const)
 * @brief Retrieves the host pointer entry stored for the given slice ID.
 * @param slice_id The ID of the slice.
 * @return The host pointer entry's slot in the value table.
 */
const HostPtr* Alligator::host_ptr(const SliceId& slice_id) const {
    return reinterpret_cast<const HostPtr*>(static_cast<const char*>(host_ptrs_->data) + slice_id.id_ * sizeof(HostPtr));
}
/** --------------------------------------------------------------------------------------------------------- GPUBuf
 * @brief Retrieves the GPU buffer entry stored for the given slice ID.
 * @param slice_id The ID of the slice.
 * @return The GPUBuf entry's slot in the value table.
 */
GPUBuf* Alligator::gpubuf(SliceId slice_id) {
    return reinterpret_cast<GPUBuf*>(static_cast<char*>(gpubufs_->host_ptr) + slice_id.id_ * sizeof(GPUBuf));
}
/** --------------------------------------------------------------------------------------------------------- GPUBuf (const)
 * @brief Retrieves the GPU buffer entry stored for the given slice ID.
 * @param slice_id The ID of the slice.
 * @return The GPUBuf entry's slot in the value table.
 */
const GPUBuf* Alligator::gpubuf(const SliceId& slice_id) const {
    return reinterpret_cast<const GPUBuf*>(static_cast<const char*>(gpubufs_->host_ptr) + slice_id.id_ * sizeof(GPUBuf));
}
/** --------------------------------------------------------------------------------------------------------- Destroy
 * @brief Destroys the resources associated with the given slice ID.
 * @param slice_id The ID of the slice.
 */
void Alligator::destroy(SliceId slice_id) {
    Placemat::Plate* p = plate(slice_id);
    plate(slice_id) = nullptr;
    *gpubuf(slice_id) = GPUBuf{};
    *host_ptr(slice_id) = HostPtr{};
    if (!occupancy_->test_and_clear(slice_id)) {
        ALLIGATOR_THROW("Went to destroy a slice in a slot and nothing was there!!");
    }
    if (p != nullptr) {
        Placemat::call_free_later(p);
    }
}
/** --------------------------------------------------------------------------------------------------------- Host Table
 * @brief The host pointer table's first entry, indexed by pool index at an 8-byte stride.
 * @return The table base.
 */
const HostPtr* Alligator::host_table() {
    return static_cast<const HostPtr*>(inst().host_ptrs_->data);
}
/** --------------------------------------------------------------------------------------------------------- GPU Table
 * @brief The shared GPUBuf table's host mapping.
 * @return The table base.
 */
const GPUBuf* Alligator::gpu_table() {
    return static_cast<const GPUBuf*>(inst().gpubufs_->host_ptr);
}
/** --------------------------------------------------------------------------------------------------------- GPU Table Address
 * @brief The shared GPUBuf table's device address, 0 without a compute device.
 * @return The device address.
 */
uint64_t Alligator::gpu_table_address() {
    return inst().gpubufs_->device_base;
}
/** --------------------------------------------------------------------------------------------------------- Instance
 * @brief Retrieves the singleton instance of the Alligator.
 * @return A reference to the Alligator instance.
 */
Alligator& Alligator::inst() {
    // The tracker and its registry come first so they outlive the worker join in the destructor.
    static const size_t tracker_ready = Memory::total_allocations();
    static Alligator instance;
    static_cast<void>(tracker_ready);
    return instance;
}
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Destroys the Alligator instance and signals it to stop.
 */
Alligator::~Alligator() {
    requested_thread_count_.store(0, std::memory_order_release);
    stop_signal_.store(true, std::memory_order_release);
    for (std::atomic<WorkerThread*>& slot : worker_threads_) {
        WorkerThread* worker = slot.load(std::memory_order_acquire);
        if (worker != nullptr && worker->thread_.joinable()) {
            worker->thread_.join();
        }
    }
}
} // namespace buffetalligator
