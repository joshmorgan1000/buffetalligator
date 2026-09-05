#pragma once
/** --------------------------------------------------------------------------------------------------------- Memory Tracker
 * @file tracker.hpp
 * @brief Header for the BuffetAlligator Memory Tracker class, which provides memory tracking and leak detection
 * capabilities.
 */
#include <buffetalligator.hpp>
#include <memory/buffet.hpp>
#ifndef BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING
#define BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING 0
#endif
#if BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING
#include <source_location>
#include <chrono>
#endif

namespace buffetalligator {
class Alligator;
/** --------------------------------------------------------------------------------------------------------- Memory
 * @class Memory
 * @brief The `Memory` class provides static methods for tracking memory allocations and
 * deallocations within the BuffetAlligator library. It is used to detect memory leaks and report memory usage
 * statistics. This class is not intended to be instantiated; all methods are static.
 */
class Memory {
private:
    /// @brief AtomicContainer to track total memory allocations and deallocations
    AtomicContainer* total_allocations_;
    /// @brief AtomicContainer to track total memory allocations
    AtomicContainer* total_freed_;
    /** ------------------------------------------------------------------------------------------- PlacematDetails
     * @struct PlacematDetails
     * @brief Struct to hold detailed memory tracking information for a specific Placemat.
     */
    struct PlacematDetails {
        /// @brief AtomicContainer to track total heap allocations
        AtomicContainer* total_allocations_;
        /// @brief AtomicContainer to track total heap deallocations
        AtomicContainer* total_freed_;
        /// @brief AtomicContainer to track total GPU deallocations
        AtomicContainer* total_available_;
        /// @brief AtomicContainer to allocate memory on the heap
        AtomicContainer* allocation_method_;
        /// @brief AtomicContainer to deallocate memory on the heap
        AtomicContainer* deallocation_method_;
    };
    /// @brief Vector holding detailed memory tracking information for each Placemat.
    std::vector<PlacematDetails> placemat_details_;
#if BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING
    /** ------------------------------------------------------------------------------------------- AllocationInfo
     * @struct AllocationInfo
     * @brief Struct to hold information about a memory allocation
     */
    class AllocationInfo {
    private:
        uint64_t highdef_timestamp_;  ///< High-resolution timestamp of the allocation
        size_t size_;                 ///< Size of the allocated memory in bytes
        std::string file_;            ///< Source file where the allocation occurred
        int line_;                    ///< Line number in the source file where the allocation occurred
        std::string function_;        ///< Function name where the allocation occurred
        Buffet* ptr_;                   ///< Pointer to the allocated memory
        void (*deleter_)(Buffet*);      ///< Custom deleter function for the allocated memory
        friend class Memory;          ///< Allow Memory class to access private members
    public:
        /** ------------------------------------------------------------------------------------------- Constructor
         * @brief Constructs an AllocationInfo object with the given parameters.
         * @param size The size of the allocated memory in bytes.
         * @param location The source location where the allocation occurred.
         * @param ptr The pointer to the allocated memory.
         * @param deleter The custom deleter function for the allocated memory.
         */
        AllocationInfo(
            size_t size,
            const std::source_location& location,
            Buffet* ptr,
            void (*deleter)(Buffet*)
        ) : highdef_timestamp_(std::chrono::high_resolution_clock::now().time_since_epoch().count())
        , size_(size)
        , file_(location.file_name())
        , line_(location.line())
        , function_(location.function_name())
        , ptr_(ptr)
        , deleter_(deleter) {}
        /** ------------------------------------------------------------------------------------------- Deleted Copy/Move
         * @brief Deleted copy and move constructors and assignment operators to prevent copying or
         * moving.
         */
        AllocationInfo(const AllocationInfo&) = delete;
        AllocationInfo& operator=(const AllocationInfo&) = delete;
        AllocationInfo(AllocationInfo&& other)
        : highdef_timestamp_(other.highdef_timestamp_)
        , size_(other.size_)
        , file_(std::move(other.file_))
        , line_(other.line_)
        , function_(std::move(other.function_))
        , ptr_(other.ptr_)
        , deleter_(other.deleter_) {
            other.ptr_ = nullptr;
            other.deleter_ = nullptr;
        }
        AllocationInfo& operator=(AllocationInfo&& other) {
            if (this != &other) {
                call_free(this, size_);
                highdef_timestamp_ = other.highdef_timestamp_;
                size_ = other.size_;
                file_ = std::move(other.file_);
                line_ = other.line_;
                function_ = std::move(other.function_);
                ptr_ = other.ptr_;
                deleter_ = other.deleter_;
                other.ptr_ = nullptr;
                other.deleter_ = nullptr;
            }
            return *this;
        }
        /** ------------------------------------------------------------------------------------------- Destructor
         * @brief Destructor for AllocationInfo. Cleans up any resources if necessary.
         */
        ~AllocationInfo() = default;
        /** ------------------------------------------------------------------------------------------- Timestamp
         * @brief Returns the high-resolution timestamp of the allocation.
         * @return The high-resolution timestamp.
         */
        uint64_t timestamp() const { return highdef_timestamp_; }
        /** ------------------------------------------------------------------------------------------- Size
         * @brief Returns the size of the allocated memory in bytes.
         * @return The size of the allocated memory.
         */
        size_t size() const { return size_; }
        /** ------------------------------------------------------------------------------------------- File
         * @brief Returns the source file where the allocation occurred.
         * @return The source file name.
         */
        const std::string& file() const { return file_; }
        /** ------------------------------------------------------------------------------------------- Line
         * @brief Returns the line number in the source file where the allocation occurred.
         * @return The line number.
         */
        int line() const { return line_; }
        /** ------------------------------------------------------------------------------------------- Function
         * @brief Returns the function name where the allocation occurred.
         * @return The function name.
         */
        const std::string& function() const { return function_; }
        /** ------------------------------------------------------------------------------------------- Pointer
         * @brief Returns the pointer to the allocated memory.
         * @return The pointer to the allocated memory.
         */
        void* raw() { return ptr_; }
        /** ------------------------------------------------------------------------------------------- Const Pointer
         * @brief Returns the pointer to the allocated memory (const version).
         * @return The pointer to the allocated memory.
         */
        const void* raw() const { return ptr_; }
        /** ------------------------------------------------------------------------------------------- Call Free
         * @brief Calls the deleter function to free the allocated memory.
         */
        static void call_free(AllocationInfo* me, size_t size);
    };
    /// @brief Hash map to track allocations with their corresponding AllocationInfo
    std::unordered_map<void*, AllocationInfo> allocations_;
    /// @brief Mutex to protect access to the allocations_ map
    std::unique_ptr<AtomicMutex> allocations_mutex_ = std::make_unique<AtomicMutex>();
#endif
    /** ------------------------------------------------------------------------------------------- Constructor - Private
     * @brief Private constructor for singleton pattern.
     */
    Memory() {
        total_allocations_ = AtomicRegistry::create_global<uint64_t>("buffetalligator_allocations", uint64_t(0));
        total_freed_ = AtomicRegistry::create_global<uint64_t>("buffetalligator_freed", uint64_t(0));
        std::vector<std::unique_ptr<Placemat>>& placements = BuffetMenu::instance().placements_;
        for (auto& placement : placements) {
            PlacementDetails details;
            details.to
        }
    }
    /** ------------------------------------------------------------------------------------------- Get Instance
     * @brief Returns the singleton instance of the Memory tracker.
     * @return Reference to the Memory tracker instance.
     */
    static Memory& instance() {
        static Memory instance;
        return instance;
    }
    /// @brief Allow AllocationInfo to access private members of Memory
    friend class AllocationInfo;
    friend class Arena;
    friend class Buffet;
public:
    /** ------------------------------------------------------------------------------------------- Deleted Copy/Move
     * @brief Deleted copy and move constructors and assignment operators to prevent copying or
     * moving of the Memory tracker instance.
     */
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    Memory(Memory&&) = delete;
    Memory& operator=(Memory&&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Destructor for the Memory tracker. Cleans up any remaining allocations and reports
     * memory leaks if any.
     */
    ~Memory() = default;
    /** ------------------------------------------------------------------------------------------- Record Allocation
     * @brief Records one completed slab allocation for the owning placement.
     * @param placement The placement that allocated the slab.
     * @param size The page-rounded slab size.
     */
    static void record_allocation(const Placemat* placement, size_t size) {

    }
    /** ------------------------------------------------------------------------------------------- Record Deallocation
     * @brief Records one completed slab deallocation for the owning placement.
     * @param placement The placement that deallocated the slab.
     * @param size The page-rounded slab size.
     */
    static void record_deallocation(const Placemat& placement, size_t size) {
        instance().total_freed_->fetch_add<uint64_t>(static_cast<uint64_t>(size), std::memory_order_relaxed);
        instance().placement_freed_[placement.type()].fetch_add(size, std::memory_order_relaxed);
        if (placement.type() == buffettypes::HEAP || placement.type() == buffettypes::ALIGNED_HEAP) {
            instance().total_heap_freed_->fetch_add<uint64_t>(static_cast<uint64_t>(size), std::memory_order_relaxed);
        }
    }
    /** ------------------------------------------------------------------------------------------- Total Allocations
     * @brief Returns all slab bytes allocated through registered placements.
     * @return Total allocated bytes.
     */
    static size_t total_allocations() {
        return instance().total_allocations_->load<uint64_t>(std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Total Freed
     * @brief Returns all slab bytes returned through registered placements.
     * @return Total freed bytes.
     */
    static size_t total_freed() {
        return instance().total_freed_->load<uint64_t>(std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Placemat Usage
     * @brief Returns live slab bytes owned by one registered placement.
     * @param placement The placement to query.
     * @return Live bytes for the placement.
     */
    static size_t placement_usage(const Placemat& placement) {
        return instance().placement_allocations_[placement.type()].load(std::memory_order_relaxed) -
            instance().placement_freed_[placement.type()].load(std::memory_order_relaxed);
    }
    /** ------------------------------------------------------------------------------------------- Total Heap Allocations
     * @brief Returns the total number of bytes allocated on the heap.
     * @return Total heap allocations in bytes.
     */
    static size_t total_heap_allocations() {
        return instance().total_heap_allocations_->load<uint64_t>(std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Total GPU Allocations
     * @brief Returns the total number of bytes allocated on the GPU.
     * @return Total GPU allocations in bytes.
     */
    static size_t total_gpu_allocations() {
        return instance().total_gpu_allocations_->load<uint64_t>(std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Total Heap Freed
     * @brief Returns the total number of bytes freed from the heap.
     * @return Total heap freed in bytes.
     */
    static size_t total_heap_freed() {
        return instance().total_heap_freed_->load<uint64_t>(std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Total GPU Freed
     * @brief Returns the total number of bytes freed from the GPU.
     * @return Total GPU freed in bytes.
     */
    static size_t total_gpu_freed() {
        return instance().total_gpu_freed_->load<uint64_t>(std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Total Heap Available
     * @brief Returns the total number of bytes available on the heap.
     * @return Total heap available in bytes.
     */
    static size_t total_heap_available() {
        return instance().total_heap_available_->load<uint64_t>(std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Total GPU Available
     * @brief Returns the total number of bytes available on the GPU.
     * @return Total GPU available in bytes.
     */
    static size_t total_gpu_available() {
        return instance().total_gpu_available_->load<uint64_t>(std::memory_order_acquire);
    }
    /** ------------------------------------------------------------------------------------------- Set Total Heap Available
     * @brief Sets the total number of bytes available on the heap.
     * @param size The total heap available in bytes.
     */
    static void set_total_heap_available(size_t size) {
        instance().total_heap_available_->store<uint64_t>(size, std::memory_order_release);
    }
    /** ------------------------------------------------------------------------------------------- Set Total GPU Available
     * @brief Sets the total number of bytes available on the GPU.
     * @param size The total GPU available in bytes.
     */
    static void set_total_gpu_available(size_t size) {
        instance().total_gpu_available_->store<uint64_t>(size, std::memory_order_release);
    }
    /** ------------------------------------------------------------------------------------------- Allocate Heap
     * @brief Allocates memory on the heap with the specified size and alignment. Tracks the
     * allocation for memory leak detection.
     * @param size The size of the memory to allocate in bytes.
     * @param alignment The alignment of the memory to allocate (default is 4096).
     * @param location The source location where the allocation is requested (default is current location).
     * @return Pointer to the allocated memory.
     * @throws MemoryException if the allocation fails.
     */
    static Buffet* allocate_heap(
        size_t size
#if BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING
        , void (*deleter)(Buffet*) = nullptr
        , const std::source_location& location = std::source_location::current()
#endif
    ) {
        void* method_void = instance().heap_allocation_method_->load<void*>(std::memory_order_acquire);
        Buffet* (*method)(size_t) = reinterpret_cast<Buffet* (*)(size_t)>(method_void);
        if (method == nullptr) [[unlikely]] {
            ALLIGATOR_THROW("Memory::allocate_heap(): no heap allocation method is registered.");
        }
        Buffet* ptr = method(size);
#if BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING
        AllocationInfo info(
            size,
            location,
            ptr,
            deleter
        );
        instance().allocations_mutex_->lock();
        instance().allocations_.emplace(ptr, std::move(info));
        instance().allocations_mutex_->unlock();
#endif
        instance().total_heap_allocations_->fetch_add<uint64_t>(static_cast<uint64_t>(size), std::memory_order_relaxed);
        return ptr;
    }
    /** ------------------------------------------------------------------------------------------- Free Heap
     * @brief Frees memory previously allocated on the heap. Updates the memory tracking
     * information.
     * @param ptr Pointer to the memory to free.
     * @param size The size of the memory to free in bytes (optional, for tracking purposes).
     */
    static void free_heap(
        Buffet* ptr,
#if BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING
        const std::source_location& location = std::source_location::current()
#else
        size_t size
#endif
    ) {
#if BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING
        instance().allocations_mutex_->lock_shared();
        auto it = instance().allocations_.find(ptr);
        if (it != instance().allocations_.end()) {
            const size_t freed_size = it->second.size();
            instance().allocations_mutex_->unlock_shared();
            AllocationInfo::call_free(&it->second, freed_size);
            instance().total_heap_freed_->fetch_add(freed_size, std::memory_order_relaxed);
        } else {
            instance().allocations_mutex_->unlock_shared();
            std::string errormsg = "Memory::free_heap(): Attempted to free untracked or already "
                "freed memory at address " + std::to_string(reinterpret_cast<uintptr_t>(ptr)) + ".";
            ALLIGATOR_THROW(errormsg);
        }
#else
        void* method_void = instance().heap_deallocation_method_->load<void*>(std::memory_order_acquire);
        void (*dealloc_method)(Buffet*) = reinterpret_cast<void (*)(Buffet*)>(method_void);
        if (dealloc_method == nullptr) [[unlikely]] {
            ALLIGATOR_THROW("Memory::free_heap(): no heap deallocation method is registered.");
        }
        dealloc_method(ptr);
        instance().total_heap_freed_->fetch_add<uint64_t>(static_cast<size_t>(size), std::memory_order_relaxed);
#endif
    }
    /** ------------------------------------------------------------------------------------------- Set Heap Allocation Method
     * @brief Sets the method used for heap memory allocation. This method will be used by the
     * Memory tracker to manage heap memory.
     * @param method Pointer to the function used for heap memory allocation.
     */
    static void set_heap_allocation_method(Buffet* (*method)(size_t)) {
        void* method_void = reinterpret_cast<void*>(method);
        instance().heap_allocation_method_->store<void*>(method_void, std::memory_order_relaxed);
    }
    /** ------------------------------------------------------------------------------------------- Set Heap Deallocation Method
     * @brief Sets the method used for heap memory deallocation. This method will be used by the
     * Memory tracker to manage heap memory.
     * @param d_method Pointer to the function used for heap memory deallocation.
     */
    static void set_heap_deallocation_method(void (*d_method)(Buffet*)) {
        void* method = reinterpret_cast<void*>(d_method);
        instance().heap_deallocation_method_->store<void*>(method, std::memory_order_relaxed);
    }
    /** ------------------------------------------------------------------------------------------- Set GPU Allocation/Deallocation Methods
     * @brief Sets the methods used for GPU memory allocation and deallocation. These methods will
     * be used by the Memory tracker to manage GPU memory.
     * @param method Pointer to the function used for GPU memory allocation.
     * @param method Pointer to the function used for GPU memory deallocation.
     */
    static void set_gpu_allocation_method(Buffet* (*method)(size_t)) {
        void* method_void = reinterpret_cast<void*>(method);
        instance().gpu_allocation_method_->store<void*>(method_void, std::memory_order_release);
    }
    /** ------------------------------------------------------------------------------------------- Set GPU Deallocation Method
     * @brief Sets the method used for GPU memory deallocation. This method will be used by the
     * Memory tracker to manage GPU memory.
     * @param method Pointer to the function used for GPU memory deallocation (Buffet*).
     */
    static void set_gpu_deallocation_method(void (*method)(Buffet*)) {
        void* method_void = reinterpret_cast<void*>(method);
        instance().gpu_deallocation_method_->store<void*>(method_void, std::memory_order_release);
    }
    /** ------------------------------------------------------------------------------------------- Allocate GPU
     * @brief Allocates memory on the GPU with the specified size. Tracks the allocation for
     * memory leak detection.
     * @param size The size of the memory to allocate in bytes.
     * @param location The source location where the allocation is requested (default is current
     * location).
     * @return Pointer to the allocated GPU memory.
     * @throws MemoryException if the allocation fails or if the GPU allocation method is not set.
     */
    static Buffet* allocate_gpu(
        size_t size
#if BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING
        , void (*deleter)(Buffet*) = nullptr
        , const std::source_location& location = std::source_location::current()
#endif
    ) {
        void* method_void = instance().gpu_allocation_method_->load<void*>(std::memory_order_acquire);
        Buffet* (*alloc_method)(size_t) = reinterpret_cast<Buffet* (*)(size_t)>(method_void);
        if (alloc_method == nullptr) [[unlikely]] {
            ALLIGATOR_THROW("Memory::allocate_gpu(): no GPU allocation method is registered.");
        }
        Buffet* ptr = alloc_method(size);
#if BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING
        AllocationInfo info(
            size,
            location,
            ptr,
            deleter
        );
        instance().allocations_mutex_->lock();
        instance().allocations_.emplace(ptr, std::move(info));
        instance().allocations_mutex_->unlock();
#endif
        instance().total_gpu_allocations_->fetch_add(static_cast<size_t>(size), std::memory_order_relaxed);
        return ptr;
    }
    /** ------------------------------------------------------------------------------------------- Free GPU
     * @brief Frees memory previously allocated on the GPU. Updates the memory tracking
     * information.
     * @param ptr Pointer to the GPU memory to free.
     * @param size The size of the GPU memory to free in bytes (optional, for tracking purposes).
     */
    static void free_gpu(
        Buffet* ptr,
#if BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING
        const std::source_location& location = std::source_location::current()
#else
        size_t size
#endif
    ) {
#if BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING
        instance().allocations_mutex_->lock_shared();
        auto it = instance().allocations_.find(ptr);
        if (it != instance().allocations_.end()) {
            const size_t freed_size = it->second.size();
            instance().allocations_mutex_->unlock_shared();
            AllocationInfo::call_free(&it->second, freed_size);
            instance().total_gpu_freed_->fetch_add(freed_size, std::memory_order_relaxed);
        } else {
            instance().allocations_mutex_->unlock_shared();
            std::string errormsg = "Memory::free_gpu(): Attempted to free untracked or already "
                "freed GPU memory at address " + std::to_string(reinterpret_cast<uintptr_t>(ptr)) + ".";
            ALLIGATOR_THROW(errormsg);
        }
#else
        void* method_void = instance().gpu_deallocation_method_->load<void*>(std::memory_order_acquire);
        void (*dealloc_method)(Buffet*) = reinterpret_cast<void (*)(Buffet*)>(method_void);
        if (dealloc_method == nullptr) [[unlikely]] {
            ALLIGATOR_THROW("Memory::free_gpu(): no GPU deallocation method is registered.");
        }
        dealloc_method(ptr);
        instance().total_gpu_freed_->fetch_add(static_cast<size_t>(size), std::memory_order_relaxed);
#endif
    }
    /** ------------------------------------------------------------------------------------------- MemoryReport
     * @struct MemoryReport
     * @brief Struct to hold memory usage statistics.
     */
    struct MemoryReport {
        uint64_t total_heap_allocations;
        uint64_t total_gpu_allocations;
        uint64_t total_heap_freed;
        uint64_t total_gpu_freed;
        uint64_t current_heap_usage() const { return total_heap_allocations - total_heap_freed; }
        uint64_t current_gpu_usage() const { return total_gpu_allocations - total_gpu_freed; }
    };
    /** ------------------------------------------------------------------------------------------- Get Memory Report
     * @brief Returns a report of the current memory usage statistics.
     * @return MemoryReport struct containing memory usage statistics.
     */
    static MemoryReport get_memory_report() {
        MemoryReport report{
            .total_heap_allocations = instance().total_heap_allocations_->load<std::uint64_t>(std::memory_order_relaxed),
            .total_gpu_allocations = instance().total_gpu_allocations_->load<std::uint64_t>(std::memory_order_relaxed),
            .total_heap_freed = instance().total_heap_freed_->load<std::uint64_t>(std::memory_order_relaxed),
            .total_gpu_freed = instance().total_gpu_freed_->load<std::uint64_t>(std::memory_order_relaxed),
        };
        return report;
    }
    /** ------------------------------------------------------------------------------------------- Get Current Heap Usage
     * @brief Returns the current heap memory usage in bytes.
     * @return Current heap memory usage in bytes.
     */
    static size_t heap_usage() {
        return instance().total_heap_allocations_->load<std::uint64_t>(std::memory_order_relaxed) -
               instance().total_heap_freed_->load<std::uint64_t>(std::memory_order_relaxed);
    }
    /** ------------------------------------------------------------------------------------------- Get Current GPU Usage
     * @brief Returns the current GPU memory usage in bytes.
     * @return Current GPU memory usage in bytes.
     */
    static size_t gpu_usage() {
        return instance().total_gpu_allocations_->load<std::uint64_t>(std::memory_order_relaxed) -
               instance().total_gpu_freed_->load<std::uint64_t>(std::memory_order_relaxed);
    }
};
#if BUFFETALLIGATOR_ENABLE_CODELOCATION_TRACKING
/** --------------------------------------------------------------------------------------------------------- AllocationInfo::Free
 * @brief Inline implementation of the AllocationInfo::free method. This method is responsible for freeing
 * the allocated memory and updating the memory tracking information in the Memory class.
 * @param ptr Pointer to the memory to free.
 * @param size The size of the memory to free in bytes.
 */
inline void Memory::AllocationInfo::call_free(
    AllocationInfo* info,
    size_t size
) {
    Buffet* ptr = info->ptr_;
    Memory::instance().allocations_mutex_->lock();
    auto it = Memory::instance().allocations_.find(ptr);
    if (it != Memory::instance().allocations_.end()) {
        Memory::instance().allocations_.erase(it);
        Memory::instance().allocations_mutex_->unlock();
        if (info->deleter_ && ptr) {
            info->deleter_(ptr);
        }
    } else {
        Memory::instance().allocations_mutex_->unlock();
        std::string errormsg = "Memory::AllocationInfo::free(): Attempted to free untracked or "
            "already freed memory at address " + std::to_string(reinterpret_cast<uintptr_t>(ptr)) + ".";
        BUFFETALLIGATOR_MEMORY_THROW(errormsg);
    }
}
#endif
} // namespace buffetalligator
