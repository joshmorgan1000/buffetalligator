/** --------------------------------------------------------------------------------------------------------- Core Claim Test
 * @file core_claim_test.cpp
 * @brief Verifies C plate lifetime, placement contracts, zeroing, and concurrent claims.
 */
extern "C" {
#include "core/ba_core.h"
}
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
thread_local unsigned caller_allocations = 0;
/** --------------------------------------------------------------------------------------------------------- Require
 * @brief Fails a core contract assertion.
 */
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
/** --------------------------------------------------------------------------------------------------------- Allocate
 * @brief Obtains a zeroed test allocation on the calling thread.
 */
ba_handle_t* allocate(size_t bytes, void*) {
    ++caller_allocations;
    void* memory = std::aligned_alloc(64, bytes);
    if (!memory) return nullptr;
    auto* handle = static_cast<ba_handle_t*>(std::malloc(sizeof(ba_handle_t)));
    if (!handle) { std::free(memory); return nullptr; }
    std::memset(memory, 0, bytes);
    *handle = {memory, nullptr};
    return handle;
}
/** --------------------------------------------------------------------------------------------------------- Deallocate
 * @brief Releases test data and its C handle.
 */
void deallocate(ba_handle_t* handle, void*) { std::free(handle->substrate_handle); std::free(handle); }
/** --------------------------------------------------------------------------------------------------------- Host
 * @brief Resolves the test allocation's host address.
 */
void* host(ba_handle_t* handle) { return handle->substrate_handle; }
/** --------------------------------------------------------------------------------------------------------- Failed Host Allocation
 * @brief Allocates a handle whose host mapping cannot be obtained.
 */
ba_handle_t* failed_host_allocate(size_t, void*) { return new ba_handle_t{nullptr, nullptr}; }
std::atomic<unsigned> failed_substrates{0};
std::atomic<unsigned> failed_handles{0};
/** --------------------------------------------------------------------------------------------------------- Failed Substrate Release
 * @brief Counts cleanup of a failed substrate mapping.
 */
void failed_host_free(ba_handle_t*, void*) { failed_substrates.fetch_add(1); }
/** --------------------------------------------------------------------------------------------------------- Failed Handle Release
 * @brief Counts and deletes a failed allocation handle.
 */
void failed_handle_destroy(ba_handle_t* handle) { failed_handles.fetch_add(1); delete handle; }
/** --------------------------------------------------------------------------------------------------------- Claim Sizes
 * @brief Checks requested sizes, alignment, initial contents, and direct allocation routing.
 */
void sizes(uint32_t type) {
    for (size_t size : {1, 63, 64, 65, 1000, 4096, 65536}) {
        for (size_t operation = 0; operation < 10000; ++operation) {
            ba_slice_t slice;
            require(ba_claim(type, size, 0, &slice) == BA_OK, "claim failed");
            require(reinterpret_cast<uintptr_t>(slice.ptr) % 64 == 0, "unaligned claim");
            require(slice.meta >> BA_SLOT_BITS == size, "incorrect size");
            const auto* begin = static_cast<unsigned char*>(slice.ptr);
            require(std::all_of(begin, begin + size, [](unsigned char value) { return value == 0; }), "claim not zeroed");
            std::memset(slice.ptr, 0xa5, size);
            ba_release(&slice);
        }
    }
    ba_slice_t first, second;
    require(ba_claim(type, 4096, 0, &first) == BA_OK, "first adjacent claim failed");
    require(ba_claim(type, 1024, 0, &second) == BA_OK, "second adjacent claim failed");
    require(static_cast<char*>(second.ptr) - static_cast<char*>(first.ptr) == 4096, "nonadjacent claims");
    ba_release(&first); ba_release(&second);
    ba_stats_t statistics;
    ba_stats(type, &statistics);
    ba_slice_t before, direct, after;
    require(ba_claim(type, 64, 0, &before) == BA_OK, "before direct failed");
    require(ba_claim(type, statistics.plate_bytes + 64, 0, &direct) == BA_OK, "direct failed");
    require(ba_claim(type, 64, 0, &after) == BA_OK, "after direct failed");
    require((direct.meta & BA_SLOT_MASK) != (before.meta & BA_SLOT_MASK), "direct reused thread plate");
    require(static_cast<char*>(after.ptr) - static_cast<char*>(before.ptr) == 64, "direct changed thread cursor");
    ba_slice_t novel;
    const unsigned allocations = caller_allocations;
    require(ba_claim(type, statistics.slab_bytes, 0, &novel) == BA_OK, "novel failed");
    require(ba_slice_handle(&novel) != ba_slice_handle(&after), "novel reused slab handle");
    if (type == 2) require(caller_allocations == allocations + 1, "novel did not allocate on caller");
    ba_release(&before); ba_release(&direct); ba_release(&after); ba_release(&novel);
}
/** --------------------------------------------------------------------------------------------------------- Concurrent Claims
 * @brief Exercises plate turnover with thread-exit sealing under contention.
 */
void concurrent(uint32_t type) {
    for (size_t operation = 0; operation < 100000; ++operation) {
        ba_slice_t slice;
        require(ba_claim(type, 1024, 0, &slice) == BA_OK, "concurrent claim failed");
        auto* memory = static_cast<unsigned char*>(slice.ptr);
        require(memory[0] == 0 && memory[1023] == 0, "concurrent dirty claim");
        memory[0] = 19; memory[1023] = 91;
        ba_release(&slice);
    }
}
/** --------------------------------------------------------------------------------------------------------- Held Slab
 * @brief Retains a direct plate while advancing beyond its parent slab.
 */
void held_slab(uint32_t type, ba_slice_t* held) {
    ba_stats_t statistics; ba_stats(type, &statistics);
    require(ba_claim(type, statistics.slab_bytes / 2, 0, held) == BA_OK, "held direct failed");
    ba_retain(held);
    ba_slice_t copy = *held;
    ba_release(&copy);
    for (unsigned index = 0; index < 4; ++index) {
        ba_slice_t slice;
        require(ba_claim(type, statistics.slab_bytes / 2, 0, &slice) == BA_OK, "held rollover failed");
        ba_release(&slice);
    }
}
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs the private C core contracts independently of the C++ arena.
 */
int main() {
    require(ba_placement_count() == 2 && ba_placement_default() == 1, "builtin identifiers");
    require(!std::strcmp(ba_placement_name(0), "heap") && !std::strcmp(ba_placement_name(1), "aligned_heap"), "builtin names");
    ba_placement_desc_t descriptor{};
    descriptor.struct_size = sizeof(descriptor); descriptor.name = "core_test";
    descriptor.slab_bytes = 64ull * 1024 * 1024; descriptor.base_alignment = 64;
    descriptor.alloc = allocate; descriptor.free = deallocate; descriptor.host_ptr = host;
    uint32_t type;
    require(ba_placement_register(&descriptor, &type) == BA_OK && type == 2, "custom registration");
    for (uint32_t placement = 0; placement < 3; ++placement) std::thread(sizes, placement).join();
    ba_slice_t held;
    std::thread(held_slab, type, &held).join();
    ba_stats_t before; ba_stats(type, &before);
    require(before.slab_bytes_allocated - before.slab_bytes_freed >= 2 * before.slab_bytes, "held slab retired early");
    ba_release(&held);
    ba_stats_t after; ba_stats(type, &after);
    const uint64_t retire_deadline = ba_os_now_ns() + 10000000000ull;
    while (after.slab_bytes_freed + after.reserved_bytes <= before.slab_bytes_freed + before.reserved_bytes && ba_os_now_ns() < retire_deadline) {
        ba_os_yield(); ba_stats(type, &after);
    }
    require(after.slab_bytes_freed + after.reserved_bytes >= before.slab_bytes_freed + before.reserved_bytes + after.slab_bytes, "held slab not retired");
    std::array<std::thread, 16> workers;
    for (auto& worker : workers) worker = std::thread(concurrent, type);
    for (auto& worker : workers) worker.join();
    ba_stats(type, &after);
    const uint64_t settle_deadline = ba_os_now_ns() + 10000000000ull;
    while (after.slab_bytes_allocated - after.slab_bytes_freed > (after.runway_target + 1) * after.slab_bytes && ba_os_now_ns() < settle_deadline) {
        ba_os_yield(); ba_stats(type, &after);
    }
    require(after.slab_bytes_allocated - after.slab_bytes_freed <= (after.runway_target + 1) * after.slab_bytes, "slabs leaked after thread exit");
    descriptor.base_alignment = 16;
    require(ba_placement_register(&descriptor, &type) == BA_E_ALIGNMENT, "bad alignment accepted");
    descriptor.base_alignment = 64;
    descriptor.alloc = failed_host_allocate;
    descriptor.free = failed_host_free;
    descriptor.destroy_handle = failed_handle_destroy;
    require(ba_placement_register(&descriptor, &type) == BA_E_ALLOC, "failed host accepted");
    require(failed_substrates.load() == 1 && failed_handles.load() == 1, "failed host handle leaked");
    ba_slice_t invalid;
    require(ba_claim(ba_placement_count(), 64, 0, &invalid) == BA_E_PLACEMENT, "unknown placement accepted");
}
