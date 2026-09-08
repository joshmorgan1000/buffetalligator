/** --------------------------------------------------------------------------------------------------------- Arena Worker
 * @file ba_worker.c
 * @brief Owns background allocation, reclamation, and prepared slab queues.
 */
#define BA_INTERNAL
#include "ba_core.h"
#include <stdlib.h>
#include <string.h>

#define BA_ORDER_CAPACITY 4096u
/** --------------------------------------------------------------------------------------------------------- Order Cell
 * @brief Publishes one worker order through a sequence-numbered ring cell.
 */
typedef struct ba_order_cell {
    _Atomic uint64_t sequence; ///< Producer and consumer generation.
    ba_order_t order; ///< Published work item.
} ba_order_cell_t;
static ba_order_cell_t g_orders[BA_ORDER_CAPACITY];
static BA_ALIGN(64) _Atomic uint64_t g_enqueue_position;
static BA_ALIGN(64) uint64_t g_dequeue_position;
static _Atomic int g_sleeping;
static _Atomic int g_stopped;
static ba_event_t* g_wake;
static ba_event_t* g_completion;
static ba_pressure_t g_pressure;
/** --------------------------------------------------------------------------------------------------------- Enqueue
 * @brief Publishes an order and wakes a sleeping worker.
 */
void ba_enqueue(ba_order_t order) {
    const uint64_t ticket = atomic_fetch_add_explicit(&g_enqueue_position, 1, memory_order_relaxed);
    ba_order_cell_t* cell = &g_orders[ticket & (BA_ORDER_CAPACITY - 1)];
    while (atomic_load_explicit(&cell->sequence, memory_order_acquire) != ticket) ba_os_yield();
    cell->order = order;
    atomic_store_explicit(&cell->sequence, ticket + 1, memory_order_seq_cst);
    if (atomic_load_explicit(&g_sleeping, memory_order_seq_cst)) ba_event_signal(g_wake);
}
/** --------------------------------------------------------------------------------------------------------- Dequeue
 * @brief Consumes a ready order without reserving an empty ring cell.
 */
static int ba_dequeue(ba_order_t* out) {
    ba_order_cell_t* cell = &g_orders[g_dequeue_position & (BA_ORDER_CAPACITY - 1)];
    if (atomic_load_explicit(&cell->sequence, memory_order_seq_cst) != g_dequeue_position + 1) return 0;
    *out = cell->order;
    atomic_store_explicit(&cell->sequence, g_dequeue_position + BA_ORDER_CAPACITY, memory_order_release);
    ++g_dequeue_position;
    return 1;
}
/** --------------------------------------------------------------------------------------------------------- Runway Pop
 * @brief Transfers one prepared slab to the exclusive current-slab consumer.
 */
ba_slab_t* ba_runway_pop(ba_placement_t* placement) {
    const uint32_t head = atomic_load_explicit(&placement->runway_head, memory_order_relaxed);
    if (head == atomic_load_explicit(&placement->runway_tail, memory_order_acquire)) return NULL;
    ba_slab_t* slab = placement->runway[head & 63u];
    atomic_fetch_sub_explicit(&placement->reserved, slab->bytes, memory_order_relaxed);
    atomic_store_explicit(&placement->runway_head, head + 1, memory_order_release);
    slab->opened_ns = ba_os_now_ns();
    return slab;
}
/** --------------------------------------------------------------------------------------------------------- Runway Push
 * @brief Publishes a completed slab into the worker's prepared queue.
 */
static void ba_runway_push(ba_placement_t* placement, ba_slab_t* slab) {
    const uint32_t tail = atomic_load_explicit(&placement->runway_tail, memory_order_relaxed);
    placement->runway[tail & 63u] = slab;
    atomic_fetch_add_explicit(&placement->reserved, slab->bytes, memory_order_relaxed);
    atomic_store_explicit(&placement->runway_tail, tail + 1, memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Replenish
 * @brief Builds slabs until the prepared queue meets its policy target.
 */
static void ba_trim_free(ba_placement_t* placement, uint64_t cap) {
    while (placement->free_head && atomic_load_explicit(&placement->reserved, memory_order_relaxed) > cap) {
        ba_slab_t* slab = placement->free_head;
        placement->free_head = slab->next;
        placement->free_bytes -= slab->bytes;
        atomic_fetch_sub_explicit(&placement->reserved, slab->bytes, memory_order_relaxed);
        ba_slab_release(slab);
    }
}
/** --------------------------------------------------------------------------------------------------------- Runway Target
 * @brief Derives prepared depth from consumption, build latency, misses, and budget.
 */
static uint32_t ba_runway_target(ba_placement_t* placement) {
    const ba_tunables_t policy = *ba_policy();
    const double consumed = atomic_load_explicit(&placement->consume_rate, memory_order_relaxed) *
        atomic_load_explicit(&placement->build_ewma, memory_order_relaxed) / placement->slab_bytes;
    uint64_t desired = (uint64_t)consumed + ((double)(uint64_t)consumed < consumed) + policy.runway_floor +
        atomic_load_explicit(&placement->runway_misses, memory_order_relaxed);
    const uint64_t used = atomic_load_explicit(&placement->used, memory_order_relaxed);
    const uint64_t reserve = atomic_load_explicit(&placement->reserved, memory_order_relaxed);
    const uint64_t live = used > reserve ? used - reserve : 0;
    const uint64_t budget = atomic_load_explicit(&placement->budget, memory_order_relaxed);
    const uint64_t capacity = budget > live ? (budget - live) / placement->slab_bytes : 0;
    if (g_pressure == BA_PRESSURE_CRITICAL) desired = policy.runway_floor;
    if (desired > capacity) desired = capacity;
    if (desired > 64) desired = 64;
    atomic_store_explicit(&placement->runway_target, (uint32_t)desired, memory_order_relaxed);
    return (uint32_t)desired;
}
/** --------------------------------------------------------------------------------------------------------- Replenish
 * @brief Reuses zeroed slabs before building new prepared capacity.
 */
static void ba_trim_runway(ba_placement_t* placement, uint32_t target) {
    ba_slab_t* current;
    for (;;) {
        current = atomic_load_explicit(&placement->current, memory_order_acquire);
        if (current == BA_ADVANCING) { ba_os_yield(); continue; }
        if (atomic_compare_exchange_weak_explicit(&placement->current, &current, BA_ADVANCING,
            memory_order_acq_rel, memory_order_acquire)) break;
    }
    ba_slab_t* retired = NULL;
    while (atomic_load_explicit(&placement->runway_tail, memory_order_relaxed) -
        atomic_load_explicit(&placement->runway_head, memory_order_relaxed) > target) {
        ba_slab_t* slab = ba_runway_pop(placement);
        slab->next = retired;
        retired = slab;
    }
    atomic_store_explicit(&placement->current, current, memory_order_release);
    while (retired) {
        ba_slab_t* slab = retired;
        retired = slab->next;
        ba_slab_release(slab);
    }
}
/** --------------------------------------------------------------------------------------------------------- Replenish
 * @brief Reuses zeroed slabs before building prepared capacity.
 */
static void ba_replenish(ba_placement_t* placement) {
    if (placement->trimmed) return;
    const uint32_t target = ba_runway_target(placement);
    ba_trim_runway(placement, target);
    while (atomic_load_explicit(&placement->runway_tail, memory_order_relaxed) -
        atomic_load_explicit(&placement->runway_head, memory_order_acquire) < target) {
        ba_slab_t* current;
        for (;;) {
            current = atomic_load_explicit(&placement->current, memory_order_acquire);
            if (current == BA_ADVANCING) { ba_os_yield(); continue; }
            if (atomic_compare_exchange_weak_explicit(&placement->current, &current, BA_ADVANCING,
                memory_order_acq_rel, memory_order_acquire)) break;
        }
        atomic_store_explicit(&placement->runway_building, 1, memory_order_release);
        atomic_store_explicit(&placement->current, current, memory_order_release);
        ba_slab_t* slab;
        if (placement->free_head) {
            slab = placement->free_head;
            placement->free_head = slab->next;
            placement->free_bytes -= slab->bytes;
            atomic_fetch_sub_explicit(&placement->reserved, slab->bytes, memory_order_relaxed);
            atomic_store_explicit(&slab->live_plates, 1, memory_order_relaxed);
        } else if (ba_slab_build(placement, &slab) != BA_OK) {
            atomic_store_explicit(&placement->runway_building, 0, memory_order_release);
            break;
        }
        ba_runway_push(placement, slab);
        atomic_store_explicit(&placement->runway_building, 0, memory_order_release);
    }
    ba_trim_free(placement, (uint64_t)target * placement->slab_bytes);
}
/** --------------------------------------------------------------------------------------------------------- Rezero
 * @brief Rezeros the touched prefix through the kernel or the placement's zeroing contract.
 */
static int ba_rezero(ba_placement_t* placement, ba_handle_t* handle, uint8_t* base, uint64_t touched) {
    if (placement->descriptor.flags & BA_PLACEMENT_OS_PAGES) {
        const uint64_t page = ba_machine()->page;
        return ba_os_rezero(base, (touched + page - 1) & ~(page - 1));
    }
    if (placement->descriptor.zero) placement->descriptor.zero(handle, 0, touched, placement->context);
    else memset(base, 0, touched);
    return 0;
}
/** --------------------------------------------------------------------------------------------------------- Recycle Slab
 * @brief Rezeros a fully retired slab and returns it to the bounded free list.
 */
static void ba_recycle_slab(ba_slab_t* slab) {
    ba_placement_t* placement = ba_placement_at(slab->placement);
    if (placement->trimmed || g_pressure == BA_PRESSURE_CRITICAL) { ba_slab_release(slab); return; }
    const uint64_t bump = atomic_load_explicit(&slab->bump, memory_order_relaxed);
    if (ba_rezero(placement, slab->handle, slab->base, bump < slab->bytes ? bump : slab->bytes) != 0) {
        atomic_store_explicit(&placement->fault, BA_E_ALLOC, memory_order_release);
        ba_slab_release(slab);
        return;
    }
    atomic_store_explicit(&slab->bump, 0, memory_order_relaxed);
    atomic_store_explicit(&slab->live_plates, 0, memory_order_relaxed);
    slab->next = placement->free_head;
    placement->free_head = slab;
    placement->free_bytes += slab->bytes;
    atomic_fetch_add_explicit(&placement->reserved, slab->bytes, memory_order_relaxed);
    ba_trim_free(placement, (uint64_t)atomic_load_explicit(&placement->runway_target, memory_order_relaxed) * slab->bytes);
}
/** --------------------------------------------------------------------------------------------------------- Novel Cache Cell
 * @brief Publishes one zeroed allocation with its ring generation.
 */
typedef struct ba_cache_cell {
    _Atomic uint64_t sequence; ///< Ring generation.
    ba_handle_t* handle; ///< Cached substrate handle.
    uint8_t* base; ///< Cached host address.
} ba_cache_cell_t;
/** --------------------------------------------------------------------------------------------------------- Novel Class
 * @brief Stores a bounded MPMC cache for one allocation size.
 */
typedef struct ba_cache_class {
    BA_ALIGN(64) _Atomic uint64_t enqueue; ///< Next producer ticket.
    BA_ALIGN(64) _Atomic uint64_t dequeue; ///< Next consumer ticket.
    ba_cache_cell_t cells[16]; ///< Sequence-numbered entries.
} ba_cache_class_t;
/** --------------------------------------------------------------------------------------------------------- Cache Class
 * @brief Resolves a power-of-two capacity to its cache ring.
 */
static uint32_t ba_cache_index(uint64_t bytes) {
    uint32_t index = 0;
    while (bytes > 65536) { bytes >>= 1; ++index; }
    return index;
}
/** --------------------------------------------------------------------------------------------------------- Cache Pop
 * @brief Claims one zeroed cached allocation without blocking on an empty ring.
 */
int ba_novel_cache_pop(ba_placement_t* placement, uint64_t bytes, ba_handle_t** handle, uint8_t** base) {
    ba_cache_class_t* cache = atomic_load_explicit(&placement->novel_cache, memory_order_acquire);
    if (!cache) return 0;
    ba_cache_class_t* ring = &cache[ba_cache_index(bytes)];
    uint64_t ticket = atomic_load_explicit(&ring->dequeue, memory_order_relaxed);
    ba_cache_cell_t* cell;
    for (;;) {
        cell = &ring->cells[ticket & 15u];
        const uint64_t sequence = atomic_load_explicit(&cell->sequence, memory_order_acquire);
        const int64_t difference = (int64_t)(sequence - (ticket + 1));
        if (difference < 0) return 0;
        if (!difference && atomic_compare_exchange_weak_explicit(&ring->dequeue, &ticket, ticket + 1,
            memory_order_relaxed, memory_order_relaxed)) break;
        ticket = atomic_load_explicit(&ring->dequeue, memory_order_relaxed);
    }
    *handle = cell->handle; *base = cell->base;
    atomic_fetch_sub_explicit(&placement->cached_bytes, bytes, memory_order_relaxed);
    atomic_store_explicit(&cell->sequence, ticket + 16, memory_order_release);
    return 1;
}
/** --------------------------------------------------------------------------------------------------------- Cache Push
 * @brief Publishes a zeroed novel allocation within class and byte limits.
 */
static int ba_cache_push(ba_placement_t* placement, ba_plate_t* plate) {
    const uint64_t limit = placement->descriptor.novel_cache_bytes;
    if (!limit || plate->bytes < 65536 || plate->bytes > (1ull << 40) || (plate->bytes & (plate->bytes - 1))) return 0;
    const uint64_t cached = atomic_load_explicit(&placement->cached_bytes, memory_order_relaxed);
    if (plate->bytes > limit || cached > limit - plate->bytes) return 0;
    ba_cache_class_t* cache = atomic_load_explicit(&placement->novel_cache, memory_order_relaxed);
    if (!cache) {
        const uint64_t page = ba_machine()->page;
        const uint64_t bytes = (25 * sizeof(*cache) + page - 1) & ~(page - 1);
        cache = ba_os_map(bytes, page, 0);
        if (!cache) { atomic_store_explicit(&placement->fault, BA_E_ALLOC, memory_order_relaxed); return 0; }
        for (uint32_t index = 0; index < 25; ++index) {
            for (uint64_t slot = 0; slot < 16; ++slot) atomic_init(&cache[index].cells[slot].sequence, slot);
        }
        atomic_store_explicit(&placement->novel_cache, cache, memory_order_release);
    }
    ba_cache_class_t* ring = &cache[ba_cache_index(plate->bytes)];
    const uint64_t ticket = atomic_load_explicit(&ring->enqueue, memory_order_relaxed);
    ba_cache_cell_t* cell = &ring->cells[ticket & 15u];
    if (atomic_load_explicit(&cell->sequence, memory_order_acquire) != ticket) return 0;
    if (ba_rezero(placement, plate->handle, plate->base, plate->bytes) != 0) {
        atomic_store_explicit(&placement->fault, BA_E_ALLOC, memory_order_relaxed);
        return 0;
    }
    atomic_store_explicit(&ring->enqueue, ticket + 1, memory_order_relaxed);
    cell->handle = plate->handle; cell->base = plate->base;
    atomic_fetch_add_explicit(&placement->cached_bytes, plate->bytes, memory_order_relaxed);
    atomic_store_explicit(&cell->sequence, ticket + 1, memory_order_release);
    return 1;
}
/** --------------------------------------------------------------------------------------------------------- Trim Novel Cache
 * @brief Returns cached allocations until the requested byte cap is met.
 */
static void ba_trim_cache(ba_placement_t* placement, uint64_t cap) {
    for (uint32_t index = 25; index-- > 0;) {
        const uint64_t bytes = 65536ull << index;
        ba_handle_t* handle;
        uint8_t* base;
        while (atomic_load_explicit(&placement->cached_bytes, memory_order_relaxed) > cap &&
            ba_novel_cache_pop(placement, bytes, &handle, &base)) {
            ba_novel_dispose(placement, handle, base, bytes);
        }
    }
}
/** --------------------------------------------------------------------------------------------------------- Retire Novel
 * @brief Caches a zeroed allocation or returns it to its placement.
 */
static void ba_retire_novel(ba_plate_t* plate) {
    ba_placement_t* placement = ba_placement_at(plate->placement);
    if (!placement->trimmed && g_pressure != BA_PRESSURE_CRITICAL && ba_cache_push(placement, plate)) {
        ba_slot_recycle(plate);
        if (g_pressure == BA_PRESSURE_WARN) ba_trim_cache(placement, placement->descriptor.novel_cache_bytes / 2);
    } else ba_novel_release(plate);
}
/** --------------------------------------------------------------------------------------------------------- Trim Placement
 * @brief Releases all idle capacity and suppresses periodic reserve rebuilding.
 */
static void ba_trim_placement(ba_placement_t* placement) {
    placement->trimmed = 1;
    ba_trim_runway(placement, 0);
    ba_trim_free(placement, 0);
    ba_trim_cache(placement, 0);
    ba_event_signal(placement->trim_event);
    atomic_store_explicit(&placement->trim_done, 1, memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Initialize Placement
 * @brief Prepares the initial current slab and runway before registration returns.
 */
static void ba_initialize_placement(ba_placement_t* placement) {
    const ba_tunables_t policy = *ba_policy();
    ba_slab_t* current;
    placement->status = ba_slab_build(placement, &current);
    if (placement->status == BA_OK) {
        atomic_store_explicit(&placement->current, current, memory_order_release);
        for (uint32_t index = 0; index < policy.runway_floor; ++index) {
            ba_slab_t* slab;
            placement->status = ba_slab_build(placement, &slab);
            if (placement->status != BA_OK) break;
            ba_runway_push(placement, slab);
        }
        if (placement->status != BA_OK) {
            ba_slab_t* slab;
            while ((slab = ba_runway_pop(placement))) ba_slab_release(slab);
            ba_slab_release(current);
            atomic_store_explicit(&placement->current, NULL, memory_order_release);
        }
    }
    ba_event_signal(placement->completion);
    atomic_store_explicit(&placement->completed, 1, memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Dispatch
 * @brief Executes a non-stop order on the allocator thread.
 */
static void ba_dispatch(ba_order_t order) {
    ba_placement_t* placement = ba_placement_at(order.placement);
    switch (order.kind) {
    case BA_ORDER_INIT_PLACEMENT: ba_initialize_placement(placement); break;
    case BA_ORDER_REPLENISH: if (order.arg == 1) ba_trim_placement(placement); else { placement->trimmed = 0; ba_replenish(placement); } break;
    case BA_ORDER_RETIRE_SLAB: ba_recycle_slab((ba_slab_t*)(uintptr_t)order.arg); break;
    case BA_ORDER_RETIRE_NOVEL: ba_retire_novel(ba_plate_at((uint32_t)order.arg)); break;
    default: break;
    }
}
/** --------------------------------------------------------------------------------------------------------- Worker Main
 * @brief Drains orders and uses bounded idle spinning before an event wait.
 */
static void ba_worker_main(void* ignored) {
    ba_tunables_t policy = *ba_policy();
    (void)ignored;
    unsigned spins = 0;
    uint64_t next_poll = ba_os_now_ns() + (uint64_t)policy.pressure_poll_ms * 1000000;
    ba_order_t order;
    for (;;) {
        if (ba_os_now_ns() >= next_poll) {
            ba_tunables_get(&policy);
            g_pressure = ba_os_pressure();
            for (uint32_t type = 0; type < ba_placement_count(); ++type) {
                ba_placement_t* placement = ba_placement_at(type);
                atomic_exchange_explicit(&placement->runway_misses, 0, memory_order_relaxed);
                ba_replenish(placement);
                if (g_pressure != BA_PRESSURE_NONE) {
                    const uint64_t runway = (uint64_t)(atomic_load_explicit(&placement->runway_tail, memory_order_relaxed) -
                        atomic_load_explicit(&placement->runway_head, memory_order_relaxed)) * placement->slab_bytes;
                    ba_trim_free(placement, runway + (g_pressure == BA_PRESSURE_WARN ? placement->slab_bytes : 0));
                    ba_trim_cache(placement, g_pressure == BA_PRESSURE_WARN ? placement->descriptor.novel_cache_bytes / 2 : 0);
                }
            }
            next_poll = ba_os_now_ns() + (uint64_t)policy.pressure_poll_ms * 1000000;
        }
        if (ba_dequeue(&order)) {
            spins = 0;
            if (order.kind == BA_ORDER_STOP) break;
            ba_dispatch(order);
            continue;
        }
        if (++spins < BA_ORDER_CAPACITY) { ba_os_yield(); continue; }
        atomic_store_explicit(&g_sleeping, 1, memory_order_seq_cst);
        if (!ba_dequeue(&order)) {
            ba_event_wait(g_wake, (uint64_t)policy.pressure_poll_ms * 1000000);
        } else {
            atomic_store_explicit(&g_sleeping, 0, memory_order_seq_cst);
            if (order.kind == BA_ORDER_STOP) break;
            ba_dispatch(order);
        }
        atomic_store_explicit(&g_sleeping, 0, memory_order_seq_cst);
        spins = 0;
    }
    while (ba_dequeue(&order)) ba_dispatch(order);
    for (uint32_t type = 0; type < ba_placement_count(); ++type) {
        ba_placement_t* placement = ba_placement_at(type);
        ba_slab_t* slab;
        while ((slab = ba_runway_pop(placement))) ba_slab_release(slab);
        ba_trim_free(placement, 0);
        ba_trim_cache(placement, 0);
        slab = atomic_exchange_explicit(&placement->current, NULL, memory_order_acq_rel);
        if (slab && atomic_fetch_sub_explicit(&slab->live_plates, 1, memory_order_acq_rel) == 1) ba_slab_release(slab);
    }
    ba_event_signal(g_completion);
    atomic_store_explicit(&g_stopped, 1, memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Worker Start
 * @brief Initializes ring generations and launches the detached allocator thread.
 */
void ba_worker_start(void) {
    for (uint64_t index = 0; index < BA_ORDER_CAPACITY; ++index) atomic_init(&g_orders[index].sequence, index);
    g_wake = ba_event_create(); g_completion = ba_event_create();
    if (!g_wake || !g_completion || ba_os_thread_start(ba_worker_main, NULL) != 0) abort();
}
/** --------------------------------------------------------------------------------------------------------- Worker Stop
 * @brief Waits until all queued work and owned reserves have been released.
 */
void ba_worker_stop(void) {
    ba_enqueue((ba_order_t){BA_ORDER_STOP, 0, 0});
    while (!atomic_load_explicit(&g_stopped, memory_order_acquire)) ba_event_wait(g_completion, 1000000000);
    ba_event_destroy(g_wake); ba_event_destroy(g_completion);
}
/** --------------------------------------------------------------------------------------------------------- Synchronous Trim
 * @brief Waits for the worker to return all currently idle placement capacity.
 */
void ba_worker_trim(uint32_t type) {
    ba_placement_t* placement = ba_placement_at(type);
    while (atomic_flag_test_and_set_explicit(&placement->trim_lock, memory_order_acquire)) ba_os_yield();
    placement->trim_event = ba_event_create();
    if (!placement->trim_event) abort();
    atomic_store_explicit(&placement->trim_done, 0, memory_order_relaxed);
    ba_enqueue((ba_order_t){BA_ORDER_REPLENISH, type, 1});
    while (!atomic_load_explicit(&placement->trim_done, memory_order_acquire)) ba_event_wait(placement->trim_event, 1000000000);
    ba_event_destroy(placement->trim_event);
    placement->trim_event = NULL;
    atomic_flag_clear_explicit(&placement->trim_lock, memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- Cache Available
 * @brief Counts published cache entries without reporting an in-progress worker insertion.
 */
uint64_t ba_cache_available(ba_placement_t* placement) {
    ba_cache_class_t* cache = atomic_load_explicit(&placement->novel_cache, memory_order_acquire);
    if (!cache) return 0;
    uint64_t bytes = 0;
    for (uint32_t index = 0; index < 25; ++index) {
        ba_cache_class_t* ring = &cache[index];
        const uint64_t head = atomic_load_explicit(&ring->dequeue, memory_order_acquire);
        const uint64_t tail = atomic_load_explicit(&ring->enqueue, memory_order_acquire);
        for (uint64_t offset = 0; offset < 16 && head + offset < tail; ++offset) {
            const uint64_t ticket = head + offset;
            if (atomic_load_explicit(&ring->cells[ticket & 15u].sequence, memory_order_acquire) == ticket + 1) bytes += 65536ull << index;
        }
    }
    return bytes;
}
