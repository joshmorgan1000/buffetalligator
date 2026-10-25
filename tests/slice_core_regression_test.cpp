/** --------------------------------------------------------------------------------------------------------- Slice Core Regression
 * @file slice_core_regression_test.cpp
 * @brief Exercises granule views, shared ownership, dedicated buffers, and concurrent rollover through
 * production Slices on counted placements.
 */
#include <alligator.hpp>
#include "functional_support.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <set>
#include <thread>
#include <utility>
#include <vector>

using namespace buffetalligator;
using functional::require;
namespace {
std::atomic<size_t> factory_calls{0};
std::atomic<size_t> last_factory_bytes{0};
std::atomic<size_t> allocation_count{0};
std::atomic<size_t> fail_at{SIZE_MAX};
std::array<std::atomic<size_t>, 32768> releases{};
/** --------------------------------------------------------------------------------------------------------- Tracked Placement
 * @brief Real aligned storage under an independently counted allocation identity; one type per slab size.
 */
template<int Tag, size_t Slab>
struct alignas(16) Tracked {
    AlignedHeapBuffer memory;
    size_t identity = SIZE_MAX;
    Tracked(size_t bytes = 0) : memory(bytes) {
        if (bytes == 0) return;
        identity = allocation_count.fetch_add(1, std::memory_order_relaxed);
        require(identity < releases.size(), "core regression exhausted its allocation identities");
    }
    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;
    Tracked(Tracked&& other) noexcept
    : memory(std::move(other.memory)), identity(std::exchange(other.identity, SIZE_MAX)) {}
    Tracked& operator=(Tracked&& other) noexcept {
        memory = std::move(other.memory); std::swap(identity, other.identity); return *this;
    }
    ~Tracked() {
        if (identity == SIZE_MAX) return;
        require(releases[identity].fetch_add(1, std::memory_order_relaxed) == 0,
            "allocation backing was released more than once");
    }
    operator bool() const { return bool(memory); }
    bool operator==(std::nullptr_t) const { return memory == nullptr; }
    static void deleter_impl(void* pointer) { delete static_cast<Tracked*>(pointer); }
    static void (*deleter())(void*) { return deleter_impl; }
    static void* host_ptr(void* pointer) { return static_cast<Tracked*>(pointer)->memory.raw(); }
    static size_t size_of(void* pointer) { return static_cast<Tracked*>(pointer)->memory.size(); }
    static void* factory(size_t bytes) {
        last_factory_bytes.store(bytes, std::memory_order_relaxed);
        const size_t invocation = factory_calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (invocation == fail_at.load(std::memory_order_relaxed)) throw std::bad_alloc();
        return new Tracked(bytes);
    }
    static size_t default_size() { return Slab; }
    static size_t* type_idx() { static size_t idx = 0; return &idx; }
    static const char* type_name() {
        return Tag == 0 ? "SliceCoreSmall" : Tag == 1 ? "SliceCoreRollover" : "SliceCoreChurn";
    }
    static uint64_t device_address(void* pointer) {
        return UINT64_C(0x10000000000) + static_cast<Tracked*>(pointer)->identity * UINT64_C(0x100000);
    }
    size_t size() const { return memory.size(); }
    void* raw() { return memory.raw(); }
    const void* raw() const { return memory.raw(); }
};
using SmallPlacement = Tracked<0, 256>;
using RolloverPlacement = Tracked<1, 256>;
using ChurnPlacement = Tracked<2, 4096>;
static_assert(IsABuffetType<SmallPlacement>, "Tracked must satisfy IsABuffetType");
/** --------------------------------------------------------------------------------------------------------- Backing Of
 * @brief Returns the tracked allocation behind a live Slice.
 */
template<typename Placement>
Placement* backing_of(const Slice& slice) {
    return static_cast<Placement*>(Alligator::buffet_of(slice));
}
/** --------------------------------------------------------------------------------------------------------- Final Release
 * @brief Checks every tracked backing released exactly once by process exit.
 */
struct FinalRelease {
    ~FinalRelease() {
        /// Every buffer releases at most once; each lane's current, prepared and previous links and each
        /// placement's parked spare are freed by the Alligator's teardown phase, which runs after this check.
        const size_t allocations = allocation_count.load(std::memory_order_relaxed);
        size_t standing = 0;
        for (size_t identity = 0; identity < allocations; ++identity) {
            const size_t count = releases[identity].load(std::memory_order_relaxed);
            if (count > 1) std::_Exit(EXIT_FAILURE);
            standing += count == 0;
        }
        if (standing > 3 * (3 * CHAIN_LANES + 1)) std::_Exit(EXIT_FAILURE);
    }
};
/** --------------------------------------------------------------------------------------------------------- Null And Granules
 * @brief Checks null ownership and the public granule lengths and adjacency of consecutive claims.
 */
void null_and_granules(const BuffetDescriptor* small) {
    LOG_INFO_STREAM << "Checking null handles, moves, and 0/1/63/64/65-byte allocation requests";
    const size_t before = factory_calls.load(std::memory_order_relaxed);
    Slice empty;
    Slice zero(size_t(0), small);
    require(!empty && !zero && empty.raw() == nullptr
        && empty.size_bytes() == 0, "zero/default Slice acquired ownership");
    empty.free();
    empty.free();
    require(factory_calls.load(std::memory_order_relaxed) == before,
        "zero/default Slice invoked an allocation factory");
    constexpr std::array<size_t, 4> requests{1, 63, 64, 65};
    std::array<Slice, requests.size()> claims;
    for (size_t index = 0; index < requests.size(); ++index) {
        claims[index] = Slice(requests[index], small);
        require(claims[index].size_bytes() == ((requests[index] + 63) & ~size_t(63)),
            "public length did not round up to whole granules");
        require(claims[index].placement() == small, "claim reported another placement");
    }
    /// A record granule precedes every payload, so consecutive granule claims sit two granules apart.
    require(claims[1].data() == claims[0].data() + 2 * sizeof(SliceRecord),
        "adjacent small reservations overlap or skip a granule");
    const void* identifier = claims[0].raw();
    Slice moved(std::move(claims[0]));
    require(!claims[0] && moved.raw() == identifier, "move construction lost its null source contract");
    Slice& alias = moved;
    moved = std::move(alias);
    moved = alias;
    require(moved.raw() == identifier, "self-assignment changed Slice ownership");
    moved.free();
    moved.free();
    require(!moved && moved.raw() == nullptr, "repeated free did not preserve the null state");
}
/** --------------------------------------------------------------------------------------------------------- Granule Limits
 * @brief Checks that requests past the 32-bit granule range are rejected before reaching any factory.
 */
void granule_limits(const BuffetDescriptor* small) {
    LOG_INFO_STREAM << "Checking overflow rejection";
    Slice preserved(size_t(65), small);
    const void* identifier = preserved.raw();
    for (const size_t bytes : std::array<size_t, 2>{uint64_t{UINT32_MAX} + 1, SIZE_MAX}) {
        const size_t before = factory_calls.load(std::memory_order_relaxed);
        bool rejected = false;
        try { Slice invalid(bytes, small); }
        catch (const AlligatorException&) { rejected = true; }
        require(rejected && factory_calls.load(std::memory_order_relaxed) == before,
            "oversized Slice request reached its factory or escaped rejection");
        rejected = false;
        try { preserved.resize(bytes, true, false, small); }
        catch (const AlligatorException&) { rejected = true; }
        require(rejected && preserved.raw() == identifier && preserved.size_bytes() == 128,
            "oversized resize changed its live source");
    }
}
/** --------------------------------------------------------------------------------------------------------- Copies And Views
 * @brief Checks shared copies and outward-rounded nested views while original owners retire.
 */
void copies_and_views(const BuffetDescriptor* small) {
    LOG_INFO_STREAM << "Checking copied ownership and outward-rounded nested Slice bounds";
    Slice original(size_t(193), true, small);
    original.data<uint64_t>()[16] = 84;
    Slice copied(original);
    require(copied.raw() == original.raw() && copied.size_bytes() == 256,
        "copy did not share its source record");
    Slice view = original.slice(65, 65);
    Slice nested = view.slice(63, 2);
    Slice tail = original.slice(65);
    Slice last = original.slice(255, 1);
    require(view.raw() == original.data() + 64 && view.size_bytes() == 128,
        "unaligned view did not round its start down and end up");
    require(nested.raw() == view.raw() && nested.size_bytes() == 128,
        "nested outward rounding changed the parent's address domain");
    require(tail.raw() == original.data() + 64 && tail.size_bytes() == 192
        && last.raw() == original.data() + 192 && last.size_bytes() == 64,
        "tail or final-byte view exceeded its parent granules");
    require(!original.slice(0, 0) && !original.slice(256), "empty views retained a granule");
    bool rejected = false;
    try { (void)original.slice(255, 2); }
    catch (const AlligatorException&) { rejected = true; }
    require(rejected, "out-of-parent requested bounds were silently rounded");
    original.free();
    copied.free();
    require(view.data<uint64_t>()[8] == 84 && nested.data<uint64_t>()[8] == 84,
        "retiring original identifiers invalidated nested backing ownership");
}
/** --------------------------------------------------------------------------------------------------------- Dedicated Ownership
 * @brief Verifies novel, exact-slab, and oversized claims own a dedicated buffer released by the last owner.
 */
void dedicated_ownership(const BuffetDescriptor* small) {
    LOG_INFO_STREAM << "Checking dedicated buffer ownership and exact last-owner release";
    constexpr std::array<size_t, 3> requests{17, 256, 320};
    for (size_t index = 0; index < requests.size(); ++index) {
        Slice original(requests[index], index == 0, small);
        SmallPlacement* backing = backing_of<SmallPlacement>(original);
        const size_t identity = backing->identity;
        require(original.is_novel()
            && original.raw() == static_cast<uint8_t*>(backing->memory.raw()) + sizeof(SliceRecord)
            && original.size_bytes() + sizeof(SliceRecord) == backing->memory.size(),
            "dedicated claim published the chain slab's allocation metadata");
        Slice retained(original);
        original.free();
        require(releases[identity].load(std::memory_order_relaxed) == 0,
            "dedicated backing retired while a copied owner remained");
        std::memset(retained.raw(), 0xA5, retained.size_bytes());
        retained.free();
        require(releases[identity].load(std::memory_order_relaxed) == 1,
            "dedicated backing did not release with its last owner");
    }
}
/** --------------------------------------------------------------------------------------------------------- Concurrent Rollover
 * @brief Forces synchronized claimers to retain every result across repeated small-slab retirement.
 */
struct Rollover {
    static constexpr size_t workers = 16, rounds = 256;
    const BuffetDescriptor* placement;
    std::barrier<> gate{workers};
    std::array<std::array<Slice, rounds>, workers> claims;
    explicit Rollover(const BuffetDescriptor* selected) : placement(selected) {}
    static void run(Rollover* work, size_t worker) {
        for (size_t round = 0; round < rounds; ++round) {
            work->gate.arrive_and_wait();
            Slice& result = work->claims[worker][round];
            result = Slice(size_t(64), work->placement);
            result.get_as<uint64_t>() = (uint64_t(worker + 1) << 32) | round;
            if (worker == 0 && round % 64 == 0)
                LOG_INFO_STREAM << "Checking concurrent Slice rollover round " << round;
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Rollover Ownership
 * @brief Checks exact identifiers, payloads, and retired allocation releases after concurrent growth.
 */
void rollover_ownership(const BuffetDescriptor* placement) {
    const size_t first_allocation = allocation_count.load(std::memory_order_relaxed);
    {
        Rollover work(placement);
        std::vector<std::thread> workers;
        for (size_t worker = 0; worker < Rollover::workers; ++worker)
            workers.emplace_back(&Rollover::run, &work, worker);
        for (auto& worker : workers) worker.join();
        std::set<const void*> identifiers;
        for (size_t worker = 0; worker < Rollover::workers; ++worker) {
            for (size_t round = 0; round < Rollover::rounds; ++round) {
                const Slice& retained = work.claims[worker][round];
                require(identifiers.insert(retained.raw()).second
                    && retained.get_as<uint64_t>() == ((uint64_t(worker + 1) << 32) | round),
                    "concurrent slab retirement duplicated an identifier or invalidated payload");
                const size_t identity = backing_of<RolloverPlacement>(retained)->identity;
                require(releases[identity].load(std::memory_order_relaxed) == 0,
                    "concurrent rollover released backing before publication ownership completed");
            }
        }
    }
    const size_t last_allocation = allocation_count.load(std::memory_order_relaxed);
    size_t released = 0;
    /// A retired link frees one Kitchen hop after its last release, so the count is given a moment to land.
    for (int wait = 0; wait < 1000; ++wait) {
        released = 0;
        for (size_t identity = first_allocation; identity < last_allocation; ++identity)
            released += releases[identity].load(std::memory_order_relaxed);
        if (last_allocation - first_allocation - released <= 3 * CHAIN_LANES + 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    /// Each lane keeps the link behind its current one, the current one and the one ahead, and the placement
    /// keeps one parked spare.
    require(released > 0 && last_allocation - first_allocation - released <= 3 * CHAIN_LANES + 1,
        "retired slabs remain pinned beyond the lanes' chains and the parked spare");
}
/** --------------------------------------------------------------------------------------------------------- Concurrent Churn
 * @brief Claims, checks, copies, and frees on small slabs so buffers retire and indices recycle.
 */
struct Churn {
    static constexpr size_t workers = 16, rounds = 20000, held = 3;
    const BuffetDescriptor* placement;
    std::barrier<> gate{workers};
    std::atomic<bool> overlapped{false};
    explicit Churn(const BuffetDescriptor* selected) : placement(selected) {}
    static void run(Churn* work, size_t worker) {
        std::array<Slice, held> live;
        work->gate.arrive_and_wait();
        for (size_t round = 0; round < rounds; ++round) {
            Slice& slot = live[round % held];
            const uint64_t expected = (uint64_t(worker + 1) << 32) | (round - held);
            if (round >= held && slot.get_as<uint64_t>() != expected)
                work->overlapped.store(true, std::memory_order_relaxed);
            Slice copy(slot);
            slot = Slice(size_t(64), work->placement);
            slot.get_as<uint64_t>() = (uint64_t(worker + 1) << 32) | round;
            if (worker == 0 && round % 5000 == 0)
                LOG_INFO_STREAM << "Checking Slice churn round " << round;
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Churn Ownership
 * @brief Checks that no claim overlaps another and every retired buffer releases exactly once.
 */
void churn_ownership(const BuffetDescriptor* placement) {
    const size_t first_allocation = allocation_count.load(std::memory_order_relaxed);
    {
        Churn work(placement);
        std::vector<std::thread> workers;
        for (size_t worker = 0; worker < Churn::workers; ++worker)
            workers.emplace_back(&Churn::run, &work, worker);
        for (auto& worker : workers) worker.join();
        require(!work.overlapped.load(std::memory_order_relaxed),
            "concurrent churn handed one granule to two live Slices");
    }
    const size_t last_allocation = allocation_count.load(std::memory_order_relaxed);
    size_t released = 0;
    for (size_t identity = first_allocation; identity < last_allocation; ++identity)
        released += releases[identity].load(std::memory_order_relaxed);
    require(released > 0 && last_allocation - first_allocation - released <= 3 * CHAIN_LANES + 1,
        "churned buffers remain pinned beyond the lanes' chains and the parked spare");
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs production Slice allocation and ownership regressions with Release-enabled checks.
 */
int main() {
    LOG_INFO_STREAM << "Starting Slice ownership regressions and final-release verification";
    require(setenv("ALLIGATOR_GPU_BACKEND", "cpu", 1) == 0,
        "CPU-only allocation regression could not select its backend");
    static FinalRelease verification;
    const BuffetDescriptor* small = BuffetDescriptors::descriptor<SmallPlacement>();
    const BuffetDescriptor* rollover = BuffetDescriptors::descriptor<RolloverPlacement>();
    const BuffetDescriptor* churn = BuffetDescriptors::descriptor<ChurnPlacement>();
    null_and_granules(small);
    granule_limits(small);
    copies_and_views(small);
    dedicated_ownership(small);
    rollover_ownership(rollover);
    churn_ownership(churn);
    LOG_INFO_STREAM << "Slice granule and allocator ownership regressions passed";
}
