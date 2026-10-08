/** --------------------------------------------------------------------------------------------------------- Buffet Alligator
 * @file alligator.cpp
 * @brief Implementation of the Alligator class: Slice tables and memory management only; the
 * thread pool lives in the Kitchen.
 */
#include <alligator.hpp>
#include <gpu/runtime.hpp>
#include <vulkan/shader_state.hpp>
#include <loggingutils.hpp>
#include <memory/tracker.hpp>
#include <memory/lifetime.hpp>
#include <containers/bitplane.hpp>
#include <new>
extern "C" void ba_net_shutdown(void);

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
                try {
                    region_backing[last] = std::make_unique<SliceRegion>(metadata_placement_, last);
                } catch (...) {
                    regions[last].store(nullptr, std::memory_order_release);
                    regions[last].notify_all();
                    throw;
                }
                r = region_backing[last]->host_ptr;
                directory_[last] = region_backing[last]->device_address;
                regions[last].store(r, std::memory_order_release);
                regions[last].notify_all();
                break;
            }
            regions[last].wait(reinterpret_cast<Region*>(-1), std::memory_order_acquire);
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
    if (r->slots[slot_id].owners.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        r->slots[slot_id].clear();
        r->gpu_slots[slot_id].clear();
        r->release(&r->slots[slot_id]);
    }
    slice.id_ = 0xFFFFFFFFu;
}
/** --------------------------------------------------------------------------------------------------------- Constructor
 * @brief Constructs an Alligator instance.
 */
Alligator::Alligator() {
    metadata_placement_ = gpu_device().placement;
    directory_backing_ = metadata_placement_->factory(64 * sizeof(uint64_t));
    try {
        directory_ = static_cast<uint64_t*>(metadata_placement_->host_ptr(directory_backing_, 0));
        directory_address_ = metadata_placement_->device_address(directory_backing_);
        region_backing[0] = std::make_unique<SliceRegion>(metadata_placement_, 0);
    } catch (...) {
        metadata_placement_->deleter(directory_backing_);
        throw;
    }
    directory_[0] = region_backing[0]->device_address;
    regions[0].store(region_backing[0]->host_ptr, std::memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Drains accepted work before releasing allocator roots and Slice metadata.
 */
Alligator::~Alligator() {
    ba_net_shutdown();
    ShaderState::drain();
    ChainBuffet::release_chains();
    for (size_t i = 0; i < regions.size(); ++i) {
        region_backing[i].reset();
        regions[i].store(nullptr, std::memory_order_release);
    }
    metadata_placement_->deleter(directory_backing_);
}
/** ------------------------------------------------------------------------------------------- Instance
 * @brief Returns the singleton instance of the Alligator.
 * @return The Alligator instance.
 */
Alligator& Alligator::inst() {
    static RuntimeFinalizer lifetime(new Alligator, &RuntimeFinalizer::delete_owner<Alligator>,
        RuntimeFinalizer::Phase::Arena);
    return *static_cast<Alligator*>(lifetime.object);
}
/** ------------------------------------------------------------------------------------------- GPU Table
 * @brief Returns one region's host mapping of the records reached through the GPU directory.
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
/** --------------------------------------------------------------------------------------------------------- GPU Directory
 * @brief Returns the device address of the arena's stable region directory.
 */
uint64_t Alligator::gpu_directory_address() { return inst().directory_address_; }
/** --------------------------------------------------------------------------------------------------------- SliceRegion Constructor
 * @brief Constructs a SliceRegion with the given BuffetDescriptor.
 * @param desc The BuffetDescriptor to use for allocation.
 */
Alligator::SliceRegion::SliceRegion(const BuffetDescriptor* desc, uint8_t index) : descriptor(desc) {
    handle = descriptor->factory(sizeof(Region));
    try {
#if defined(BUFFETALLIGATOR_TEST_REGION_CONSTRUCTION)
        BUFFETALLIGATOR_TEST_REGION_CONSTRUCTION(index);
#endif
        device_address = descriptor->device_address(handle);
        host_ptr = std::construct_at(static_cast<Region*>(descriptor->host_ptr(handle, 0)));
        host_ptr->region = index;
    } catch (...) {
        descriptor->deleter(handle);
        throw;
    }
}
/** --------------------------------------------------------------------------------------------------------- SliceRegion Destructor
 * @brief Destructs the SliceRegion, releasing its resources.
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
