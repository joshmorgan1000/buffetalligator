/** --------------------------------------------------------------------------------------------------------- Host Memory
 * @file host_memory.cpp
 * @brief The host's physical, available, and resident bytes, each from one kernel query.
 */
#include <alligator.hpp>
#include <cstdio>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#endif

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Available Host Bytes
 * @brief Queries the host, in one call, for the bytes it can hand out without reclaiming.
 * @return The available bytes.
 */
size_t available_host_bytes() {
#if defined(__APPLE__)
    vm_statistics64_data_t statistics;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&statistics), &count)
        != KERN_SUCCESS) ALLIGATOR_THROW("host_statistics64 could not read the host's page counts");
    const size_t pages = size_t(statistics.free_count) + statistics.inactive_count + statistics.speculative_count;
    return pages * static_cast<size_t>(sysconf(_SC_PAGESIZE));
#elif defined(__linux__)
    FILE* meminfo = std::fopen("/proc/meminfo", "r");
    if (meminfo == nullptr) ALLIGATOR_THROW("/proc/meminfo could not be opened");
    char line[128];
    unsigned long long kibibytes = 0;
    while (std::fgets(line, sizeof(line), meminfo) != nullptr) {
        if (std::sscanf(line, "MemAvailable: %llu kB", &kibibytes) == 1) break;
    }
    std::fclose(meminfo);
    return static_cast<size_t>(kibibytes) * 1024;
#else
#error "available_host_bytes needs a host memory query for this platform"
#endif
}
/** --------------------------------------------------------------------------------------------------------- Host Memory Usage Query
 * @brief Queries the host for its physical bytes, its available bytes, and this process's resident bytes.
 * @return The host memory usage.
 */
HostMemoryUsage host_memory_usage() {
    HostMemoryUsage usage{0, available_host_bytes(), 0};
#if defined(__APPLE__)
    uint64_t physical = 0;
    size_t length = sizeof(physical);
    if (sysctlbyname("hw.memsize", &physical, &length, nullptr, 0) != 0)
        ALLIGATOR_THROW("sysctl hw.memsize could not read the host's physical memory");
    usage.physical_bytes = physical;
    mach_task_basic_info_data_t information;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&information), &count)
        != KERN_SUCCESS) ALLIGATOR_THROW("task_info could not read this process's resident size");
    usage.resident_bytes = information.resident_size;
#elif defined(__linux__)
    struct sysinfo information{};
    if (sysinfo(&information) != 0) ALLIGATOR_THROW("sysinfo could not read the host's physical memory");
    usage.physical_bytes = uint64_t(information.totalram) * information.mem_unit;
    FILE* status = std::fopen("/proc/self/statm", "r");
    if (status == nullptr) ALLIGATOR_THROW("/proc/self/statm could not be opened");
    unsigned long virtual_pages = 0, resident_pages = 0;
    const int fields = std::fscanf(status, "%lu %lu", &virtual_pages, &resident_pages);
    std::fclose(status);
    if (fields != 2) ALLIGATOR_THROW("/proc/self/statm could not be read");
    usage.resident_bytes = uint64_t(resident_pages) * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
#endif
    return usage;
}
} // namespace buffetalligator
