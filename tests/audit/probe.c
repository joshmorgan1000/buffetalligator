/** --------------------------------------------------------------------------------------------------------- Allocation Probe
 * @file probe.c
 * @brief Records real library allocation calls without replacing their implementation.
 */
#include "probe.h"
#include <stdatomic.h>
#include <stdlib.h>

static _Atomic int all_threads;
static _Thread_local int selected_thread;
static _Atomic uint64_t counters[7];
void* ba_audit_os_map_real(uint64_t bytes, uint64_t alignment, int large);
int ba_audit_os_commit_real(void* base, uint64_t bytes);
/** --------------------------------------------------------------------------------------------------------- Record
 * @brief Records one selected call and its requested byte count.
 */
static void record(size_t field, uint64_t bytes) {
    if (!selected_thread && !atomic_load_explicit(&all_threads, memory_order_relaxed)) return;
    atomic_fetch_add_explicit(&counters[field], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&counters[6], bytes, memory_order_relaxed);
}
/** --------------------------------------------------------------------------------------------------------- Begin
 * @brief Resets counters before publishing the selected observation scope.
 */
void ba_audit_begin(int observe_all) {
    for (size_t index = 0; index < 7; ++index) atomic_store_explicit(&counters[index], 0, memory_order_relaxed);
    selected_thread = 1;
    atomic_store_explicit(&all_threads, observe_all, memory_order_release);
}
/** --------------------------------------------------------------------------------------------------------- End
 * @brief Returns counts after closing the observation window.
 */
ba_audit_counts ba_audit_end(void) {
    selected_thread = 0;
    atomic_store_explicit(&all_threads, 0, memory_order_release);
    ba_audit_counts result = {
        atomic_load_explicit(&counters[0], memory_order_relaxed),
        atomic_load_explicit(&counters[1], memory_order_relaxed),
        atomic_load_explicit(&counters[2], memory_order_relaxed),
        atomic_load_explicit(&counters[3], memory_order_relaxed),
        atomic_load_explicit(&counters[4], memory_order_relaxed),
        atomic_load_explicit(&counters[5], memory_order_relaxed),
        atomic_load_explicit(&counters[6], memory_order_relaxed)
    };
    return result;
}
/** --------------------------------------------------------------------------------------------------------- Malloc
 * @brief Counts a library malloc call before forwarding it.
 */
void* ba_audit_malloc(size_t bytes) { record(0, bytes); return malloc(bytes); }
/** --------------------------------------------------------------------------------------------------------- Calloc
 * @brief Counts a library calloc call before forwarding it.
 */
void* ba_audit_calloc(size_t count, size_t bytes) { record(1, count * bytes); return calloc(count, bytes); }
/** --------------------------------------------------------------------------------------------------------- Realloc
 * @brief Counts a library realloc call before forwarding it.
 */
void* ba_audit_realloc(void* pointer, size_t bytes) { record(2, bytes); return realloc(pointer, bytes); }
/** --------------------------------------------------------------------------------------------------------- Aligned Allocation
 * @brief Counts a library aligned allocation before forwarding it.
 */
void* ba_audit_aligned_alloc(size_t alignment, size_t bytes) {
    record(3, bytes);
    return aligned_alloc(alignment, bytes);
}
/** --------------------------------------------------------------------------------------------------------- Map
 * @brief Counts a core mapping request before calling the real platform implementation.
 */
void* ba_os_map(uint64_t bytes, uint64_t alignment, int large) {
    record(4, bytes);
    return ba_audit_os_map_real(bytes, alignment, large);
}
/** --------------------------------------------------------------------------------------------------------- Commit
 * @brief Counts a registry commitment before calling the real platform implementation.
 */
int ba_os_commit(void* base, uint64_t bytes) {
    record(5, bytes);
    return ba_audit_os_commit_real(base, bytes);
}
