/** --------------------------------------------------------------------------------------------------------- Slice Core Regression
 * @file slice_core_regression_test.cpp
 * @brief Exercises granule views, allocation ownership, rollover, and retry through production Slices.
 */
#include <alligator.hpp>
#include <memory/tracker.hpp>
#include "functional_support.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <cstdlib>
#include <set>
#include <thread>
#include <vector>

using namespace buffetalligator;
using functional::require;
namespace {
std::atomic<size_t> factory_calls{0};
std::atomic<size_t> last_factory_bytes{0};
std::atomic<size_t> allocation_count{0};
std::atomic<size_t> fail_at{SIZE_MAX};
std::array<std::atomic<size_t>, 16384> releases{};
/** --------------------------------------------------------------------------------------------------------- Tracked Backing
 * @brief Associates real aligned storage with an independently counted allocation identity.
 */
struct TrackedBacking {
    AlignedHeapBuffer memory;
    const size_t identity;
    TrackedBacking(size_t bytes, size_t identifier) : memory(bytes), identity(identifier) {}
};
/** --------------------------------------------------------------------------------------------------------- Tracked Factory
 * @brief Injects a selected allocation failure before constructing real backing storage.
 */
void* tracked_factory(size_t bytes) {
    last_factory_bytes.store(bytes, std::memory_order_relaxed);
    const size_t invocation = factory_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (invocation == fail_at.load(std::memory_order_relaxed)) throw std::bad_alloc();
    const size_t identity = allocation_count.fetch_add(1, std::memory_order_relaxed);
    require(identity < releases.size(), "core regression exhausted its allocation identities");
    return new TrackedBacking(bytes, identity);
}
/** --------------------------------------------------------------------------------------------------------- Tracked Deleter
 * @brief Rejects duplicate backing release before ending the allocation's lifetime.
 */
void tracked_deleter(void* pointer) {
    auto* backing = static_cast<TrackedBacking*>(pointer);
    require(releases[backing->identity].fetch_add(1, std::memory_order_relaxed) == 0,
        "allocation backing was released more than once");
    delete backing;
}
/** --------------------------------------------------------------------------------------------------------- Tracked Host Pointer
 * @brief Resolves production host accesses into the selected tracked backing.
 */
void* tracked_host(void* pointer, size_t offset) {
    return static_cast<unsigned char*>(static_cast<TrackedBacking*>(pointer)->memory.raw()) + offset;
}
/** --------------------------------------------------------------------------------------------------------- Tracked Size
 * @brief Returns the real backing length used by production slab reservations.
 */
size_t tracked_size(void* pointer) {
    return static_cast<TrackedBacking*>(pointer)->memory.size();
}
/** --------------------------------------------------------------------------------------------------------- Tracked Address
 * @brief Gives each CPU-only test allocation a distinct numerical device address.
 */
uint64_t tracked_address(void* pointer) {
    return UINT64_C(0x10000000000)
        + static_cast<TrackedBacking*>(pointer)->identity * UINT64_C(0x100000);
}
const BuffetDescriptor small_placement{
    "SliceCoreSmall", tracked_deleter, tracked_host, tracked_size, tracked_factory, 2, 256,
    tracked_address};
const BuffetDescriptor retry_placement{
    "SliceCoreRetry", tracked_deleter, tracked_host, tracked_size, tracked_factory, 3, 128,
    tracked_address};
const BuffetDescriptor rollover_placement{
    "SliceCoreRollover", tracked_deleter, tracked_host, tracked_size, tracked_factory, 5, 256,
    tracked_address};
/** --------------------------------------------------------------------------------------------------------- Verify Final Release
 * @brief Checks current roots and prepared successors after their actual process-exit destructors.
 */
void verify_final_release(void*) {
    const size_t allocations = allocation_count.load(std::memory_order_relaxed);
    for (size_t identity = 0; identity < allocations; ++identity) {
        require(releases[identity].load(std::memory_order_relaxed) == 1,
            "process exit retained an internal allocation or prepared successor");
    }
    LOG_INFO_STREAM << "All tracked Slice backings released exactly once";
}
/** --------------------------------------------------------------------------------------------------------- Null And Granules
 * @brief Checks null ownership and the public granule lengths and offsets of adjacent claims.
 */
void null_and_granules() {
    LOG_INFO_STREAM << "Checking null handles, moves, and 0/1/63/64/65-byte allocation requests";
    const size_t before = factory_calls.load(std::memory_order_relaxed);
    Slice empty;
    Slice zero(size_t(0), &small_placement);
    require(!empty && !zero && empty.id() == UINT32_MAX && empty.raw() == nullptr
        && empty.size_bytes() == 0, "zero/default Slice acquired ownership");
    empty.free();
    empty.free();
    require(factory_calls.load(std::memory_order_relaxed) == before,
        "zero/default Slice invoked an allocation factory");
    constexpr std::array<size_t, 4> requests{1, 63, 64, 65};
    std::array<Slice, requests.size()> claims;
    for (size_t index = 0; index < requests.size(); ++index) {
        claims[index] = Slice(requests[index], &small_placement);
        const GPUBuf& record = *Alligator::gpubuf_for(claims[index]);
        require(claims[index].size_bytes() == ((requests[index] + 63) & ~size_t(63))
            && record.size == (requests[index] + 63) / 64,
            "public length and GPUBuf granules disagree");
        auto* backing = static_cast<TrackedBacking*>(SliceEntry::from_slice(claims[index])->token()->buffet());
        require(record.address == tracked_address(backing)
            && claims[index].raw() == tracked_host(backing, uint64_t(record.offset) << 6),
            "claim address or granule offset resolves another allocation");
    }
    require(claims[1].data() == claims[0].data() + 64
        && claims[2].data() == claims[1].data() + 64,
        "adjacent small reservations overlap or skip a granule");
    const uint32_t identifier = claims[0].id();
    Slice moved(std::move(claims[0]));
    require(!claims[0] && moved.id() == identifier, "move construction lost its null source contract");
    Slice& alias = moved;
    moved = std::move(alias);
    moved = alias;
    require(moved.id() == identifier, "self-assignment changed Slice ownership");
    moved.free();
    moved.free();
    require(!moved && moved.raw() == nullptr, "repeated free did not preserve the null state");
}
/** --------------------------------------------------------------------------------------------------------- Granule Limits
 * @brief Checks maximum-size alignment and rejected overflow without allocating enormous backing storage.
 */
void granule_limits() {
    LOG_INFO_STREAM << "Checking maximum granule lengths and overflow rejection";
    constexpr size_t maximum = uint64_t{UINT32_MAX} << 6;
    for (const size_t bytes : std::array<size_t, 2>{maximum - 63, maximum}) {
        fail_at.store(factory_calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        bool rejected = false;
        try { Slice intercepted(bytes, true, &small_placement); }
        catch (const std::bad_alloc&) { rejected = true; }
        require(rejected && last_factory_bytes.load(std::memory_order_relaxed) == maximum,
            "largest representable request wrapped before reaching the allocation factory");
    }
    Slice preserved(size_t(65), &small_placement);
    const uint32_t identifier = preserved.id();
    for (const size_t bytes : std::array<size_t, 2>{maximum + 1, SIZE_MAX}) {
        const size_t before = factory_calls.load(std::memory_order_relaxed);
        bool rejected = false;
        try { Slice invalid(bytes, &small_placement); }
        catch (const AlligatorException&) { rejected = true; }
        require(rejected && factory_calls.load(std::memory_order_relaxed) == before,
            "oversized Slice request reached its factory or escaped rejection");
        rejected = false;
        try { preserved.resize(bytes, true, false, &small_placement); }
        catch (const AlligatorException&) { rejected = true; }
        require(rejected && preserved.id() == identifier && preserved.size_bytes() == 128,
            "oversized resize changed its live source");
        rejected = false;
        try { SharedBuffet invalid(bytes); }
        catch (const AlligatorException&) { rejected = true; }
        require(rejected, "oversized SharedBuffet request escaped rejection");
        rejected = false;
        try { ChainBuffet invalid(bytes); }
        catch (const AlligatorException&) { rejected = true; }
        require(rejected, "oversized ChainBuffet request escaped rejection");
    }
}
/** --------------------------------------------------------------------------------------------------------- Copies And Views
 * @brief Checks copied metadata and outward-rounded nested views while original owners retire.
 */
void copies_and_views() {
    LOG_INFO_STREAM << "Checking copied ownership and outward-rounded nested Slice bounds";
    Slice original(size_t(193), true, &small_placement);
    original.data<uint64_t>()[16] = 84;
    Slice copied(original);
    require(copied.id() != original.id() && copied.raw() == original.raw()
        && copied.size_bytes() == 256, "copy did not publish a separate retained identifier");
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
/** --------------------------------------------------------------------------------------------------------- Stack Ownership
 * @brief Keeps a Slice live after its caller-owned ChainBuffet leaves scope.
 */
void stack_ownership() {
    LOG_INFO_STREAM << "Checking Slice backing after stack ChainBuffet destruction";
    const auto* heap = BuffetDescriptors::descriptor_for(static_cast<AlignedHeapBuffer*>(nullptr));
    const size_t before = Memory::placement_freed(*heap);
    Slice retained;
    {
        ChainBuffet stack(size_t(256));
        retained = stack.claim(64);
        retained.get_as<uint64_t>() = 123;
    }
    require(retained.get_as<uint64_t>() == 123 && Memory::placement_freed(*heap) == before,
        "stack ChainBuffet destruction released live Slice backing");
    retained.free();
    require(Memory::placement_freed(*heap) == before + 256,
        "last stack-buffer Slice did not release its backing exactly once");
}
/** --------------------------------------------------------------------------------------------------------- Dedicated Ownership
 * @brief Verifies novel, exact-slab, and oversized records name their dedicated allocation.
 */
void dedicated_ownership() {
    LOG_INFO_STREAM << "Checking dedicated buffer addresses and exact last-owner release";
    constexpr std::array<size_t, 3> requests{17, 256, 320};
    for (size_t index = 0; index < requests.size(); ++index) {
        Slice original(requests[index], index == 0, &small_placement);
        auto* backing = static_cast<TrackedBacking*>(SliceEntry::from_slice(original)->token()->buffet());
        const size_t identity = backing->identity;
        const GPUBuf& record = *Alligator::gpubuf_for(original);
        require(original.is_novel() && record.address == tracked_address(backing)
            && record.offset == 0 && original.raw() == backing->memory.raw()
            && original.size_bytes() == backing->memory.size(),
            "dedicated claim published the chain slab's allocation metadata");
        Slice retained(original);
        original.free();
        require(releases[identity].load(std::memory_order_relaxed) == 0,
            "dedicated backing retired while a copied owner remained");
        retained.free();
        require(releases[identity].load(std::memory_order_relaxed) == 1,
            "dedicated backing retained an abandoned token wrapper");
    }
}
/** --------------------------------------------------------------------------------------------------------- Factory Retry
 * @brief Exercises initial-root, successor, and prepared-successor exceptions without stuck sentinels.
 */
void factory_retry() {
    LOG_INFO_STREAM << "Checking failed root and successor allocation retries";
    fail_at.store(factory_calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    bool rejected = false;
    try { Slice failed(size_t(64), &retry_placement); }
    catch (const std::bad_alloc&) { rejected = true; }
    require(rejected, "controlled initial-root factory failure was not observed");
    Slice first(size_t(64), &retry_placement);
    Slice second(size_t(64), &retry_placement);
    first.get_as<uint64_t>() = 91;
    fail_at.store(factory_calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    rejected = false;
    try { Slice failed(size_t(64), &retry_placement); }
    catch (const std::bad_alloc&) { rejected = true; }
    require(rejected, "controlled successor factory failure was not observed");
    Slice retried(size_t(64), &retry_placement);
    Slice filled(size_t(64), &retry_placement);
    fail_at.store(factory_calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    rejected = false;
    try { Slice failed(size_t(64), &retry_placement); }
    catch (const std::bad_alloc&) { rejected = true; }
    require(rejected, "controlled prepared-successor failure was not observed");
    Slice retried_again(size_t(64), &retry_placement);
    require(first.get_as<uint64_t>() == 91 && retried && retried_again,
        "allocation failure lost a live slab or prevented retry");
    const BuffetDescriptor missing{"SliceCoreMissing", tracked_deleter, tracked_host, tracked_size,
        tracked_factory, 4, 128, tracked_address};
    rejected = false;
    try { Slice invalid(size_t(64), &missing); }
    catch (const AlligatorException&) { rejected = true; }
    require(rejected, "unregistered placement gap silently selected another allocator");
}
/** --------------------------------------------------------------------------------------------------------- Concurrent Rollover
 * @brief Forces synchronized claimers to retain every result across repeated small-slab retirement.
 */
struct Rollover {
    static constexpr size_t workers = 16, rounds = 256;
    std::barrier<> gate{workers};
    std::array<std::array<Slice, rounds>, workers> claims;
    static void run(Rollover* work, size_t worker) {
        for (size_t round = 0; round < rounds; ++round) {
            work->gate.arrive_and_wait();
            Slice& result = work->claims[worker][round];
            result = Slice(size_t(64), &rollover_placement);
            result.get_as<uint64_t>() = (uint64_t(worker + 1) << 32) | round;
            if (worker == 0 && round % 64 == 0)
                LOG_INFO_STREAM << "Checking concurrent Slice rollover round " << round;
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Rollover Ownership
 * @brief Checks exact identifiers, payloads, and retired allocation releases after concurrent growth.
 */
void rollover_ownership() {
    const size_t first_allocation = allocation_count.load(std::memory_order_relaxed);
    {
        Rollover work;
        std::vector<std::thread> workers;
        for (size_t worker = 0; worker < Rollover::workers; ++worker)
            workers.emplace_back(&Rollover::run, &work, worker);
        for (auto& worker : workers) worker.join();
        std::set<uint32_t> identifiers;
        for (size_t worker = 0; worker < Rollover::workers; ++worker) {
            for (size_t round = 0; round < Rollover::rounds; ++round) {
                const Slice& retained = work.claims[worker][round];
                require(identifiers.insert(retained.id()).second
                    && retained.get_as<uint64_t>() == ((uint64_t(worker + 1) << 32) | round),
                    "concurrent slab retirement duplicated an identifier or invalidated payload");
                auto* backing = static_cast<TrackedBacking*>(SliceEntry::from_slice(retained)->token()->buffet());
                require(releases[backing->identity].load(std::memory_order_relaxed) == 0,
                    "concurrent rollover released backing before publication ownership completed");
            }
        }
    }
    const size_t last_allocation = allocation_count.load(std::memory_order_relaxed);
    size_t released = 0;
    for (size_t identity = first_allocation; identity < last_allocation; ++identity)
        released += releases[identity].load(std::memory_order_relaxed);
    require(released + 2 == last_allocation - first_allocation,
        "retired slabs remain pinned beyond the current and prepared roots");
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Runs production Slice allocation and ownership regressions with Release-enabled checks.
 */
int main() {
    LOG_INFO_STREAM << "Starting Slice ownership regressions and final-release verification";
    require(setenv("ALLIGATOR_GPU_BACKEND", "cpu", 1) == 0,
        "CPU-only allocation regression could not select its backend");
    static RuntimeFinalizer verification(nullptr, &verify_final_release);
    BuffetDescriptors::register_descriptor(&small_placement);
    BuffetDescriptors::register_descriptor(&retry_placement);
    BuffetDescriptors::register_descriptor(&rollover_placement);
    null_and_granules();
    granule_limits();
    copies_and_views();
    stack_ownership();
    dedicated_ownership();
    factory_retry();
    rollover_ownership();
    LOG_INFO_STREAM << "Slice granule and allocator ownership regressions passed";
}
