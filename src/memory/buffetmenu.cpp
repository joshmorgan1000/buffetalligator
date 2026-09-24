/** --------------------------------------------------------------------------------------------------------- BuffetMenu Implementation
 * @file buffetmenu.cpp
 * @brief Implementation of the BuffetMenu class and related functionality.
 */
#include <alligator.hpp>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

extern "C" void ba_net_shutdown(void);

namespace buffetalligator {
namespace {
/** --------------------------------------------------------------------------------------------------------- Heap Block
 * @struct HeapBlock
 * @brief The substrate handle of a heap slab: the calloc'd block, its first usable byte, and its size.
 */
struct HeapBlock {
    void* block;  ///< The calloc'd allocation handed back to free.
    void* host;   ///< The first usable byte, aligned for the aligned heap.
    size_t size;  ///< The usable size in bytes.
};
/** --------------------------------------------------------------------------------------------------------- Heap Allocate
 * @brief Allocates one zeroed slab from the C heap.
 * @param size The slab size in bytes.
 * @return The host pointer and the slab's HeapBlock.
 */
std::pair<void*, void*> heap_allocate(size_t size, void*) {
    void* memory = std::calloc(1, size);
    if (memory == nullptr) [[unlikely]] {
        throw std::bad_alloc();
    }
    return {memory, new HeapBlock{memory, memory, size}};
}
/** --------------------------------------------------------------------------------------------------------- Aligned Heap Allocate
 * @brief Allocates one zeroed, 64-byte-aligned slab from the C heap.
 * @param size The slab size in bytes.
 * @return The aligned host pointer and the slab's HeapBlock.
 */
std::pair<void*, void*> aligned_heap_allocate(size_t size, void*) {
    void* memory = std::calloc(1, size + 63);
    if (memory == nullptr) [[unlikely]] {
        throw std::bad_alloc();
    }
    void* aligned_memory = reinterpret_cast<void*>(
        (reinterpret_cast<uintptr_t>(memory) + 63) & ~static_cast<uintptr_t>(63)
    );
    return {aligned_memory, new HeapBlock{memory, aligned_memory, size}};
}
/** --------------------------------------------------------------------------------------------------------- Heap Deallocate
 * @brief Frees a heap slab's block and its HeapBlock.
 * @param host_ptr The host pointer of the block.
 * @param substrate_handle The slab's HeapBlock.
 * @return The cleared host pointer and substrate handle pair.
 */
std::pair<void*, void*> heap_deallocate(void* host_ptr, void* substrate_handle) {
    static_cast<void>(host_ptr);
    HeapBlock* heap_block = static_cast<HeapBlock*>(substrate_handle);
    std::free(heap_block->block);
    delete heap_block;
    return {nullptr, nullptr};
}
/** --------------------------------------------------------------------------------------------------------- Heap Context
 * @brief The heap placements carry no context.
 * @return Always returns nullptr.
 */
void* heap_context() {
    return nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Heap Try Resize
 * @brief Resizes one whole-block heap allocation with realloc, which expands in place whenever the
 * allocator can. Failure returns nulls and leaves the original block untouched.
 * @param host_ptr The block's host pointer.
 * @param substrate_handle The slab's HeapBlock.
 * @param new_size The requested size in bytes.
 * @return The resized pair, or nulls when realloc failed.
 */
std::pair<void*, void*> heap_try_resize(void* host_ptr, void* substrate_handle, size_t new_size) {
    static_cast<void>(host_ptr);
    HeapBlock* heap_block = static_cast<HeapBlock*>(substrate_handle);
    void* resized = std::realloc(heap_block->block, new_size);
    if (resized == nullptr) [[unlikely]] {
        return {nullptr, nullptr};
    }
    *heap_block = HeapBlock{resized, resized, new_size};
    return {resized, heap_block};
}
/** --------------------------------------------------------------------------------------------------------- Aligned Heap Try Resize
 * @brief Resizes an aligned heap allocation, shifting the payload when realloc moved it to a
 * block with a different alignment pad.
 * @param host_ptr not used. It is just a view.
 * @param substrate_handle The slab's HeapBlock.
 * @param new_size The requested size in bytes.
 * @return The resized pair, or nulls when realloc failed.
 */
std::pair<void*, void*> aligned_heap_try_resize(void*, void* substrate_handle, size_t new_size) {
    HeapBlock* heap_block = static_cast<HeapBlock*>(substrate_handle);
    const size_t old_pad = static_cast<uint8_t*>(heap_block->host) - static_cast<uint8_t*>(heap_block->block);
    void* resized = std::realloc(heap_block->block, new_size + 63);
    if (resized == nullptr) [[unlikely]] {
        return {nullptr, nullptr};
    }
    void* aligned_resized = reinterpret_cast<void*>(
        (reinterpret_cast<uintptr_t>(resized) + 63) & ~static_cast<uintptr_t>(63)
    );
    const size_t new_pad = static_cast<uint8_t*>(aligned_resized) - static_cast<uint8_t*>(resized);
    if (new_pad != old_pad) {
        std::memmove(aligned_resized, static_cast<uint8_t*>(resized) + old_pad, std::min(heap_block->size, new_size));
    }
    *heap_block = HeapBlock{resized, aligned_resized, new_size};
    return {aligned_resized, heap_block};
}
/** --------------------------------------------------------------------------------------------------------- Heap Get Host Pointer
 * @brief Retrieves the first usable byte of a heap slab.
 * @param substrate_handle The slab's HeapBlock.
 * @return The slab's host pointer.
 */
HostPtr heap_get_host_pointer(void* substrate_handle) {
    return HostPtr{static_cast<HeapBlock*>(substrate_handle)->host};
}
/** --------------------------------------------------------------------------------------------------------- Heap Get GPUBuf
 * @brief The heap slab's GPUBuf, addressed by its host pointer.
 * @param substrate_handle The slab's HeapBlock.
 * @return The GPUBuf spanning the whole slab.
 */
GPUBuf heap_get_gpu_buf(void* substrate_handle) {
    const HeapBlock* heap_block = static_cast<HeapBlock*>(substrate_handle);
    return GPUBuf{reinterpret_cast<uint64_t>(heap_block->host), static_cast<uint32_t>(heap_block->size), 0};
}
}
/** --------------------------------------------------------------------------------------------------------- Memory Usage
 * @brief Reads host capacity, available-memory estimates, and process residency.
 */
HostMemoryUsage BuffetMenu::memory_usage() {
#if defined(__APPLE__)
    HostMemoryUsage usage{};
    size_t length = sizeof(usage.physical_bytes);
    if (sysctlbyname("hw.memsize", &usage.physical_bytes, &length, nullptr, 0) != 0) {
        throw std::system_error(errno, std::generic_category(), "Reading physical memory");
    }
    const mach_port_t host = mach_host_self();
    vm_size_t page_size = 0;
    vm_statistics64_data_t statistics{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    const kern_return_t page_status = host_page_size(host, &page_size);
    const kern_return_t memory_status = host_statistics64(
        host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&statistics), &count
    );
    mach_port_deallocate(mach_task_self(), host);
    if (page_status != KERN_SUCCESS || memory_status != KERN_SUCCESS) {
        ALLIGATOR_THROW("Reading macOS host memory statistics failed");
    }
    usage.available_bytes = (uint64_t(statistics.free_count) + statistics.inactive_count) * page_size;
    mach_task_basic_info_data_t task{};
    count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&task), &count) != KERN_SUCCESS) {
        ALLIGATOR_THROW("Reading macOS process residency failed");
    }
    usage.resident_bytes = task.resident_size;
    return usage;
#elif defined(__linux__)
    HostMemoryUsage usage{};
    std::ifstream memory("/proc/meminfo");
    std::string line;
    bool have_total = false;
    bool have_available = false;
    while (std::getline(memory, line)) {
        std::istringstream fields(line);
        std::string name;
        uint64_t kilobytes = 0;
        fields >> name >> kilobytes;
        if (name == "MemTotal:") {
            usage.physical_bytes = kilobytes * 1024;
            have_total = true;
        } else if (name == "MemAvailable:") {
            usage.available_bytes = kilobytes * 1024;
            have_available = true;
        }
    }
    if (!have_total || !have_available) ALLIGATOR_THROW("Reading /proc/meminfo failed");
    std::ifstream process("/proc/self/statm");
    uint64_t virtual_pages = 0;
    uint64_t resident_pages = 0;
    const long page_size = sysconf(_SC_PAGESIZE);
    if (!(process >> virtual_pages >> resident_pages) || page_size <= 0) {
        ALLIGATOR_THROW("Reading Linux process residency failed");
    }
    usage.resident_bytes = resident_pages * uint64_t(page_size);
    return usage;
#else
#error Host memory reporting requires macOS or Linux
#endif
}
/** ------------------------------------------------------------------------------------------- Register Type
 * @brief Registers one process-lifetime Placemat before the first Slice is created.
 * @param name The name of the placement.
 * @param default_slab_size The default size of slabs for this placement.
 * @param bump_alignment The alignment requirement for bump allocations.
 * @param allocate The allocation function for this placement.
 * @param deallocate The deallocation function for this placement.
 * @param get_context The context retrieval function for this placement.
 * @param set_as_default Whether to set this placement as the default.
 * @param resize The optional in-place resize hook.
 * @param device_address The substrate-to-device-address hook, host-only by default.
 * @return The stable placement identifier.
 */
uint8_t BuffetMenu::register_type(
    const char* name,
    size_t default_slab_size,
    size_t bump_alignment,
    AllocateMethod allocate,
    DeallocateMethod deallocate,
    GetContextMethod get_context,
    GetHostPtrMethod get_host_ptr,
    ResizeMethod resize,
    GetGPUBufMethod get_gpu_buf,
    bool set_as_default
) {
    set_read_this(placemat_construction, 4 + 13);
    std::unique_ptr<Placemat> placement = std::make_unique<Placemat>();
    unset_read_this(placemat_construction);
    placement->name_ = name;
    placement->default_slab_size_ = default_slab_size;
    placement->bump_alignment_ = bump_alignment;
    placement->allocator_ = allocate;
    placement->deallocator_ = deallocate;
    placement->get_context_ = get_context;
    placement->resizer_ = resize;
    placement->get_host_ptr_ = get_host_ptr;
    placement->get_gpu_buf_ = get_gpu_buf;
    placement->type_ = static_cast<uint16_t>(instance().placement_indices_.size());
    if (set_as_default) {
        default_placement_slot() = placement.get();
    }
    const uint8_t type = static_cast<uint8_t>(instance().placement_indices_.size());
    instance().placement_indices_[name] = type;
    set_read_this(add_placement, 90 + 9);
    placemats()[type] = std::move(placement);
    unset_read_this(add_placement);
    instance().placemat_count_.fetch_add(1, std::memory_order_release);
    return type;
}
/** ------------------------------------------------------------------------------------------- Get
 * @brief Returns the registered Placemat for an identifier.
 * @param type The placement identifier.
 * @return The registered placement factory.
 */
Placemat* BuffetMenu::get(uint8_t type) {
    set_read_this(add_placement, 33 + 66);
    Placemat* placement = instance().placemats()[type].get();
    unset_read_this(add_placement);
    return placement;
}
/** ------------------------------------------------------------------------------------------- Get by Name
 * @brief Returns the registered Placemat for a given name.
 * @param name The name of the placement.
 * @return The registered placement factory, or nullptr if not found.
 */
Placemat* BuffetMenu::get(const std::string& name) {
    if (!instance().builtins_ready_.load(std::memory_order_acquire)) {
        ensure_builtins();
    }
    auto it = instance().placement_indices_.find(name);
    Placemat* placement = nullptr;
    if (it != instance().placement_indices_.end()) {
        set_read_this(add_placement, 33 + 66);
        placement = instance().placemats().at(it->second).get();
        unset_read_this(add_placement);
    }
    return placement;
}
/** ------------------------------------------------------------------------------------------- Count
 * @brief Returns the number of Placemat types registered so far.
 * @return The registered placement count.
 */
size_t BuffetMenu::count() {
    if (!instance().builtins_ready_.load(std::memory_order_acquire)) {
        ensure_builtins();
    }
    return instance().placemat_count_.load(std::memory_order_acquire);
}
/** ------------------------------------------------------------------------------------------- Default Placement
 * @brief Returns the default Placemat instance, registering the built-ins first if needed.
 * @return The default Placemat pointer.
 */
const Placemat*& BuffetMenu::default_placement() {
    if (!instance().builtins_ready_.load(std::memory_order_acquire)) {
        ensure_builtins();
    }
    if (!instance().device_default_ready_.load(std::memory_order_acquire)) {
        ensure_device_default();
    }
    return default_placement_slot();
}
/** --------------------------------------------------------------------------------------------------------- Shutdown
 * @brief Stops channel reactors at application quiescence before the process exits.
 */
void BuffetMenu::shutdown() { ba_net_shutdown(); }
/** ------------------------------------------------------------------------------------------- Register Change Listener
 * @brief Registers a change listener that will be called when certain events occur.
 * @param context The context pointer to be passed to the callback.
 * @param callback The callback function to be invoked.
 */
void BuffetMenu::register_change_listener(
    void* context,
    void (*callback)(void*)
) {
    instance().change_listeners_.emplace_back(context, callback);
}
/** ------------------------------------------------------------------------------------------- Placemats
 * @brief Returns the flat index-to-Placemat table; the registration paths write it, gated
 * readers use it.
 * @return The 256-entry placemat table.
 */
std::array<std::unique_ptr<Placemat>, 256>& BuffetMenu::placemats() {
    static std::array<std::unique_ptr<Placemat>, 256> places;
    CHECK_read_this(add_placement);
    return places;
}
/** --------------------------------------------------------------------------------------------------------- Host Placements
 * @brief The heap built-ins resolved once at load; PAGE_ALIGNED stays null until MmapAllocator
 * registers the page-backed placement.
 */
const Placemat* const Placemat::HEAP = BuffetMenu::get("heap");
const Placemat* const Placemat::HEAP_ALIGNED = BuffetMenu::get("aligned_heap");
const Placemat* Placemat::PAGE_ALIGNED = nullptr;
/** --------------------------------------------------------------------------------------------------------- Ensure Builtins Slow
 * @brief Registers the heap (0) and aligned-heap (1) placements exactly once; a caller that
 * loses the claim waits until the identifiers are stable.
 */
void BuffetMenu::ensure_builtins() {
    BuffetMenu& menu = instance();
    bool unclaimed = false;
    if (!menu.builtins_claimed_.compare_exchange_strong(unclaimed, true, std::memory_order_acq_rel)) {
        while (!menu.builtins_ready_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return;
    }
    register_type(
        "heap",
        64ull * 1024 * 1024,
        16,
        &heap_allocate,
        &heap_deallocate,
        &heap_context,
        &heap_get_host_pointer,
        &heap_try_resize,
        &heap_get_gpu_buf
    );
    register_type(
        "aligned_heap",
        64ull * 1024 * 1024,
        64,
        &aligned_heap_allocate,
        &heap_deallocate,
        &heap_context,
        &heap_get_host_pointer,
        &aligned_heap_try_resize,
        &heap_get_gpu_buf,
        true
    );
    menu.builtins_ready_.store(true, std::memory_order_release);
}
/** ------------------------------------------------------------------------------------------- Default Placement Slot
 * @brief The storage behind default_placement(), reachable without the built-in check.
 * @return The default Placemat pointer slot.
 */
const Placemat*& BuffetMenu::default_placement_slot() {
    static const Placemat* default_placement = nullptr;
    return default_placement;
}
/** ------------------------------------------------------------------------------------------- Notify Change Listeners
 * @brief Notifies all registered change listeners by invoking their callbacks with the
 * provided context.
 */
void BuffetMenu::notify_change_listeners() {
    for (auto& listener : change_listeners_) {
        listener.second(listener.first);
    }
}
/** ------------------------------------------------------------------------------------------- Instance
 * @brief Retrieves the singleton instance of BuffetMenu.
 * @return The singleton BuffetMenu instance.
 */
BuffetMenu& BuffetMenu::instance() {
    static BuffetMenu instance;
    return instance;
}
} // namespace buffetalligator