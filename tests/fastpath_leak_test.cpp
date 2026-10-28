/** --------------------------------------------------------------------------------------------------------- Fast Path Leak Test
 * @file fastpath_leak_test.cpp
 * @brief Churns claims, copies, views, novel and typed claims across threads and pools, then checks that every
 * record is released, that the buffer count settles to the pools the allocator keeps, and that resident memory
 * stops growing between rounds. Runs on whichever backend the process freezes on, so it covers Vulkan buffers
 * on a GPU machine and heap buffers under ALLIGATOR_GPU_BACKEND=cpu, where a sanitizer can also watch it.
 */
#include <alligator.hpp>
#include <alligator/containers.hpp>
#include "functional_support.hpp"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

using namespace buffetalligator;
using functional::require;
namespace {
constexpr size_t THREADS = 4;                   ///< Claiming threads per round.
constexpr size_t ROUNDS = 3;                    ///< Rounds; resident memory is compared across the last two.
constexpr size_t CLAIMS = 20000;                ///< Claims per thread per round, enough to roll pools over.
constexpr size_t KEEP = 256;                    ///< Claims each thread keeps live before releasing them.
constexpr uint64_t RESIDENT_SLACK = 48ull << 20;  ///< Growth allowed between settled rounds.
/** --------------------------------------------------------------------------------------------------------- Counted
 * @brief A typed payload whose constructions and destructions are counted.
 */
struct alignas(64) Counted {
    static inline std::atomic<int64_t> live{0};
    uint64_t value = 0;
    Counted() { live.fetch_add(1, std::memory_order_relaxed); }
    ~Counted() { live.fetch_sub(1, std::memory_order_relaxed); }
};
/** --------------------------------------------------------------------------------------------------------- Claimed Records
 * @brief Records in the pool whose claim bit is set, the null record included.
 */
size_t claimed_records() {
    size_t claimed = 0;
    for (uint32_t index = 0; index < 0x100000u; ++index) claimed += (IBuffet::ref(index)->id_ & 0x80000000u) != 0;
    return claimed;
}
/** --------------------------------------------------------------------------------------------------------- Churn
 * @brief One thread's share of a round: mixed sizes, copies, views, a few novel and typed claims.
 */
void churn(size_t seed) {
    std::vector<Slice> kept;
    kept.reserve(KEEP);
    for (size_t index = 0; index < CLAIMS; ++index) {
        const size_t bytes = 64 + ((seed * 7919 + index * 104729) % 8192);
        Slice claim(bytes);
        claim.data<uint32_t>()[0] = static_cast<uint32_t>(index);
        if ((index & 7) == 0) {
            Slice copy(claim);
            Slice view = claim.slice(0, 64);
            require(copy.raw() == claim.raw() && view.raw() == claim.raw(), "a copy or a view moved the bytes");
        }
        if ((index & 63) == 0) {
            Slice novel(bytes, true);
            require(novel.is_novel(), "a novel claim is not on its own buffer");
        }
        if ((index & 127) == 0) {
            SliceT<Counted> typed(true);
            SliceT<Counted> shared = typed;
            typed.free();
            require(Counted::live.load() >= 1, "a typed element went before its last handle");
        }
        if ((index & 255) == 0) {
            uint64_t stack[8] = {index, 1, 2, 3, 4, 5, 6, 7};
            WeakSlice weak(stack, sizeof(stack));
            Slice owned = weak.slice();
            require(owned.get_as<uint64_t>() == index, "a weak copy lost its bytes");
        }
        kept.push_back(std::move(claim));
        if (kept.size() == KEEP) kept.clear();
    }
}
/** --------------------------------------------------------------------------------------------------------- Round
 * @brief Runs the churn on every thread and waits for all of them.
 */
void round(size_t number) {
    std::vector<std::thread> threads;
    for (size_t thread = 0; thread < THREADS; ++thread) threads.emplace_back(&churn, number * THREADS + thread);
    for (std::thread& thread : threads) thread.join();
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Churns, then checks records, buffers, typed destructors and resident memory.
 */
int main() {
    Slice warmup(64);
    warmup.free();
    const size_t records_before = claimed_records();
    const uint32_t buffers_before = IBuffet::live();
    LOG_INFO_STREAM << "Baseline: " << records_before << " claimed records, " << buffers_before << " live buffers";
    uint64_t resident_previous = 0;
    for (size_t number = 0; number < ROUNDS; ++number) {
        round(number);
        const uint64_t resident = host_memory_usage().resident_bytes;
        const size_t records = claimed_records();
        const uint32_t buffers = IBuffet::live();
        LOG_INFO_STREAM << "Round " << number + 1 << ": " << records << " claimed records, " << buffers
            << " live buffers, resident " << (resident >> 20) << " MiB";
        require(records == records_before, "claimed records did not return to the baseline: ", records,
            " against ", records_before);
        require(Counted::live.load() == 0, "typed elements outlived their last handles: ", Counted::live.load());
        /// Two current pools, up to six recycled spares and the table are all the allocator keeps.
        require(buffers <= buffers_before + 8, "buffers kept growing: ", buffers, " live against a baseline of ",
            buffers_before);
        if (number + 1 == ROUNDS) {
            require(resident <= resident_previous + RESIDENT_SLACK,
                "resident memory grew between settled rounds: ", resident >> 20, " MiB after ",
                resident_previous >> 20, " MiB");
        }
        resident_previous = resident;
    }
    LOG_INFO_STREAM << "Fast path leak check passed";
    return 0;
}
