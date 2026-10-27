/** --------------------------------------------------------------------------------------------------------- Dedicated Backing Test
 * @file dedicated_backing_test.cpp
 * @brief Verifies concurrent dedicated claims own distinct backing buffers released exactly once by their last
 * owner.
 */
#include <alligator.hpp>
#include <core/ba_descriptor.h>
#include "functional_support.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <cstdlib>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace buffetalligator;
using functional::require;
namespace {
constexpr size_t worker_count = 8;
constexpr size_t claims_per_worker = 4096;
constexpr size_t phase_count = worker_count * claims_per_worker;
constexpr size_t live_count = 2 * phase_count;
constexpr uint64_t payload_mask = UINT64_C(0xA1136A7020261021);
std::atomic<size_t> identities{0};
std::atomic<size_t> dedicated_allocations{0};
std::atomic<size_t> dedicated_deallocations{0};
std::array<std::atomic<uint32_t>, 2 * live_count + 64> releases{};
/** --------------------------------------------------------------------------------------------------------- Dedicated Backing
 * @brief One real buffer under an independent release identity; the lane slabs count separately from claims.
 */
struct alignas(16) DedicatedBacking {
    AlignedHeapBuffer memory;
    size_t identity = SIZE_MAX;
    DedicatedBacking(size_t bytes = 0) : memory(bytes) {
        if (bytes == 0) return;
        identity = identities.fetch_add(1, std::memory_order_relaxed);
        require(identity < releases.size(), "dedicated backing test exhausted its release identities");
        if (bytes == 64) dedicated_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    DedicatedBacking(const DedicatedBacking&) = delete;
    DedicatedBacking& operator=(const DedicatedBacking&) = delete;
    DedicatedBacking(DedicatedBacking&& other) noexcept
    : memory(std::move(other.memory)), identity(std::exchange(other.identity, SIZE_MAX)) {}
    DedicatedBacking& operator=(DedicatedBacking&& other) noexcept {
        memory = std::move(other.memory);
        std::swap(identity, other.identity);
        return *this;
    }
    ~DedicatedBacking() {
        if (identity == SIZE_MAX) return;
        require(releases[identity].fetch_add(1, std::memory_order_relaxed) == 0,
            "a dedicated backing was released more than once");
        if (memory.size() == 64) dedicated_deallocations.fetch_add(1, std::memory_order_relaxed);
    }
    operator bool() const { return bool(memory); }
    bool operator==(std::nullptr_t) const { return memory == nullptr; }
    static void deleter_impl(void* buffer) { delete static_cast<DedicatedBacking*>(buffer); }
    static void (*deleter())(void*) { return deleter_impl; }
    static void* host_ptr(void* buffer) { return static_cast<DedicatedBacking*>(buffer)->memory.raw(); }
    static size_t size_of(void* buffer) { return static_cast<DedicatedBacking*>(buffer)->memory.size(); }
    static void* factory(size_t bytes) { return new DedicatedBacking(bytes); }
    static size_t default_size() { return functional::SLAB_BYTES; }
    static size_t* type_idx() { static size_t idx = 0; return &idx; }
    static const char* type_name() { return "DedicatedBacking"; }
    static uint64_t device_address(void* buffer) {
        return UINT64_C(0x100000000) + (static_cast<DedicatedBacking*>(buffer)->identity << 8);
    }
    size_t size() const { return memory.size(); }
    void* raw() { return memory.raw(); }
};
static_assert(IsABuffetType<DedicatedBacking>, "DedicatedBacking must satisfy IsABuffetType");
const BuffetDescriptor* placement = nullptr;
/** --------------------------------------------------------------------------------------------------------- Backing Of
 * @brief Reads the actual registered backing retained by a live Slice.
 */
DedicatedBacking* backing_of(const Slice& slice) {
    return static_cast<DedicatedBacking*>(Alligator::buffet_of(slice));
}
/** --------------------------------------------------------------------------------------------------------- Populate
 * @brief Claims a dedicated buffer and writes identity markers at both ends of its payload.
 */
void populate(Slice& slice) {
    slice = Slice(size_t{64}, true, placement);
    const uint64_t payload = backing_of(slice)->identity ^ payload_mask;
    slice.data<uint64_t>()[0] = payload;
    slice.data<uint64_t>()[7] = ~payload;
}
/** --------------------------------------------------------------------------------------------------------- Concurrent Claims
 * @brief Coordinates simultaneous publication while every previous backing remains live.
 */
struct ConcurrentClaims {
    std::vector<Slice>* slices;
    size_t first;
    std::barrier<> gate{worker_count};
    static void run(ConcurrentClaims* claims, size_t worker) {
        for (size_t index = 0; index < claims_per_worker; ++index) {
            if ((index & 63) == 0) claims->gate.arrive_and_wait();
            populate((*claims->slices)[claims->first + worker * claims_per_worker + index]);
            if (worker == 0 && (index & 2047) == 0)
                LOG_INFO_STREAM << "Concurrent dedicated claims per worker: " << index;
        }
    }
};
/** --------------------------------------------------------------------------------------------------------- Publish Concurrently
 * @brief Publishes one full phase across synchronized workers.
 */
void publish_concurrently(std::vector<Slice>& slices, size_t first) {
    ConcurrentClaims claims{&slices, first};
    std::array<std::thread, worker_count> workers;
    for (size_t worker = 0; worker < worker_count; ++worker)
        workers[worker] = std::thread(&ConcurrentClaims::run, &claims, worker);
    for (auto& worker : workers) worker.join();
}
/** --------------------------------------------------------------------------------------------------------- Verify Live
 * @brief Checks independent backing addresses, payloads, and device addresses across every live claim.
 */
void verify_live(const std::vector<Slice>& slices) {
    std::unordered_set<const void*> addresses;
    addresses.reserve(slices.size());
    for (const Slice& slice : slices) {
        DedicatedBacking* backing = backing_of(slice);
        const uint64_t payload = backing->identity ^ payload_mask;
        require(slice.is_novel() && slice.size_bytes() == 64 && slice.placement() == placement,
            "dedicated claim changed its size, ownership, or placement");
        require(addresses.insert(slice.raw()).second,
            "simultaneously live dedicated claims aliased a backing address");
        require(slice.data<uint64_t>()[0] == payload && slice.data<uint64_t>()[7] == ~payload,
            "concurrent publication overwrote another live payload");
        require(reinterpret_cast<const ba_slice_t*>(&slice)->address == DedicatedBacking::device_address(backing),
            "dedicated claim published another backing's device address");
        require(releases[backing->identity].load(std::memory_order_relaxed) == 0,
            "live dedicated backing was already released");
    }
    require(addresses.size() == slices.size(), "dedicated backing test lost live claims");
}
/** --------------------------------------------------------------------------------------------------------- Verify Releases
 * @brief Checks that every dedicated allocation completed exactly one backing deallocation.
 */
void verify_releases() {
    require(dedicated_deallocations.load(std::memory_order_relaxed)
        == dedicated_allocations.load(std::memory_order_relaxed),
        "dedicated backing remained retained after every Slice was released");
    const size_t allocated = identities.load(std::memory_order_relaxed);
    for (size_t identity = 0; identity < allocated; ++identity)
        require(releases[identity].load(std::memory_order_relaxed) <= 1,
            "a dedicated allocation was released more than once");
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Retains two phases of independent backing buffers, releases them, and publishes them again.
 */
int main() {
    require(setenv("ALLIGATOR_GPU_BACKEND", "cpu", 1) == 0, "CPU selection failed");
    placement = BuffetDescriptors::descriptor<DedicatedBacking>();
    std::vector<Slice> slices(live_count);
    LOG_INFO_STREAM << "Retaining " << phase_count << " distinct dedicated backings";
    for (size_t index = 0; index < phase_count; ++index) {
        populate(slices[index]);
        if ((index & 16383) == 0) LOG_INFO_STREAM << "Retained dedicated backings: " << index;
    }
    LOG_INFO_STREAM << "Publishing " << phase_count << " more dedicated backings concurrently";
    publish_concurrently(slices, phase_count);
    require(dedicated_allocations.load(std::memory_order_relaxed) == live_count,
        "dedicated claims reused backing or allocated hidden buffers");
    verify_live(slices);
    Slice copied(slices.front());
    Slice view = slices.back().slice(0, 64);
    const size_t copied_identity = backing_of(copied)->identity;
    const size_t viewed_identity = backing_of(view)->identity;
    slices.clear();
    require(dedicated_deallocations.load(std::memory_order_relaxed) == live_count - 2
        && releases[copied_identity].load(std::memory_order_relaxed) == 0
        && releases[viewed_identity].load(std::memory_order_relaxed) == 0,
        "retirement lost retained ownership or failed to reclaim independent backings");
    require(copied.get_as<uint64_t>() == (copied_identity ^ payload_mask)
        && view.get_as<uint64_t>() == (viewed_identity ^ payload_mask),
        "copy or subview lost its backing after the other owners retired");
    copied.free();
    view.free();
    verify_releases();
    LOG_INFO_STREAM << "Publishing " << live_count << " dedicated backings again after release";
    slices.resize(live_count);
    publish_concurrently(slices, 0);
    publish_concurrently(slices, phase_count);
    verify_live(slices);
    slices.clear();
    verify_releases();
    LOG_INFO_STREAM << "Distinct backing, concurrent publication, and exact reclamation passed";
}
