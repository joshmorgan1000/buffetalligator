/** --------------------------------------------------------------------------------------------------------- Vulkan Transfer Performance
 * @file vulkan_performance.cpp
 * @brief Measures the library's actual host-to-device and device-to-host Slice synchronization.
 */
#include <alligator.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace {
using namespace buffetalligator;
using Clock = std::chrono::steady_clock;
/** --------------------------------------------------------------------------------------------------------- Transfer
 * @brief Uploads a payload and validates its last byte after device readback.
 */
void transfer(Slice& slice, unsigned char value) {
    slice.data()[slice.size_bytes() - 1] = value;
    slice.vulkan_sync(true);
    slice.vulkan_sync(false);
    if (slice.data()[slice.size_bytes() - 1] != value)
        throw std::runtime_error("Vulkan transfer validation failed");
}
/** --------------------------------------------------------------------------------------------------------- Measure
 * @brief Reports repeated synchronization latency after device and buffer initialization.
 */
void measure(const Placemat* placement, uint32_t flags, size_t bytes, unsigned repetition) {
    constexpr size_t count = 1000;
    Slice slice(bytes, placement);
    const auto buffer = slice.vulkan_buffer();
    if (buffer.range != bytes) throw std::runtime_error("Vulkan buffer range changed");
    for (size_t index = 0; index < 32; ++index) transfer(slice, static_cast<unsigned char>(index));
    std::vector<uint64_t> latency(count);
    const auto wall = Clock::now();
    for (size_t index = 0; index < count; ++index) {
        const auto begin = Clock::now();
        transfer(slice, static_cast<unsigned char>(index));
        latency[index] = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
    }
    const uint64_t elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - wall).count());
    std::sort(latency.begin(), latency.end());
    std::printf("%u,%u,%zu,%zu,%llu,%llu,%llu,%llu,%llu\n", flags, repetition, bytes, count,
        (unsigned long long)elapsed, (unsigned long long)latency[(count - 1) * 50 / 100],
        (unsigned long long)latency[(count - 1) * 95 / 100], (unsigned long long)latency[(count - 1) * 99 / 100],
        (unsigned long long)latency.back());
}
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Measures supported property combinations without substituting another placement.
 */
int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    try {
        size_t supported = 0;
        std::puts("memory_property_flags,repetition,payload_bytes,upload_download_pairs,elapsed_ns,p50_ns,p95_ns,p99_ns,max_ns");
        for (uint32_t flags : {1u, 2u, 6u, 14u}) {
            const Placemat* placement = nullptr;
            try { placement = BuffetMenu::vulkan(flags); }
            catch (const AlligatorException& error) {
                std::fprintf(stderr, "Unsupported Vulkan flags %u: %s\n", flags, error.what());
                continue;
            }
            ++supported;
            for (unsigned repetition = 1; repetition <= 3; ++repetition) {
                for (size_t bytes : {4096u, 1048576u}) {
                    std::fprintf(stderr, "Measuring Vulkan flags=%u bytes=%zu repetition=%u/3\n", flags, bytes, repetition);
                    measure(placement, flags, bytes, repetition);
                }
            }
        }
        BuffetMenu::shutdown();
        return supported ? 0 : 77;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Vulkan benchmark failed: %s\n", error.what());
        return 1;
    }
}
