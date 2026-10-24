/** --------------------------------------------------------------------------------------------------------- Buffet Alligator
 * @file alligator.cpp
 * @brief The Alligator: every claim and release, the slot regions with their tables and their directory,
 * and teardown.
 */
#include <logging.hpp>
#include <alligator.hpp>
#include <alligator/kitchen.hpp>
#include <alligator/easygpu.hpp>
#include <alligator/mmapbuffet.hpp>
#include <gpu/backend.hpp>
#include <gpu/easyvulkan.hpp>
#include <memory/tracker.hpp>
#include <containers/bitplane.hpp>
#include <utility>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Retire Handle
 * @brief A link whose count reached zero: a dedicated link goes whole, since only its slots ever saw it; a chain
 * link is first closed to late claimers, then freed unless one pinned it meanwhile, and its shell, handle and
 * count stay for whoever is still walking the chain.
 * @param pointer The `SharedBuffet` whose count reached zero.
 */
void Alligator::retire_handle(void* pointer) {
    SharedBuffet* handle = static_cast<SharedBuffet*>(pointer);
    ChainBuffet* link = static_cast<ChainBuffet*>(handle->data_->chainbuffet);
    if (link->next_.load(std::memory_order_acquire) == nullptr) {
        delete handle;
        return;
    }
    std::atomic_ref<SharedBuffet*>(link->parent_).store(nullptr, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (handle->data_->refs.refcount() != 0) return;
    link->free();
}
/** --------------------------------------------------------------------------------------------------------- Registry Seed
 * @brief The registry opens with the heap at index zero, so a lookup by index is valid before any Chomp exists;
 * the Alligator's construction registers the rest and lets the GPU probe move the default.
 */
BuffetDescriptors::BuffetDescriptors() {
    add<AlignedHeapBuffer>();
}
/** --------------------------------------------------------------------------------------------------------- ChompID Constructor
 * @brief Constructs a ChompID from the given region, claim, and type indices, and novelty flag.
 * @param region_index The index of the region.
 * @param claim_index The index of the claim within the region.
 * @param type_index The index of the type.
 * @param is_novel Whether the claim is novel.
 */
ChompID::ChompID(uint32_t region_index, uint32_t claim_index, uint32_t type_index, bool is_novel)
: id_((is_novel ? ((region_index & NOVEL_CLAIM_MASK) | NOVEL_CLAIM_REGION_MASK) << CHOMP_TYPE_BITS
: (region_index & COMBINED_REGION_MASK) << CHOMP_TYPE_BITS)
| ((claim_index & CLAIM_MASK) << (CHOMP_TYPE_BITS + REGION_BITS))
| (type_index & CHOMP_TYPES_MASK)) {}
/** --------------------------------------------------------------------------------------------------------- ChompID::entry
 * @brief Retrieves the ChompEntry corresponding to this ChompID.
 * @return Pointer to the ChompEntry.
 */
ChompEntry* ChompID::entry() {
    return Alligator::inst().region_backing()->at(get_chomp_region_index(id_))
        .region()->chomp_slots.data() + get_chomp_claim_index(id_);
}
/** --------------------------------------------------------------------------------------------------------- ChompID::entry
 * @brief Retrieves the ChompEntry corresponding to this ChompID.
 * @return Pointer to the ChompEntry.
 */
const ChompEntry* ChompID::entry() const {
    return Alligator::inst().region_backing()->at(get_chomp_region_index(id_))
        .region()->chomp_slots.data() + get_chomp_claim_index(id_);
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
/** ------------------------------------------------------------------------------------------- Current ChainBuffet for Index
 * @brief Retrieves the current ChainBuffet instance for the specified index.
 * @param idx Index of the BuffetDescriptor.
 * @return Reference to the atomic pointer holding the current ChainBuffet.
 */
void* Alligator::current_for(const BuffetDescriptor* desc, void** expected, void* desired) {
    static std::array<std::atomic<void*>, 128> instances{nullptr};
    size_t idx = *desc->type_idx_getter();
    std::atomic<void*>& current = instances[idx];
    if (expected != nullptr || desired != nullptr) {
        if (current.compare_exchange_strong(*expected, desired)) {
            return desired;
        } else {
            return nullptr; // Failed to compare_exchange_strong, return nullptr
        }
    }
    while (current.load(std::memory_order_acquire) == nullptr
        || current.load(std::memory_order_acquire) == reinterpret_cast<void*>(-1)
    ) {
        void* expected = nullptr;
        if (current.compare_exchange_strong(expected, reinterpret_cast<void*>(-1))) {
            ChainBuffet* n = new ChainBuffet(nullptr, desc, desc->default_size_method(), true);
            current.store(n, std::memory_order_release);
            break;
        }
        std::this_thread::yield();
    }
    return current.load(std::memory_order_acquire);
}
/** --------------------------------------------------------------------------------------------------------- ChompRegion::region
 * @brief The region's slot tables, cut on the first touch so the directory's thousand regions cost nothing until used.
 * @return Pointer to the Region.
 */
Alligator::Region* Alligator::ChompRegion::region() {
    HostDetails* details = std::atomic_ref<HostDetails*>(host_details).load(std::memory_order_acquire);
    return details ? details->region : materialize();
}
/** --------------------------------------------------------------------------------------------------------- ChompRegion::region (const)
 * @brief The region's slot tables, cut on the first touch.
 * @return Pointer to the Region.
 */
const Alligator::Region* Alligator::ChompRegion::region() const {
    return const_cast<ChompRegion*>(this)->region();
}
/** --------------------------------------------------------------------------------------------------------- ChompRegion::materialize
 * @brief Cuts the region's slot tables from the region placement and publishes them; a race's loser returns the
 * winner's tables and lets its own go.
 * @return Pointer to the Region.
 */
Alligator::Region* Alligator::ChompRegion::materialize() {
    const BuffetDescriptor* desc = BuffetDescriptors::get(default_region_allocator());
    void* raw = desc->active_factory(sizeof(Region));
    Region* table = new (desc->host_ptr(raw)) Region();
    HostDetails* details = new HostDetails{table, raw, desc};
    HostDetails* expected = nullptr;
    if (!std::atomic_ref<HostDetails*>(host_details).compare_exchange_strong(expected, details,
            std::memory_order_acq_rel)) {
        table->~Region();
        desc->active_deleter(raw);
        delete details;
        return expected->region;
    }
    std::atomic_ref<uint64_t>(device_address).store(desc->device_address(raw), std::memory_order_release);
    return table;
}
/** --------------------------------------------------------------------------------------------------------- ChompRegion Constructor
 * @brief A directory entry starts empty; its tables follow on the first touch.
 */
Alligator::ChompRegion::ChompRegion() {}
/** --------------------------------------------------------------------------------------------------------- ChompRegion Destructor
 * @brief Returns the region's slot tables to their placement.
 */
Alligator::ChompRegion::~ChompRegion() {
    if (!host_details) return;
    host_details->region->~Region();
    host_details->desc->active_deleter(host_details->handle);
    delete host_details;
    host_details = nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Alligator::region_backing
 * @brief Retrieves the backing array of ChompRegions.
 * @return Pointer to the array of ChompRegions.
 */
std::array<Alligator::ChompRegion, COMBINED_REGION_SIZE>* Alligator::region_backing() {
    return static_cast<std::array<ChompRegion, COMBINED_REGION_SIZE>*>(region_backing_host_);
}
/** --------------------------------------------------------------------------------------------------------- ChompID Copies
 * @brief A ChompID is the bare id; copying it carries no ownership.
 * @param other The ChompID to copy from.
 * @return Reference to the newly created ChompID.
 */
ChompID::ChompID(const ChompID& other) : id_(other.id_) {}
/** --------------------------------------------------------------------------------------------------------- ChompID Assignment
 * @brief Assigns the value of another ChompID to this one.
 * @param other The ChompID to assign from.
 * @return Reference to this ChompID after assignment.
 */
ChompID& ChompID::operator=(const ChompID& other) { id_ = other.id_; return *this; }
/** --------------------------------------------------------------------------------------------------------- ChompID Destructor
 * @brief Destroys the ChompID, releasing any associated resources.
 */
ChompID::~ChompID() {}
/** --------------------------------------------------------------------------------------------------------- ChompID::gpubuf
 * @brief Retrieves the GPUBuf record corresponding to this ChompID.
 * @return Pointer to the GPUBuf.
 */
GPUBuf* ChompID::gpubuf() {
    return Alligator::inst().region_backing()->at(get_chomp_region_index(id_))
        .region()->gpu_slots.data() + get_chomp_claim_index(id_);
}
/** --------------------------------------------------------------------------------------------------------- ChompID::gpubuf (const)
 * @brief Retrieves the GPUBuf record corresponding to this ChompID.
 * @return Pointer to the GPUBuf.
 */
const GPUBuf* ChompID::gpubuf() const {
    return Alligator::inst().region_backing()->at(get_chomp_region_index(id_))
        .region()->gpu_slots.data() + get_chomp_claim_index(id_);
}
/** --------------------------------------------------------------------------------------------------------- ChompID::owners
 * @brief The slot's owner count: the Chomps sharing this id, the last of which clears the slot.
 * @return Pointer to the count.
 */
uint32_t* ChompID::owners() {
    return Alligator::inst().region_backing()->at(get_chomp_region_index(id_))
        .region()->owner_slots.data() + get_chomp_claim_index(id_);
}
/** --------------------------------------------------------------------------------------------------------- ChompID::owners (const)
 * @brief The slot's owner count: the Chomps sharing this id, the last of which clears the slot.
 * @return Pointer to the count.
 */
const uint32_t* ChompID::owners() const {
    return Alligator::inst().region_backing()->at(get_chomp_region_index(id_))
        .region()->owner_slots.data() + get_chomp_claim_index(id_);
}
/** --------------------------------------------------------------------------------------------------------- ChompEntry
 * @brief Retrieves the ChompEntry record corresponding to this ChompID.
 * @param chomp The Chomp whose entry is being retrieved.
 * @return Pointer to the ChompEntry.
 */
ChompEntry* Alligator::entry(Chomp& chomp) { return ChompID(chomp.id_).entry(); }
/** --------------------------------------------------------------------------------------------------------- ChompEntry (const)
 * @brief Retrieves the ChompEntry record corresponding to this ChompID.
 * @param chomp The Chomp whose entry is being retrieved.
 * @return Pointer to the ChompEntry.
 */
const ChompEntry* Alligator::entry(const Chomp& chomp) const { return ChompID(chomp.id_).entry(); }
/** --------------------------------------------------------------------------------------------------------- GPUBuf
 * @brief Retrieves the GPUBuf record corresponding to this ChompID.
 * @param chomp The Chomp whose GPUBuf is being retrieved.
 * @return Pointer to the GPUBuf.
 */
GPUBuf* Alligator::gpubuf(const Chomp& chomp) { return ChompID(chomp.id_).gpubuf(); }
/** --------------------------------------------------------------------------------------------------------- GPUBuf (const)
 * @brief Retrieves the GPUBuf record corresponding to this ChompID.
 * @param chomp The Chomp whose GPUBuf is being retrieved.
 * @return Pointer to the GPUBuf.
 */
const GPUBuf* Alligator::gpubuf(const Chomp& chomp) const { return ChompID(chomp.id_).gpubuf(); }
/** --------------------------------------------------------------------------------------------------------- Buffet Of
 * @brief The buffet object behind a Chomp's slot: the link's shared buffer as the placement's hooks see it.
 * @param chomp The Chomp.
 * @return The buffet object.
 */
void* Alligator::buffet_of(const Chomp& chomp) {
    return static_cast<ChainBuffet*>(buffer_of(chomp)->data_->chainbuffet)->buffer_;
}
/** --------------------------------------------------------------------------------------------------------- Buffer Of
 * @brief The link handle behind a Chomp's slot, where the link's reference count lives.
 * @param chomp The Chomp.
 * @return The handle.
 */
SharedBuffet* Alligator::buffer_of(const Chomp& chomp) {
    return std::atomic_ref<SharedBuffet*>(ChompID(chomp.id_).entry()->buffet).load(std::memory_order_acquire);
}
/** --------------------------------------------------------------------------------------------------------- Retain
 * @brief One more Chomp on a slot: one more count on the link handle behind it.
 * @param chomp The Chomp being shared.
 */
void Alligator::retain(const Chomp& chomp) {
    std::atomic_ref<uint32_t>(*ChompID(chomp.id_).owners()).fetch_add(1, std::memory_order_acq_rel);
}
/** --------------------------------------------------------------------------------------------------------- Directory Address
 * @brief The device address of the region directory shaders resolve ids through.
 * @return The address.
 */
uint64_t Alligator::gpu_directory_address() const {
    return region_backing_device_address_;
}
/** --------------------------------------------------------------------------------------------------------- Instance
 * @brief The process Alligator, built on first use and never destroyed before static owners retire.
 * @return The Alligator.
 */
Alligator& Alligator::inst() {
    static Alligator* alligator = new Alligator;
    return *alligator;
}
/** --------------------------------------------------------------------------------------------------------- Default Region Allocator
 * @brief The descriptor index the region directory and regions are allocated from.
 * @return The index, writable so backend selection can point it at device-visible memory.
 */
size_t& Alligator::default_region_allocator() {
    static size_t index = 0;
    return index;
}
/** --------------------------------------------------------------------------------------------------------- Destroy
 * @brief Drops one reference on the chain buffer behind a Chomp's slot; the last reference clears the slot,
 * frees the buffer, and for a dedicated link deletes the link itself.
 * @param chomp The Chomp to destroy.
 */
void Alligator::destroy(Chomp& chomp) {
    ChompID chomp_id(chomp.id_);
    /// Copies share the slot and count on its own word; only the last one out touches the link's count.
    if (std::atomic_ref<uint32_t>(*chomp_id.owners()).fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    ChompEntry* slot = chomp_id.entry();
    SharedBuffet* handle = std::atomic_ref<SharedBuffet*>(slot->buffet).load(std::memory_order_acquire);
    const uint32_t type = uint32_t(get_chomp_type_index(chomp.id_));
    if (type != 0) {
        const ChompTypeRecord* record = ChompTypes::get(type);
        record->destroy(chomp.id_, (size_t(chomp_id.gpubuf()->size) << 6) / record->size_per_element);
    }
    chomp_id.gpubuf()->clear();
    std::atomic_ref<SharedBuffet*>(slot->buffet).store(nullptr, std::memory_order_release);
    if (handle->data_->refs.decrement_refcount() == 0) retire_handle(handle);
}
/** --------------------------------------------------------------------------------------------------------- Alligator Constructor
 * @brief Registers every buffet type, lets the GPU probe make its placement the default, and cuts the region
 * directory from that default so shaders can read it; the regions themselves follow on first touch.
 */
Alligator::Alligator() {
    BuffetDescriptors::descriptor<AlignedHeapBuffer>();
    GPUStatic::gpu_selection();
    BuffetDescriptors::descriptor<VulkanBuffer>();
    BuffetDescriptors::descriptor<MmapBuffet>();
    const BuffetDescriptor* desc = BuffetDescriptors::default_placement();
    default_region_allocator() = *desc->type_idx_getter();
    region_backing_raw_ = desc->active_factory(sizeof(std::array<ChompRegion, COMBINED_REGION_SIZE>));
    region_backing_host_ = new (desc->host_ptr(region_backing_raw_)) std::array<ChompRegion, COMBINED_REGION_SIZE>();
    region_backing_device_address_ = desc->device_address(region_backing_raw_);
    LOG_INFO_STREAM << "Buffet Alligator initialized";
}
/** --------------------------------------------------------------------------------------------------------- Finalize
 * @brief The teardown's allocator phase, after the Kitchen has stopped: every placement's current link, the one
 * prepared ahead of it and the one behind it let go of their own count, so a drained chain frees its buffers.
 */
void Alligator::finalize() {
    for (size_t index = 0; const BuffetDescriptor* desc = BuffetDescriptors::get(index); ++index) {
        void* probe = nullptr;
        current_for(desc, &probe, nullptr);
        ChainBuffet* current = static_cast<ChainBuffet*>(probe);
        if (current == nullptr || current == reinterpret_cast<ChainBuffet*>(-1)) continue;
        ChainBuffet* links[3] = {current->prev_.load(std::memory_order_acquire), current,
            current->next_.load(std::memory_order_acquire)};
        void* retiring = current;
        current_for(desc, &retiring, nullptr);
        for (ChainBuffet* link : links) {
            if (link == nullptr || link == reinterpret_cast<ChainBuffet*>(-1)) continue;
            SharedBuffet* handle = std::atomic_ref<SharedBuffet*>(link->parent_).exchange(nullptr,
                std::memory_order_seq_cst);
            if (handle && handle->data_->refs.decrement_refcount() == 0) retire_handle(handle);
        }
    }
}
/** --------------------------------------------------------------------------------------------------------- Alligator Destructor
 * @brief Returns every region's tables and the directory to their placement.
 */
Alligator::~Alligator() {
    const BuffetDescriptor* desc = BuffetDescriptors::get(default_region_allocator());
    if (region_backing_host_) std::destroy_at(region_backing());
    if (region_backing_raw_) desc->active_deleter(region_backing_raw_);
    region_backing_host_ = nullptr;
    region_backing_raw_ = nullptr;
    region_backing_device_address_ = 0;
}
/** --------------------------------------------------------------------------------------------------------- Alligator::claim
 * @brief Claims a Chomp from the Alligator.
 * @param size The size of the Chomp to claim.
 * @param novel Whether the Chomp should be allocated in a novel buffet.
 * @param placement The placement descriptor for the Chomp.
 * @param type The type index for the Chomp.
 * @return The claimed Chomp.
 */
Chomp Alligator::claim(
    size_t size,
    bool novel,
    const BuffetDescriptor* placement,
    uint32_t type
) {
    if (size == 0) [[unlikely]] return Chomp{};
    if (size > 0xFFFFFFFFu - 63u) [[unlikely]] ALLIGATOR_THROW("Alligator::claim: requested size exceeds 32-bit limit");
    size = (size + 63u) & ~size_t{63};
    SharedBuffet* const taken = reinterpret_cast<SharedBuffet*>(-1);
    if (novel || size >= placement->default_size_method()) {
        ChompEntry* slot = nullptr;
        ChompID chomp_id;
        while (slot == nullptr) {
            const uint32_t counter = inst().novel_idx.fetch_add(1, std::memory_order_relaxed);
            chomp_id = ChompID(
                (counter >> CLAIM_BITS) & NOVEL_CLAIM_MASK, counter & CLAIM_MASK, type, true
            );
            SharedBuffet* expected = nullptr;
            if (std::atomic_ref<SharedBuffet*>(chomp_id.entry()->buffet).compare_exchange_strong(
                    expected, taken, std::memory_order_acq_rel)) slot = chomp_id.entry();
        }
        SharedBuffet* handle = new SharedBuffet(placement, size, true);
        ChainBuffet* link = static_cast<ChainBuffet*>(handle->data_->chainbuffet);
        link->bump_ptr_.store(static_cast<uint32_t>(size), std::memory_order_release);
        chomp_id.gpubuf()->set(placement->device_address(link->buffer_), static_cast<uint32_t>(size >> 6), 0);
        std::atomic_ref<uint32_t>(*chomp_id.owners()).store(1u, std::memory_order_release);
        std::atomic_ref<SharedBuffet*>(slot->buffet).store(handle, std::memory_order_release);
        Chomp chomp;
        chomp.id_ = chomp_id.id();
        return chomp;
    }
    ChompEntry* slot = nullptr;
    ChompID chomp_id;
    while (slot == nullptr) {
        const uint32_t counter = inst().transient_idx.fetch_add(1, std::memory_order_relaxed);
        chomp_id = ChompID(
            (counter >> CLAIM_BITS) % NUMBER_OF_TRANSIENT_REGIONS, counter & CLAIM_MASK, type, false
        );
        SharedBuffet* expected = nullptr;
        if (std::atomic_ref<SharedBuffet*>(chomp_id.entry()->buffet).compare_exchange_strong(
                expected, taken, std::memory_order_acq_rel)) slot = chomp_id.entry();
    }
    std::atomic_ref<uint32_t>(*chomp_id.owners()).store(1u, std::memory_order_release);
    ChainBuffet* current = static_cast<ChainBuffet*>(current_for(placement));
    current->claim_into(slot, chomp_id.gpubuf(), size);
    Chomp chomp;
    chomp.id_ = chomp_id.id();
    return chomp;
}
} // namespace buffetalligator
