/** --------------------------------------------------------------------------------------------------------- Slice Wrappers
 * @file slice.cpp
 * @brief Implements the public slice API over the C11 arena.
 */
#include <alligator.hpp>
#include <cstring>
extern "C" {
#include "core/ba_core.h"
int ba_vulkan_transfer(const ba_slice_t* slice, int to_device);
}

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Slice Layout
 * @brief Verifies the two-word C and C++ slice representations.
 */
struct SliceLayout {
    static_assert(sizeof(Slice) == sizeof(ba_slice_t));
    static_assert(alignof(Slice) == alignof(ba_slice_t));
    static_assert(offsetof(Slice, meta_) == offsetof(ba_slice_t, meta));
    static_assert(offsetof(Slice, cached_) == offsetof(ba_slice_t, ptr));
};
namespace {
/** --------------------------------------------------------------------------------------------------------- Check Claim
 * @brief Logs and throws a core allocation failure with its placement name.
 */
void check_claim(ba_status_t status, const Placemat* placement) {
    if (status == BA_OK) return;
    const std::string message = std::string(ba_status_name(status)) + ": " + placement->name();
    LOG_ERROR_STREAM << message;
    ALLIGATOR_THROW(message);
}
}
/** --------------------------------------------------------------------------------------------------------- Default Placement
 * @brief Resolves the current public default placement.
 */
const Placemat* Slice::default_placement() { return BuffetMenu::default_placement(); }
/** --------------------------------------------------------------------------------------------------------- Claim Constructor
 * @brief Claims a zeroed range from a registered placement.
 */
Slice::Slice(size_t size, const Placemat* placement) {
    if (!size) return;
    check_claim(ba_claim(placement->type(), size, 0, reinterpret_cast<ba_slice_t*>(this)), placement);
}
/** --------------------------------------------------------------------------------------------------------- Novel Constructor
 * @brief Claims a zeroed range with optional dedicated allocation.
 */
Slice::Slice(size_t size, bool novel, const Placemat* placement) {
    if (!size) return;
    check_claim(ba_claim(placement->type(), size, novel ? BA_CLAIM_NOVEL : 0, reinterpret_cast<ba_slice_t*>(this)), placement);
}
/** --------------------------------------------------------------------------------------------------------- External Constructor
 * @brief Copies external bytes into a newly claimed range.
 */
Slice::Slice(const void* source, size_t size, bool novel, const Placemat* placement) {
    if (!source || !size) return;
    check_claim(ba_claim(placement->type(), size, novel ? BA_CLAIM_NOVEL : 0, reinterpret_cast<ba_slice_t*>(this)), placement);
    std::memcpy(cached_, source, size);
    if (ba_vulkan_transfer(reinterpret_cast<const ba_slice_t*>(this), 1)) {
        free();
        ALLIGATOR_THROW("Vulkan upload failed");
    }
}
/** --------------------------------------------------------------------------------------------------------- Copy Constructor
 * @brief Retains the source plate and copies its two slice words.
 */
Slice::Slice(const Slice& other) : meta_(other.meta_), cached_(other.cached_) {
    if (meta_ != BA_NULL_META) ba_retain(reinterpret_cast<const ba_slice_t*>(&other));
}
/** --------------------------------------------------------------------------------------------------------- Copy Assignment
 * @brief Releases the destination and retains the source slice.
 */
Slice& Slice::operator=(const Slice& other) {
    if (this != &other) {
        free();
        if (other.meta_ != BA_NULL_META) ba_retain(reinterpret_cast<const ba_slice_t*>(&other));
        meta_ = other.meta_; cached_ = other.cached_;
    }
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Move Constructor
 * @brief Transfers the two slice words and nulls the source.
 */
Slice::Slice(Slice&& other) noexcept : meta_(other.meta_), cached_(other.cached_) {
    other.meta_ = BA_NULL_META; other.cached_ = nullptr;
}
/** --------------------------------------------------------------------------------------------------------- Move Assignment
 * @brief Releases the destination and transfers the source words.
 */
Slice& Slice::operator=(Slice&& other) noexcept {
    if (this != &other) {
        free();
        meta_ = other.meta_; cached_ = other.cached_;
        other.meta_ = BA_NULL_META; other.cached_ = nullptr;
    }
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Placement
 * @brief Resolves the public wrapper for this slice's owning placement.
 */
const Placemat* Slice::placement() const {
    return meta_ == BA_NULL_META ? nullptr : BuffetMenu::get(ba_slice_placement(reinterpret_cast<const ba_slice_t*>(this)));
}
/** --------------------------------------------------------------------------------------------------------- Novel Backing
 * @brief Reports dedicated backing without changing the Slice's packed representation.
 */
bool Slice::is_novel() const noexcept {
    return ba_slice_is_novel(reinterpret_cast<const ba_slice_t*>(this)) != 0;
}
/** --------------------------------------------------------------------------------------------------------- View
 * @brief Retains a checked subrange of this slice.
 */
Slice Slice::slice(size_t offset, size_t length) const {
    Slice result;
    const ba_status_t status = ba_view(reinterpret_cast<const ba_slice_t*>(this), offset, length, reinterpret_cast<ba_slice_t*>(&result));
    if (status == BA_E_RANGE) {
        const char* message = offset >= size_bytes() ? "Slice::slice: offset exceeds slice size" : "Slice::slice: length exceeds slice size";
        LOG_ERROR_STREAM << message;
        ALLIGATOR_THROW(message);
    }
    return result;
}
/** --------------------------------------------------------------------------------------------------------- Free
 * @brief Releases this slice and replaces it with the null representation.
 */
void Slice::free() { ba_release(reinterpret_cast<ba_slice_t*>(this)); }
/** --------------------------------------------------------------------------------------------------------- Resize
 * @brief Resizes a slice with optional preservation of its existing bytes.
 */
void Slice::resize(
    size_t new_size,
    bool preserve_data,
    bool novel_buffer,
    const Placemat* placement
) {
    if (is_null()) {
        *this = Slice(new_size, novel_buffer, placement);
        return;
    }
    if (new_size == size_bytes()) {
        return;
    }
    if (new_size < size_bytes() && preserve_data) {
        Slice shrunk = slice(0, new_size);
        *this = std::move(shrunk);
        return;
    }
    Slice grown(new_size, novel_buffer, placement);
    if (preserve_data && !is_null() && !grown.is_null()) {
        if (ba_vulkan_transfer(reinterpret_cast<const ba_slice_t*>(this), 0))
            ALLIGATOR_THROW("Vulkan readback failed");
        std::memcpy(grown.raw(), raw(), std::min(size_bytes(), new_size));
        if (ba_vulkan_transfer(reinterpret_cast<const ba_slice_t*>(&grown), 1))
            ALLIGATOR_THROW("Vulkan upload failed");
    }
    *this = std::move(grown);
}
/** --------------------------------------------------------------------------------------------------------- Row Allocation
 * @brief Stores a Slice owner immediately before the row without a general-purpose heap allocation.
 */
void* SliceMap::Row::operator new(size_t bytes) {
    Slice storage(bytes + sizeof(Slice));
    void* row = storage.data<std::byte>() + sizeof(Slice);
    void* owner = storage.raw();
    ::new (owner) Slice(std::move(storage));
    return row;
}
/** --------------------------------------------------------------------------------------------------------- Row Deallocation
 * @brief Moves the backing owner off-row before releasing the row's allocation.
 */
void SliceMap::Row::operator delete(void* pointer) noexcept {
    auto* owner = reinterpret_cast<Slice*>(static_cast<std::byte*>(pointer) - sizeof(Slice));
    Slice storage(std::move(*owner));
    owner->~Slice();
}
/** --------------------------------------------------------------------------------------------------------- Lookup Slot
 * @brief Searches atomic index buckets and screens IDs before acquiring a matching row.
 */
int64_t SliceMap::lookup_slot(int64_t id, size_t first) const {
    if (!capacity_ || id == SENTINEL) return -1;
    const size_t hash = hash_id(id);
    const size_t fingerprint = hash & ~index_mask_;
    size_t bucket = hash & index_mask_;
    for (size_t scanned = 0; scanned <= index_mask_; ++scanned) {
        const size_t entry = index_bucket(bucket).load(std::memory_order_acquire);
        if (!entry) break;
        const size_t slot = (entry & index_mask_) - 1;
        if ((entry & ~index_mask_) == fingerprint
            && std::atomic_ref<int64_t>(const_cast<int64_t&>(ids()[slot]))
            .load(std::memory_order_relaxed) == id) {
            return slot >= first ? static_cast<int64_t>(slot) : -1;
        }
        bucket = (bucket + 1) & index_mask_;
    }
    return -1;
}
/** --------------------------------------------------------------------------------------------------------- Find Internal
 * @brief Locates the slot index for the given ID, or -1 if not found.
 * @param id The ID to search for.
 * @return The slot index containing the ID, or -1 if not found.
 */
int64_t SliceMap::find_internal(int64_t id) const {
    if (id == SENTINEL) return -1;
    const int64_t needle = static_cast<int64_t>(id);
    int64_t slot = lookup_slot(needle, 0);
    while (slot >= 0) {
        if (verify_slot(slot, needle)) return slot;
        slot = lookup_slot(needle, static_cast<size_t>(slot) + 1);
    }
    return -1;
}
/** --------------------------------------------------------------------------------------------------------- Find Internal
 * @brief Locates the slot index for the given ID, or -1 if not found.
 * @param id The ID to search for.
 * @return The slot index containing the ID, or -1 if not found.
 */
int64_t SliceMap::find_internal(uint32_t id) const {
    return find_internal(static_cast<int64_t>(id));
}
/** --------------------------------------------------------------------------------------------------------- Get Slice Internal
 * @brief Retrieves the payload slice for the given ID, or an empty slice if not found.
 * @param id The ID to search for.
 * @return The payload slice associated with the ID, or an empty slice if not found.
 */
Slice SliceMap::get_slice_internal(int64_t id) {
    if (id == SENTINEL) return Slice();
    const int64_t needle = static_cast<int64_t>(id);
    int64_t slot = lookup_slot(needle, 0);
    while (slot >= 0) {
        Row* row = slot_row(slot).load(std::memory_order_acquire);
        hazard_protect(0, row);
        if (verify_held(slot, needle, row)) {
            Slice out = row->payload.slice();
            hazard_clear(0);
            return out;
        }
        hazard_clear(0);
        slot = lookup_slot(needle, static_cast<size_t>(slot) + 1);
    }
    return Slice();
}
/** --------------------------------------------------------------------------------------------------------- Get Slice Internal
 * @brief Retrieves the payload slice for the given ID, or an empty slice if not found.
 * @param id The ID to search for.
 * @return The payload slice associated with the ID, or an empty slice if not found.
 */
Slice SliceMap::get_slice_internal(uint32_t id) {
    return get_slice_internal(static_cast<int64_t>(id));
}
/** --------------------------------------------------------------------------------------------------------- Placement Handle
 * @brief Resolves the underlying handle for a non-null slice.
 */
Placemat::Handle* Placemat::get_for(const Slice* slice) {
    if (!slice || slice->is_null()) return nullptr;
    return reinterpret_cast<Placemat::Handle*>(ba_slice_handle(reinterpret_cast<const ba_slice_t*>(slice)));
}
}
namespace buffetalligator {
namespace {
/** --------------------------------------------------------------------------------------------------------- Placement Statistics
 * @brief Reads a single placement's core counters.
 */
ba_stats_t memory_stats(const Placemat& placement) {
    ba_stats_t value; ba_stats(placement.type(), &value); return value;
}
/** --------------------------------------------------------------------------------------------------------- System Snapshot
 * @brief Reads a fresh operating-system memory snapshot.
 */
ba_sysinfo_t memory_system() {
    ba_sysinfo_t value; ba_sysinfo(&value); return value;
}
}
/** --------------------------------------------------------------------------------------------------------- total_allocations
 * @brief Returns all slab and novel bytes obtained from placements.
 */
size_t Memory::total_allocations() { ba_stats_t value; ba_stats_total(&value); return value.slab_bytes_allocated + value.novel_bytes_allocated; }
/** --------------------------------------------------------------------------------------------------------- total_freed
 * @brief Returns all slab and novel bytes returned to placements.
 */
size_t Memory::total_freed() { ba_stats_t value; ba_stats_total(&value); return value.slab_bytes_freed + value.novel_bytes_freed; }
/** --------------------------------------------------------------------------------------------------------- placement_allocations
 * @brief Returns all bytes obtained from one placement.
 */
size_t Memory::placement_allocations(const Placemat& placement) { auto value = memory_stats(placement); return value.slab_bytes_allocated + value.novel_bytes_allocated; }
/** --------------------------------------------------------------------------------------------------------- placement_freed
 * @brief Returns all bytes returned to one placement.
 */
size_t Memory::placement_freed(const Placemat& placement) { auto value = memory_stats(placement); return value.slab_bytes_freed + value.novel_bytes_freed; }
/** --------------------------------------------------------------------------------------------------------- placement_usage
 * @brief Returns bytes currently owned by one placement.
 */
size_t Memory::placement_usage(const Placemat& placement) { auto value = memory_stats(placement); return value.slab_bytes_allocated + value.novel_bytes_allocated - value.slab_bytes_freed - value.novel_bytes_freed; }
/** --------------------------------------------------------------------------------------------------------- placement_reserved
 * @brief Returns free-list and runway capacity.
 */
size_t Memory::placement_reserved(const Placemat& placement) { return memory_stats(placement).reserved_bytes; }
/** --------------------------------------------------------------------------------------------------------- placement_live
 * @brief Returns placement usage excluding free-list and runway reserves.
 */
size_t Memory::placement_live(const Placemat& placement) { auto value = memory_stats(placement); const uint64_t used = value.slab_bytes_allocated + value.novel_bytes_allocated - value.slab_bytes_freed - value.novel_bytes_freed; return used > value.reserved_bytes ? used - value.reserved_bytes : 0; }
/** --------------------------------------------------------------------------------------------------------- placement_budget
 * @brief Returns the placement capacity budget.
 */
size_t Memory::placement_budget(const Placemat& placement) { return memory_stats(placement).budget_bytes; }
/** --------------------------------------------------------------------------------------------------------- placement_available
 * @brief Returns budget headroom excluding live ownership.
 */
size_t Memory::placement_available(const Placemat& placement) { const size_t budget = placement_budget(placement); if (budget == SIZE_MAX) return SIZE_MAX; const size_t live = placement_live(placement); return budget > live ? budget - live : 0; }
/** --------------------------------------------------------------------------------------------------------- set_placement_budget
 * @brief Sets a placement capacity ceiling.
 */
void Memory::set_placement_budget(const Placemat& placement, size_t bytes) { ba_budget_set(placement.type(), bytes); }
/** --------------------------------------------------------------------------------------------------------- placement_novel_cached
 * @brief Returns published zeroed novel-cache bytes.
 */
size_t Memory::placement_novel_cached(const Placemat& placement) { return memory_stats(placement).novel_cache_bytes; }
/** --------------------------------------------------------------------------------------------------------- placement_runway_target
 * @brief Returns the current prepared slab target.
 */
size_t Memory::placement_runway_target(const Placemat& placement) { return memory_stats(placement).runway_target; }
/** --------------------------------------------------------------------------------------------------------- placement_slab_size
 * @brief Returns the resolved slab capacity.
 */
size_t Memory::placement_slab_size(const Placemat& placement) { return memory_stats(placement).slab_bytes; }
/** --------------------------------------------------------------------------------------------------------- trim
 * @brief Synchronously releases idle placement reserves and novel caches.
 */
void Memory::trim(const Placemat& placement) { ba_trim(placement.type()); }
/** --------------------------------------------------------------------------------------------------------- trim_all
 * @brief Synchronously releases idle capacity across all placements.
 */
void Memory::trim_all() { ba_trim_all(); }
/** --------------------------------------------------------------------------------------------------------- system_physical
 * @brief Returns physical memory capacity.
 */
size_t Memory::system_physical() { return memory_system().physical; }
/** --------------------------------------------------------------------------------------------------------- system_available
 * @brief Returns available operating-system memory.
 */
size_t Memory::system_available() { return memory_system().available; }
/** --------------------------------------------------------------------------------------------------------- system_limit
 * @brief Returns the effective process memory ceiling.
 */
size_t Memory::system_limit() { return memory_system().limit; }
/** --------------------------------------------------------------------------------------------------------- system_headroom
 * @brief Returns available memory constrained by process headroom.
 */
size_t Memory::system_headroom() { auto value = memory_system(); const uint64_t limit = value.limit ? value.limit : value.physical; return std::min(value.available, limit > value.rss ? limit - value.rss : uint64_t(0)); }
/** --------------------------------------------------------------------------------------------------------- page_size
 * @brief Returns the native OS page size.
 */
size_t Memory::page_size() { return memory_system().page; }
/** --------------------------------------------------------------------------------------------------------- large_page_size
 * @brief Returns a usable reported large-page size or zero.
 */
size_t Memory::large_page_size() { return memory_system().large_page; }
/** --------------------------------------------------------------------------------------------------------- hardware_threads
 * @brief Returns hardware threads available to the process.
 */
unsigned Memory::hardware_threads() { return memory_system().hw_threads; }
/** --------------------------------------------------------------------------------------------------------- pressure
 * @brief Returns the operating-system memory pressure level.
 */
Memory::Pressure Memory::pressure() { return static_cast<Pressure>(ba_pressure()); }
}
