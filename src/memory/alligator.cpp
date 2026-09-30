/** --------------------------------------------------------------------------------------------------------- Buffet Alligator
 * @file alligator.cpp
 * @brief Implementation of the Alligator class: Slice tables and memory management only; the
 * thread pool lives in the Kitchen.
 */
#include <alligator.hpp>
#include <alligator/kitchen.hpp>
#include <loggingutils.hpp>
#include <memory/tracker.hpp>
#include <containers/bitplane.hpp>
#include <new>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Next ID
 * @brief Generates the next unique slice ID.
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
                region_backing[last] = std::make_unique<SliceRegion>();
                r = region_backing[last].get()->host_ptr;
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
    uint32_t slice_id = slice.id() >> 3;  // first 3 bits are the BuffetDescriptor
    const uint8_t region_id = slice_id & 0x3f;
    const uint32_t slot_id = slice_id >> 6;
    return regions[region_id].load(std::memory_order_acquire)->slots[slot_id];
}
/** --------------------------------------------------------------------------------------------------------- Entry (const)
 * @brief Returns a const reference to the SliceEntry corresponding to the given slice.
 * @param slice The slice to resolve.
 * @return Const reference to the SliceEntry.
 */
const SliceEntry& Alligator::entry(const Slice& slice) const {
    uint32_t slice_id = slice.id() >> 3;  // first 3 bits are the BuffetDescriptor
    const uint8_t region_id = slice_id & 0x3f;
    const uint32_t slot_id = slice_id >> 6;
    return regions[region_id].load(std::memory_order_acquire)->slots[slot_id];
}
/** --------------------------------------------------------------------------------------------------------- GPUBuf
 * @brief Returns the GPUBuf corresponding to the given slice.
 * @param slice The slice to resolve.
 * @return The slice's GPUBuf entry.
 */
GPUBuf* Alligator::gpubuf(const Slice& slice) {
    const uint32_t slice_id = slice.id() >> 3;
    return &regions[slice_id & 0x3f].load(std::memory_order_acquire)->gpu_slots[slice_id >> 6];
}
/** --------------------------------------------------------------------------------------------------------- GPUBuf (const)
 * @brief Returns the const GPUBuf corresponding to the given slice.
 * @param slice The slice to resolve.
 * @return The slice's const GPUBuf entry.
 */
const GPUBuf* Alligator::gpubuf(const Slice& slice) const {
    const uint32_t slice_id = slice.id() >> 3;
    return &regions[slice_id & 0x3f].load(std::memory_order_acquire)->gpu_slots[slice_id >> 6];
}
/** --------------------------------------------------------------------------------------------------------- Destroy
 * @brief Destroys the given slice.
 * @param slice The slice to destroy.
 */
void Alligator::destroy(Slice& slice) {
    const uint32_t slice_id = slice.id() >> 3;
    const uint32_t slot_id = slice_id >> 6;
    Region* r = regions[slice_id & 0x3f].load(std::memory_order_acquire);
    r->slots[slot_id].clear();
    r->gpu_slots[slot_id].clear();
    r->release(&r->slots[slot_id]);
    slice.id_ = 0xFFFFFFFFu;
}
/** --------------------------------------------------------------------------------------------------------- Constructor
 * @brief Constructs an Alligator instance.
 */
Alligator::Alligator() {
    region_backing[0] = std::make_unique<SliceRegion>();
    regions[0].store(region_backing[0].get()->host_ptr, std::memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Destructs the Alligator instance.
 */
Alligator::~Alligator() {
    for (size_t i = 0; i < regions.size(); ++i) {
        region_backing[i].reset();
        regions[i].store(nullptr, std::memory_order_release);
    }
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
 * @brief The shared GPUBuf table's host mapping, the same bytes shaders read at gpu_table_address().
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
/** --------------------------------------------------------------------------------------------------------- SliceRegion Constructor
 * @brief Constructs a SliceRegion with the given BuffetDescriptor.
 * @param desc The BuffetDescriptor to use for allocation.
 */
Alligator::SliceRegion::SliceRegion(const BuffetDescriptor* desc) : descriptor(desc) {
    handle = descriptor->factory(sizeof(Region));
    device_address = descriptor->device_address(handle);
    host_ptr = static_cast<Region*>(descriptor->host_ptr(handle, 0));
}
/** --------------------------------------------------------------------------------------------------------- SliceRegion Destructor
 * @brief Destructs the SliceRegion, releasing its resources.
 */
Alligator::SliceRegion::~SliceRegion() {
    if (handle) {
        descriptor->deleter(handle);
        handle = nullptr;
        device_address = 0;
        host_ptr = nullptr;
    }
}
} // namespace buffetalligator
