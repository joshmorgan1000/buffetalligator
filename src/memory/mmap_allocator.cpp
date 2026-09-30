/** --------------------------------------------------------------------------------------------------------- Mmap Allocator
 * @file mmap_allocator.cpp
 * @brief Allocates MmapBuffer storage as zero-filled MAP_SHARED mappings of unlinked scratch files.
 */
#include <alligator/easymmap.hpp>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace buffetalligator {
namespace {
/** --------------------------------------------------------------------------------------------------------- Mmap Context
 * @brief The scratch directory and page size, fixed by register_type before the first allocation.
 */
struct MmapContext {
    std::string directory;
    size_t page_size = 0;
    const BuffetDescriptor* descriptor = nullptr;
};
MmapContext context;
}
/** --------------------------------------------------------------------------------------------------------- MmapBuffer Constructor
 * @brief Creates, reserves, unlinks, and maps one zero-filled scratch file without touching its pages.
 * @param size Buffer size in bytes, rounded up to 64.
 */
MmapBuffer::MmapBuffer(size_t size) : size_((size + 63) & ~size_t{63}) {
    std::string pattern = context.directory + "/buffetalligator-XXXXXX";
    descriptor_ = mkstemp(pattern.data());
    if (descriptor_ < 0) {
        throw std::system_error(errno, std::generic_category(), "Creating scratch file");
    }
    if (unlink(pattern.c_str()) != 0) {
        const int error = errno;
        release();
        throw std::system_error(error, std::generic_category(), "Unlinking scratch file");
    }
    if (fcntl(descriptor_, F_SETFD, FD_CLOEXEC) < 0) {
        const int error = errno;
        release();
        throw std::system_error(error, std::generic_category(), "Configuring scratch descriptor");
    }
#if defined(__APPLE__)
    fstore_t reservation{};
    reservation.fst_flags = F_ALLOCATEALL;
    reservation.fst_posmode = F_PEOFPOSMODE;
    reservation.fst_length = static_cast<off_t>(size_);
    if (fcntl(descriptor_, F_PREALLOCATE, &reservation) < 0) {
        const int error = errno;
        release();
        throw std::system_error(error, std::generic_category(), "Reserving scratch file storage");
    }
#else
    const int reservation = posix_fallocate(descriptor_, 0, static_cast<off_t>(size_));
    if (reservation != 0) {
        release();
        throw std::system_error(reservation, std::generic_category(), "Reserving scratch file storage");
    }
#endif
    if (ftruncate(descriptor_, static_cast<off_t>(size_)) != 0) {
        const int error = errno;
        release();
        throw std::system_error(error, std::generic_category(), "Sizing scratch file");
    }
    void* mapping = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor_, 0);
    if (mapping == MAP_FAILED) {
        const int error = errno;
        release();
        throw std::system_error(error, std::generic_category(), "Mapping scratch file");
    }
    mapping_ = mapping;
}
/** --------------------------------------------------------------------------------------------------------- MmapBuffer Release
 * @brief Unmaps and closes the scratch file; the unlinked file disappears with its last descriptor.
 */
void MmapBuffer::release() {
    if (mapping_ != nullptr && munmap(mapping_, size_) != 0) {
        LOG_ERROR_STREAM << "Unmapping scratch file failed: " << errno;
    }
    if (descriptor_ >= 0 && close(descriptor_) != 0) {
        LOG_ERROR_STREAM << "Closing scratch file failed: " << errno;
    }
    mapping_ = nullptr;
    descriptor_ = -1;
}
/** --------------------------------------------------------------------------------------------------------- Buffer For
 * @brief The MmapBuffer backing a Slice claimed from this type.
 * @param slice The Slice.
 * @return Its backing buffer.
 */
const MmapBuffer& MmapBuffer::buffer_for(const Slice& slice) {
    return *static_cast<const MmapBuffer*>(SliceEntry::from_slice(slice)->token()->buffet());
}
/** --------------------------------------------------------------------------------------------------------- Register Type
 * @brief Checks the scratch directory and registers the mmap descriptor once.
 * @param directory A directory on the filesystem that should back the scratch files.
 * @return The registered descriptor.
 */
const BuffetDescriptor* MmapBuffer::register_type(const std::string& directory) {
    if (context.descriptor) ALLIGATOR_THROW("The mmap buffet type is already registered");
    struct stat status{};
    if (stat(directory.c_str(), &status) != 0) {
        throw std::system_error(errno, std::generic_category(), "Opening scratch directory " + directory);
    }
    if (!S_ISDIR(status.st_mode)) ALLIGATOR_THROW("The mmap scratch path must be a directory: " + directory);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) ALLIGATOR_THROW("Reading the system page size failed");
    context.directory = directory;
    context.page_size = static_cast<size_t>(page_size);
    const BuffetDescriptor* descriptor = BuffetDescriptors::descriptor_for(static_cast<MmapBuffer*>(nullptr));
    BuffetDescriptors::register_descriptor(descriptor);
    context.descriptor = descriptor;
    return context.descriptor;
}
/** --------------------------------------------------------------------------------------------------------- Flush
 * @brief Synchronously writes a Slice's page-aligned mapped range back to its scratch file.
 * @param slice A Slice claimed from this type.
 */
void MmapBuffer::flush(const Slice& slice) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(slice.raw());
    const uintptr_t aligned = address - address % context.page_size;
    if (msync(reinterpret_cast<void*>(aligned), slice.size_bytes() + address - aligned, MS_SYNC) != 0) {
        throw std::system_error(errno, std::generic_category(), "Flushing scratch pages");
    }
}
/** --------------------------------------------------------------------------------------------------------- File Descriptor
 * @brief The borrowed descriptor of the scratch file behind a Slice.
 * @param slice A Slice claimed from this type.
 * @return The descriptor, valid while the Slice lives.
 */
int MmapBuffer::file_descriptor(const Slice& slice) {
    return buffer_for(slice).descriptor_;
}
/** --------------------------------------------------------------------------------------------------------- File Offset
 * @brief A Slice's byte offset within its scratch file, including sub-slice offsets.
 * @param slice A Slice claimed from this type.
 * @return The byte offset.
 */
uint64_t MmapBuffer::file_offset(const Slice& slice) {
    return static_cast<uint64_t>(static_cast<const char*>(slice.raw())
        - static_cast<const char*>(buffer_for(slice).mapping_));
}
} // namespace buffetalligator
