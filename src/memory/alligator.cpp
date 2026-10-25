/** --------------------------------------------------------------------------------------------------------- Buffet Alligator
 * @file alligator.cpp
 * @brief The Alligator: every claim and release, the lanes of chains each placement keeps, and teardown.
 */
#include <logging.hpp>
#include <alligator.hpp>
#include <alligator/kitchen.hpp>
#include <alligator/easygpu.hpp>
#include <alligator/mmapbuffet.hpp>
#include <gpu/backend.hpp>
#include <gpu/easyvulkan.hpp>
#include <memory/chainbuffet.hpp>
#include <memory/tracker.hpp>
#include <atomic>
#include <thread>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Registry Seed
 * @brief The registry opens with the heap at index zero, so a lookup by index is valid before any Slice exists;
 * the Alligator's construction registers the rest and lets the GPU probe move the default.
 */
BuffetDescriptors::BuffetDescriptors() {
    add<AlignedHeapBuffer>();
}
/** --------------------------------------------------------------------------------------------------------- BuffetDescriptors record_allocation
 * @brief Forwards one completed buffet allocation to the memory tracker.
 * @param type The buffet type index.
 * @param bytes The allocation size in bytes.
 * @param buffet The new buffet handle.
 */
void BuffetDescriptors::record_allocation(size_t type, size_t bytes, const void* buffet) {
    const BuffetDescriptor& placement = *BuffetDescriptors::get(type);
    Memory::record_allocation(placement, bytes);
    Memory::record_code_location(placement, bytes, buffet);
}
/** --------------------------------------------------------------------------------------------------------- BuffetDescriptors record_deallocation
 * @brief Forwards one buffet release to the memory tracker.
 * @param type The buffet type index.
 * @param bytes The allocation size in bytes.
 * @param buffet The buffet handle being released.
 */
void BuffetDescriptors::record_deallocation(size_t type, size_t bytes, const void* buffet) {
    Memory::record_deallocation(*BuffetDescriptors::get(type), bytes);
    Memory::forget_code_location(buffet);
}
/** --------------------------------------------------------------------------------------------------------- Chain Lane
 * @brief The lane this thread claims from, picked at its first claim.
 */
thread_local size_t Alligator::chain_lane = SIZE_MAX;
/** --------------------------------------------------------------------------------------------------------- Current ChainBuffet for Placement
 * @brief The current link of this thread's lane of the placement's chains, made on the first touch; with an
 * expected link it is a compare-and-swap of the lane's current link instead.
 * @param desc The placement.
 * @param expected The link the lane is expected to be on, or null to read.
 * @param desired The link to move the lane to.
 * @return The current link, or the desired link when the swap landed and null when it did not.
 */
ChainBuffet* Alligator::current_for(const BuffetDescriptor* desc, ChainBuffet** expected, ChainBuffet* desired) {
    static std::array<std::atomic<ChainBuffet*>, 128 * CHAIN_LANES> heads{};
    static std::atomic<size_t> lane_counter{0};
    /// A thread's lane is fixed at its first claim, so a placement's claimers spread across its chains.
    if (chain_lane == SIZE_MAX) chain_lane = lane_counter.fetch_add(1, std::memory_order_relaxed) % CHAIN_LANES;
    std::atomic<ChainBuffet*>& current = heads[*desc->type_idx_getter() * CHAIN_LANES + chain_lane];
    if (expected != nullptr || desired != nullptr) {
        return current.compare_exchange_strong(*expected, desired, std::memory_order_acq_rel) ? desired : nullptr;
    }
    ChainBuffet* link = current.load(std::memory_order_acquire);
    while (link == nullptr || link == PENDING_LINK) {
        ChainBuffet* none = nullptr;
        if (current.compare_exchange_strong(none, PENDING_LINK, std::memory_order_acq_rel)) {
            link = ChainBuffet::open(desc, desc->default_size_method(), false, true);
            current.store(link, std::memory_order_release);
            return link;
        }
        std::this_thread::yield();
        link = current.load(std::memory_order_acquire);
    }
    return link;
}
/** --------------------------------------------------------------------------------------------------------- Buffet Of
 * @brief The buffet object behind a Slice's bytes: the link's buffer as the placement's hooks see it.
 * @param slice The Slice.
 * @return The buffet object.
 */
void* Alligator::buffet_of(const Slice& slice) {
    const uintptr_t bits = reinterpret_cast<uintptr_t>(slice.record_);
    return reinterpret_cast<SliceRecord*>(bits & ~SLICE_VIEW_FLAG)->link->buffer_;
}
/** --------------------------------------------------------------------------------------------------------- Retain
 * @brief One more Slice on a claim: an owner on the record, or a view on the link whose bytes it keeps.
 * @param slice The Slice being shared.
 */
void Alligator::retain(const Slice& slice) {
    const uintptr_t bits = reinterpret_cast<uintptr_t>(slice.record_);
    SliceRecord* record = reinterpret_cast<SliceRecord*>(bits & ~SLICE_VIEW_FLAG);
    if (bits & SLICE_VIEW_FLAG) record->link->count_.fetch_add(1, std::memory_order_acq_rel);
    else std::atomic_ref<uint32_t>(record->owners).fetch_add(1, std::memory_order_acq_rel);
}
/** --------------------------------------------------------------------------------------------------------- Instance
 * @brief The process Alligator, built on first use and never destroyed before static owners retire.
 * @return The Alligator.
 */
Alligator& Alligator::inst() {
    static Alligator* alligator = new Alligator;
    return *alligator;
}
/** --------------------------------------------------------------------------------------------------------- Destroy
 * @brief Drops one Slice from its claim: a view lets go of the link it kept; the last owner out runs the element
 * type's destructor over the whole claim and lets go of the link it is on.
 * @param slice The Slice to destroy.
 */
void Alligator::destroy(Slice& slice) {
    const uintptr_t bits = reinterpret_cast<uintptr_t>(slice.record_);
    SliceRecord* record = reinterpret_cast<SliceRecord*>(bits & ~SLICE_VIEW_FLAG);
    if (bits & SLICE_VIEW_FLAG) {
        record->link->release();
        return;
    }
    if (std::atomic_ref<uint32_t>(record->owners).fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    if (record->type != 0) {
        const SliceTypeRecord* type = SliceTypes::get(record->type);
        type->destroy(record->host, (size_t(record->gpu.size) << 6) / type->size_per_element);
    }
    record->link->release();
}
/** --------------------------------------------------------------------------------------------------------- Alligator Constructor
 * @brief Registers every buffet type and lets the GPU probe make its placement the default.
 */
Alligator::Alligator() {
    BuffetDescriptors::descriptor<AlignedHeapBuffer>();
    GPUStatic::gpu_selection();
    BuffetDescriptors::descriptor<VulkanBuffer>();
    BuffetDescriptors::descriptor<MmapBuffet>();
    LOG_INFO_STREAM << "Buffet Alligator initialized";
}
/** --------------------------------------------------------------------------------------------------------- Finalize
 * @brief The teardown's allocator phase, after the Kitchen has stopped: every lane's current link, the one ahead
 * of it and the one behind it let go of the chain's hold, so a drained chain frees its slabs, and the parked
 * spares go back to their placements.
 */
void Alligator::finalize() {
    for (size_t index = 0; const BuffetDescriptor* desc = BuffetDescriptors::get(index); ++index) {
        for (size_t lane = 0; lane < CHAIN_LANES; ++lane) {
            chain_lane = lane;
            ChainBuffet* probe = nullptr;
            current_for(desc, &probe, nullptr);
            ChainBuffet* current = probe;
            if (current == nullptr || current == PENDING_LINK) continue;
            ChainBuffet* links[3] = {current->prev_.load(std::memory_order_acquire), current,
                current->next_.load(std::memory_order_acquire)};
            ChainBuffet* retiring = current;
            current_for(desc, &retiring, nullptr);
            for (ChainBuffet* link : links) {
                if (link == nullptr || link == PENDING_LINK) continue;
                link->close();
            }
        }
        /// The pending sentinel closes the slot, so a slab retired after teardown frees instead of parking.
        void* spare = ChainBuffet::spares_[index].exchange(PENDING_LINK, std::memory_order_acq_rel);
        if (spare && spare != PENDING_LINK) desc->active_deleter(spare);
    }
    chain_lane = SIZE_MAX;
}
/** --------------------------------------------------------------------------------------------------------- Alligator Destructor
 * @brief The Alligator owns nothing of its own beyond the lanes, which finalize has already drained.
 */
Alligator::~Alligator() {}
/** --------------------------------------------------------------------------------------------------------- Alligator::claim
 * @brief Claims a Slice: a record and its payload cut from this thread's lane of the placement, or a dedicated
 * link for a novel or slab-sized request.
 * @param size The size of the Slice to claim.
 * @param novel Whether the Slice should be allocated in a novel buffet.
 * @param placement The placement descriptor for the Slice.
 * @param type The type index for the Slice.
 * @return The claimed Slice.
 */
Slice Alligator::claim(
    size_t size,
    bool novel,
    const BuffetDescriptor* placement,
    uint32_t type
) {
    if (size == 0) [[unlikely]] return Slice{};
    if (size > 0xFFFFFFFFu - 63u) [[unlikely]] ALLIGATOR_THROW("Alligator::claim: requested size exceeds 32-bit limit");
    const size_t total = sizeof(SliceRecord) + ((size + 63u) & ~size_t{63});
    SliceRecord* record;
    if (novel || total >= placement->default_size_method()) {
        /// A dedicated link serves this one record and closes behind it, so the record's release frees it.
        ChainBuffet* link = ChainBuffet::open(placement, total, true, false);
        record = link->claim(total);
        link->count_.fetch_add(ChainBuffet::CLOSED, std::memory_order_release);
    } else {
        record = current_for(placement)->claim(total);
    }
    record->owners = 1u;
    record->type = type;
    Slice slice;
    slice.gpu_ = record->gpu;
    slice.host_ = record->host;
    slice.record_ = record;
    return slice;
}
} // namespace buffetalligator
