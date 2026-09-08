/** --------------------------------------------------------------------------------------------------------- Arena Core
 * @file ba_core.c
 * @brief Implements placements, reference-counted plates, and thread-local claims.
 */
#define BA_INTERNAL
#include "ba_core.h"
#include <string.h>
#include <stdlib.h>

#if defined(_MSC_VER)
#define BA_COLD __declspec(noinline)
#else
#define BA_COLD __attribute__((noinline, cold))
#endif
static ba_sysinfo_t g_sys;
static ba_tunables_t g_tunables = {3, 4, 256, 4, 16, 1, 50, 3};
static ba_placement_t g_placements[BA_MAX_PLACEMENTS];
static _Atomic uint32_t g_placement_count;
static _Atomic uint32_t g_default;
static ba_plate_t g_plates[BA_PLATE_COUNT];
static _Atomic uint64_t g_slot_head;
static _Atomic uint32_t g_slot_fresh = 1;
static _Atomic uint64_t g_os_live_bytes;
static uint64_t g_os_budget;
static uint32_t g_slot_stride;
static _Atomic int g_closed;
static _Atomic int g_initialized;
static atomic_flag g_registration_lock = ATOMIC_FLAG_INIT;
static atomic_flag g_policy_lock = ATOMIC_FLAG_INIT;
static atomic_flag g_header_lock = ATOMIC_FLAG_INIT;
static ba_slab_t* g_header_free;
static _Thread_local ba_tls_plate_t g_tls[BA_MAX_PLACEMENTS];
/** --------------------------------------------------------------------------------------------------------- Slab Record
 * @brief Stores a pooled slab header and its built-in substrate handle.
 */
typedef struct ba_slab_record {
    ba_slab_t slab; ///< Aligned header.
    ba_handle_t handle; ///< Built-in allocation handle.
} ba_slab_record_t;
/** --------------------------------------------------------------------------------------------------------- Minimum
 * @brief Selects the smaller unsigned quantity.
 */
static uint64_t ba_min(uint64_t first, uint64_t second) { return first < second ? first : second; }
/** --------------------------------------------------------------------------------------------------------- Round Up
 * @brief Rounds a value to a power-of-two multiple.
 */
static uint64_t ba_round(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}
/** --------------------------------------------------------------------------------------------------------- Power Ceiling
 * @brief Rounds positive sizes up to a power of two.
 */
static uint64_t ba_pow2_ceil(uint64_t value) {
    if (value <= 1) return 1;
    --value;
    value |= value >> 1; value |= value >> 2; value |= value >> 4;
    value |= value >> 8; value |= value >> 16; value |= value >> 32;
    return value + 1;
}
/** --------------------------------------------------------------------------------------------------------- Power Floor
 * @brief Rounds positive sizes down to a power of two.
 */
static uint64_t ba_pow2_floor(uint64_t value) {
    return value ? ba_pow2_ceil(value / 2 + 1) : 0;
}
/** --------------------------------------------------------------------------------------------------------- Spin Lock
 * @brief Acquires an infrequently used metadata lock.
 */
static void ba_lock(atomic_flag* lock) {
    while (atomic_flag_test_and_set_explicit(lock, memory_order_acquire)) ba_os_yield();
}
/** --------------------------------------------------------------------------------------------------------- Unlock
 * @brief Publishes protected metadata changes.
 */
static void ba_unlock(atomic_flag* lock) { atomic_flag_clear_explicit(lock, memory_order_release); }
/** --------------------------------------------------------------------------------------------------------- Header Pop
 * @brief Obtains a slab header from an OS-page-backed pool.
 */
static ba_slab_t* ba_header_pop(void) {
    ba_lock(&g_header_lock);
    ba_slab_t* header = g_header_free;
    if (header) g_header_free = header->next;
    ba_unlock(&g_header_lock);
    if (header) return header;
    const size_t bytes = ba_round(64 * sizeof(ba_slab_record_t), g_sys.page);
    ba_slab_record_t* records = ba_os_map(bytes, g_sys.page, 0);
    if (!records) return NULL;
    const size_t count = bytes / sizeof(*records);
    ba_lock(&g_header_lock);
    for (size_t index = 1; index < count; ++index) {
        records[index].slab.next = g_header_free;
        g_header_free = &records[index].slab;
    }
    ba_unlock(&g_header_lock);
    return &records[0].slab;
}
/** --------------------------------------------------------------------------------------------------------- Header Push
 * @brief Returns a retired header to the metadata pool.
 */
static void ba_header_push(ba_slab_t* header) {
    ba_lock(&g_header_lock);
    header->next = g_header_free;
    g_header_free = header;
    ba_unlock(&g_header_lock);
}
/** --------------------------------------------------------------------------------------------------------- Charge Counter
 * @brief Reserves bytes atomically without exceeding a budget.
 */
static int ba_charge_counter(_Atomic uint64_t* counter, uint64_t bytes, uint64_t budget) {
    uint64_t used = atomic_load_explicit(counter, memory_order_relaxed);
    for (;;) {
        if (used > budget || bytes > budget - used) return 0;
        if (atomic_compare_exchange_weak_explicit(counter, &used, used + bytes,
            memory_order_relaxed, memory_order_relaxed)) return 1;
    }
}
/** --------------------------------------------------------------------------------------------------------- Charge
 * @brief Reserves both placement and global OS capacity before allocation.
 */
static ba_status_t ba_charge(ba_placement_t* placement, uint64_t bytes) {
    if (!ba_charge_counter(&placement->used, bytes, atomic_load_explicit(&placement->budget, memory_order_relaxed))) return BA_E_BUDGET;
    if ((placement->descriptor.flags & BA_PLACEMENT_OS_PAGES) && !ba_charge_counter(&g_os_live_bytes, bytes, g_os_budget)) {
        atomic_fetch_sub_explicit(&placement->used, bytes, memory_order_relaxed);
        return BA_E_BUDGET;
    }
    return BA_OK;
}
/** --------------------------------------------------------------------------------------------------------- Uncharge
 * @brief Returns capacity after allocation failure or deallocation.
 */
static void ba_uncharge(ba_placement_t* placement, uint64_t bytes) {
    atomic_fetch_sub_explicit(&placement->used, bytes, memory_order_relaxed);
    if (placement->descriptor.flags & BA_PLACEMENT_OS_PAGES) atomic_fetch_sub_explicit(&g_os_live_bytes, bytes, memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Slab Build
 * @brief Obtains a budgeted zeroed slab and publishes its open pin.
 */
ba_status_t ba_slab_build(ba_placement_t* placement, ba_slab_t** out) {
    const ba_tunables_t policy = *ba_policy();
    const uint64_t begin = ba_os_now_ns();
    ba_status_t status = ba_charge(placement, placement->slab_bytes);
    if (status != BA_OK) return status;
    ba_slab_t* slab = ba_header_pop();
    if (!slab) { ba_uncharge(placement, placement->slab_bytes); return BA_E_ALLOC; }
    slab->placement = placement->type;
    slab->bytes = placement->slab_bytes;
    slab->context = placement->context;
    if (placement->descriptor.flags & BA_PLACEMENT_OS_PAGES) {
        slab->base = ba_os_map(slab->bytes, g_sys.granule, (placement->descriptor.flags & BA_PLACEMENT_LARGE_PAGES) && g_sys.large_page);
        slab->handle = &((ba_slab_record_t*)slab)->handle;
        *slab->handle = (ba_handle_t){slab->base, slab->context};
    } else {
        slab->handle = placement->descriptor.alloc(slab->bytes, slab->context);
        slab->base = slab->handle ? placement->descriptor.host_ptr(slab->handle) : NULL;
    }
    if (!slab->base) {
        if (slab->handle && !(placement->descriptor.flags & BA_PLACEMENT_OS_PAGES)) {
            placement->descriptor.free(slab->handle, slab->context);
            if (placement->descriptor.destroy_handle) placement->descriptor.destroy_handle(slab->handle);
        }
        ba_header_push(slab); ba_uncharge(placement, placement->slab_bytes); return BA_E_ALLOC;
    }
    atomic_store_explicit(&slab->bump, 0, memory_order_relaxed);
    atomic_store_explicit(&slab->live_plates, 1, memory_order_relaxed);
    slab->opened_ns = ba_os_now_ns();
    atomic_fetch_add_explicit(&placement->slab_allocated, slab->bytes, memory_order_relaxed);
    const uint64_t sample = slab->opened_ns - begin;
    uint64_t previous = atomic_load_explicit(&placement->build_ewma, memory_order_relaxed);
    const uint64_t updated = previous ? previous - (previous >> policy.ewma_shift) + (sample >> policy.ewma_shift) : sample;
    atomic_store_explicit(&placement->build_ewma, updated, memory_order_relaxed);
    *out = slab;
    return BA_OK;
}
/** --------------------------------------------------------------------------------------------------------- Slab Release
 * @brief Returns one slab allocation and recycles its header.
 */
void ba_slab_release(ba_slab_t* slab) {
    ba_placement_t* placement = &g_placements[slab->placement];
    const uint64_t bytes = slab->bytes;
    if (placement->descriptor.flags & BA_PLACEMENT_OS_PAGES) ba_os_unmap(slab->base, bytes);
    else {
        placement->descriptor.free(slab->handle, slab->context);
        if (placement->descriptor.destroy_handle) placement->descriptor.destroy_handle(slab->handle);
    }
    atomic_fetch_add_explicit(&placement->slab_freed, bytes, memory_order_release);
    ba_uncharge(placement, bytes);
    ba_header_push(slab);
}
/** --------------------------------------------------------------------------------------------------------- Slot Pop
 * @brief Claims a tagged free-list slot or a fresh registry entry.
 */
static uint32_t ba_slot_pop(void) {
    uint64_t head = atomic_load_explicit(&g_slot_head, memory_order_acquire);
    for (;;) {
        const uint32_t slot = (uint32_t)head;
        if (!slot) {
            const uint32_t fresh = atomic_fetch_add_explicit(&g_slot_fresh, 1, memory_order_relaxed);
            if (fresh >= BA_PLATE_COUNT) return 0;
            const uint32_t columns = BA_PLATE_COUNT / g_slot_stride;
            return (fresh % columns) * g_slot_stride + fresh / columns;
        }
        const uint32_t next = atomic_load_explicit(&g_plates[slot].next_free, memory_order_relaxed);
        const uint64_t replacement = (head & 0xffffffff00000000ull) | next;
        if (atomic_compare_exchange_weak_explicit(&g_slot_head, &head, replacement,
            memory_order_acq_rel, memory_order_acquire)) return slot;
    }
}
/** --------------------------------------------------------------------------------------------------------- Slot Push
 * @brief Publishes a retired slot with a new ABA tag.
 */
static void ba_slot_push(uint32_t slot) {
    uint64_t head = atomic_load_explicit(&g_slot_head, memory_order_acquire);
    do {
        atomic_store_explicit(&g_plates[slot].next_free, (uint32_t)head, memory_order_relaxed);
    } while (!atomic_compare_exchange_weak_explicit(&g_slot_head, &head,
        ((head + (1ull << 32)) & 0xffffffff00000000ull) | slot, memory_order_acq_rel, memory_order_acquire));
}
/** --------------------------------------------------------------------------------------------------------- Plate Dead
 * @brief Identifies the unique reference transition that retires a sealed plate.
 */
static int ba_plate_dead(uint64_t state) {
    return (state & BA_SEALED_BIT) && (uint32_t)state == (uint32_t)((state >> 32) & 0x7fffffffu) + BA_BALANCE_BIAS;
}
/** --------------------------------------------------------------------------------------------------------- Plate Retire
 * @brief Releases backing ownership when a sealed plate loses its final reference.
 */
void ba_novel_dispose(ba_placement_t* placement, ba_handle_t* handle, uint8_t* base, uint64_t bytes) {
    if (placement->descriptor.flags & BA_PLACEMENT_OS_PAGES) {
        ba_os_unmap(base, bytes);
        ba_header_push((ba_slab_t*)((uint8_t*)handle - offsetof(ba_slab_record_t, handle)));
    } else {
        placement->descriptor.free(handle, placement->context);
        if (placement->descriptor.destroy_handle) placement->descriptor.destroy_handle(handle);
    }
    ba_uncharge(placement, bytes);
    atomic_fetch_add_explicit(&placement->novel_freed, bytes, memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Novel Release
 * @brief Releases a novel allocation and its retired registry entry.
 */
void ba_novel_release(ba_plate_t* plate) {
    ba_novel_dispose(&g_placements[plate->placement], plate->handle, plate->base, plate->bytes);
    ba_slot_recycle(plate);
}
/** --------------------------------------------------------------------------------------------------------- Retire Slab
 * @brief Transfers final slab ownership to the worker or releases after shutdown.
 */
static void ba_retire_slab(ba_slab_t* slab) {
    if (atomic_load_explicit(&g_closed, memory_order_acquire)) ba_slab_release(slab);
    else ba_enqueue((ba_order_t){BA_ORDER_RETIRE_SLAB, slab->placement, (uint64_t)(uintptr_t)slab});
}
/** --------------------------------------------------------------------------------------------------------- Plate Retire
 * @brief Transfers dead novel plates and slabs to the allocator worker.
 */
static BA_COLD void ba_plate_retire(ba_plate_t* plate) {
    if (plate->kind == BA_PLATE_NOVEL) {
        if (atomic_load_explicit(&g_closed, memory_order_acquire)) ba_novel_release(plate);
        else ba_enqueue((ba_order_t){BA_ORDER_RETIRE_NOVEL, plate->placement, (uint64_t)(plate - g_plates)});
        return;
    }
    if (atomic_fetch_sub_explicit(&plate->slab->live_plates, 1, memory_order_acq_rel) == 1) ba_retire_slab(plate->slab);
    plate->kind = BA_PLATE_FREE;
    ba_slot_push((uint32_t)(plate - g_plates));
}
/** --------------------------------------------------------------------------------------------------------- Geometry
 * @brief Computes a thread plate size from observed claim sizes and placement policy.
 */
static uint64_t ba_plate_size(ba_placement_t* placement, uint64_t mean) {
    const ba_tunables_t policy = *ba_policy();
    const uint64_t ceiling = ba_pow2_floor(atomic_load_explicit(&placement->budget, memory_order_relaxed) / ((uint64_t)g_sys.hw_threads * 64));
    uint64_t bytes = ba_min(ba_pow2_ceil(mean * policy.claims_per_plate), ceiling);
    if (bytes < g_sys.granule) bytes = g_sys.granule;
    if (placement->descriptor.slab_bytes) bytes = ba_min(bytes, placement->slab_bytes / 8);
    return bytes & ~(uint64_t)63;
}
/** --------------------------------------------------------------------------------------------------------- Seal
 * @brief Publishes the owner's final issued count and updates claim geometry.
 */
static void ba_seal(uint32_t type) {
    const ba_tunables_t policy = *ba_policy();
    ba_tls_plate_t* local = &g_tls[type];
    ba_plate_t* plate = &g_plates[local->slot];
    const uint64_t issued = local->issued;
    const uint64_t used = (uint64_t)(local->cursor - plate->base);
    *local = (ba_tls_plate_t){0};
    ba_placement_t* placement = &g_placements[type];
    if (issued) {
        uint32_t previous = atomic_load_explicit(&placement->claim_ewma, memory_order_relaxed);
        const uint32_t mean = previous - (previous >> policy.ewma_shift) + (uint32_t)((used / issued) >> policy.ewma_shift);
        atomic_store_explicit(&placement->claim_ewma, mean, memory_order_relaxed);
        atomic_store_explicit(&placement->plate_bytes, ba_plate_size(placement, mean), memory_order_relaxed);
    }
    const uint64_t addend = BA_SEALED_BIT | (issued << 32);
    const uint64_t state = atomic_fetch_add_explicit(&plate->state, addend, memory_order_acq_rel) + addend;
    if (ba_plate_dead(state)) ba_plate_retire(plate);
}
/** --------------------------------------------------------------------------------------------------------- Thread Exit
 * @brief Seals all plates owned by the exiting thread.
 */
static void ba_thread_exit(void* ignored) {
    (void)ignored;
    for (uint32_t type = 0; type < BA_MAX_PLACEMENTS; ++type) if (g_tls[type].limit) ba_seal(type);
}
/** --------------------------------------------------------------------------------------------------------- Carve
 * @brief Claims exclusive current-slab access until the new plate owns a slab reference.
 */
static ba_status_t ba_carve(ba_placement_t* placement, uint64_t bytes, uint32_t kind, uint32_t* out) {
    const ba_tunables_t policy = *ba_policy();
    const uint32_t slot = ba_slot_pop();
    if (!slot) return BA_E_SLOTS;
    for (;;) {
        ba_slab_t* slab = atomic_load_explicit(&placement->current, memory_order_acquire);
        if (slab == BA_ADVANCING) { ba_os_yield(); continue; }
        if (!atomic_compare_exchange_weak_explicit(&placement->current, &slab, BA_ADVANCING,
            memory_order_acq_rel, memory_order_acquire)) continue;
        const uint64_t offset = atomic_fetch_add_explicit(&slab->bump, bytes, memory_order_relaxed);
        if (offset <= slab->bytes && bytes <= slab->bytes - offset) {
            atomic_fetch_add_explicit(&slab->live_plates, 1, memory_order_relaxed);
            ba_plate_t* plate = &g_plates[slot];
            plate->slab = slab; plate->handle = slab->handle; plate->base = slab->base + offset;
            plate->bytes = bytes; plate->placement = placement->type; plate->kind = kind;
            const uint64_t state = BA_BALANCE_BIAS | (kind == BA_PLATE_DIRECT ? BA_SEALED_BIT | (1ull << 32) : 0);
            atomic_store_explicit(&plate->state, state, memory_order_release);
            atomic_fetch_add_explicit(&placement->carved, bytes, memory_order_relaxed);
            atomic_store_explicit(&placement->current, slab, memory_order_release);
            *out = slot;
            return BA_OK;
        }
        ba_slab_t* next = ba_runway_pop(placement);
        while (!next && atomic_load_explicit(&placement->runway_building, memory_order_acquire)) {
            ba_os_yield();
            next = ba_runway_pop(placement);
        }
        if (!next) next = ba_runway_pop(placement);
        ba_status_t status = BA_OK;
        if (!next) {
            atomic_fetch_add_explicit(&placement->runway_misses, 1, memory_order_relaxed);
            status = ba_slab_build(placement, &next);
        }
        if (status == BA_OK) {
            const uint64_t now = ba_os_now_ns();
            const uint64_t elapsed = now - slab->opened_ns;
            const double sample = elapsed ? (double)slab->bytes / elapsed : 0;
            const double previous = atomic_load_explicit(&placement->consume_rate, memory_order_relaxed);
            const double divisor = (double)(1ull << policy.ewma_shift);
            atomic_store_explicit(&placement->consume_rate, previous ? previous + (sample - previous) / divisor : sample, memory_order_relaxed);
            next->opened_ns = now;
        }
        atomic_store_explicit(&placement->current, status == BA_OK ? next : slab, memory_order_release);
        if (status != BA_OK) { ba_slot_push(slot); return status; }
        if (atomic_fetch_sub_explicit(&slab->live_plates, 1, memory_order_acq_rel) == 1) ba_retire_slab(slab);
        ba_enqueue((ba_order_t){BA_ORDER_REPLENISH, placement->type, 0});
    }
}
/** --------------------------------------------------------------------------------------------------------- Novel Claim
 * @brief Allocates a dedicated region on the caller thread.
 */
static ba_status_t ba_claim_novel(ba_placement_t* placement, size_t bytes, size_t rounded, ba_slice_t* out) {
    uint64_t capacity = (placement->descriptor.flags & BA_PLACEMENT_OS_PAGES) ? ba_round(rounded, g_sys.granule) : rounded;
    if (placement->descriptor.novel_cache_bytes && capacity <= (1ull << 40)) {
        capacity = ba_pow2_ceil(capacity < 65536 ? 65536 : capacity);
        ba_handle_t* handle;
        uint8_t* base;
        const uint32_t slot = ba_slot_pop();
        if (!slot) return BA_E_SLOTS;
        if (ba_novel_cache_pop(placement, capacity, &handle, &base)) {
            ba_plate_t* plate = &g_plates[slot];
            plate->slab = NULL; plate->placement = placement->type; plate->kind = BA_PLATE_NOVEL;
            plate->bytes = capacity; plate->handle = handle; plate->base = base;
            atomic_store_explicit(&plate->state, BA_SEALED_BIT | (1ull << 32) | BA_BALANCE_BIAS, memory_order_release);
            *out = (ba_slice_t){((uint64_t)bytes << BA_SLOT_BITS) | slot, base};
            return BA_OK;
        }
        ba_slot_push(slot);
    }
    ba_status_t status = ba_charge(placement, capacity);
    if (status != BA_OK) return status;
    const uint32_t slot = ba_slot_pop();
    if (!slot) { ba_uncharge(placement, capacity); return BA_E_SLOTS; }
    ba_plate_t* plate = &g_plates[slot];
    plate->slab = NULL; plate->placement = placement->type; plate->kind = BA_PLATE_NOVEL; plate->bytes = capacity;
    if (placement->descriptor.flags & BA_PLACEMENT_OS_PAGES) {
        ba_slab_t* record = ba_header_pop();
        if (!record) { ba_slot_push(slot); ba_uncharge(placement, capacity); return BA_E_ALLOC; }
        plate->handle = &((ba_slab_record_t*)record)->handle;
        plate->base = ba_os_map(capacity, g_sys.granule, (placement->descriptor.flags & BA_PLACEMENT_LARGE_PAGES) && g_sys.large_page);
        *plate->handle = (ba_handle_t){plate->base, placement->context};
        if (!plate->base) ba_header_push(record);
    } else {
        plate->handle = placement->descriptor.alloc(capacity, placement->context);
        plate->base = plate->handle ? placement->descriptor.host_ptr(plate->handle) : NULL;
    }
    if (!plate->base) {
        if (plate->handle && !(placement->descriptor.flags & BA_PLACEMENT_OS_PAGES)) {
            placement->descriptor.free(plate->handle, placement->context);
            if (placement->descriptor.destroy_handle) placement->descriptor.destroy_handle(plate->handle);
        }
        ba_slot_push(slot); ba_uncharge(placement, capacity); return BA_E_ALLOC;
    }
    atomic_fetch_add_explicit(&placement->novel_allocated, capacity, memory_order_relaxed);
    atomic_store_explicit(&plate->state, BA_SEALED_BIT | (1ull << 32) | BA_BALANCE_BIAS, memory_order_release);
    *out = (ba_slice_t){((uint64_t)bytes << BA_SLOT_BITS) | slot, plate->base};
    return BA_OK;
}
/** --------------------------------------------------------------------------------------------------------- Claim Slow
 * @brief Routes oversized claims and refills exhausted thread plates.
 */
static BA_COLD ba_status_t ba_claim_slow(uint32_t type, size_t bytes, size_t rounded, uint32_t flags, ba_slice_t* out) {
    if (atomic_load_explicit(&g_closed, memory_order_acquire)) return BA_E_CLOSED;
    if (type >= atomic_load_explicit(&g_placement_count, memory_order_acquire)) return BA_E_PLACEMENT;
    if (bytes > (UINT64_MAX >> BA_SLOT_BITS) || rounded < bytes) return BA_E_RANGE;
    if (!bytes) { *out = (ba_slice_t){BA_NULL_META, NULL}; return BA_OK; }
    ba_placement_t* placement = &g_placements[type];
    const uint32_t fault = atomic_load_explicit(&placement->fault, memory_order_relaxed);
    if (fault) return (ba_status_t)fault;
    if ((flags & BA_CLAIM_NOVEL) || rounded > placement->slab_bytes) return ba_claim_novel(placement, bytes, rounded, out);
    const uint64_t plate_bytes = atomic_load_explicit(&placement->plate_bytes, memory_order_relaxed);
    uint32_t slot;
    if (rounded > plate_bytes) {
        const ba_status_t status = ba_carve(placement, rounded, BA_PLATE_DIRECT, &slot);
        if (status == BA_OK) *out = (ba_slice_t){((uint64_t)bytes << BA_SLOT_BITS) | slot, g_plates[slot].base};
        return status;
    }
    if (g_tls[type].limit) ba_seal(type);
    const ba_status_t status = ba_carve(placement, plate_bytes, BA_PLATE_THREAD, &slot);
    if (status != BA_OK) return status;
    ba_os_tls_set(g_tls);
    g_tls[type] = (ba_tls_plate_t){g_plates[slot].base + rounded, g_plates[slot].base + plate_bytes, slot, 1};
    *out = (ba_slice_t){((uint64_t)bytes << BA_SLOT_BITS) | slot, g_plates[slot].base};
    return BA_OK;
}
/** --------------------------------------------------------------------------------------------------------- Claim
 * @brief Bumps within the owner thread's plate without atomic operations.
 */
ba_status_t ba_claim(uint32_t type, size_t bytes, uint32_t flags, ba_slice_t* out) {
    ba_tls_plate_t* plate = &g_tls[type];
    const size_t rounded = (bytes + 63u) & ~(size_t)63u;
    if (flags | ((size_t)((uintptr_t)plate->limit - (uintptr_t)plate->cursor) < rounded)) return ba_claim_slow(type, bytes, rounded, flags, out);
    out->ptr = plate->cursor;
    out->meta = ((uint64_t)bytes << BA_SLOT_BITS) | plate->slot;
    plate->cursor += rounded;
    plate->issued += 1;
    return BA_OK;
}
/** --------------------------------------------------------------------------------------------------------- Retain
 * @brief Adds one outstanding reference to a live plate.
 */
void ba_retain(const ba_slice_t* slice) {
    atomic_fetch_sub_explicit(&g_plates[slice->meta & BA_SLOT_MASK].state, 1, memory_order_acq_rel);
}
/** --------------------------------------------------------------------------------------------------------- Release
 * @brief Drops a reference and nulls the released slice.
 */
void ba_release(ba_slice_t* slice) {
    if (slice->meta == BA_NULL_META) return;
    ba_plate_t* plate = &g_plates[slice->meta & BA_SLOT_MASK];
    const uint64_t state = atomic_fetch_add_explicit(&plate->state, 1, memory_order_acq_rel) + 1;
    *slice = (ba_slice_t){BA_NULL_META, NULL};
    if (ba_plate_dead(state)) ba_plate_retire(plate);
}
/** --------------------------------------------------------------------------------------------------------- View
 * @brief Retains a checked subrange of a slice.
 */
ba_status_t ba_view(const ba_slice_t* slice, size_t offset, size_t length, ba_slice_t* out) {
    if (!length || slice->meta == BA_NULL_META) { *out = (ba_slice_t){BA_NULL_META, NULL}; return BA_OK; }
    const uint64_t bytes = slice->meta >> BA_SLOT_BITS;
    if (offset > bytes) return BA_E_RANGE;
    if (length == SIZE_MAX) length = bytes - offset;
    if (length > bytes - offset) return BA_E_RANGE;
    if (!length) { *out = (ba_slice_t){BA_NULL_META, NULL}; return BA_OK; }
    ba_retain(slice);
    *out = (ba_slice_t){((uint64_t)length << BA_SLOT_BITS) | (slice->meta & BA_SLOT_MASK), (uint8_t*)slice->ptr + offset};
    return BA_OK;
}
/** --------------------------------------------------------------------------------------------------------- Slice Placement
 * @brief Resolves the owning placement of a live slice.
 */
uint32_t ba_slice_placement(const ba_slice_t* slice) { return g_plates[slice->meta & BA_SLOT_MASK].placement; }
/** --------------------------------------------------------------------------------------------------------- Slice Handle
 * @brief Resolves the substrate handle of a live slice.
 */
ba_handle_t* ba_slice_handle(const ba_slice_t* slice) { return g_plates[slice->meta & BA_SLOT_MASK].handle; }
/** --------------------------------------------------------------------------------------------------------- Register Placement
 * @brief Validates a descriptor and publishes its completed initial slab.
 */
ba_status_t ba_placement_register(const ba_placement_desc_t* descriptor, uint32_t* out) {
    const ba_tunables_t policy = *ba_policy();
    if (descriptor->struct_size != sizeof(*descriptor)) return BA_E_PLACEMENT;
    if (descriptor->base_alignment < 64 || (descriptor->base_alignment & (descriptor->base_alignment - 1))) return BA_E_ALIGNMENT;
    if (!(descriptor->flags & BA_PLACEMENT_OS_PAGES) && (!descriptor->alloc || !descriptor->free || !descriptor->host_ptr)) return BA_E_PLACEMENT;
    ba_lock(&g_registration_lock);
    const uint32_t type = atomic_load_explicit(&g_placement_count, memory_order_relaxed);
    if (atomic_load_explicit(&g_closed, memory_order_acquire) || type == BA_MAX_PLACEMENTS) {
        ba_unlock(&g_registration_lock); return type == BA_MAX_PLACEMENTS ? BA_E_PLACEMENT : BA_E_CLOSED;
    }
    ba_placement_t* placement = &g_placements[type];
    placement->descriptor = *descriptor;
    atomic_flag_clear(&placement->trim_lock);
    placement->type = type;
    placement->context = descriptor->context ? descriptor->context() : NULL;
    uint64_t budget = descriptor->budget_bytes;
    if (!budget) budget = (descriptor->flags & BA_PLACEMENT_OS_PAGES) ? g_os_budget :
        descriptor->query_available ? descriptor->query_available(placement->context) : UINT64_MAX;
    atomic_store_explicit(&placement->budget, budget, memory_order_relaxed);
    placement->slab_bytes = descriptor->slab_bytes ? ba_round(descriptor->slab_bytes, g_sys.granule) : 0;
    uint64_t plate = ba_plate_size(placement, g_sys.page);
    if (!descriptor->slab_bytes) {
        const uint64_t ceiling = ba_pow2_floor(budget / policy.slab_budget_divisor);
        const uint64_t floor = 16 * g_sys.granule;
        const uint64_t desired = ba_pow2_ceil(plate * g_sys.hw_threads * policy.plates_per_thread);
        placement->slab_bytes = ba_min(desired < floor ? floor : desired, ceiling);
    }
    if (!plate || !placement->slab_bytes || placement->slab_bytes > budget || plate > placement->slab_bytes) {
        ba_unlock(&g_registration_lock); return BA_E_BUDGET;
    }
    atomic_store_explicit(&placement->plate_bytes, plate, memory_order_relaxed);
    atomic_store_explicit(&placement->claim_ewma, (uint32_t)g_sys.page, memory_order_relaxed);
    atomic_store_explicit(&placement->runway_target, policy.runway_floor, memory_order_relaxed);
    placement->completion = ba_event_create();
    if (!placement->completion) { ba_unlock(&g_registration_lock); return BA_E_ALLOC; }
    atomic_store_explicit(&placement->completed, 0, memory_order_relaxed);
    ba_enqueue((ba_order_t){BA_ORDER_INIT_PLACEMENT, type, 0});
    while (!atomic_load_explicit(&placement->completed, memory_order_acquire)) ba_event_wait(placement->completion, 1000000000);
    const ba_status_t status = placement->status;
    ba_event_destroy(placement->completion);
    placement->completion = NULL;
    if (status == BA_OK) {
        atomic_store_explicit(&g_placement_count, type + 1, memory_order_release);
        if (!type || (descriptor->flags & BA_PLACEMENT_DEFAULT)) atomic_store_explicit(&g_default, type, memory_order_release);
        *out = type;
    }
    ba_unlock(&g_registration_lock);
    return status;
}
/** --------------------------------------------------------------------------------------------------------- Initialize
 * @brief Probes runtime policy and registers the two built-in placements exactly once.
 */
void ba_init(void) {
    const ba_tunables_t policy = *ba_policy();
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&g_initialized, &expected, 1, memory_order_acq_rel, memory_order_acquire)) {
        while (atomic_load_explicit(&g_initialized, memory_order_acquire) != 2) ba_os_yield();
        return;
    }
    ba_os_probe(&g_sys);
    g_slot_stride = (g_sys.cache_line + sizeof(ba_plate_t) - 1) / sizeof(ba_plate_t);
    const uint64_t headroom = ba_min(g_sys.available, g_sys.limit > g_sys.rss ? g_sys.limit - g_sys.rss : 0);
    g_os_budget = headroom / policy.headroom_den * policy.headroom_num;
    if (ba_os_tls_key(ba_thread_exit) != 0) abort();
    ba_worker_start();
    for (uint32_t type = 0; type < 2; ++type) {
        ba_placement_desc_t descriptor = {0};
        descriptor.struct_size = sizeof(descriptor);
        descriptor.name = type ? "aligned_heap" : "heap";
        descriptor.base_alignment = (uint32_t)g_sys.granule;
        descriptor.flags = BA_PLACEMENT_OS_PAGES | (type ? BA_PLACEMENT_LARGE_PAGES | BA_PLACEMENT_DEFAULT : 0);
        uint32_t registered;
        if (ba_placement_register(&descriptor, &registered) != BA_OK) abort();
    }
    atomic_store_explicit(&g_initialized, 2, memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Load
 * @brief Initializes built-in placements before application entry.
 */
#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) void (*ba_load)(void) = ba_init;
#else
__attribute__((constructor)) static void ba_load(void) { ba_init(); }
#endif
/** --------------------------------------------------------------------------------------------------------- Placement Count
 * @brief Returns the number of fully registered placements.
 */
uint32_t ba_placement_count(void) { return atomic_load_explicit(&g_placement_count, memory_order_acquire); }
/** --------------------------------------------------------------------------------------------------------- Default Placement
 * @brief Returns the current default identifier.
 */
uint32_t ba_placement_default(void) { return atomic_load_explicit(&g_default, memory_order_acquire); }
/** --------------------------------------------------------------------------------------------------------- Placement Name
 * @brief Returns a registered placement name.
 */
const char* ba_placement_name(uint32_t type) { return g_placements[type].descriptor.name; }
/** --------------------------------------------------------------------------------------------------------- Find Placement
 * @brief Resolves a registered name to its stable identifier.
 */
int ba_placement_find(const char* name) {
    for (uint32_t type = 0; type < ba_placement_count(); ++type) if (!strcmp(name, ba_placement_name(type))) return (int)type;
    return -1;
}
/** --------------------------------------------------------------------------------------------------------- Describe Placement
 * @brief Copies the registered descriptor and resolved slab size.
 */
void ba_placement_describe(uint32_t type, ba_placement_desc_t* out) {
    *out = g_placements[type].descriptor;
    out->slab_bytes = g_placements[type].slab_bytes;
}
/** --------------------------------------------------------------------------------------------------------- Statistics
 * @brief Reads placement allocation counters and current resource policy.
 */
void ba_stats(uint32_t type, ba_stats_t* out) {
    ba_placement_t* placement = &g_placements[type];
    *out = (ba_stats_t){0};
    out->slab_bytes_freed = atomic_load_explicit(&placement->slab_freed, memory_order_acquire);
    out->slab_bytes_allocated = atomic_load_explicit(&placement->slab_allocated, memory_order_relaxed);
    out->novel_bytes_freed = atomic_load_explicit(&placement->novel_freed, memory_order_acquire);
    out->novel_bytes_allocated = atomic_load_explicit(&placement->novel_allocated, memory_order_relaxed);
    out->reserved_bytes = atomic_load_explicit(&placement->reserved, memory_order_relaxed);
    out->novel_cache_bytes = ba_cache_available(placement);
    out->plate_bytes_carved = atomic_load_explicit(&placement->carved, memory_order_relaxed);
    out->budget_bytes = atomic_load_explicit(&placement->budget, memory_order_relaxed);
    out->slab_bytes = placement->slab_bytes;
    out->plate_bytes = atomic_load_explicit(&placement->plate_bytes, memory_order_relaxed);
    out->runway_target = atomic_load_explicit(&placement->runway_target, memory_order_relaxed);
    out->runway_misses = atomic_load_explicit(&placement->runway_misses, memory_order_relaxed);
    out->claim_ewma = atomic_load_explicit(&placement->claim_ewma, memory_order_relaxed);
    out->build_ns_ewma = atomic_load_explicit(&placement->build_ewma, memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Total Statistics
 * @brief Sums completed allocation counters across all placements.
 */
void ba_stats_total(ba_stats_t* out) {
    *out = (ba_stats_t){0};
    for (uint32_t type = 0; type < ba_placement_count(); ++type) {
        ba_stats_t sample; ba_stats(type, &sample);
        out->slab_bytes_allocated += sample.slab_bytes_allocated;
        out->slab_bytes_freed += sample.slab_bytes_freed;
        out->novel_bytes_allocated += sample.novel_bytes_allocated;
        out->novel_bytes_freed += sample.novel_bytes_freed;
        out->reserved_bytes += sample.reserved_bytes;
        out->plate_bytes_carved += sample.plate_bytes_carved;
        out->novel_cache_bytes += sample.novel_cache_bytes;
    }
}
/** --------------------------------------------------------------------------------------------------------- System Information
 * @brief Returns a fresh OS capacity snapshot.
 */
void ba_sysinfo(ba_sysinfo_t* out) { ba_os_probe(out); }
/** --------------------------------------------------------------------------------------------------------- Budget Set
 * @brief Sets the maximum capacity charged to a placement.
 */
void ba_budget_set(uint32_t type, uint64_t bytes) {
    atomic_store_explicit(&g_placements[type].budget, bytes, memory_order_relaxed);
    if (!atomic_load_explicit(&g_closed, memory_order_acquire)) ba_enqueue((ba_order_t){BA_ORDER_REPLENISH, type, 0});
}
/** --------------------------------------------------------------------------------------------------------- Get Tunables
 * @brief Copies arena policy settings.
 */
void ba_tunables_get(ba_tunables_t* out) {
    ba_lock(&g_policy_lock);
    *out = g_tunables;
    ba_unlock(&g_policy_lock);
}
/** --------------------------------------------------------------------------------------------------------- Set Tunables
 * @brief Replaces policy settings while the caller holds application quiescence.
 */
void ba_tunables_set(const ba_tunables_t* settings) {
    if (!settings->headroom_den || !settings->claims_per_plate || !settings->plates_per_thread ||
        !settings->slab_budget_divisor || settings->runway_floor > 64 ||
        !settings->pressure_poll_ms || settings->ewma_shift > 31) abort();
    ba_lock(&g_policy_lock);
    g_tunables = *settings;
    ba_unlock(&g_policy_lock);
}
/** --------------------------------------------------------------------------------------------------------- Pressure
 * @brief Returns the current pressure level.
 */
ba_pressure_t ba_pressure(void) { return ba_os_pressure(); }
/** --------------------------------------------------------------------------------------------------------- Trim
 * @brief Requests release of idle placement reserves.
 */
void ba_trim(uint32_t type) { if (!atomic_load_explicit(&g_closed, memory_order_acquire)) ba_worker_trim(type); }
/** --------------------------------------------------------------------------------------------------------- Trim All
 * @brief Requests release of idle reserves in all placements.
 */
void ba_trim_all(void) { for (uint32_t type = 0; type < ba_placement_count(); ++type) ba_trim(type); }
/** --------------------------------------------------------------------------------------------------------- Shutdown
 * @brief Closes the allocator worker.
 */
void ba_shutdown(void) {
    if (atomic_exchange_explicit(&g_closed, 1, memory_order_acq_rel)) return;
    ba_thread_exit(NULL);
    ba_os_tls_set(NULL);
    ba_worker_stop();
}
/** --------------------------------------------------------------------------------------------------------- Placement Runtime
 * @brief Resolves a worker-owned placement record.
 */
ba_placement_t* ba_placement_at(uint32_t type) { return &g_placements[type]; }
/** --------------------------------------------------------------------------------------------------------- Plate Runtime
 * @brief Resolves a worker-owned retired plate.
 */
ba_plate_t* ba_plate_at(uint32_t slot) { return &g_plates[slot]; }
/** --------------------------------------------------------------------------------------------------------- Policy
 * @brief Returns the arena's quiescently configured policy.
 */
const ba_tunables_t* ba_policy(void) {
    static _Thread_local ba_tunables_t snapshot;
    ba_tunables_get(&snapshot);
    return &snapshot;
}
/** --------------------------------------------------------------------------------------------------------- Machine
 * @brief Returns immutable startup hardware geometry.
 */
const ba_sysinfo_t* ba_machine(void) { return &g_sys; }
/** --------------------------------------------------------------------------------------------------------- Recycle Slot
 * @brief Returns a cached novel plate's slot to the registry free list.
 */
void ba_slot_recycle(ba_plate_t* plate) {
    plate->kind = BA_PLATE_FREE;
    ba_slot_push((uint32_t)(plate - g_plates));
}
/** --------------------------------------------------------------------------------------------------------- Status Name
 * @brief Returns the stable symbolic name of a core status.
 */
const char* ba_status_name(ba_status_t status) {
    static const char* const names[] = {"BA_OK", "BA_E_BUDGET", "BA_E_SLOTS", "BA_E_PLACEMENT", "BA_E_ALIGNMENT", "BA_E_RANGE", "BA_E_ALLOC", "BA_E_CLOSED"};
    return names[status];
}
