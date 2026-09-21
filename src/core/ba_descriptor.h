#pragma once
/** --------------------------------------------------------------------------------------------------------- Arena Boundary
 * @file ba_descriptor.h
 * @brief Declares the C boundary descriptor and arena operations shared by the private queue
 * and network transports, implemented over the C++ arena in slice_channel.cpp.
 */
#include <stdint.h>
#include <stddef.h>

#define BA_NULL_ID UINT32_MAX
enum { BA_CLAIM_NOVEL = 1u << 0 };
/** --------------------------------------------------------------------------------------------------------- Descriptor
 * @brief Carries one alligator pool id; consumers transfer descriptors without interpreting
 * their contents and resolve size and pointer through the boundary helpers.
 */
typedef struct ba_slice {
    uint32_t id; ///< The alligator pool slot; BA_NULL_ID when null.
} ba_slice_t;
/** --------------------------------------------------------------------------------------------------------- Claim
 * @brief Claims zeroed arena storage for a registered placement or returns nonzero.
 */
int ba_claim(uint32_t type, size_t bytes, uint32_t flags, ba_slice_t* out);
/** --------------------------------------------------------------------------------------------------------- Release
 * @brief Drops one descriptor's arena ownership and nulls it.
 */
void ba_release(ba_slice_t* descriptor);
/** --------------------------------------------------------------------------------------------------------- Slice Placement
 * @brief Resolves the owning placement identifier of a live descriptor.
 */
uint32_t ba_slice_placement(const ba_slice_t* descriptor);
/** --------------------------------------------------------------------------------------------------------- Slice Novel
 * @brief Reports whether a live descriptor's backing is a dedicated novel buffer.
 */
int ba_slice_is_novel(const ba_slice_t* descriptor);
/** --------------------------------------------------------------------------------------------------------- Slice Size
 * @brief Resolves a live descriptor's byte size; 0 when null.
 */
size_t ba_slice_size(const ba_slice_t* descriptor);
/** --------------------------------------------------------------------------------------------------------- Slice Pointer
 * @brief Resolves a live descriptor's host pointer; NULL when null.
 */
void* ba_slice_ptr(const ba_slice_t* descriptor);
/** --------------------------------------------------------------------------------------------------------- Placement Name
 * @brief Returns a registered placement's process-lifetime name, or NULL when unknown.
 */
const char* ba_placement_name(uint32_t type);
