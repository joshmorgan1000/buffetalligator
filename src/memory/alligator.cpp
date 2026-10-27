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
#include <memory/slicetype.hpp>
#include <memory/tracker.hpp>
#include <atomic>
#include <cstring>
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
/** --------------------------------------------------------------------------------------------------------- Current ChainBuffet for Placement
 * @brief The current link of this thread's lane of the placement's chains, made on the first touch; with an
 * expected link it is a compare-and-swap of the lane's current link instead.
 * @param desc The placement.
 * @param expected The link the lane is expected to be on, or null to read.
 * @param desired The link to move the lane to.
 * @return The current link, or the desired link when the swap landed and null when it did not.
 */
/// @brief The lanes' current links, four per placement, grown as placements register; the sentinel while growing.
static std::atomic<void*> heads;
/// @brief The placements the lanes cover.
static std::atomic<size_t> num_heads{0};
ChainBuffet* Alligator::current_for(const BuffetDescriptor* desc, ChainBuffet** expected, ChainBuffet* desired) {
    static std::atomic<size_t> lane_counter{0};
    static thread_local size_t lane = lane_counter.fetch_add(1, std::memory_order_relaxed) % 4;
    const size_t current_size = num_heads.load(std::memory_order_acquire);
    if (current_size <= desc->index) {
        /// A probe or swap of lanes that have no links yet finds nothing; only a claim grows the heads.
        if (expected != nullptr || desired != nullptr) return nullptr;
        void* desired = reinterpret_cast<void*>(-1ll);
        void* expected = heads.load(std::memory_order_acquire);
        if (expected != desired && heads.compare_exchange_strong(expected, desired, std::memory_order_acq_rel)) {
            /// The new lanes start empty; each makes its link on its first claim.
            std::atomic<ChainBuffet*>* lane_heads = new std::atomic<ChainBuffet*>[(desc->index + 1) * 4]();
            std::memcpy(lane_heads, expected, current_size * 4 * sizeof(std::atomic<ChainBuffet*>));
            num_heads.store(desc->index + 1, std::memory_order_release);
            heads.store(reinterpret_cast<void*>(lane_heads), std::memory_order_release);
        }
    }
    while (heads.load(std::memory_order_acquire) == reinterpret_cast<void*>(-1ll)) {
        std::this_thread::yield();
    }
    if (desired == nullptr && expected == nullptr) {
        std::atomic<ChainBuffet*>& current =
            reinterpret_cast<std::atomic<ChainBuffet*>*>(heads.load(std::memory_order_acquire))[desc->index * 4 + lane];
        ChainBuffet* link = current.load(std::memory_order_acquire);
        while (link == nullptr || link == PENDING_LINK) {
            ChainBuffet* none = nullptr;
            if (current.compare_exchange_strong(none, PENDING_LINK, std::memory_order_acq_rel)) {
                /// A lane's first link carries the placement's own table when it has one, otherwise the chain's.
                const types::SliceSemantics* table = desc->semantics != nullptr
                    ? static_cast<const types::SliceSemantics*>(desc->semantics)
                    : &SliceStatics::semantics_of(typeid(ChainBuffet));
                std::atomic<ChainBuffet*>* lane_heads =
                    reinterpret_cast<std::atomic<ChainBuffet*>*>(heads.load(std::memory_order_acquire));
                link = new ChainBuffet(
                    desc,
                    desc->default_size_method(),
                    const_cast<types::SliceSemantics*>(table),
                    desc->type != 0 ? desc->type : table->type,
                    lane_heads + desc->index * 4 + lane,
                    1,
                    false
                );
                current.store(link, std::memory_order_release);
                return link;
            }
            std::this_thread::yield();
            link = current.load(std::memory_order_acquire);
        }
        return link;
    }
    reinterpret_cast<std::atomic<ChainBuffet*>*>(
        heads.load(std::memory_order_acquire)
    )[desc->index * 4 + lane].compare_exchange_strong(
        *expected,
        desired,
        std::memory_order_acq_rel
    );
    return *expected;
}
/** --------------------------------------------------------------------------------------------------------- Buffet Of
 * @brief The buffet object behind a Slice's bytes: the link's buffer as the placement's hooks see it.
 * @param slice The Slice.
 * @return The buffet object.
 */
void* Alligator::buffet_of(const Slice& slice) {
    return slice.is_pooled() ? static_cast<ChainBuffet*>(slice.parent_)->handle_
                             : static_cast<Slice*>(slice.parent_)->parent_;
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
 * @brief A chain root's last handle is gone: the root's count leaves its link.
 * @param root The root.
 */
void Alligator::destroy(Slice& root) {
    SliceStatics::CHAIN_BUFFER_SEMANTICS.destructor_fn(root);
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
 * @brief The teardown's allocator phase, after the Kitchen has stopped: every lane of every placement lets go
 * of its current link, the one ahead of it and the one behind it, so a drained chain frees its slabs.
 */
void Alligator::finalize() {
    void* grown = heads.load(std::memory_order_acquire);
    if (grown == nullptr || grown == reinterpret_cast<void*>(-1ll)) return;
    std::atomic<ChainBuffet*>* lanes = static_cast<std::atomic<ChainBuffet*>*>(grown);
    const size_t slots = num_heads.load(std::memory_order_acquire) * 4;
    for (size_t slot = 0; slot < slots; ++slot) {
        ChainBuffet* current = lanes[slot].exchange(nullptr, std::memory_order_acq_rel);
        if (current == nullptr || current == PENDING_LINK) continue;
        ChainBuffet* links[3] = {current->prev_.load(std::memory_order_acquire), current,
            current->next_.load(std::memory_order_acquire)};
        if (links[0] == current) links[0] = nullptr;
        for (ChainBuffet* link : links) {
            if (link == nullptr || link == PENDING_LINK) continue;
            while (ChainBuffet::decrement_refcount(link)) {}
        }
    }
}
/** --------------------------------------------------------------------------------------------------------- Alligator Destructor
 * @brief The Alligator owns nothing of its own beyond the lanes, which finalize has already drained.
 */
Alligator::~Alligator() {}
/** --------------------------------------------------------------------------------------------------------- For Type
 * @brief Retrieves the placement descriptor for the given type index.
 * @param type The type index.
 * @return The placement descriptor for the type.
 */
const BuffetDescriptor* Alligator::for_type(uint32_t type) {
    const BuffetDescriptor* placement = SliceStatics::semantics(type).placement;
    return placement != nullptr ? placement : Slice::default_placement();
}
/** --------------------------------------------------------------------------------------------------------- Add Typed Placement
 * @brief A type's placement: the base placement's hooks under the registry's next index, so the type's claims
 * come from chains of their own that carry its table and type word.
 * @param base The placement whose memory the type's claims come from.
 * @param semantics The type's table.
 * @param type The type word its claims carry.
 * @return The type's placement descriptor.
 */
const BuffetDescriptor* BuffetDescriptors::add_typed(const BuffetDescriptor& base, const void* semantics, uint32_t type) {
    std::unique_lock write_lock(mutex_);
    const size_t index = list_.size();
    BuffetDescriptor* placement = new BuffetDescriptor(base);
    placement->semantics = semantics;
    placement->type = type;
    placement->index = index;
    placement->base = base.base;
    list_.emplace_back(placement);
    return placement;
}
} // namespace buffetalligator
