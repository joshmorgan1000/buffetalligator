/** --------------------------------------------------------------------------------------------------------- Realm Allocation Test
 * @file realm_allocation_test.cpp
 * @brief Verifies concurrent realm rollover and reclamation with distinct dedicated backing buffers.
 */
#include <alligator.hpp>
#include <memory/region.hpp>
#include "functional_support.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <cstdlib>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace buffetalligator;
using functional::require;
namespace {
constexpr size_t worker_count = 8;
constexpr size_t claims_per_worker = REGION_SIZE / worker_count + 8;
constexpr size_t phase_count = worker_count * claims_per_worker;
constexpr size_t live_count = 2 * phase_count;
constexpr uint64_t payload_mask = UINT64_C(0xA1136A7020261021);
std::atomic<size_t> allocations{0};
std::atomic<size_t> deallocations{0};
std::array<std::atomic<uint32_t>, 2 * live_count> releases{};
/** --------------------------------------------------------------------------------------------------------- Backing
 * @brief Associates one real 64-byte buffer with its independent release identity.
 */
struct Backing {
    AlignedHeapBuffer memory;
    size_t identity;
    Backing(size_t bytes, size_t identifier) : memory(bytes), identity(identifier) {}
};
/** --------------------------------------------------------------------------------------------------------- Allocate
 * @brief Constructs one independently owned buffer for every dedicated Slice request.
 */
void* allocate(size_t bytes) {
    require(bytes == 64, "dedicated realm request allocated a shared or oversized backing");
    const size_t identity = allocations.fetch_add(1, std::memory_order_relaxed);
    require(identity < releases.size(), "realm test exhausted its release identities");
    return new Backing(bytes, identity);
}
/** --------------------------------------------------------------------------------------------------------- Deallocate
 * @brief Rejects duplicate release before freeing the corresponding dedicated backing.
 */
void deallocate(void* pointer) {
    auto* backing = static_cast<Backing*>(pointer);
    require(releases[backing->identity].fetch_add(1, std::memory_order_relaxed) == 0,
        "realm rollover released one backing more than once");
    delete backing;
    deallocations.fetch_add(1, std::memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Host Pointer
 * @brief Resolves the registered buffer's host address at the requested offset.
 */
void* host_pointer(void* pointer, size_t offset) {
    return static_cast<uint8_t*>(static_cast<Backing*>(pointer)->memory.raw()) + offset;
}
/** --------------------------------------------------------------------------------------------------------- Backing Size
 * @brief Returns the allocated buffer's exact byte length.
 */
size_t backing_size(void* pointer) {
    return static_cast<Backing*>(pointer)->memory.size();
}
/** --------------------------------------------------------------------------------------------------------- Device Address
 * @brief Gives every backing a distinct device address independently of host pointer reuse.
 */
uint64_t device_address(void* pointer) {
    return UINT64_C(0x100000000) + (static_cast<Backing*>(pointer)->identity << 8);
}
const BuffetDescriptor placement{
    "RealmDedicated", deallocate, host_pointer, backing_size, allocate, 2, 4096, device_address};
/** --------------------------------------------------------------------------------------------------------- Backing Of
 * @brief Reads the actual registered backing retained by a live Slice.
 */
Backing* backing_of(const Slice& slice) {
    return static_cast<Backing*>(SliceEntry::from_slice(slice)->buffet());
}
/** --------------------------------------------------------------------------------------------------------- Populate
 * @brief Claims a dedicated buffer and writes identity markers at both ends of its payload.
 */
void populate(Slice& slice) {
    slice = Slice(size_t{64}, true, &placement);
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
 * @brief Checks independent IDs, backing addresses, payloads, and GPU records across every realm.
 */
void verify_live(
    const std::vector<Slice>& slices,
    std::unordered_set<uint32_t>& identifiers,
    std::unordered_set<size_t>& realms
) {
    std::unordered_set<const void*> addresses;
    addresses.reserve(slices.size());
    identifiers.reserve(slices.size());
    for (const Slice& slice : slices) {
        Backing* backing = backing_of(slice);
        const GPUBuf& record = *SliceEntry::from_slice(slice)->gpu_buf();
        const uint64_t payload = backing->identity ^ payload_mask;
        require(slice.is_novel() && slice.size_bytes() == 64 && slice.placement() == &placement,
            "dedicated realm claim changed its size, ownership, or placement");
        require(identifiers.insert(slice.id()).second && addresses.insert(slice.raw()).second,
            "simultaneously live dedicated claims aliased an identifier or backing address");
        require(slice.data<uint64_t>()[0] == payload && slice.data<uint64_t>()[7] == ~payload,
            "realm rollover or concurrent publication overwrote another live payload");
        require(record.address == device_address(backing) && record.size == 1 && record.offset == 0,
            "dedicated claim published another backing or inaccurate GPU bounds");
        require(releases[backing->identity].load(std::memory_order_relaxed) == 0,
            "live dedicated backing was already released");
        realms.insert(pool_region(slice.id()));
    }
    require(addresses.size() > REGION_SIZE && realms.size() >= 3,
        "dedicated backing test did not cross two full realm boundaries");
}
/** --------------------------------------------------------------------------------------------------------- Verify Releases
 * @brief Checks that every allocation completed exactly one backing deallocation.
 */
void verify_releases() {
    const size_t allocated = allocations.load(std::memory_order_relaxed);
    require(deallocations.load(std::memory_order_relaxed) == allocated,
        "dedicated backing remained retained after every Slice was released");
    for (size_t identity = 0; identity < allocated; ++identity)
        require(releases[identity].load(std::memory_order_relaxed) == 1,
            "a dedicated allocation was leaked or released more than once");
}
} // namespace
/** --------------------------------------------------------------------------------------------------------- Main
 * @brief Exercises more than two realms of independent backing buffers and reuses their released slots.
 */
int main() {
    require(setenv("ALLIGATOR_GPU_BACKEND", "cpu", 1) == 0, "CPU selection failed");
    BuffetDescriptors::register_descriptor(&placement);
    std::vector<Slice> slices(live_count);
    LOG_INFO_STREAM << "Retaining " << phase_count << " distinct dedicated backings across a realm";
    for (size_t index = 0; index < phase_count; ++index) {
        populate(slices[index]);
        if ((index & 16383) == 0) LOG_INFO_STREAM << "Retained dedicated backings: " << index;
    }
    LOG_INFO_STREAM << "Publishing " << phase_count << " more dedicated backings concurrently";
    publish_concurrently(slices, phase_count);
    require(allocations.load(std::memory_order_relaxed) == live_count,
        "dedicated claims reused backing or allocated hidden buffers");
    std::unordered_set<uint32_t> first_identifiers;
    std::unordered_set<size_t> first_realms;
    verify_live(slices, first_identifiers, first_realms);
    Slice copied(slices.front());
    Slice view = slices.back().slice(0, 64);
    const size_t copied_identity = backing_of(copied)->identity;
    const size_t viewed_identity = backing_of(view)->identity;
    slices.clear();
    require(deallocations.load(std::memory_order_relaxed) == live_count - 2
        && releases[copied_identity].load(std::memory_order_relaxed) == 0
        && releases[viewed_identity].load(std::memory_order_relaxed) == 0,
        "realm retirement lost retained ownership or failed to reclaim independent backings");
    require(copied.get_as<uint64_t>() == (copied_identity ^ payload_mask)
        && view.get_as<uint64_t>() == (viewed_identity ^ payload_mask),
        "copy or subview lost its backing after the other realm owners retired");
    copied.free();
    view.free();
    verify_releases();
    LOG_INFO_STREAM << "Reusing released realm capacity for " << live_count << " dedicated backings";
    slices.resize(live_count);
    publish_concurrently(slices, 0);
    publish_concurrently(slices, phase_count);
    std::unordered_set<uint32_t> reused_identifiers;
    std::unordered_set<size_t> reused_realms;
    verify_live(slices, reused_identifiers, reused_realms);
    size_t reused = 0;
    for (const uint32_t identifier : reused_identifiers)
        reused += first_identifiers.contains(identifier);
    require(reused > 0, "released Slice slots were not reused");
    for (const size_t realm : reused_realms)
        require(first_realms.contains(realm), "released realm capacity was bypassed during reuse");
    slices.clear();
    verify_releases();
    LOG_INFO_STREAM << "Distinct backing rollover, concurrent publication, reuse, and reclamation passed";
}
