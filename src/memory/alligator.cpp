/** --------------------------------------------------------------------------------------------------------- Buffet Alligator
 * @file alligator.cpp
 * @brief Implementation of the Alligator class.
 */
#include <alligator.hpp>
#include <loggingutils.hpp>
#include <memory/pressure.hpp>
#include <memory/tracker.hpp>
#include <containers/bitplane.hpp>
#include <memory/plate.hpp>
#include <chrono>
#include <new>

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
/** --------------------------------------------------------------------------------------------------------- Maybe Wakeup
 * @brief Wakes up the Alligator if necessary.
 */
void Alligator::maybe_wakeup() {
    if (active_workers_.load(std::memory_order_acquire) == 0) {
        thread_change_queue_.enqueue(ThreadChangeReq{
            std::make_unique<size_t>(0),
            nullptr
        });
    }
}
/** ------------------------------------------------------------------------------------------- Worker Thread
 * @struct WorkerThread
 * @brief One pool worker; slots 0..requested-1 hold the live set, and a worker whose slot
 * index is at or beyond the requested count decommissions itself.
 */
struct Alligator::WorkerThread {
    /// @brief The owning Alligator, handed in so the thread never touches inst().
    Alligator* alligator;
    /// @brief The unique identifier for this worker thread.
    int id;
    /// @brief Local stop flag for this worker thread.
    std::atomic<bool> stop{false};
    /// @brief The thread object, started by start() once the worker is published in its slot.
    std::thread thread_;
    /** ----------------------------------------------------------------------------- Work Loop
     * @brief Fills empty slots below the requested count, scales the request from queue depth,
     * runs tasks, and exits once its own slot falls outside the requested count.
     */
    void work() {
        Alligator& al = *alligator;
        size_t yield_count = 0;
        while (!al.stop_signal_.load(std::memory_order_acquire)
               && !stop.load(std::memory_order_acquire)) {
            const size_t current_tasks = al.task_queue_.size_approx();
            size_t active_threads = al.active_workers_.load(std::memory_order_acquire);
            if (current_tasks > 2 * active_threads
                && active_threads < al.max_thread_count_.load(std::memory_order_acquire)
            ) {
                al.thread_change_queue_.enqueue({
                    std::make_unique<size_t>(active_threads),
                    nullptr
                });
            }
            Task task;
            if (al.task_queue_.try_dequeue(task)) {
                task.execute();
                if (task.after_this_task != nullptr) {
                    al.task_queue_.enqueue(std::move(*task.after_this_task));
                }
                yield_count = 0;
            } else {
                if (++yield_count > SIZE_MAX >> 14) {
                    LOG_TRACE_STREAM << "Alligator: shutting down worker thread " << id;
                    size_t desired = active_threads - 1;
                    if (al.active_workers_.compare_exchange_strong(
                        active_threads, desired, std::memory_order_acq_rel
                    )) {
                        al.thread_change_queue_.enqueue({
                            nullptr,
                            std::make_unique<int>(id)
                        });
                        return;
                    }
                }
                std::this_thread::yield();
            }
        }
        al.active_workers_.fetch_sub(1, std::memory_order_release);
        al.thread_change_queue_.enqueue({
            nullptr,
            std::make_unique<int>(id)
        });
    }
    /** ----------------------------------------------------------------------------- Start
     * @brief Starts the thread; called only after the worker is published in its slot.
     */
    void start() {
        thread_ = std::thread(&WorkerThread::work, this);
    }
    /** ----------------------------------------------------------------------------- Constructor
     * @brief Constructs an unstarted worker.
     * @param alligator_ The owning Alligator.
     * @param id_ This worker's key in the worker map.
     */
    WorkerThread(Alligator* alligator_, int id_) : alligator(alligator_), id(id_) {}
    /** ----------------------------------------------------------------------------- Destructor
     * @brief Destroys the WorkerThread object and stops the thread.
     */
    ~WorkerThread() {
        stop.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;
};
/** --------------------------------------------------------------------------------------------------------- Thread Changer
 * @struct ThreadChanger
 * @brief Manages the thread that handles dynamic changes to the number of worker and waiter threads.
 */
struct Alligator::ThreadChanger {
    /// @brief The owning Alligator, handed in so the thread never touches inst().
    Alligator* alligator;
    /// @brief The thread object for the thread changer.
    std::thread thread_;
    /// @brief The stop signal for the thread changer.
    std::atomic<bool> stop{false};
    /** ------------------------------------------------------------------------------------------- Work Loop
     * @brief The main work loop for the thread changer.
     */
    void work() {
        Alligator& al = *alligator;
        while (!al.stop_signal_.load(std::memory_order_acquire)
                && !stop.load(std::memory_order_acquire)
        ) {
            ThreadChangeReq req;
            if (al.thread_change_queue_.wait_dequeue_timed(
                req, std::chrono::milliseconds(100))
            ) {
                if (req.shutdown_id) {
                    int id_to_shutdown = *req.shutdown_id;
                    auto it = al.worker_threads_.find(id_to_shutdown);
                    if (it != al.worker_threads_.end()) {
                        al.worker_threads_.erase(it);
                    }
                    continue;
                }
                if (req.current_active) {
                    if (*req.current_active !=
                            al.active_workers_.load(std::memory_order_acquire)
                    ) {
                        continue;
                    }
                    al.active_workers_.fetch_add(1, std::memory_order_acq_rel);
                    int new_id = rand();
                    while (al.worker_threads_.find(new_id)
                           != al.worker_threads_.end()
                    ) {
                        new_id = rand();
                    }
                    LOG_TRACE_STREAM << "Alligator: creating new worker thread " << new_id;
                    auto new_worker = std::make_unique<WorkerThread>(&al, new_id);
                    WorkerThread* worker_ptr = new_worker.get();
                    al.worker_threads_[new_id] = std::move(new_worker);
                    worker_ptr->start();
                    continue;
                }
                for (auto& [id, worker_ptr] : al.worker_threads_) {
                    if (worker_ptr) {
                        worker_ptr->stop.store(true, std::memory_order_release);
                    }
                }
            }
        }
    }
    /** ------------------------------------------------------------------------------------------- Start
     * @brief Starts the thread changer by launching its work loop in a separate thread.
     */
    void start() {
        thread_ = std::thread(&ThreadChanger::work, this);
    }
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Constructs an unstarted thread changer.
     * @param alligator_ The owning Alligator.
     */
    ThreadChanger(Alligator* alligator_) : alligator(alligator_) {}
    ~ThreadChanger() {
        stop.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    ThreadChanger(const ThreadChanger&) = delete;
    ThreadChanger& operator=(const ThreadChanger&) = delete;
};
/** ------------------------------------------------------------------------------------------- Waiting Thread
 * @struct WaitingThread
 * @brief One pool waiting thread; slots 0..requested-1 hold the live set, and a waiting
 * thread whose slot index is at or beyond the requested count decommissions itself.
 */
struct Alligator::WaitingThread {
    /// @brief The owning Alligator, handed in so the thread never touches inst().
    Alligator* alligator;
    /// @brief The thread object, started by start() once the worker is published in its slot.
    std::thread thread_;
    /// @brief The stop signal for this waiting thread.
    std::atomic<bool> stop{false};
    /** ----------------------------------------------------------------------------- Work Loop
     * @brief Fills empty slots below the requested count, scales the request from
     * queue depth, runs tasks, and exits once its own slot falls outside the
     * requested count.
     */
    void work() {
        Alligator& al = *alligator;
        while (!al.stop_signal_.load(std::memory_order_acquire)
            && !stop.load(std::memory_order_acquire)
        ) {
            Task task;
            if (al.waiting_task_queue_.wait_dequeue_timed(
                task, std::chrono::milliseconds(100))
            ) {
                task.execute();
                if (task.after_this_task != nullptr) {
                    al.task_queue_.enqueue(std::move(*task.after_this_task));
                }
            }
        }
    }
    /** ----------------------------------------------------------------------------- Start
     * @brief Starts the thread; called only after the waiting thread is published
     * in its slot.
     */
    void start() {
        thread_ = std::thread(&WaitingThread::work, this);
    }
    /** ----------------------------------------------------------------------------- Constructor
     * @brief Constructs an unstarted waiting thread.
     * @param alligator_ The owning Alligator.
     */
    WaitingThread(Alligator* alligator_) : alligator(alligator_) {}
    /** ----------------------------------------------------------------------------- Destructor
     * @brief Destroys the WaitingThread object and stops the thread.
     */
    ~WaitingThread() {
        stop.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    WaitingThread(const WaitingThread&) = delete;
    WaitingThread& operator=(const WaitingThread&) = delete;
};
/** --------------------------------------------------------------------------------------------------------- Alligator Constructor
 * @brief Constructs a new Alligator object and initializes its resources.
 */
Alligator::Alligator() {
    plates_ = static_cast<Plate**>(std::aligned_alloc(64, (1 << POOL_BITS) * sizeof(Plate*)));
    host_ptrs_ = static_cast<HostPtr*>(std::aligned_alloc(64, (1 << POOL_BITS) * sizeof(HostPtr)));
    const Placemat* table_placement = VulkanKernel::table_placement();
    const size_t table_bytes = (1 << POOL_BITS) * sizeof(GPUBuf);
    auto [table_host, table_substrate] = table_placement->allocate()(
        table_bytes, table_placement->get_context()());
    gpu_table_ = static_cast<GPUBuf*>(table_host);
    occupancy_ = std::make_unique<ConcurrentBitplane>(1 << POOL_BITS);
    gpubufs_ = new Plate(const_cast<Placemat*>(table_placement), table_bytes, table_substrate, true);
    plate(SliceId(gpubufs_->slice_id.load(std::memory_order_acquire))) = gpubufs_;
    for (size_t i = 0; i < std::thread::hardware_concurrency(); ++i) {
        waiting_threads_.emplace_back(std::make_unique<WaitingThread>(this));
        waiting_threads_.back()->start();
    }
    thread_changer_ = std::make_unique<ThreadChanger>(this);
    thread_changer_->start();
}
/** --------------------------------------------------------------------------------------------------------- Claim
 * @brief Claims a slice from the occupancy bitplane.
 * @return The ID of the claimed slice.
 */
Plate*& Alligator::plate(SliceId slice_id) {
    return plates_[slice_id.id_];
}
/** --------------------------------------------------------------------------------------------------------- Plate (const)
 * @brief Retrieves the plate associated with the given slice ID.
 * @param slice_id The ID of the slice.
 * @return A pointer to the plate if it exists, nullptr otherwise.
 */
const Plate*& Alligator::plate(const SliceId& slice_id) const {
    return const_cast<const Plate*&>(plates_[slice_id.id_]);
}
/** --------------------------------------------------------------------------------------------------------- HostPtr
 * @brief Retrieves the host pointer entry stored for the given slice ID.
 * @param slice_id The ID of the slice.
 * @return The host pointer entry's slot in the value table.
 */
HostPtr* Alligator::host_ptr(SliceId slice_id) {
    return host_ptrs_ + slice_id.id_;
}
/** --------------------------------------------------------------------------------------------------------- HostPtr (const)
 * @brief Retrieves the host pointer entry stored for the given slice ID.
 * @param slice_id The ID of the slice.
 * @return The host pointer entry's slot in the value table.
 */
const HostPtr* Alligator::host_ptr(const SliceId& slice_id) const {
    return host_ptrs_ + slice_id.id_;
}
/** --------------------------------------------------------------------------------------------------------- GPUBuf
 * @brief Retrieves the GPU buffer entry stored for the given slice ID.
 * @param slice_id The ID of the slice.
 * @return The GPUBuf entry's slot in the value table.
 */
GPUBuf* Alligator::gpubuf(SliceId slice_id) {
    return gpu_table_ + slice_id.id_;
}
/** --------------------------------------------------------------------------------------------------------- GPUBuf (const)
 * @brief Retrieves the GPU buffer entry stored for the given slice ID.
 * @param slice_id The ID of the slice.
 * @return The GPUBuf entry's slot in the value table.
 */
const GPUBuf* Alligator::gpubuf(const SliceId& slice_id) const {
    return gpu_table_ + slice_id.id_;
}
/** --------------------------------------------------------------------------------------------------------- Destroy
 * @brief Destroys the resources associated with the given slice ID.
 * @param slice_id The ID of the slice.
 */
void Alligator::destroy(SliceId slice_id) {
    Plate* p = plate(slice_id);
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
    return inst().host_ptrs_;
}
/** --------------------------------------------------------------------------------------------------------- GPU Table
 * @brief The shared GPUBuf table's host mapping.
 * @return The table base.
 */
const GPUBuf* Alligator::gpu_table() {
    return inst().gpu_table_;
}
/** --------------------------------------------------------------------------------------------------------- GPU Table Address
 * @brief The shared GPUBuf table's device address, 0 without a compute device.
 * @return The device address.
 */
uint64_t Alligator::gpu_table_address() {
    const GetGPUBufMethod get_gpu_buf = inst().gpubufs_->placemat->get_gpu_buf();
    return get_gpu_buf == nullptr ? 0 : get_gpu_buf(inst().gpubufs_->substrate_handle).address;
}
/** --------------------------------------------------------------------------------------------------------- Alligator Storage
 * @brief Raw storage for the singleton; zero-initialized, so it exists before any dynamic init runs.
 */
alignas(Alligator) static unsigned char alligator_storage[sizeof(Alligator)];
/// @brief Count of live AlligatorInitializer objects, one per including translation unit.
static size_t alligator_initializer_count = 0;
/** --------------------------------------------------------------------------------------------------------- Alligator Initializer
 * @brief The first including translation unit constructs the logger, tracker and built-ins, then the Alligator.
 */
AlligatorInitializer::AlligatorInitializer() {
    if (alligator_initializer_count++ == 0) {
        static_cast<void>(threadsafe_logger::logging::GlobalLoggingContext::instance());
        static_cast<void>(threadsafe_logger::logging::GlobalLoggingContext::progress_mutex());
        static_cast<void>(Memory::total_allocations());
        static_cast<void>(BuffetMenu::get("heap"));
        new (alligator_storage) Alligator();
    }
}
/** --------------------------------------------------------------------------------------------------------- Alligator Finalizer
 * @brief The last including translation unit to tear down destroys the Alligator.
 */
AlligatorInitializer::~AlligatorInitializer() {
    if (--alligator_initializer_count == 0) {
        Alligator::inst().~Alligator();
    }
}
/** --------------------------------------------------------------------------------------------------------- Instance
 * @brief Retrieves the singleton instance of the Alligator.
 * @return A reference to the Alligator instance.
 */
Alligator& Alligator::inst() {
    return *std::launder(reinterpret_cast<Alligator*>(alligator_storage));
}
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Destroys the Alligator instance and signals it to stop.
 */
Alligator::~Alligator() {
    thread_changer_->stop.store(true, std::memory_order_release);
    stop_signal_.store(true, std::memory_order_release);
    if (thread_changer_ && thread_changer_->thread_.joinable()) {
        thread_changer_->thread_.join();
    }
    worker_threads_.clear();
    waiting_threads_.clear();
}
} // namespace buffetalligator
