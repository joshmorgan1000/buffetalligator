/** --------------------------------------------------------------------------------------------------------- Mmap Allocator
 * @file mmap_allocator.cpp
 * @brief Allocates MmapBuffet storage as zero-filled MAP_SHARED mappings of unlinked scratch files.
 */
#include <alligator/mmapbuffet.hpp>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cstring>
#include <unistd.h>

namespace buffetalligator {
namespace {
/** --------------------------------------------------------------------------------------------------------- Mmap Context
 * @brief The scratch directory and page size, fixed by set_directory before the first mapping.
 */
struct MmapContext {
    std::string directory;
    size_t page_size = 0;
};
MmapContext context;
}
/** --------------------------------------------------------------------------------------------------------- MmapBuffet Constructor
 * @brief Creates, reserves, unlinks, and maps one zero-filled scratch file without touching its pages.
 * @param size Buffer size in bytes, rounded up to 64.
 * @param novel Whether the mapping is dedicated to one claim.
 */
MmapBuffet::MmapBuffet(size_t size, bool novel) : IBuffet((size + 63) & ~size_t{63}, novel) {
    const size_t bytes = this->size();
    std::string pattern = context.directory + "/buffetalligator-XXXXXX";
    descriptor_ = mkstemp(pattern.data());
    if (descriptor_ < 0) {
        ALLIGATOR_THROW(std::string("Creating scratch file: ") + std::strerror(errno));
    }
    if (unlink(pattern.c_str()) != 0) {
        const int error = errno;
        release();
        ALLIGATOR_THROW(std::string("Unlinking scratch file: ") + std::strerror(error));
    }
    if (fcntl(descriptor_, F_SETFD, FD_CLOEXEC) < 0) {
        const int error = errno;
        release();
        ALLIGATOR_THROW(std::string("Configuring scratch descriptor: ") + std::strerror(error));
    }
#if defined(__APPLE__)
    fstore_t reservation{};
    reservation.fst_flags = F_ALLOCATEALL;
    reservation.fst_posmode = F_PEOFPOSMODE;
    reservation.fst_length = static_cast<off_t>(bytes);
    if (fcntl(descriptor_, F_PREALLOCATE, &reservation) < 0) {
        const int error = errno;
        release();
        ALLIGATOR_THROW(std::string("Reserving scratch file storage: ") + std::strerror(error));
    }
#else
    const int reservation = posix_fallocate(descriptor_, 0, static_cast<off_t>(bytes));
    if (reservation != 0) {
        release();
        ALLIGATOR_THROW(
            std::string("Reserving scratch file storage: ") + std::strerror(reservation));
    }
#endif
    if (ftruncate(descriptor_, static_cast<off_t>(bytes)) != 0) {
        const int error = errno;
        release();
        ALLIGATOR_THROW(std::string("Sizing scratch file: ") + std::strerror(error));
    }
    void* mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor_, 0);
    if (mapping == MAP_FAILED) {
        const int error = errno;
        release();
        ALLIGATOR_THROW(std::string("Mapping scratch file: ") + std::strerror(error));
    }
    mapped_ = mapping;
    address_ = reinterpret_cast<uint64_t>(mapping);
}
/** --------------------------------------------------------------------------------------------------------- MmapBuffet Release
 * @brief Unmaps and closes the scratch file; the unlinked file disappears with its last descriptor.
 */
void MmapBuffet::release() {
    if (mapped_ != nullptr && munmap(mapped_, size()) != 0) {
        LOG_ERROR_STREAM << "Unmapping scratch file failed: " << errno;
    }
    if (descriptor_ >= 0 && close(descriptor_) != 0) {
        LOG_ERROR_STREAM << "Closing scratch file failed: " << errno;
    }
    mapped_ = nullptr;
    descriptor_ = -1;
}
/** --------------------------------------------------------------------------------------------------------- Set Directory
 * @brief Checks the scratch directory once; every mapping after it lands there.
 * @param directory A directory on the filesystem that should back the scratch files.
 */
void MmapBuffet::set_directory(const std::string& directory) {
    struct stat status{};
    if (stat(directory.c_str(), &status) != 0) {
        ALLIGATOR_THROW("Opening scratch directory " + directory + ": " + std::strerror(errno));
    }
    if (!S_ISDIR(status.st_mode)) ALLIGATOR_THROW("The mmap scratch path must be a directory: " + directory);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) ALLIGATOR_THROW("Reading the system page size failed");
    context.directory = directory;
    context.page_size = static_cast<size_t>(page_size);
}
/** --------------------------------------------------------------------------------------------------------- Flush
 * @brief Synchronously writes a Slice's page-aligned mapped range back to its scratch file.
 * @param slice A Slice claimed from this type.
 */
void MmapBuffet::flush(const Slice& slice) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(slice.raw());
    const uintptr_t aligned = address - address % context.page_size;
    if (msync(reinterpret_cast<void*>(aligned), slice.size_bytes() + address - aligned, MS_SYNC) != 0) {
        ALLIGATOR_THROW(std::string("Flushing scratch pages: ") + std::strerror(errno));
    }
}
/** --------------------------------------------------------------------------------------------------------- File Descriptor
 * @brief The borrowed descriptor of the scratch file behind a Slice.
 * @param slice A Slice claimed from this type.
 * @return The descriptor, valid while the Slice lives.
 */
int MmapBuffet::file_descriptor(const Slice& slice) {
    return static_cast<const MmapBuffet*>(slice.buffer().get())->descriptor_;
}
/** --------------------------------------------------------------------------------------------------------- File Offset
 * @brief A Slice's byte offset within its scratch file, including sub-slice offsets.
 * @param slice A Slice claimed from this type.
 * @return The byte offset.
 */
uint64_t MmapBuffet::file_offset(const Slice& slice) {
    const MmapBuffet* buffet = static_cast<const MmapBuffet*>(slice.buffer().get());
    return static_cast<uint64_t>(static_cast<const char*>(slice.raw())
        - static_cast<const char*>(buffet->mapped()));
}
} // namespace buffetalligator
