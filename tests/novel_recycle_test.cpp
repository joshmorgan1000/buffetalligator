/** --------------------------------------------------------------------------------------------------------- Novel Recycling
 * @file novel_recycle_test.cpp
 * @brief Checks novel backing geometry, shared lifetime, and bounded concurrent slot reuse.
 */
#include "test_support.hpp"
#include <alligator.hpp>
extern "C" {
#include "core/ba_core.h"
}
#include <array>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>
#include <vector>

using buffetalligator::Slice;
static constexpr size_t claim_bytes = 6144;
/** --------------------------------------------------------------------------------------------------------- Settle
 * @brief Waits for the worker to return released novel backing.
 */
static void settle(uint32_t placement, uint64_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;) {
        ba_stats_t stats;
        ba_stats(placement, &stats);
        if (stats.novel_bytes_allocated - stats.novel_bytes_freed == expected) return;
        TEST_REQUIRE(std::chrono::steady_clock::now() < deadline, "Novel retirement did not settle");
        std::this_thread::yield();
    }
}
/** --------------------------------------------------------------------------------------------------------- Recycle
 * @brief Claims and releases bounded batches on one concurrent caller.
 */
static void recycle(std::exception_ptr* failure) {
    try {
        std::array<Slice, 64> live;
        for (size_t offset = 0; offset < 750000; offset += live.size()) {
            const size_t count = std::min(live.size(), size_t(750000) - offset);
            for (size_t index = 0; index < count; ++index) {
                live[index] = Slice(claim_bytes, true);
                TEST_EQUAL(live[index].size_bytes(), claim_bytes, "Public Slice size decoder disagrees");
                TEST_REQUIRE(live[index].data()[0] == 0 && live[index].data()[claim_bytes - 1] == 0,
                    "New backing is not zeroed");
                live[index].data()[0] = 0x5a;
                live[index].data()[claim_bytes - 1] = 0x6b;
            }
            for (size_t index = 0; index < count; ++index) live[index].free();
        }
    } catch (...) { *failure = std::current_exception(); }
}
/** --------------------------------------------------------------------------------------------------------- Geometry
 * @brief Checks novel backing around the probed large-page boundary for both built-ins.
 */
static void geometry(uint32_t placement) {
    ba_sysinfo_t machine;
    ba_sysinfo(&machine);
    ba_placement_desc_t description;
    ba_placement_describe(placement, &description);
    const auto* owner = buffetalligator::BuffetMenu::get(placement);
    const size_t large = machine.large_page ? machine.large_page : machine.page;
    for (const size_t bytes : {claim_bytes, large - 64, large, large + 64}) {
        const bool use_large = (description.flags & BA_PLACEMENT_LARGE_PAGES)
            && machine.large_page && bytes >= machine.large_page;
        const uint64_t granule = use_large ? machine.large_page : machine.page;
        const uint64_t expected = (bytes + granule - 1) & ~(granule - 1);
        ba_stats_t before, after;
        ba_stats(placement, &before);
        Slice slice(bytes, true, owner);
        ba_stats(placement, &after);
        TEST_EQUAL(slice.size_bytes(), bytes, "Slice size mismatch at page boundary");
        TEST_EQUAL(after.novel_bytes_allocated - before.novel_bytes_allocated, expected,
            "Novel backing geometry mismatch");
        TEST_EQUAL(reinterpret_cast<uintptr_t>(slice.raw()) % granule, 0, "Novel mapping alignment mismatch");
        slice.free();
        settle(placement, 0);
    }
}
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Checks capacity beyond the former ceiling and more claims than the current slot count.
 */
int main() {
    test_support::start(__FILE__);
    geometry(0);
    geometry(1);
    const uint32_t placement = ba_placement_default();
    ba_sysinfo_t machine;
    ba_sysinfo(&machine);
    const uint64_t expected = (claim_bytes + machine.page - 1) & ~(machine.page - 1);
    Slice owner(claim_bytes, true);
    owner.data()[0] = 0x7c;
    Slice retained = owner.slice(0, 64);
    owner.free();
    TEST_REQUIRE(retained.size_bytes() == 64 && retained.data()[0] == 0x7c,
        "Sub-slice did not retain novel backing");
    std::vector<Slice> held;
    held.reserve(150000);
    for (size_t index = 0; index < 150000; ++index) held.emplace_back(64, true);
    held.clear();
    settle(placement, expected);
    std::array<std::thread, 4> workers;
    std::array<std::exception_ptr, 4> failures;
    for (size_t index = 0; index < workers.size(); ++index)
        workers[index] = std::thread(recycle, &failures[index]);
    for (auto& worker : workers) worker.join();
    for (const auto& failure : failures) if (failure) std::rethrow_exception(failure);
    settle(placement, expected);
    TEST_EQUAL(retained.data()[0], 0x7c, "Recycling changed retained backing");
    retained.free();
    settle(placement, 0);
    LOG_INFO_STREAM << "PASS novel recycling: bytes=" << claim_bytes << " backing=" << expected
        << " recycled_claims=3000000 workers=4 slot_bits=" << BA_SLOT_BITS;
}
