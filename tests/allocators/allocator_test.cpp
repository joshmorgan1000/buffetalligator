/** --------------------------------------------------------------------------------------------------------- Allocator Tests
 * @file allocator_test.cpp
 * @brief Exercises host statistics and file-backed Slice lifetime through the public interface.
 */
#include <alligator.hpp>
#include "../functional_support.hpp"
#include <array>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace buffetalligator;
using functional::require;
/** --------------------------------------------------------------------------------------------------------- Expected Byte
 * @brief Recognizes the value written through the mapped Slice.
 */
bool expected_byte(unsigned char value) { return value == 0x6d; }
/** --------------------------------------------------------------------------------------------------------- Host Memory
 * @brief Verifies host measurements without equating live arena capacity with resident memory.
 */
void host_memory() {
    const HostMemoryUsage before = BuffetMenu::memory_usage();
    require(before.physical_bytes > 0, "physical memory is missing");
    require(before.available_bytes <= before.physical_bytes, "available exceeds physical memory");
    require(before.resident_bytes > 0, "process residency is missing");
    Slice allocation(16 * 1024 * 1024, true);
    std::memset(allocation.raw(), 0x35, allocation.size_bytes());
    const HostMemoryUsage after = BuffetMenu::memory_usage();
    require(after.physical_bytes == before.physical_bytes, "physical capacity changed during query");
    require(after.resident_bytes >= allocation.size_bytes(), "touched memory is absent from residency");
    require(allocation.data<unsigned char>()[12345] == 0x35, "query changed owned memory");
}
/** --------------------------------------------------------------------------------------------------------- Mmap
 * @brief Checks shared-file visibility, unaligned flushing, zeroing, and deferred slab reclamation.
 */
void mmap_placement() {
    const Placemat* placement = MmapAllocator::register_type(std::filesystem::temp_directory_path());
    require(std::string(placement->name()) == "mmap", "mmap placement name mismatch");
    require(BuffetMenu::default_placement()->type() == 1, "registration changed the default");
    int descriptor = -1;
    Slice retained;
    uint64_t offset = 0;
    {
        Slice parent(16384, true, placement);
        descriptor = MmapAllocator::file_descriptor(parent);
        struct stat status{};
        require(fstat(descriptor, &status) == 0, "scratch descriptor is invalid");
        require(status.st_nlink == 0, "scratch file remains linked");
        require(status.st_size == 16384, "scratch size differs from novel slab size");
        require((fcntl(descriptor, F_GETFD) & FD_CLOEXEC) != 0, "scratch descriptor leaks across exec");
        require(parent.data<unsigned char>()[0] == 0 &&
                parent.data<unsigned char>()[16383] == 0, "fresh mapping is not zero initialized");
        retained = parent.slice(123, 37);
        offset = MmapAllocator::file_offset(retained);
        require(offset == 123, "subslice file offset is wrong");
        std::memset(retained.raw(), 0x6d, retained.size_bytes());
        MmapAllocator::flush(retained);
    }
    require(retained.data<unsigned char>()[36] == 0x6d, "parent destruction invalidated a view");
    std::array<unsigned char, 37> disk{};
    require(pread(descriptor, disk.data(), disk.size(), offset) == ssize_t(disk.size()),
            "reading scratch file failed");
    require(std::all_of(disk.begin(), disk.end(), &expected_byte),
            "mapped writes did not reach the backing file");
    retained = Slice();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fcntl(descriptor, F_GETFD) >= 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(fcntl(descriptor, F_GETFD) < 0 && errno == EBADF, "worker did not close the reclaimed slab");
    Slice first(64, placement);
    first.data<uint64_t>()[0] = 0x12345678;
    const void* address = first.raw();
    for (size_t index = 0; index < 18; ++index) {
        Slice block(8 * 1024 * 1024, placement);
        require(block.data<uint64_t>()[0] == 0, "a new chain claim was not zeroed");
        block.data<uint64_t>()[0] = index + 1;
    }
    require(first.raw() == address && first.data<uint64_t>()[0] == 0x12345678,
            "chain advancement invalidated an active mapped Slice");
}
int main(int count, char** arguments) {
    return functional::run(count, arguments, {{"host_memory", &host_memory}, {"mmap", &mmap_placement}});
}
