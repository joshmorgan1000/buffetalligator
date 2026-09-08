/** --------------------------------------------------------------------------------------------------------- Core Worker Test
 * @file core_worker_test.cpp
 * @brief Verifies background allocation, reclamation, and explicit shutdown.
 */
extern "C" {
#include "core/ba_core.h"
}
#include <atomic>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace {
const std::thread::id caller = std::this_thread::get_id();
std::atomic<unsigned> background_allocations{0};
std::atomic<unsigned> background_frees{0};
std::atomic<bool> finish_load{false};
std::atomic<unsigned> cached_novel_allocations{0};
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Fails a worker contract assertion.
 */
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
/** --------------------------------------------------------------------------------------------------------- Allocate
 * @brief Allocates a zeroed test slab and records the callback thread.
 */
ba_handle_t* allocate(size_t bytes, void*) {
    if (std::this_thread::get_id() != caller) background_allocations.fetch_add(1);
    void* memory = std::aligned_alloc(64, bytes);
    if (!memory) return nullptr;
    auto* handle = static_cast<ba_handle_t*>(std::malloc(sizeof(ba_handle_t)));
    if (!handle) { std::free(memory); return nullptr; }
    std::memset(memory, 0, bytes);
    *handle = {memory, nullptr};
    return handle;
}
/** --------------------------------------------------------------------------------------------------------- Cache Allocate
 * @brief Counts allocations of the cache test's novel size separately from slabs.
 */
ba_handle_t* cache_allocate(size_t bytes, void* context) {
    if (bytes == 8ull * 1024 * 1024) cached_novel_allocations.fetch_add(1);
    return allocate(bytes, context);
}
/** --------------------------------------------------------------------------------------------------------- Deallocate
 * @brief Releases a test allocation and records the teardown thread.
 */
void deallocate(ba_handle_t* handle, void*) {
    std::free(handle->substrate_handle); std::free(handle);
    if (std::this_thread::get_id() != caller) background_frees.fetch_add(1);
}
/** --------------------------------------------------------------------------------------------------------- Host
 * @brief Resolves the test handle's host pointer.
 */
void* host(ba_handle_t* handle) { return handle->substrate_handle; }
/** --------------------------------------------------------------------------------------------------------- Sustained Load
 * @brief Drives parallel slab turnover until the main thread ends the interval.
 */
void sustained_load(uint32_t type) {
    while (!finish_load.load(std::memory_order_relaxed)) {
        ba_slice_t slice;
        require(ba_claim(type, 1024, 0, &slice) == BA_OK, "sustained claim failed");
        ba_release(&slice);
    }
}
/** --------------------------------------------------------------------------------------------------------- Slab Edges
 * @brief Checks recycled slab boundaries after dirtying direct claims.
 */
void slab_edges(uint32_t type) {
    ba_stats_t statistics; ba_stats(type, &statistics);
    for (unsigned iteration = 0; iteration < 16; ++iteration) {
        ba_slice_t slice;
        const size_t bytes = statistics.slab_bytes - 64;
        require(ba_claim(type, bytes, 0, &slice) == BA_OK, "edge claim failed");
        auto* memory = static_cast<unsigned char*>(slice.ptr);
        for (size_t index = 0; index < 4096; ++index) {
            require(memory[index] == 0 && memory[bytes - 4096 + index] == 0, "recycled edge dirty");
        }
        std::memset(memory, 0xd5, bytes);
        ba_release(&slice);
    }
}
/** --------------------------------------------------------------------------------------------------------- Rollovers
 * @brief Dirties and drops claims across repeated slab boundaries.
 */
void rollovers(uint32_t type) {
    for (size_t index = 0; index < 100000; ++index) {
        ba_slice_t slice;
        require(ba_claim(type, 1024, 0, &slice) == BA_OK, "rollover claim failed");
        auto* memory = static_cast<unsigned char*>(slice.ptr);
        require(memory[0] == 0 && memory[1023] == 0, "dirty claim");
        std::memset(memory, 0xc3, 1024);
        ba_release(&slice);
    }
}
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs worker preallocation, retirement, and stop contracts.
 */
int main() {
    ba_placement_desc_t descriptor{};
    descriptor.struct_size = sizeof(descriptor); descriptor.name = "worker_test";
    descriptor.slab_bytes = 8ull * 1024 * 1024; descriptor.base_alignment = 64;
    descriptor.alloc = allocate; descriptor.free = deallocate; descriptor.host_ptr = host;
    uint32_t type;
    require(ba_placement_register(&descriptor, &type) == BA_OK, "registration failed");
    require(background_allocations.load() >= 2, "initial slabs were not built by worker");
    std::thread(rollovers, type).join();
    ba_stats_t statistics;
    const uint64_t deadline = ba_os_now_ns() + 10000000000ull;
    do {
        ba_stats(type, &statistics);
        if (statistics.slab_bytes_allocated - statistics.slab_bytes_freed <= (statistics.runway_target + 1) * statistics.slab_bytes) break;
        ba_os_yield();
    } while (ba_os_now_ns() < deadline);
    require(statistics.slab_bytes_allocated >= 6 * statistics.slab_bytes, "insufficient rollovers");
    require(statistics.slab_bytes_allocated - statistics.slab_bytes_freed <= (statistics.runway_target + 1) * statistics.slab_bytes, "retired slabs retained");
    std::thread(slab_edges, type).join();
    std::array<std::thread, 16> load;
    for (auto& thread : load) thread = std::thread(sustained_load, type);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    finish_load.store(true, std::memory_order_relaxed);
    for (auto& thread : load) thread.join();
    ba_stats(type, &statistics);
    ba_sysinfo_t system; ba_sysinfo(&system);
    require(statistics.runway_misses <= system.hw_threads, "excessive runway misses");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ba_stats(type, &statistics);
    require(statistics.reserved_bytes <= (statistics.runway_target + 1) * statistics.slab_bytes, "reserve cap exceeded");
    ba_slice_t novel;
    require(ba_claim(type, 8ull * 1024 * 1024, BA_CLAIM_NOVEL, &novel) == BA_OK, "novel failed");
    const unsigned freed = background_frees.load();
    ba_release(&novel);
    const uint64_t novel_deadline = ba_os_now_ns() + 10000000000ull;
    while (background_frees.load() == freed && ba_os_now_ns() < novel_deadline) ba_os_yield();
    require(background_frees.load() > freed, "novel not freed by worker");
    ba_trim(type);
    ba_stats(type, &statistics);
    require(statistics.reserved_bytes == 0, "trim left reserves");
    descriptor.name = "cache_test";
    descriptor.slab_bytes = 16ull * 1024 * 1024;
    descriptor.novel_cache_bytes = 64ull * 1024 * 1024;
    descriptor.alloc = cache_allocate;
    uint32_t cache_type;
    require(ba_placement_register(&descriptor, &cache_type) == BA_OK, "cache registration failed");
    require(ba_claim(cache_type, 8ull * 1024 * 1024, BA_CLAIM_NOVEL, &novel) == BA_OK, "cache first claim failed");
    std::memset(novel.ptr, 0x79, 8ull * 1024 * 1024);
    ba_release(&novel);
    const uint64_t cache_deadline = ba_os_now_ns() + 10000000000ull;
    do { ba_stats(cache_type, &statistics); ba_os_yield(); }
    while (!statistics.novel_cache_bytes && ba_os_now_ns() < cache_deadline);
    require(statistics.novel_cache_bytes == 8ull * 1024 * 1024, "novel did not enter cache");
    require(ba_claim(cache_type, 8ull * 1024 * 1024, BA_CLAIM_NOVEL, &novel) == BA_OK, "cache second claim failed");
    require(cached_novel_allocations.load() == 1, "cache missed a prepared allocation");
    for (size_t index = 0; index < 8ull * 1024 * 1024; ++index) {
        require(static_cast<unsigned char*>(novel.ptr)[index] == 0, "cached novel was not zeroed");
    }
    ba_release(&novel);
    ba_trim(cache_type);
    ba_stats(cache_type, &statistics);
    require(statistics.reserved_bytes == 0 && statistics.novel_cache_bytes == 0, "trim retained cached allocations");
    descriptor.name = "budget_test";
    descriptor.slab_bytes = 1024 * 1024;
    descriptor.novel_cache_bytes = 0;
    descriptor.alloc = allocate;
    uint32_t budget_type;
    require(ba_placement_register(&descriptor, &budget_type) == BA_OK, "budget registration failed");
    ba_stats(budget_type, &statistics);
    const uint64_t base = (1ull + statistics.runway_target) * descriptor.slab_bytes;
    ba_budget_set(budget_type, base + 2 * descriptor.slab_bytes);
    ba_slice_t held[3];
    require(ba_claim(budget_type, descriptor.slab_bytes, BA_CLAIM_NOVEL, &held[0]) == BA_OK, "first budgeted allocation failed");
    require(ba_claim(budget_type, descriptor.slab_bytes, BA_CLAIM_NOVEL, &held[1]) == BA_OK, "second budgeted allocation failed");
    require(ba_claim(budget_type, descriptor.slab_bytes, BA_CLAIM_NOVEL, &held[2]) == BA_E_BUDGET, "third allocation exceeded budget");
    require(ba_claim(budget_type, descriptor.slab_bytes / 2, 0, &novel) == BA_OK, "budget denied an existing-slab claim");
    ba_release(&held[0]);
    const uint64_t previously_freed = statistics.novel_bytes_freed;
    const uint64_t restore_deadline = ba_os_now_ns() + 10000000000ull;
    do { ba_stats(budget_type, &statistics); ba_os_yield(); }
    while (statistics.novel_bytes_freed < previously_freed + descriptor.slab_bytes && ba_os_now_ns() < restore_deadline);
    require(ba_claim(budget_type, descriptor.slab_bytes, BA_CLAIM_NOVEL, &held[2]) == BA_OK, "released capacity was not reusable");
    ba_release(&held[1]); ba_release(&held[2]); ba_release(&novel);
    require(ba_pressure() <= BA_PRESSURE_CRITICAL, "invalid pressure result");
    const uint64_t stop_begin = ba_os_now_ns();
    ba_shutdown();
    require(ba_os_now_ns() - stop_begin < 10000000000ull, "shutdown timed out");
    require(ba_claim(type, 64, 0, &novel) == BA_E_CLOSED, "claim after shutdown succeeded");
}
