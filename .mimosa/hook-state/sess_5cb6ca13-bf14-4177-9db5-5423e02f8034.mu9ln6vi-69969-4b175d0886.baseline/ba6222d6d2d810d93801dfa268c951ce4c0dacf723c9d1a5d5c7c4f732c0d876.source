/** --------------------------------------------------------------------------------------------------------- Mmap Allocator
 * @file mmap_allocator.cpp
 * @brief Supplies zero-filled scratch-file mappings through the placement callbacks.
 */
#include <memory/pressure.hpp>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <sys/mman.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace buffetalligator {
namespace {
/** --------------------------------------------------------------------------------------------------------- Mmap Context
 * @brief Holds immutable scratch placement configuration established before arena startup.
 */
struct MmapContext {
    std::string directory;
    size_t page_size;
    const Placemat* placement = nullptr;
};
/** --------------------------------------------------------------------------------------------------------- Mmap Allocation
 * @brief Owns one slab's mapping and scratch file descriptor.
 */
struct MmapAllocation {
    void* mapping = MAP_FAILED;
    size_t length = 0;
    int descriptor = -1;
    MmapAllocation() = default;
    MmapAllocation(const MmapAllocation&) = delete;
    MmapAllocation& operator=(const MmapAllocation&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Releases the mapping and its unlinked backing file.
     */
    ~MmapAllocation() {
        if (mapping != MAP_FAILED && munmap(mapping, length) != 0) {
            LOG_ERROR_STREAM << "Unmapping scratch slab failed: " << errno;
        }
        if (descriptor >= 0 && close(descriptor) != 0) {
            LOG_ERROR_STREAM << "Closing scratch slab failed: " << errno;
        }
    }
};
MmapContext context;
/** --------------------------------------------------------------------------------------------------------- Allocate
 * @brief Maps a fresh zero-filled file for one slab without touching its pages.
 */
Placemat::Handle* mmap_allocate(size_t size, void*) {
    auto handle = std::make_unique<Placemat::Handle>();
    auto allocation = std::make_unique<MmapAllocation>();
    std::string pattern = context.directory + "/buffetalligator-XXXXXX";
    allocation->descriptor = mkstemp(pattern.data());
    if (allocation->descriptor < 0) {
        throw std::system_error(errno, std::generic_category(), "Creating scratch slab");
    }
    if (unlink(pattern.c_str()) != 0) {
        throw std::system_error(errno, std::generic_category(), "Unlinking scratch slab");
    }
    if (fcntl(allocation->descriptor, F_SETFD, FD_CLOEXEC) < 0) {
        throw std::system_error(errno, std::generic_category(), "Configuring scratch descriptor");
    }
#if defined(__APPLE__)
    fstore_t reservation{};
    reservation.fst_flags = F_ALLOCATEALL;
    reservation.fst_posmode = F_PEOFPOSMODE;
    reservation.fst_length = static_cast<off_t>(size);
    if (fcntl(allocation->descriptor, F_PREALLOCATE, &reservation) < 0) {
        throw std::system_error(errno, std::generic_category(), "Reserving scratch file storage");
    }
#else
    const int reservation = posix_fallocate(allocation->descriptor, 0, static_cast<off_t>(size));
    if (reservation != 0) {
        throw std::system_error(reservation, std::generic_category(), "Reserving scratch file storage");
    }
#endif
    if (ftruncate(allocation->descriptor, static_cast<off_t>(size)) != 0) {
        throw std::system_error(errno, std::generic_category(), "Sizing scratch slab");
    }
    allocation->length = size;
    allocation->mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                               allocation->descriptor, 0);
    if (allocation->mapping == MAP_FAILED) {
        throw std::system_error(errno, std::generic_category(), "Mapping scratch slab");
    }
    handle->substrate_handle = allocation.release();
    return handle.release();
}
/** --------------------------------------------------------------------------------------------------------- Deallocate
 * @brief Reclaims one mmap slab through the existing arena cleanup callback.
 */
void mmap_deallocate(Placemat::Handle* handle, void*) {
    delete static_cast<MmapAllocation*>(handle->substrate_handle);
}
/** --------------------------------------------------------------------------------------------------------- Host Pointer
 * @brief Returns the persistent host mapping for a completed slab.
 */
void* mmap_host_pointer(Placemat::Handle* handle) {
    return static_cast<MmapAllocation*>(handle->substrate_handle)->mapping;
}
/** --------------------------------------------------------------------------------------------------------- Context
 * @brief Returns the process-lifetime scratch placement context.
 */
void* mmap_context() { return &context; }
/** --------------------------------------------------------------------------------------------------------- Allocation
 * @brief Resolves the owning mmap allocation for a Slice from this placement.
 */
MmapAllocation& allocation_for(const Slice& slice) {
    return *static_cast<MmapAllocation*>(Placemat::get_for(&slice)->substrate_handle);
}
}
/** --------------------------------------------------------------------------------------------------------- Register Type
 * @brief Registers scratch allocation callbacks before any arena consumes this placement.
 */
const Placemat* MmapAllocator::register_type(const std::string& directory) {
    if (context.placement) ALLIGATOR_THROW("The mmap placement is already registered");
    struct stat status{};
    if (stat(directory.c_str(), &status) != 0) {
        throw std::system_error(errno, std::generic_category(), "Opening scratch directory");
    }
    if (!S_ISDIR(status.st_mode)) ALLIGATOR_THROW("The mmap scratch path must be a directory");
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) ALLIGATOR_THROW("Reading mmap page size failed");
    context.directory = directory;
    context.page_size = static_cast<size_t>(page_size);
    context.placement = BuffetMenu::get(BuffetMenu::register_type(
        "mmap", 64ull * 1024 * 1024, 64, &mmap_allocate, &mmap_deallocate,
        &mmap_host_pointer, &mmap_context
    ));
    return context.placement;
}
/** --------------------------------------------------------------------------------------------------------- Enable Spillover
 * @brief Uses the registered scratch directory for pressure-driven heap backing selection.
 */
void MmapAllocator::enable_spillover(uint64_t reserve_bytes, uint64_t resume_bytes) {
    MemoryPressure::configure(context.placement, reserve_bytes, resume_bytes);
}
/** --------------------------------------------------------------------------------------------------------- Flush
 * @brief Writes a Slice's page-aligned mapped range to its scratch file.
 */
void MmapAllocator::flush(const Slice& slice) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(slice.raw());
    const uintptr_t aligned = address - address % context.page_size;
    if (msync(reinterpret_cast<void*>(aligned), slice.size_bytes() + address - aligned,
              MS_SYNC) != 0) {
        throw std::system_error(errno, std::generic_category(), "Flushing scratch pages");
    }
}
/** --------------------------------------------------------------------------------------------------------- File Descriptor
 * @brief Returns the live slab's borrowed file descriptor.
 */
int MmapAllocator::file_descriptor(const Slice& slice) {
    return allocation_for(slice).descriptor;
}
/** --------------------------------------------------------------------------------------------------------- File Offset
 * @brief Resolves a Slice's byte offset in its backing file.
 */
uint64_t MmapAllocator::file_offset(const Slice& slice) {
    return static_cast<const char*>(slice.raw()) -
        static_cast<const char*>(allocation_for(slice).mapping);
}
} // namespace buffetalligator
