/** --------------------------------------------------------------------------------------------------------- Chain Buffet
 * @file chainbuffet.cpp
 * @brief Registers placements and claims Slices from chains of arena buffers.
 */
#include <alligator.hpp>
#include <alligator/easyvulkan.hpp>
#include <gpu/runtime.hpp>
#include <memory/arena.hpp>
#include <memory/tracker.hpp>
#include <atomic>
#include <cstdint>
#include <array>
#include <limits>
#include <new>

namespace buffetalligator {
namespace {
constinit std::array<std::array<std::atomic<ChainBuffet*>, CHAIN_LANES>, 8> current_chains{};
constinit std::atomic<uint32_t> next_lane{0};  ///< Round-robin lane for each new claiming thread.
thread_local const uint32_t lane = next_lane.fetch_add(1, std::memory_order_relaxed) % CHAIN_LANES;
/** --------------------------------------------------------------------------------------------------------- Count Allocation
 * @brief Reports one completed allocation through its registered placement.
 */
void count_allocation(size_t type, size_t bytes, const void* buffet) {
    const BuffetDescriptor& placement = *BuffetDescriptors::get(type);
    Memory::record_code_location(placement, bytes, buffet);
    Memory::record_allocation(placement, bytes);
}
/** --------------------------------------------------------------------------------------------------------- Count Deallocation
 * @brief Reports one released allocation through its registered placement.
 */
void count_deallocation(size_t type, size_t bytes, const void* buffet) {
    Memory::record_deallocation(*BuffetDescriptors::get(type), bytes);
    Memory::forget_code_location(buffet);
}
/** --------------------------------------------------------------------------------------------------------- Skip Record
 * @brief Leaves the tracker untouched while allocation tracking is off.
 */
void skip_record(size_t, size_t, const void*) {}
using Recorder = void (*)(size_t type, size_t bytes, const void* buffet);
constinit std::atomic<Recorder> allocation_recorder{&count_allocation};
constinit std::atomic<Recorder> deallocation_recorder{&count_deallocation};
} // namespace
/** --------------------------------------------------------------------------------------------------------- Next Descriptor Index
 * @brief Tracks the highest registered descriptor index.
 */
std::atomic<size_t>& BuffetDescriptors::next_index() {
    static std::atomic<size_t> index{2};
    return index;
}
/** --------------------------------------------------------------------------------------------------------- Descriptor List
 * @brief Stores the registered heap and Vulkan descriptors before custom placements are added.
 */
std::array<const BuffetDescriptor*, 8>& BuffetDescriptors::list() {
    static std::array<const BuffetDescriptor*, 8> descriptors{
        descriptor_for(static_cast<AlignedHeapBuffer*>(nullptr)),
        descriptor_for(static_cast<VulkanBuffer*>(nullptr)),
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
    };
    return descriptors;
}
/** --------------------------------------------------------------------------------------------------------- Get Descriptor
 * @brief Resolves an exact descriptor index without remapping unregistered placements.
 */
const BuffetDescriptor* BuffetDescriptors::get(size_t type) {
    return type < list().size() ? list()[type] : nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Register Descriptor
 * @brief Registers a descriptor at the fixed type index encoded in its Slice identifiers.
 */
size_t BuffetDescriptors::register_descriptor(const BuffetDescriptor* descriptor) {
    const size_t index = descriptor->type_idx;
    if (index >= list().size()) {
        ALLIGATOR_THROW(std::string("Buffet type ") + descriptor->type_name + " reports type index "
            + std::to_string(index) + "; placements are numbered 0 through 7");
    }
    if (list()[index] != nullptr && list()[index] != descriptor) {
        ALLIGATOR_THROW(std::string("Buffet type index ") + std::to_string(index) + " already belongs to "
            + list()[index]->type_name + "; " + descriptor->type_name + " cannot share it");
    }
    list()[index] = descriptor;
    size_t count = next_index().load(std::memory_order_acquire);
    while (count <= index && !next_index().compare_exchange_weak(
        count, index + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {}
    return index;
}
/** --------------------------------------------------------------------------------------------------------- Default Placement
 * @brief Selects the frozen placement of the active compute device at startup: its host-visible
 * buffers whenever a GPU exists, unified or discrete, and registered host memory without one.
 */
const BuffetDescriptor*& BuffetDescriptors::default_placement() {
    static const BuffetDescriptor* placement = gpu_device().placement;
    return placement;
}
/** --------------------------------------------------------------------------------------------------------- Descriptor Count
 * @brief Returns the exclusive upper bound of registered descriptor indexes.
 */
size_t BuffetDescriptors::count() { return next_index().load(std::memory_order_acquire); }
/** --------------------------------------------------------------------------------------------------------- Record Allocation
 * @brief Reports one completed allocation through the selected recorder.
 */
void BuffetDescriptors::record_allocation(size_t type, size_t bytes, const void* buffet) {
    allocation_recorder.load(std::memory_order_relaxed)(type, bytes, buffet);
}
/** --------------------------------------------------------------------------------------------------------- Record Deallocation
 * @brief Reports one released allocation through the selected recorder.
 */
void BuffetDescriptors::record_deallocation(size_t type, size_t bytes, const void* buffet) {
    deallocation_recorder.load(std::memory_order_relaxed)(type, bytes, buffet);
}
/** --------------------------------------------------------------------------------------------------------- Set Allocation Tracking
 * @brief Selects counting or no-op recorders; a later change would unbalance the counters.
 */
void BuffetDescriptors::set_allocation_tracking(bool enabled) {
    if (Arena::directory) ALLIGATOR_THROW("Set allocation tracking before the first Slice");
    allocation_recorder.store(enabled ? &count_allocation : &skip_record,
        std::memory_order_relaxed);
    deallocation_recorder.store(enabled ? &count_deallocation : &skip_record,
        std::memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Allocation Tracking
 * @brief Reports whether the counting recorders are selected.
 */
bool BuffetDescriptors::allocation_tracking() {
    return allocation_recorder.load(std::memory_order_relaxed) == &count_allocation;
}
/** --------------------------------------------------------------------------------------------------------- Constructor
 * @brief Opens the first buffer and links the two buffers ahead of it.
 */
ChainBuffet::ChainBuffet(const BuffetDescriptor* descriptor, size_t size)
: descriptor_(descriptor) {
    if (size > (OFFSET_LIMIT + 1) << 6) ALLIGATOR_THROW("ChainBuffet buffers hold at most 4 GiB");
    (void)Alligator::inst();
    granules_ = (size + 63) >> 6;
    current_.store(Arena::open(descriptor_, granules_, false), std::memory_order_relaxed);
    try {
        (void)next(current_.load(std::memory_order_relaxed), 1);
    } catch (...) {
        for (uint32_t buffer = current_.load(std::memory_order_relaxed); buffer < BUFFER_PENDING;) {
            const uint32_t successor = std::atomic_ref<uint32_t>(Arena::successors[buffer]).load(
                std::memory_order_acquire);
            Arena::retire(buffer);
            buffer = successor;
        }
        throw;
    }
}
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Walks from the current buffer through the buffers ahead, dropping the chain's hold on each;
 * buffers with live Slices remain until those Slices release.
 */
ChainBuffet::~ChainBuffet() {
    for (uint32_t buffer = current_.load(std::memory_order_acquire); buffer < BUFFER_PENDING;) {
        const uint32_t successor = std::atomic_ref<uint32_t>(Arena::successors[buffer]).load(
            std::memory_order_acquire);
        Arena::retire(buffer);
        buffer = successor;
    }
}
/** --------------------------------------------------------------------------------------------------------- Release Chains
 * @brief Drops the process-wide chains after the arena drains all asynchronous users.
 */
void ChainBuffet::release_chains() {
    for (auto& lanes : current_chains) {
        for (auto& current : lanes) delete current.exchange(nullptr, std::memory_order_acq_rel);
    }
}
/** --------------------------------------------------------------------------------------------------------- Chain
 * @brief Creates this thread's lane chain for the placement once, claims from it, and types the
 * constructed slot.
 */
Slice ChainBuffet::chain(
    const BuffetDescriptor* placement,
    size_t size,
    bool novel_buffer,
    uint8_t type
    ALLIGATOR_SITE_DEFINITION
) {
    if (!size) return Slice();
    ALLIGATOR_SITE_GUARD;
    (void)Alligator::inst();
    const GPUDevice& device = gpu_device();
    // Novel buffers past the probed ceiling or the live device budget headroom land on the host heap.
    if (novel_buffer && placement == device.placement
        && (size > device.placement_max_bytes || size > gpu_placement_headroom())) {
        placement = BuffetDescriptors::get(AlignedHeapBuffer::type_idx());
    }
    const BuffetDescriptor* selected = BuffetDescriptors::get(placement->type_idx);
    if (!selected) ALLIGATOR_THROW("Cannot claim an unregistered buffet placement");
    auto& current = current_chains[selected->type_idx][lane];
    ChainBuffet* const pending = reinterpret_cast<ChainBuffet*>(-1);
    ChainBuffet* node = current.load(std::memory_order_acquire);
    while (node == nullptr || node == pending) {
        if (node == pending) {
            current.wait(pending, std::memory_order_acquire);
            node = current.load(std::memory_order_acquire);
            continue;
        }
        if (!current.compare_exchange_weak(node, pending, std::memory_order_acq_rel,
            std::memory_order_acquire)) continue;
        try {
            node = new ChainBuffet(selected, selected->default_size);
        } catch (...) {
            current.store(nullptr, std::memory_order_release);
            current.notify_all();
            throw;
        }
        current.store(node, std::memory_order_release);
        current.notify_all();
    }
    Slice claimed = node->claim(size, novel_buffer ALLIGATOR_SITE_ARGUMENT);
    if (!type) return claimed;
    const SliceTypeRecord& record = *SliceTypes::records_[type];
    if (record.construct) record.construct(claimed.raw(), claimed.size_bytes() / record.size);
    Arena::states[claimed.id_ >> SEGMENT_BITS][claimed.id_ & SEGMENT_MASK].type = type;
    return claimed;
}
/** --------------------------------------------------------------------------------------------------------- Claim
 * @brief One fetch_add bumps the offset and counts the slot; stale or full buffers retry.
 */
Slice ChainBuffet::claim(size_t size, bool novel_buffer ALLIGATOR_SITE_DEFINITION) {
    if (!size) return Slice();
    ALLIGATOR_SITE_GUARD;
    if (size > (uint64_t{UINT32_MAX} << 6))
        ALLIGATOR_THROW("Slice size exceeds the 64-byte granule representation");
    const uint64_t granules = (size + 63) >> 6;
    // Lengths past the 20-bit field take a dedicated buffer whose record holds the exact length.
    if (novel_buffer || granules >= granules_ || granules > UNITS_LIMIT) {
        const uint32_t buffer = Arena::open(descriptor_, granules, true);
        try {
            return Slice(Slice::AdoptId{}, Arena::publish(
                Location::pack(buffer, 0, 0, granules > UNITS_LIMIT ? 0 : granules), 0));
        } catch (...) {
            Arena::retire(buffer);
            throw;
        }
    }
    uint32_t refill = BUFFER_NONE;
    for (;;) {
        const uint32_t buffer = current_.load(std::memory_order_acquire);
        BufferRecord& record = Arena::buffers[buffer];
        std::atomic_ref<uint64_t> cursor(record.cursor);
        const uint64_t previous = cursor.fetch_add(LIVE_ONE | granules,
            std::memory_order_acq_rel);
        // A count added to a retired buffer is undone without release, since nothing holds it.
        if (previous < LIVE_ONE) {
            cursor.fetch_sub(LIVE_ONE, std::memory_order_relaxed);
            continue;
        }
        if (current_.load(std::memory_order_acquire) != buffer) {
            Arena::retire(buffer);
            continue;
        }
        const uint64_t offset = previous & BUMP_MASK;
        // A full buffer hands over to its allocated successor; the swapper refills after it claims.
        if (offset + granules > record.granules) {
            uint32_t successor;
            try {
                successor = next(buffer);
            } catch (...) {
                Arena::retire(buffer);
                throw;
            }
            uint32_t expected = buffer;
            const bool swapped = current_.compare_exchange_strong(expected, successor,
                std::memory_order_acq_rel, std::memory_order_acquire);
            if (swapped) {
                Arena::retire(buffer);
                refill = successor;
            }
            Arena::retire(buffer);
            continue;
        }
        uint32_t slot;
        try {
            slot = Arena::publish(Location::pack(buffer, 0, offset, granules), 0);
        } catch (...) {
            Arena::retire(buffer);
            throw;
        }
        Slice claimed(Slice::AdoptId{}, slot);
        if (refill != BUFFER_NONE) (void)next(refill, 1);
        return claimed;
    }
}
/** --------------------------------------------------------------------------------------------------------- Next
 * @brief Fills an empty successor link first, then recurses `ahead` times; a claimer that finds
 * the link mid-allocation waits for that buffer instead of allocating another.
 */
uint32_t ChainBuffet::next(uint32_t buffer, size_t ahead) {
    std::atomic_ref<uint32_t> link(Arena::successors[buffer]);
    uint32_t successor = link.load(std::memory_order_acquire);
    while (successor >= BUFFER_PENDING) {
        if (successor == BUFFER_PENDING) {
            link.wait(BUFFER_PENDING, std::memory_order_acquire);
            successor = link.load(std::memory_order_acquire);
            continue;
        }
        if (!link.compare_exchange_weak(successor, BUFFER_PENDING, std::memory_order_acq_rel,
            std::memory_order_acquire)) continue;
        try {
            successor = Arena::open(descriptor_, granules_, false);
        } catch (...) {
            link.store(BUFFER_NONE, std::memory_order_release);
            link.notify_all();
            throw;
        }
        link.store(successor, std::memory_order_release);
        link.notify_all();
    }
    if (ahead) (void)next(successor, ahead - 1);
    return successor;
}
} // namespace buffetalligator
