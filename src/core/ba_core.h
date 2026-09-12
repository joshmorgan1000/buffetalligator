#pragma once
/** --------------------------------------------------------------------------------------------------------- Core Interface
 * @file ba_core.h
 * @brief Defines the private C11 arena interface and storage contracts.
 * NOTES:
 * - 2026-09-11 (Codex):
 *     WHY: Growing registry storage needs a larger default slot ceiling.
 *     CHANGE: The default is 24 slot bits; rebuild all consumers with the library's setting.
 */
#include <stdint.h>
#include <stddef.h>
#if !defined(__cplusplus)
#include <stdatomic.h>
#endif
#include "ba_os.h"

#define BA_SLOT_BITS ALLIGATOR_SLOT_BITS
#define BA_SLOT_MASK ((1ull << BA_SLOT_BITS) - 1ull)
#define BA_NULL_META UINT64_MAX
#define BA_PLATE_COUNT (1u << BA_SLOT_BITS)
#define BA_MAX_PLACEMENTS 32768u
#define BA_BALANCE_BIAS (1ull << 31)
#define BA_SEALED_BIT (1ull << 63)
#if defined(__cplusplus)
#define BA_ALIGN(bytes) alignas(bytes)
#else
#define BA_ALIGN(bytes) _Alignas(bytes)
#endif
/** --------------------------------------------------------------------------------------------------------- Slice
 * @brief Stores a packed byte count and plate slot beside a cached host pointer.
 */
typedef struct ba_slice {
    uint64_t meta; ///< Packed size and registry slot.
    void* ptr; ///< Cached host pointer.
} ba_slice_t;
/** --------------------------------------------------------------------------------------------------------- Status
 * @brief Identifies a core operation result.
 */
typedef enum ba_status {
    BA_OK = 0,
    BA_E_BUDGET,
    BA_E_SLOTS,
    BA_E_PLACEMENT,
    BA_E_ALIGNMENT,
    BA_E_RANGE,
    BA_E_ALLOC,
    BA_E_CLOSED
} ba_status_t;
/** --------------------------------------------------------------------------------------------------------- Handle
 * @brief Carries the placement substrate and its callback context.
 */
typedef struct ba_handle {
    void* substrate_handle; ///< Placement-owned substrate.
    void* context; ///< Callback context.
} ba_handle_t;
typedef ba_handle_t* (*ba_alloc_fn)(size_t bytes, void* context);
typedef void         (*ba_free_fn)(ba_handle_t* handle, void* context);
typedef void*        (*ba_host_ptr_fn)(ba_handle_t* handle);
typedef void*        (*ba_context_fn)(void);
typedef uint64_t     (*ba_query_fn)(void* context);
typedef void         (*ba_zero_fn)(ba_handle_t* handle, uint64_t offset, uint64_t bytes, void* context);
enum { BA_PLACEMENT_DEFAULT = 1u << 0, BA_PLACEMENT_OS_PAGES = 1u << 1, BA_PLACEMENT_LARGE_PAGES = 1u << 2 };
enum { BA_CLAIM_NOVEL = 1u << 0 };
/** --------------------------------------------------------------------------------------------------------- Placement Description
 * @brief Defines allocation callbacks, alignment, and resource policy.
 */
typedef struct ba_placement_desc {
    uint32_t       struct_size; ///< Descriptor layout size.
    uint32_t       flags; ///< Placement capability flags.
    const char*    name; ///< Process-lifetime placement name.
    uint64_t       slab_bytes; ///< Requested or resolved slab capacity.
    uint32_t       base_alignment; ///< Guaranteed substrate alignment.
    uint32_t       reserved; ///< Reserved descriptor storage.
    uint64_t       budget_bytes; ///< Placement capacity ceiling.
    uint64_t       novel_cache_bytes; ///< Novel cache capacity.
    ba_alloc_fn    alloc; ///< Allocation callback.
    ba_free_fn     free; ///< Substrate release callback.
    ba_host_ptr_fn host_ptr; ///< Stable host-address callback.
    ba_context_fn  context; ///< Context provider.
    ba_query_fn    query_available; ///< Custom capacity query.
    void (*destroy_handle)(ba_handle_t* handle); ///< Optional handle destruction callback.
    ba_zero_fn     zero; ///< Custom recycling callback.
} ba_placement_desc_t;
/** --------------------------------------------------------------------------------------------------------- Statistics
 * @brief Reports placement activity and current resource geometry.
 */
typedef struct ba_stats {
    uint64_t slab_bytes_allocated, slab_bytes_freed; ///< Cumulative slab allocation and release bytes.
    uint64_t novel_bytes_allocated, novel_bytes_freed; ///< Cumulative novel allocation and release bytes.
    uint64_t reserved_bytes; ///< Free-list and runway bytes.
    uint64_t plate_bytes_carved; ///< Cumulative carved plate bytes.
    uint64_t novel_cache_bytes; ///< Novel cache capacity.
    uint64_t budget_bytes; ///< Placement capacity ceiling.
    uint64_t slab_bytes, plate_bytes; ///< Resolved slab and thread plate capacity.
    uint32_t runway_target, runway_misses; ///< Prepared depth and recent runway misses.
    uint32_t claim_ewma; ///< Smoothed claim size.
    uint64_t build_ns_ewma; ///< Smoothed slab build duration.
} ba_stats_t;
/** --------------------------------------------------------------------------------------------------------- Tunables
 * @brief Configures runtime geometry and worker policy.
 */
typedef struct ba_tunables {
    uint32_t headroom_num; ///< Headroom fraction numerator.
    uint32_t headroom_den; ///< Headroom fraction denominator.
    uint32_t claims_per_plate; ///< Target claims per thread plate.
    uint32_t plates_per_thread; ///< Slab plate allowance per hardware thread.
    uint32_t slab_budget_divisor; ///< Budget fraction limiting slab capacity.
    uint32_t runway_floor; ///< Minimum desired prepared depth.
    uint32_t pressure_poll_ms; ///< Pressure polling period.
    uint32_t ewma_shift; ///< Moving-average weight exponent.
} ba_tunables_t;
/** --------------------------------------------------------------------------------------------------------- Status Name
 * @brief Returns the stable symbolic name of a core status.
 */
const char* ba_status_name(ba_status_t status);
/** --------------------------------------------------------------------------------------------------------- Initialize
 * @brief Probes runtime policy and registers the two built-in placements exactly once.
 */
void        ba_init(void);
/** --------------------------------------------------------------------------------------------------------- Register Placement
 * @brief Validates a descriptor and publishes its completed initial slab.
 */
ba_status_t ba_placement_register(const ba_placement_desc_t* desc, uint32_t* type_out);
/** --------------------------------------------------------------------------------------------------------- Placement Count
 * @brief Returns the number of fully registered placements.
 */
uint32_t    ba_placement_count(void);
/** --------------------------------------------------------------------------------------------------------- Default Placement
 * @brief Returns the current default identifier.
 */
uint32_t    ba_placement_default(void);
/** --------------------------------------------------------------------------------------------------------- Placement Name
 * @brief Returns a registered placement name.
 */
const char* ba_placement_name(uint32_t type);
/** --------------------------------------------------------------------------------------------------------- Find Placement
 * @brief Resolves a registered name to its stable identifier.
 */
int         ba_placement_find(const char* name);
/** --------------------------------------------------------------------------------------------------------- Describe Placement
 * @brief Copies the registered descriptor and resolved slab size.
 */
void        ba_placement_describe(uint32_t type, ba_placement_desc_t* copy_out);
/** --------------------------------------------------------------------------------------------------------- Claim
 * @brief Bumps within the owner thread's plate without atomic operations.
 */
ba_status_t ba_claim(uint32_t type, size_t bytes, uint32_t flags, ba_slice_t* out);
/** --------------------------------------------------------------------------------------------------------- Retain
 * @brief Adds one outstanding reference to a live plate.
 */
void        ba_retain(const ba_slice_t* s);
/** --------------------------------------------------------------------------------------------------------- Release
 * @brief Drops a reference and nulls the released slice.
 */
void        ba_release(ba_slice_t* s);
/** --------------------------------------------------------------------------------------------------------- View
 * @brief Retains a checked subrange of a slice.
 */
ba_status_t ba_view(const ba_slice_t* s, size_t offset, size_t length, ba_slice_t* out);
/** --------------------------------------------------------------------------------------------------------- Slice Placement
 * @brief Resolves the owning placement of a live slice.
 */
uint32_t    ba_slice_placement(const ba_slice_t* s);
/** --------------------------------------------------------------------------------------------------------- Slice Novel
 * @brief Reports whether a non-null Slice owns or views dedicated novel backing.
 */
int         ba_slice_is_novel(const ba_slice_t* s);
/** --------------------------------------------------------------------------------------------------------- Slice Handle
 * @brief Resolves the substrate handle of a live slice.
 */
ba_handle_t* ba_slice_handle(const ba_slice_t* s);
/** --------------------------------------------------------------------------------------------------------- Statistics
 * @brief Reads placement allocation counters and current resource policy.
 */
void        ba_stats(uint32_t type, ba_stats_t* out);
/** --------------------------------------------------------------------------------------------------------- Total Statistics
 * @brief Sums completed allocation counters across all placements.
 */
void        ba_stats_total(ba_stats_t* out);
/** --------------------------------------------------------------------------------------------------------- System Information
 * @brief Returns a fresh OS capacity snapshot.
 */
void        ba_sysinfo(ba_sysinfo_t* out);
/** --------------------------------------------------------------------------------------------------------- Slot Capacity
 * @brief Returns the number of currently writable backing registry entries.
 */
uint32_t    ba_slot_capacity(void);
/** --------------------------------------------------------------------------------------------------------- Pressure
 * @brief Returns the current pressure level.
 */
ba_pressure_t ba_pressure(void);
/** --------------------------------------------------------------------------------------------------------- Budget Set
 * @brief Sets the maximum capacity charged to a placement.
 */
void        ba_budget_set(uint32_t type, uint64_t bytes);
/** --------------------------------------------------------------------------------------------------------- Trim
 * @brief Requests release of idle placement reserves.
 */
void        ba_trim(uint32_t type);
/** --------------------------------------------------------------------------------------------------------- Trim All
 * @brief Requests release of idle reserves in all placements.
 */
void        ba_trim_all(void);
/** --------------------------------------------------------------------------------------------------------- Get Tunables
 * @brief Copies arena policy settings.
 */
void        ba_tunables_get(ba_tunables_t* out);
/** --------------------------------------------------------------------------------------------------------- Set Tunables
 * @brief Replaces policy settings while the caller holds application quiescence.
 */
void        ba_tunables_set(const ba_tunables_t* t);
/** --------------------------------------------------------------------------------------------------------- Shutdown
 * @brief Closes the allocator worker.
 */
void        ba_shutdown(void);
#if !defined(__cplusplus)
/** --------------------------------------------------------------------------------------------------------- Plate
 * @brief Owns one reference-counted region of a slab or novel allocation.
 */
typedef struct ba_plate {
    BA_ALIGN(64) _Atomic uint64_t state; ///< Sealed flag, issued count, and biased balance.
    struct ba_slab* slab; ///< Parent slab for carved plates.
    ba_handle_t* handle; ///< Substrate handle.
    uint8_t* base; ///< Host base address.
    uint64_t bytes; ///< Region capacity.
    uint32_t placement; ///< Placement identifier.
    uint32_t kind; ///< Plate ownership kind.
    _Atomic uint32_t next_free; ///< Tagged-stack successor slot.
    uint32_t reserved; ///< Reserved storage.
} ba_plate_t;
enum { BA_PLATE_FREE, BA_PLATE_THREAD, BA_PLATE_DIRECT, BA_PLATE_NOVEL };
/** --------------------------------------------------------------------------------------------------------- TLS Plate
 * @brief Stores the owner thread's bump cursor and unpublished issued count.
 */
typedef struct ba_tls_plate {
    uint8_t* cursor; ///< Next claim address.
    uint8_t* limit; ///< End of the plate.
    uint64_t slot; ///< Registry slot.
    uint64_t issued; ///< Owner-issued claims.
} ba_tls_plate_t;
/** --------------------------------------------------------------------------------------------------------- Slab
 * @brief Tracks an allocation carved into independently reference-counted plates.
 */
typedef struct ba_slab {
    BA_ALIGN(64) _Atomic uint64_t bump; ///< Carved bytes.
    _Atomic uint32_t live_plates; ///< Outstanding plates plus the open pin.
    uint32_t placement; ///< Owning placement.
    uint64_t bytes; ///< Allocation capacity.
    uint8_t* base; ///< Host base address.
    ba_handle_t* handle; ///< Substrate handle.
    void* context; ///< Allocation context.
    struct ba_slab* next; ///< Header pool or worker free-list link.
    uint64_t opened_ns; ///< Time of promotion to current.
} ba_slab_t;
/** --------------------------------------------------------------------------------------------------------- Order Kind
 * @brief Identifies an operation owned by the allocator worker.
 */
typedef enum {
    BA_ORDER_STOP, BA_ORDER_INIT_PLACEMENT, BA_ORDER_REPLENISH,
    BA_ORDER_RETIRE_SLAB, BA_ORDER_RETIRE_NOVEL
} ba_order_kind_t;
/** --------------------------------------------------------------------------------------------------------- Order
 * @brief Carries a placement and payload to the allocator worker.
 */
typedef struct ba_order {
    uint32_t kind; ///< Worker operation.
    uint32_t placement; ///< Owning placement.
    uint64_t arg; ///< Operation payload.
} ba_order_t;
_Static_assert(sizeof(ba_slice_t) == 16, "slice storage");
_Static_assert(sizeof(ba_plate_t) == 64, "plate storage");
_Static_assert(sizeof(ba_tls_plate_t) == 32, "TLS storage");
_Static_assert(sizeof(ba_slab_t) == 64, "slab storage");
#endif
#if defined(BA_INTERNAL)
#define BA_ADVANCING ((ba_slab_t*)(uintptr_t)1)
/** --------------------------------------------------------------------------------------------------------- Placement Runtime
 * @brief Stores placement geometry, current slab ownership, and atomic counters.
 */
typedef struct ba_placement {
    BA_ALIGN(64) uint64_t slab_bytes; ///< Slab capacity.
    _Atomic uint64_t plate_bytes; ///< Adaptive thread plate capacity.
    uint32_t type; ///< Stable identifier.
    ba_placement_desc_t descriptor; ///< Registered callbacks and policy.
    void* context; ///< Placement allocation context.
    BA_ALIGN(64) _Atomic(ba_slab_t*) current; ///< Current slab or exclusive carve token.
    BA_ALIGN(64) _Atomic uint64_t slab_allocated; ///< Allocated slab bytes.
    _Atomic uint64_t slab_freed; ///< Freed slab bytes.
    _Atomic uint64_t novel_allocated; ///< Allocated novel bytes.
    _Atomic uint64_t novel_freed; ///< Freed novel bytes.
    _Atomic uint64_t used; ///< Budget-charged bytes.
    _Atomic uint64_t budget; ///< Placement budget.
    _Atomic uint64_t reserved; ///< Prepared and free slab bytes.
    _Atomic uint64_t carved; ///< Monotonic carved capacity.
    _Atomic uint32_t claim_ewma; ///< Mean rounded claim bytes.
    _Atomic uint64_t build_ewma; ///< Mean slab build nanoseconds.
    _Atomic uint32_t runway_target; ///< Desired prepared slab count.
    _Atomic uint32_t runway_misses; ///< Misses in the latest poll interval.
    BA_ALIGN(64) _Atomic uint32_t runway_head; ///< Consumer-owned runway position.
    BA_ALIGN(64) _Atomic uint32_t runway_tail; ///< Producer-owned runway position.
    ba_slab_t* runway[64]; ///< Prepared slab ring.
    _Atomic int runway_building; ///< Worker owns a pending runway publication.
    _Atomic(void*) novel_cache; ///< Lazily published size-class cache.
    _Atomic uint64_t cached_bytes; ///< Budget-charged novel cache bytes.
    atomic_flag trim_lock; ///< Serializes synchronous trim requests.
    ba_event_t* trim_event; ///< Current trim completion event.
    _Atomic int trim_done; ///< Trim completion publication.
    _Atomic uint32_t fault; ///< Background allocation or zeroing error.
    int trimmed; ///< Suppresses idle replenishment after an explicit trim.
    ba_slab_t* free_head; ///< Worker-owned recycled slabs.
    uint64_t free_bytes; ///< Worker-owned free-list capacity.
    _Atomic double consume_rate; ///< Smoothed slab bytes consumed per nanosecond.
    ba_event_t* completion; ///< Registration completion event.
    _Atomic int completed; ///< Registration result publication.
    ba_status_t status; ///< Registration status.
} ba_placement_t;
/** --------------------------------------------------------------------------------------------------------- Placement Runtime
 * @brief Resolves a worker-owned placement record.
 */
ba_placement_t* ba_placement_at(uint32_t type);
/** --------------------------------------------------------------------------------------------------------- Plate Runtime
 * @brief Resolves a worker-owned retired plate.
 */
ba_plate_t* ba_plate_at(uint32_t slot);
/** --------------------------------------------------------------------------------------------------------- Policy
 * @brief Returns the arena's quiescently configured policy.
 */
const ba_tunables_t* ba_policy(void);
/** --------------------------------------------------------------------------------------------------------- Machine
 * @brief Returns immutable startup hardware geometry.
 */
const ba_sysinfo_t* ba_machine(void);
/** --------------------------------------------------------------------------------------------------------- Slab Build
 * @brief Obtains a budgeted zeroed slab and publishes its open pin.
 */
ba_status_t ba_slab_build(ba_placement_t* placement, ba_slab_t** out);
/** --------------------------------------------------------------------------------------------------------- Slab Release
 * @brief Returns one slab allocation and recycles its header.
 */
void ba_slab_release(ba_slab_t* slab);
/** --------------------------------------------------------------------------------------------------------- Novel Release
 * @brief Releases a novel allocation and its retired registry entry.
 */
void ba_novel_release(ba_plate_t* plate);
/** --------------------------------------------------------------------------------------------------------- Plate Retire
 * @brief Releases backing ownership when a sealed plate loses its final reference.
 */
void ba_novel_dispose(ba_placement_t* placement, ba_handle_t* handle, uint8_t* base, uint64_t bytes);
/** --------------------------------------------------------------------------------------------------------- Recycle Slot
 * @brief Returns a cached novel plate's slot to the registry free list.
 */
void ba_slot_recycle(ba_plate_t* plate);
/** --------------------------------------------------------------------------------------------------------- Cache Pop
 * @brief Claims one zeroed cached allocation without blocking on an empty ring.
 */
int ba_novel_cache_pop(ba_placement_t* placement, uint64_t bytes, ba_handle_t** handle, uint8_t** base);
/** --------------------------------------------------------------------------------------------------------- Synchronous Trim
 * @brief Waits for the worker to return all currently idle placement capacity.
 */
void ba_worker_trim(uint32_t type);
/** --------------------------------------------------------------------------------------------------------- Cache Available
 * @brief Counts published cache entries without reporting an in-progress worker insertion.
 */
uint64_t ba_cache_available(ba_placement_t* placement);
/** --------------------------------------------------------------------------------------------------------- Enqueue
 * @brief Publishes an order and wakes a sleeping worker.
 */
void ba_enqueue(ba_order_t order);
/** --------------------------------------------------------------------------------------------------------- Worker Start
 * @brief Initializes ring generations and launches the detached allocator thread.
 */
void ba_worker_start(void);
/** --------------------------------------------------------------------------------------------------------- Worker Stop
 * @brief Waits until all queued work and owned reserves have been released.
 */
void ba_worker_stop(void);
/** --------------------------------------------------------------------------------------------------------- Runway Pop
 * @brief Transfers one prepared slab to the exclusive current-slab consumer.
 */
ba_slab_t* ba_runway_pop(ba_placement_t* placement);
#endif
