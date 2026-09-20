/** --------------------------------------------------------------------------------------------------------- Host Memory
 * @file host_memory.cpp
 * @brief Queries operating-system memory counters independently of arena allocation totals.
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

namespace buffetalligator {
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
} // namespace buffetalligator
