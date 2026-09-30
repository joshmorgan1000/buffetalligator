/** --------------------------------------------------------------------------------------------------------- Memory Leak Test
 * @file memory_leak_test.cpp
 * @brief Churns Slice ownership across hardware threads and checks backing and process memory recovery.
 */
#include <alligator.hpp>
#include <alligator/easygpu.hpp>
#include <alligator/easyvulkan.hpp>
#include <memory/tracker.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <thread>
#include <vector>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <malloc.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#endif
#if defined(__SANITIZE_ADDRESS__)
#define ALLIGATOR_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ALLIGATOR_TEST_ASAN 1
#endif
#endif
#if defined(ALLIGATOR_TEST_ASAN) && __has_include(<sanitizer/allocator_interface.h>)
#include <sanitizer/allocator_interface.h>
#define ALLIGATOR_TEST_SANITIZER_ALLOCATIONS 1
#endif

using namespace buffetalligator;
namespace {
constexpr size_t slab_bytes = 64ull << 20;  ///< The built-in placements' slab size.
constexpr size_t rounds = 4;                ///< Churn rounds per placement.
constexpr size_t ring_length = 4;           ///< Slices each thread keeps alive at once.
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Stops the test on a violated contract.
 */
void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAILED: %s\n", message); std::abort(); }
}
/** --------------------------------------------------------------------------------------------------------- Settle
 * @brief Waits up to ten seconds for retired backing usage to recover.
 */
bool settle(const BuffetDescriptor& placement, size_t usage) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto report_at = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (Memory::placement_usage(placement) != usage) {
        const auto now = std::chrono::steady_clock::now();
        if (now > deadline) return false;
        if (now >= report_at) {
            LOG_INFO_STREAM << placement.type_name << ": waiting for backing usage "
                << Memory::placement_usage(placement) << " to recover to " << usage << " bytes";
            report_at = now + std::chrono::seconds(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}
/** --------------------------------------------------------------------------------------------------------- Malloc In Use
 * @brief Bytes the C allocator has handed out and not had back, excluding the freed blocks it caches.
 */
std::optional<size_t> malloc_in_use() {
#if defined(ALLIGATOR_TEST_SANITIZER_ALLOCATIONS)
    return __sanitizer_get_current_allocated_bytes();
#elif defined(ALLIGATOR_TEST_ASAN)
    return std::nullopt;
#elif defined(__APPLE__)
    malloc_statistics_t statistics{};
    malloc_zone_statistics(nullptr, &statistics);
    return statistics.size_in_use;
#elif defined(__linux__)
    const struct mallinfo2 info = mallinfo2();
    return info.uordblks + info.hblkhd;
#endif
}
/** --------------------------------------------------------------------------------------------------------- Settled Residency
 * @brief Process residency after the C allocator returns the freed blocks it caches.
 */
size_t settled_residency() {
#if defined(__APPLE__)
    malloc_zone_pressure_relief(nullptr, 0);
#elif defined(__linux__)
    malloc_trim(0);
#endif
#if defined(__APPLE__)
    mach_task_basic_info_data_t information{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    require(task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
        reinterpret_cast<task_info_t>(&information), &count) == KERN_SUCCESS,
        "reading process residency failed");
    return information.resident_size;
#elif defined(__linux__)
    FILE* status = std::fopen("/proc/self/statm", "r");
    require(status != nullptr, "opening process residency failed");
    unsigned long virtual_pages = 0, resident_pages = 0;
    require(std::fscanf(status, "%lu %lu", &virtual_pages, &resident_pages) == 2,
        "reading process residency failed");
    std::fclose(status);
    return resident_pages * static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
}
/** --------------------------------------------------------------------------------------------------------- Churn
 * @brief One thread's claim, share, view, novel and every resize path, with a bounded live ring.
 */
void churn(const BuffetDescriptor* placement, size_t max_claim, size_t claims, size_t seed) {
    std::array<Slice, ring_length> ring;
    for (size_t claim = 0; claim < claims; ++claim) {
        const size_t mixed = (seed * 2654435761u + claim * 40503u) % max_claim + 1;
        Slice& slot = ring[claim % ring_length];
        const uint8_t marker = static_cast<uint8_t>(claim ^ 0x5a);
        switch (claim % 16) {
        case 0:  // Sole novel owner grows in place where the placement can realloc.
            slot = Slice(mixed, true, placement);
            slot.data<uint8_t>()[0] = marker;
            slot.resize(mixed + 4096, true, true, placement);
            require(slot.size_bytes() == ((mixed + 4096 + 63) & ~size_t(63)) && slot.data<uint8_t>()[0] == marker,
                "novel growth lost its size or payload");
            break;
        case 1:  // Shrinking keeps the backing and narrows to a view.
            slot = Slice(mixed + 64, true, placement);
            slot.data<uint8_t>()[0] = marker;
            slot.resize(mixed / 2 + 1, true, true, placement);
            require(slot.size_bytes() == ((mixed / 2 + 1 + 63) & ~size_t(63)) && slot.data<uint8_t>()[0] == marker,
                "shrink lost its size or payload");
            break;
        case 2: {  // A shared novel buffer must grow by copy and leave the other owner intact.
            slot = Slice(mixed, true, placement);
            slot.data<uint8_t>()[0] = marker;
            Slice other_owner = slot;
            slot.resize(mixed + 4096, true, true, placement);
            require(slot.size_bytes() == ((mixed + 4096 + 63) & ~size_t(63)) && slot.data<uint8_t>()[0] == marker &&
                other_owner.size_bytes() == ((mixed + 63) & ~size_t(63)) && other_owner.data<uint8_t>()[0] == marker,
                "shared growth disturbed an owner");
            break;
        }
        case 3:  // A chain claim grows by copy out of its slab.
            slot = Slice(mixed, false, placement);
            slot.data<uint8_t>()[0] = marker;
            slot.resize(mixed + 4096, true, false, placement);
            require(slot.size_bytes() == ((mixed + 4096 + 63) & ~size_t(63)) && slot.data<uint8_t>()[0] == marker,
                "chain growth lost its size or payload");
            break;
        case 4:  // Discarding resize reallocates without carrying the payload.
            slot = Slice(mixed, false, placement);
            slot.resize(mixed + 4096, false, false, placement);
            require(slot.size_bytes() == ((mixed + 4096 + 63) & ~size_t(63)), "discarding resize returned the wrong size");
            break;
        case 5:  // Resizing a null Slice claims fresh storage.
            slot = Slice();
            slot.resize(mixed, true, false, placement);
            require(slot.size_bytes() == ((mixed + 63) & ~size_t(63)), "null resize returned the wrong size");
            break;
        default:
            slot = Slice(mixed, false, placement);
            require(slot.size_bytes() == ((mixed + 63) & ~size_t(63)), "chain claim returned the wrong size");
        }
        slot.data<uint8_t>()[0] = static_cast<uint8_t>(claim);
        Slice shared = slot;
        Slice view = shared.slice(0, std::max<size_t>(1, mixed / 2));
        require(view.data<uint8_t>()[0] == static_cast<uint8_t>(claim), "view lost its backing");
    }
}
/** --------------------------------------------------------------------------------------------------------- Exercise
 * @brief Checks backing bytes and allocator residency after repeated ownership churn.
 */
void exercise(const BuffetDescriptor* placement, size_t live_budget, size_t churn_budget, bool device_memory) {
    const size_t threads = std::max(1u, std::thread::hardware_concurrency());
    const size_t max_claim = std::min<size_t>(8ull << 20, live_budget / (threads * ring_length * 2));
    const size_t claims = churn_budget / ((rounds + 1) * threads * (max_claim / 2));  // A total leak stays inside the budget.
    for (size_t index = 0; index < 3; ++index) {
        Slice warm(placement->default_size / 2, false, placement);
    }
    const size_t usage_before = Memory::placement_usage(*placement);
    const size_t allocated_before = Memory::placement_allocations(*placement);
    size_t resident_before = 0;
    std::optional<size_t> malloc_before;
    for (size_t round = 0; round <= rounds; ++round) {
        LOG_INFO_STREAM << placement->type_name << ": ownership churn round "
            << (round + 1) << '/' << (rounds + 1);
        if (round == 1) {
            require(settle(*placement, usage_before), "warmup round leaked");
            resident_before = settled_residency();  // The warmup round has touched the live chain.
            malloc_before = malloc_in_use();
        }
        std::vector<std::thread> team;
        for (size_t thread = 0; thread < threads; ++thread) {
            team.emplace_back(&churn, placement, max_claim, claims, round * threads + thread);
        }
        for (std::thread& worker : team) worker.join();
    }
    const size_t churned = Memory::placement_allocations(*placement) - allocated_before;
    require(churned > 4 * slab_bytes, "churn did not cycle enough slabs to expose a leak");
    if (!settle(*placement, usage_before)) {
        LOG_ERROR_STREAM << placement->type_name << ": retained backing bytes changed from "
            << usage_before << " to " << Memory::placement_usage(*placement);
        require(false, "retired backing bytes leaked");
    }
    const std::optional<size_t> malloc_after = malloc_in_use();
    const size_t resident_after = settled_residency();
    const long long resident_growth = static_cast<long long>(resident_after) - static_cast<long long>(resident_before);
    LOG_INFO_STREAM << placement->type_name << ": churned " << (churned >> 20)
        << " MiB, live " << (usage_before >> 20) << " MiB, residency change "
        << (resident_growth >> 20) << " MiB";
    if (malloc_before && malloc_after) {
        const long long malloc_growth = static_cast<long long>(*malloc_after)
            - static_cast<long long>(*malloc_before);
        LOG_INFO_STREAM << placement->type_name << ": application-owned heap change "
            << malloc_growth << " bytes";
        require(malloc_growth < (1ll << 20), "the application heap kept memory the churn released");
    }
#if defined(ALLIGATOR_TEST_ASAN)
    static_cast<void>(device_memory);
    LOG_INFO_STREAM << "ASan process residency includes sanitizer retention and is diagnostic only";
#else
    require(!device_memory || resident_growth < static_cast<long long>(slab_bytes),
        "device memory residency grew past the live chain");
#endif
}
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Sizes the live and total churn budgets from available memory, then churns each host-visible
 * placement; even a total leak of every churned slab stays under a quarter of available memory.
 */
int main() {
#if defined(ALLIGATOR_TEST_SANITIZER_ALLOCATIONS)
    LOG_INFO_STREAM << "ASan heap accounting uses live application allocations, excluding quarantine";
#elif defined(ALLIGATOR_TEST_ASAN)
    LOG_INFO_STREAM << "ASan live-allocation statistics are unavailable; exact backing ownership "
        << "checks remain active and process residency is diagnostic only";
#endif
    uint64_t available_bytes = 0;
#if defined(__APPLE__)
    vm_statistics64_data_t statistics{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    require(host_statistics64(mach_host_self(), HOST_VM_INFO64,
        reinterpret_cast<host_info64_t>(&statistics), &count) == KERN_SUCCESS,
        "reading available memory failed");
    available_bytes = static_cast<uint64_t>(statistics.free_count + statistics.inactive_count)
        * vm_kernel_page_size;
#elif defined(__linux__)
    struct sysinfo information{};
    require(sysinfo(&information) == 0, "reading available memory failed");
    available_bytes = static_cast<uint64_t>(information.freeram + information.bufferram)
        * information.mem_unit;
#endif
    const size_t live_budget = std::min<size_t>(1ull << 30, available_bytes / 8);
    const size_t churn_budget = std::min<size_t>(8ull << 30, available_bytes / 4);
    require(live_budget >= 4 * slab_bytes, "not enough available memory to churn safely");
    static_cast<void>(Slice::default_placement());
    exercise(BuffetDescriptors::get(0), live_budget, churn_budget, false);
    if (GPU::exists()) {
        exercise(VulkanContext::buffer_placement(), live_budget, churn_budget, true);
    }
    std::printf("memory leak test passed\n");
    return 0;
}
