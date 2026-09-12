#pragma once
/** --------------------------------------------------------------------------------------------------------- Allocation Probe
 * @file probe.h
 * @brief Counts allocation entry points in separately instrumented library objects.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct ba_audit_counts {
    uint64_t malloc_calls;
    uint64_t calloc_calls;
    uint64_t realloc_calls;
    uint64_t aligned_calls;
    uint64_t map_calls;
    uint64_t commit_calls;
    uint64_t requested_bytes;
} ba_audit_counts;
/** --------------------------------------------------------------------------------------------------------- Begin
 * @brief Resets counters and selects the calling thread or all library threads for observation.
 */
void ba_audit_begin(int all_threads);
/** --------------------------------------------------------------------------------------------------------- End
 * @brief Stops observation and returns the measured counters.
 */
ba_audit_counts ba_audit_end(void);
#ifdef __cplusplus
}
#endif
