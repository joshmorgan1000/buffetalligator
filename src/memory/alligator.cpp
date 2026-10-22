/** --------------------------------------------------------------------------------------------------------- Buffet Alligator
 * @file alligator.cpp
 * @brief The Alligator: every claim and release, the slot regions with their tables and their directory,
 * and teardown.
 */
#include <alligator.hpp>
#include <alligator/kitchen.hpp>
#include <loggingutils.hpp>
#include <memory/tracker.hpp>
#include <containers/bitplane.hpp>
#include <utility>

namespace buffetalligator {
std::atomic<bool> Alligator::finalizing_{false};
const SliceTypeRecord* SliceTypes::records_[256] = {};
std::atomic<uint32_t> SliceTypes::count_{0};
/** --------------------------------------------------------------------------------------------------------- Register Type
 * @brief Assigns the next type index to a record and throws past 254 types.
 * @param record The element type's lifecycle hooks, which must outlive the process.
 * @return The assigned type index.
 */
uint8_t SliceTypes::register_type(const SliceTypeRecord* record) {
    const uint32_t index = count_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (index >= CHAIN_BUFFET) ALLIGATOR_THROW("SliceTypes supports at most 254 element types");
    records_[index] = record;
    return static_cast<uint8_t>(index);
}
/** --------------------------------------------------------------------------------------------------------- Next ID
 * @brief Generates the next unique slice ID, making a region and its directory entry on first use.
 * @param placement The placement whose type bits the id carries.
 * @return The next slice ID.
 */
uint32_t Alligator::next_id(const BuffetDescriptor* placement) {
    uint8_t last = last_region.load(std::memory_order_acquire);
    size_t tries = 0;
    while (true) {
        Region* r = regions[last].load(std::memory_order_acquire);
        while (r == nullptr || r == reinterpret_cast<Region*>(-1)) {
            Region* expected = nullptr;
            if (regions[last].compare_exchange_weak(expected, reinterpret_cast<Region*>(-1), std::memory_order_acq_rel)) {
                region_backing[last] = std::make_unique<SliceRegion>(metadata_placement_, last);
                r = region_backing[last]->host_ptr;
                directory_[last] = region_backing[last]->device_address;
                regions[last].store(r, std::memory_order_release);
                break;
            }
            std::this_thread::yield();
            r = regions[last].load(std::memory_order_acquire);
        }
        SliceEntry* entry = r->claim(skip_at_most.load(std::memory_order_acquire));
        if (entry != nullptr) {
            return ((((entry - r->slice_table()) << 6)
                | (last & 0x3f)) << 3) | (placement->type_idx & 0x7);
        }
        // A failed CAS leaves the winner's region in `last`, so concurrent advances move one region, not N.
        const uint8_t desired = (last + 1) & 0x3f;
        if (last_region.compare_exchange_strong(last, desired, std::memory_order_acq_rel)) {
            last = desired;
        }
        if (++tries == 64) {
            tries = 0;
            if (skip_at_most.fetch_add(1, std::memory_order_acq_rel) == 1024) {
                LOG_WARN_STREAM << "Alligator: advanced skip_at_most beyond 1024";
            }
        }
    }
}
/** --------------------------------------------------------------------------------------------------------- Entry
 * @brief Returns a reference to the SliceEntry corresponding to the given slice.
 * @param slice The slice to resolve.
 * @return Reference to the SliceEntry.
 */
SliceEntry& Alligator::entry(const Slice& slice) {
    return regions[(slice.id_ >> 3) & 0x3f].load(std::memory_order_acquire)->slots[slice.id_ >> 9];
}
/** --------------------------------------------------------------------------------------------------------- Entry (const)
 * @brief Returns a const reference to the SliceEntry corresponding to the given slice.
 * @param slice The slice to resolve.
 * @return Const reference to the SliceEntry.
 */
const SliceEntry& Alligator::entry(const Slice& slice) const {
    return regions[(slice.id_ >> 3) & 0x3f].load(std::memory_order_acquire)->slots[slice.id_ >> 9];
}
/** --------------------------------------------------------------------------------------------------------- GPUBuf
 * @brief Returns the GPUBuf corresponding to the given slice.
 * @param slice The slice to resolve.
 * @return The slice's GPUBuf entry.
 */
GPUBuf* Alligator::gpubuf(const Slice& slice) {
    return &regions[(slice.id_ >> 3) & 0x3f].load(std::memory_order_acquire)->gpu_slots[slice.id_ >> 9];
}
/** --------------------------------------------------------------------------------------------------------- GPUBuf (const)
 * @brief Returns the const GPUBuf corresponding to the given slice.
 * @param slice The slice to resolve.
 * @return The slice's const GPUBuf entry.
 */
const GPUBuf* Alligator::gpubuf(const Slice& slice) const {
    return &regions[(slice.id_ >> 3) & 0x3f].load(std::memory_order_acquire)->gpu_slots[slice.id_ >> 9];
}
/** --------------------------------------------------------------------------------------------------------- Claim
 * @brief The one way to get a Slice: a span of the placement's current ChainBuffet, or a novel buffer for a
 * novel or buffer-sized request, in a fresh slot carrying the type, whose record constructs the bytes.
 * @param size The size in bytes, rounded up to 64-byte granules; zero claims nothing.
 * @param novel Whether the Slice gets a dedicated buffer.
 * @param placement The placement to claim from.
 * @param type The SliceTypes record index the slot carries, zero for raw bytes.
 * @return The claimed Slice.
 */
Slice Alligator::claim(size_t size, bool novel, const BuffetDescriptor* placement, uint8_t type) {
    if (size == 0) [[unlikely]] return Slice();
    if (size > (uint64_t{UINT32_MAX} << 6)) [[unlikely]] {
        ALLIGATOR_THROW("Slice size exceeds the 64-byte granule representation");
    }
    Alligator& arena = inst();
    SliceEntry* entry = nullptr;
    if (novel || size >= placement->default_size) {
        /// A novel buffer is one ChainBuffet with no successor; its construction count goes once its slot counts.
        ChainBuffet* buffet = new ChainBuffet(placement, (size + 63) & ~size_t{63}, false);
        entry = buffet->claim(size);
        ChainBuffet::fetch_sub_ref_count(buffet);
    } else {
        ChainBuffet* held = hold_current(placement);
        entry = held->claim(size);
        release_buffet(held);
    }
    /// The regions fill in order, so the entry's region is nearly always the first one looked at.
    uint32_t id = UINT32_MAX;
    for (size_t index = 0; index < arena.regions.size(); ++index) {
        Region* region = arena.regions[index].load(std::memory_order_acquire);
        if (region == nullptr || region == reinterpret_cast<Region*>(-1)) continue;
        if (entry >= region->slots.data() && entry < region->slots.data() + region->slots.size()) {
            id = (static_cast<uint32_t>(entry - region->slots.data()) << 9)
                | (static_cast<uint32_t>(index) << 3) | (placement->type_idx & 0x7);
            break;
        }
    }
    if (type != 0u) {
        std::atomic_ref<uint32_t>(entry->state).fetch_or(
            static_cast<uint32_t>(type) << SliceEntry::TYPE_SHIFT, std::memory_order_release);
        const SliceTypeRecord* record = SliceTypes::records_[type];
        if (record->construct != nullptr) {
            Region& region = *arena.regions[(id >> 3) & 0x3f].load(std::memory_order_acquire);
            record->construct(region.host_ptrs[id >> 9], (uint64_t{region.gpu_slots[id >> 9].size} << 6) / record->size);
        }
    }
    return Slice::from_id(id);
}
/** --------------------------------------------------------------------------------------------------------- Destroy
 * @brief Drops one handle; the last one runs the slot's type destroy, frees the slot, and drops its count on
 * what it counted against: its ChainBuffet, or the parent it was cut from, which may go the same way.
 * @param slice The slice being released, left null.
 */
void Alligator::destroy(Slice& slice) {
    uint32_t id = std::exchange(slice.id_, UINT32_MAX);
    for (;;) {
        Region& region = *regions[(id >> 3) & 0x3f].load(std::memory_order_acquire);
        const uint32_t slot = id >> 9;
        const uint32_t previous = std::atomic_ref<uint32_t>(region.slots[slot].state)
            .fetch_sub(1u, std::memory_order_acq_rel);
        if ((previous & SliceEntry::OWNERS_MASK) != 1u) return;
        const uint8_t type = static_cast<uint8_t>(previous >> SliceEntry::TYPE_SHIFT);
        if (type != 0u) {
            const SliceTypeRecord* record = SliceTypes::records_[type];
            if (record->destroy != nullptr) {
                record->destroy(region.host_ptrs[slot], (uint64_t{region.gpu_slots[slot].size} << 6) / record->size);
            }
        }
        const uint32_t parent = region.parents[slot];
        region.release(&region.slots[slot]);
        Region& above = *regions[(parent >> 3) & 0x3f].load(std::memory_order_acquire);
        if (std::atomic_ref<uint32_t>(above.slots[parent >> 9].state).load(std::memory_order_acquire)
                >> SliceEntry::TYPE_SHIFT == SliceTypes::CHAIN_BUFFET) {
            release_buffet(static_cast<ChainBuffet*>(above.host_ptrs[parent >> 9]));
            return;
        }
        id = parent;
    }
}
/** --------------------------------------------------------------------------------------------------------- Constructor
 * @brief Makes the region directory and the first region in the metadata placement.
 */
Alligator::Alligator() {
    metadata_placement_ = BuffetDescriptors::default_placement();
    directory_backing_ = metadata_placement_->factory(64 * sizeof(uint64_t));
    try {
        directory_ = static_cast<uint64_t*>(metadata_placement_->host_ptr(directory_backing_, 0));
        directory_address_ = metadata_placement_->device_address != nullptr
            ? metadata_placement_->device_address(directory_backing_) : 0;
        region_backing[0] = std::make_unique<SliceRegion>(metadata_placement_, 0);
    } catch (...) {
        metadata_placement_->deleter(directory_backing_);
        throw;
    }
    directory_[0] = region_backing[0]->device_address;
    regions[0].store(region_backing[0]->host_ptr, std::memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Marks teardown, so a ChainBuffet released from here on deletes in place. The regions, the directory
 * and the current ChainBuffets stay with the process, so a Slice released after this point still resolves.
 */
Alligator::~Alligator() {
    finalizing_.store(true, std::memory_order_release);
    for (size_t i = 0; i < regions.size(); ++i) region_backing[i].release();
}
/** ------------------------------------------------------------------------------------------- Instance
 * @brief Returns the singleton instance of the Alligator.
 * @return The Alligator instance.
 */
Alligator& Alligator::inst() {
    static Alligator instance;
    return instance;
}
/** ------------------------------------------------------------------------------------------- GPU Table
 * @brief One region's host mapping of the GPUBuf records reached through the directory.
 * @param region_id The region ID of the GPU table.
 * @return The table base.
 */
const GPUBuf* Alligator::gpu_table(uint8_t region_id) {
    return inst().regions[region_id].load(std::memory_order_acquire)->gpu_table();
}
/** ------------------------------------------------------------------------------------------- GPUBuf For
 * @brief The writable GPUBuf for a live slice.
 * @param slice The slice to resolve.
 * @return The slice's GPUBuf entry.
 */
GPUBuf* Alligator::gpubuf_for(const Slice& slice) {
    return inst().gpubuf(slice);
}
/** ------------------------------------------------------------------------------------------- Device Address
 * @brief The device address of a Slice's first byte.
 * @param slice The slice to resolve.
 * @return The device address.
 */
uint64_t Alligator::device_address(const Slice& slice) {
    const GPUBuf& record = *inst().gpubuf(slice);
    return record.address + (uint64_t{record.offset} << 6);
}
/** ------------------------------------------------------------------------------------------- GPU Directory
 * @brief The stable device address of the region directory shaders index with a Slice's region bits.
 * @return The directory's device address.
 */
uint64_t Alligator::gpu_directory_address() { return inst().directory_address_; }
/** --------------------------------------------------------------------------------------------------------- SliceRegion Constructor
 * @brief Makes one region in the metadata placement and constructs its tables in place.
 * @param desc The placement the region's tables live in.
 * @param index The region's index.
 */
Alligator::SliceRegion::SliceRegion(const BuffetDescriptor* desc, uint8_t index) : descriptor(desc) {
    handle = descriptor->factory(sizeof(Region));
    try {
        device_address = descriptor->device_address != nullptr ? descriptor->device_address(handle) : 0;
        host_ptr = std::construct_at(static_cast<Region*>(descriptor->host_ptr(handle, 0)));
        host_ptr->region = index;
    } catch (...) {
        descriptor->deleter(handle);
        throw;
    }
}
/** --------------------------------------------------------------------------------------------------------- SliceRegion Destructor
 * @brief Destroys the region's tables and releases their buffet.
 */
Alligator::SliceRegion::~SliceRegion() {
    if (handle) {
        std::destroy_at(host_ptr);
        descriptor->deleter(handle);
        handle = nullptr;
        device_address = 0;
        host_ptr = nullptr;
    }
}
} // namespace buffetalligator
