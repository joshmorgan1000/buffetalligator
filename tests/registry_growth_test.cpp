/** --------------------------------------------------------------------------------------------------------- Registry Growth Test
 * @file registry_growth_test.cpp
 * @brief Checks concurrent growth, failed commits, slot exhaustion, and retained slice lifetime.
 */
#include "test_support.hpp"
extern "C" {
#include "core/ba_core.h"
int ba_test_os_commit_real(void* base, uint64_t bytes);
}
#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

static std::atomic<bool> fail_commit{false};
static std::atomic<uint32_t> commit_calls{0};
/** --------------------------------------------------------------------------------------------------------- Commit
 * @brief Injects commit failure while retaining the real platform implementation.
 */
extern "C" int ba_os_commit(void* base, uint64_t bytes) {
    commit_calls.fetch_add(1, std::memory_order_relaxed);
    return fail_commit.load(std::memory_order_relaxed) ? -1 : ba_test_os_commit_real(base, bytes);
}
/** --------------------------------------------------------------------------------------------------------- Allocate
 * @brief Allocates small aligned backing for the exhaustion test.
 */
static ba_handle_t* allocate(size_t bytes, void*) {
    void* backing = ::operator new(bytes, std::align_val_t{64});
    std::memset(backing, 0, bytes);
    return new ba_handle_t{backing, nullptr};
}
/** --------------------------------------------------------------------------------------------------------- Release
 * @brief Releases custom backing and its placement handle.
 */
static void release(ba_handle_t* handle, void*) {
    ::operator delete(handle->substrate_handle, std::align_val_t{64});
    delete handle;
}
/** --------------------------------------------------------------------------------------------------------- Host Pointer
 * @brief Resolves the test placement's host pointer.
 */
static void* host_pointer(ba_handle_t* handle) { return handle->substrate_handle; }
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Fills a small registry while another thread retains and views a live anchor.
 */
int main() {
    test_support::start(__FILE__);
    ba_placement_desc_t description{};
    description.struct_size = sizeof(description);
    description.name = "registry_growth";
    description.slab_bytes = 65536;
    description.base_alignment = 64;
    description.budget_bytes = 64ull << 20;
    description.alloc = allocate;
    description.free = release;
    description.host_ptr = host_pointer;
    uint32_t placement = 0;
    TEST_EQUAL(ba_placement_register(&description, &placement), BA_OK, "placement registration failed");
    ba_sysinfo_t machine;
    ba_sysinfo(&machine);
    const uint32_t stride = std::min(BA_PLATE_COUNT, (machine.cache_line + 63) / 64);
    const uint32_t initial = ba_slot_capacity();
    TEST_REQUIRE(initial > stride && initial < BA_PLATE_COUNT, "registry did not start small");
    const size_t prefix_claims = initial / stride - 1;
    std::vector<ba_slice_t> held(BA_PLATE_COUNT - 1, ba_slice_t{BA_NULL_META, nullptr});
    for (size_t index = 0; index < prefix_claims; ++index) {
        TEST_EQUAL(ba_claim(placement, 64, BA_CLAIM_NOVEL, &held[index]), BA_OK,
            "initial claim failed");
    }
    TEST_EQUAL(ba_slot_capacity(), initial, "initial claims grew the registry early");
    *static_cast<uint64_t*>(held[0].ptr) = 0x123456789abcdef0ull;
    ba_slice_t anchor;
    TEST_EQUAL(ba_view(&held[0], 0, 16, &anchor), BA_OK, "anchor view failed");
    const void* original_pointer = anchor.ptr;
    ba_stats_t before, after;
    ba_trim(placement);
    ba_stats(placement, &before);
    fail_commit.store(true, std::memory_order_relaxed);
    for (size_t attempt = 0; attempt < 4; ++attempt) {
        ba_slice_t rejected{BA_NULL_META, nullptr};
        TEST_EQUAL(ba_claim(placement, 64, BA_CLAIM_NOVEL, &rejected), BA_E_ALLOC,
            "commit failure did not report BA_E_ALLOC");
        TEST_REQUIRE(rejected.meta == BA_NULL_META && rejected.ptr == nullptr,
            "commit failure published a slice");
    }
    ba_slice_t rejected_plate{BA_NULL_META, nullptr};
    TEST_EQUAL(ba_claim(placement, 64, 0, &rejected_plate), BA_E_ALLOC,
        "thread-plate growth failure did not report BA_E_ALLOC");
    fail_commit.store(false, std::memory_order_relaxed);
    ba_stats(placement, &after);
    TEST_EQUAL(before.novel_bytes_allocated, after.novel_bytes_allocated,
        "commit failure allocated backing");
    TEST_EQUAL(ba_slot_capacity(), initial, "commit failure published capacity");
    const uint64_t live_bytes = after.slab_bytes_allocated - after.slab_bytes_freed
        + after.novel_bytes_allocated - after.novel_bytes_freed;
    ba_budget_set(placement, live_bytes + 64);
    TEST_EQUAL(ba_claim(placement, 64, BA_CLAIM_NOVEL, &held[prefix_claims]), BA_OK,
        "claim after failed growth did not recover");
    ba_budget_set(placement, description.budget_bytes);
    TEST_EQUAL((held[prefix_claims].meta & BA_SLOT_MASK), initial,
        "failed growth consumed a fresh slot");
    TEST_REQUIRE(ba_slot_capacity() > initial, "successful commit did not grow capacity");
    const size_t threads = 2 * machine.hw_threads;
    std::barrier start(static_cast<std::ptrdiff_t>(threads + 2));
    std::atomic<bool> reading{true};
    std::atomic<uint64_t> views{0};
    std::thread reader([&] {
        start.arrive_and_wait();
        do {
            ba_slice_t view;
            TEST_EQUAL(ba_view(&anchor, 0, 8, &view), BA_OK, "view during growth failed");
            TEST_REQUIRE(view.ptr == original_pointer &&
                *static_cast<uint64_t*>(view.ptr) == 0x123456789abcdef0ull,
                "growth invalidated a retained slice");
            ba_slice_t copy = view;
            ba_retain(&copy);
            ba_release(&view);
            ba_release(&copy);
            views.fetch_add(1, std::memory_order_relaxed);
        } while (reading.load(std::memory_order_acquire));
    });
    std::vector<std::thread> workers;
    const size_t begin = prefix_claims + 1;
    const size_t remaining = held.size() - begin;
    for (size_t worker = 0; worker < threads; ++worker) {
        workers.emplace_back([&, worker] {
            start.arrive_and_wait();
            const size_t first = begin + remaining * worker / threads;
            const size_t last = begin + remaining * (worker + 1) / threads;
            for (size_t index = first; index < last; ++index) {
                TEST_EQUAL(ba_claim(placement, 64, BA_CLAIM_NOVEL, &held[index]), BA_OK,
                    "concurrent growth claim failed");
                TEST_EQUAL(*static_cast<uint64_t*>(held[index].ptr), 0, "backing is not zeroed");
                *static_cast<uint64_t*>(held[index].ptr) = index;
            }
        });
    }
    start.arrive_and_wait();
    for (auto& worker : workers) worker.join();
    reading.store(false, std::memory_order_release);
    reader.join();
    TEST_REQUIRE(views.load() != 0, "concurrent reader did not execute");
    TEST_EQUAL(ba_slot_capacity(), BA_PLATE_COUNT, "registry did not reach its encoding ceiling");
    std::vector<uint32_t> slots;
    for (size_t index = 0; index < held.size(); ++index) {
        const auto& slice = held[index];
        slots.push_back(static_cast<uint32_t>(slice.meta & BA_SLOT_MASK));
        TEST_EQUAL(slice.meta >> BA_SLOT_BITS, 64, "growth changed the size encoding");
        if (index >= begin) TEST_EQUAL(*static_cast<uint64_t*>(slice.ptr), index,
            "concurrent backing allocations overlap");
    }
    std::sort(slots.begin(), slots.end());
    for (size_t index = 0; index < slots.size(); ++index)
        TEST_EQUAL(slots[index], index + 1, "slot IDs are duplicated or missing");
    for (size_t attempt = 0; attempt < 1024; ++attempt) {
        ba_slice_t rejected{BA_NULL_META, nullptr};
        TEST_EQUAL(ba_claim(placement, 64, BA_CLAIM_NOVEL, &rejected), BA_E_SLOTS,
            "exhaustion did not report BA_E_SLOTS");
    }
    const uint64_t reusable_slot = held.back().meta & BA_SLOT_MASK;
    ba_release(&held.back());
    ba_trim(placement);
    TEST_EQUAL(ba_claim(placement, 64, BA_CLAIM_NOVEL, &held.back()), BA_OK,
        "exhausted registry could not reuse a returned slot");
    TEST_EQUAL((held.back().meta & BA_SLOT_MASK), reusable_slot, "returned slot was not reused");
    for (auto& slice : held) ba_release(&slice);
    ba_trim(placement);
    ba_stats(placement, &after);
    TEST_EQUAL(after.novel_bytes_allocated - after.novel_bytes_freed, 64,
        "failure or exhaustion leaked backing charges");
    ba_shutdown();
    TEST_REQUIRE(anchor.ptr == original_pointer &&
        *static_cast<uint64_t*>(anchor.ptr) == 0x123456789abcdef0ull,
        "shutdown invalidated live registry storage");
    ba_release(&anchor);
    ba_stats(placement, &after);
    TEST_EQUAL(after.novel_bytes_allocated, after.novel_bytes_freed,
        "late release failed to retire the final slice");
    std::printf("registry growth passed: threads=%zu slots=%u commits=%u views=%llu\n",
        threads, BA_PLATE_COUNT - 1, commit_calls.load(),
        static_cast<unsigned long long>(views.load()));
}
