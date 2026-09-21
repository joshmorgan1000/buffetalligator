/** --------------------------------------------------------------------------------------------------------- Vulkan Tests
 * @file vulkan_test.cpp
 * @brief Verifies mapped Slice storage and device addressing on the process-lifetime Vulkan device.
 */
#include <alligator.hpp>
#include "../functional_support.hpp"
#include <cstring>

using namespace buffetalligator;
using functional::require;

int main() {
    if (!VulkanKernel::available()) {
        return 77;
    }
    LOG_INFO_STREAM << "Vulkan device: " << VulkanKernel::device_name();
    constexpr size_t bytes = 1u << 20;
    Slice slice(bytes, false, Placemat::HOST_VISIBLE);
    require(slice.valid(), "claiming a HOST_VISIBLE slice failed");
    require(slice.size_bytes() == bytes, "claimed slice size differs");
    std::memset(slice.data(), 0x5A, bytes);
    require(static_cast<const uint8_t*>(slice.raw())[0] == 0x5A
        && static_cast<const uint8_t*>(slice.raw())[bytes - 1] == 0x5A,
        "mapped writes did not land");
    Slice view = slice.slice(64, 128);
    require(view.size_bytes() == 128 && view.valid(), "sub-slice of the mapped slice failed");
    const uint64_t address = VulkanKernel::device_address(slice);
    require(address != 0, "device address of a HOST_VISIBLE slice is zero");
    require(VulkanKernel::device_address(view) == address + 64,
        "sub-slice device address does not track its offset");
    view.free();
    slice.free();
    require(!slice.valid(), "freeing the slice did not null it");
    return 0;
}
