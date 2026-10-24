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
 * @brief The scratch directory and page size, fixed by register_type before the first allocation.
 */
struct MmapContext {
    std::string directory;
    size_t page_size = 0;
    const BuffetDescriptor* descriptor = nullptr;
};
MmapContext context;
}
/** --------------------------------------------------------------------------------------------------------- MmapBuffet Constructor
 * @brief Creates, reserves, unlinks, and maps one zero-filled scratch file without touching its pages.
 * @param size Buffer size in bytes, rounded up to 64.
 */
MmapBuffet::MmapBuffet(size_t size) : size_((size + 63) & ~size_t{63}) {
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
    reservation.fst_length = static_cast<off_t>(size_);
    if (fcntl(descriptor_, F_PREALLOCATE, &reservation) < 0) {
        const int error = errno;
        release();
        ALLIGATOR_THROW(std::string("Reserving scratch file storage: ") + std::strerror(error));
    }
#else
    const int reservation = posix_fallocate(descriptor_, 0, static_cast<off_t>(size_));
    if (reservation != 0) {
        release();
        ALLIGATOR_THROW(
            std::string("Reserving scratch file storage: ") + std::strerror(reservation));
    }
#endif
    if (ftruncate(descriptor_, static_cast<off_t>(size_)) != 0) {
        const int error = errno;
        release();
        ALLIGATOR_THROW(std::string("Sizing scratch file: ") + std::strerror(error));
    }
    void* mapping = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor_, 0);
    if (mapping == MAP_FAILED) {
        const int error = errno;
        release();
        ALLIGATOR_THROW(std::string("Mapping scratch file: ") + std::strerror(error));
    }
    mapping_ = mapping;
}
/** --------------------------------------------------------------------------------------------------------- MmapBuffet Release
 * @brief Unmaps and closes the scratch file; the unlinked file disappears with its last descriptor.
 */
void MmapBuffet::release() {
    if (mapping_ != nullptr && munmap(mapping_, size_) != 0) {
        LOG_ERROR_STREAM << "Unmapping scratch file failed: " << errno;
    }
    if (descriptor_ >= 0 && close(descriptor_) != 0) {
        LOG_ERROR_STREAM << "Closing scratch file failed: " << errno;
    }
    mapping_ = nullptr;
    descriptor_ = -1;
}
/** --------------------------------------------------------------------------------------------------------- Register Type
 * @brief Checks the scratch directory and registers the mmap descriptor once.
 * @param directory A directory on the filesystem that should back the scratch files.
 * @return The registered descriptor.
 */
const BuffetDescriptor* MmapBuffet::register_type(const std::string& directory) {
    if (context.descriptor) ALLIGATOR_THROW("The mmap buffet type is already registered");
    struct stat status{};
    if (stat(directory.c_str(), &status) != 0) {
        ALLIGATOR_THROW("Opening scratch directory " + directory + ": " + std::strerror(errno));
    }
    if (!S_ISDIR(status.st_mode)) ALLIGATOR_THROW("The mmap scratch path must be a directory: " + directory);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) ALLIGATOR_THROW("Reading the system page size failed");
    context.directory = directory;
    context.page_size = static_cast<size_t>(page_size);
    context.descriptor = BuffetDescriptors::descriptor<MmapBuffet>();
    return context.descriptor;
}
/** --------------------------------------------------------------------------------------------------------- Flush
 * @brief Synchronously writes a Chomp's page-aligned mapped range back to its scratch file.
 * @param chomp A Chomp claimed from this type.
 */
void MmapBuffet::flush(const Chomp& chomp) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(chomp.raw());
    const uintptr_t aligned = address - address % context.page_size;
    if (msync(reinterpret_cast<void*>(aligned), chomp.size_bytes() + address - aligned, MS_SYNC) != 0) {
        ALLIGATOR_THROW(std::string("Flushing scratch pages: ") + std::strerror(errno));
    }
}
/** --------------------------------------------------------------------------------------------------------- File Descriptor
 * @brief The borrowed descriptor of the scratch file behind a Chomp.
 * @param chomp A Chomp claimed from this type.
 * @return The descriptor, valid while the Chomp lives.
 */
int MmapBuffet::file_descriptor(const Chomp& chomp) {
    return static_cast<const MmapBuffet*>(Alligator::buffet_of(chomp))->descriptor_;
}
/** --------------------------------------------------------------------------------------------------------- File Offset
 * @brief A Chomp's byte offset within its scratch file, including sub-chomp offsets.
 * @param chomp A Chomp claimed from this type.
 * @return The byte offset.
 */
uint64_t MmapBuffet::file_offset(const Chomp& chomp) {
    const MmapBuffet* buffet = static_cast<const MmapBuffet*>(Alligator::buffet_of(chomp));
    return static_cast<uint64_t>(static_cast<const char*>(chomp.raw())
        - static_cast<const char*>(buffet->mapping_));
}
} // namespace buffetalligator
