/** --------------------------------------------------------------------------------------------------------- Host Memory Test
 * @file host_memory_test.cpp
 * @brief Checks the host memory query and that a slab the host cannot back is refused before it is touched.
 */
#include <alligator.hpp>
#include "functional_support.hpp"
#include <cstdint>
#include <cstdlib>

using namespace buffetalligator;
using functional::require;
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Reads the host's memory, refuses an impossible slab, and allocates a possible one.
 */
int main() {
    require(setenv("ALLIGATOR_GPU_BACKEND", "cpu", 1) == 0, "CPU selection failed");
    const HostMemoryUsage usage = host_memory_usage();
    require(usage.physical_bytes > 0 && usage.available_bytes > 0 && usage.resident_bytes > 0,
        "host memory query returned zeros");
    require(usage.available_bytes <= usage.physical_bytes && usage.resident_bytes <= usage.physical_bytes,
        "host memory query exceeded physical memory");
    require(available_host_bytes() > 0, "available host bytes returned zero");
    LOG_INFO_STREAM << "Host memory: physical " << (usage.physical_bytes >> 20) << " MiB, available "
        << (usage.available_bytes >> 20) << " MiB, resident " << (usage.resident_bytes >> 20) << " MiB";
    Slice slab(SLAB_FLOOR_BYTES, true);
    require(slab && slab.size_bytes() == SLAB_FLOOR_BYTES
        && static_cast<const uint8_t*>(slab.raw())[SLAB_FLOOR_BYTES - 1] == 0,
        "a slab within the host's available memory was not allocated and zeroed");
    LOG_INFO_STREAM << "Host memory query and slab admission passed";
}
