#pragma once
/** --------------------------------------------------------------------------------------------------------- Arena Boundary
 * @file ba_descriptor.h
 * @brief Declares the C boundary descriptor and arena operations shared by the private queue
 * and network transports, implemented over the C++ arena in chomp_channel.cpp.
 */
#include <stdint.h>
#include <stddef.h>

#define BA_NULL_ID UINT32_MAX
enum { BA_CLAIM_NOVEL = 1u << 0 };
/** --------------------------------------------------------------------------------------------------------- Descriptor
 * @brief Carries one alligator pool id; consumers transfer descriptors without interpreting
 * their contents and resolve size and pointer through the boundary helpers.
 */
typedef struct ba_chomp {
    uint32_t id; ///< The alligator pool slot; BA_NULL_ID when null.
} ba_chomp_t;
/** --------------------------------------------------------------------------------------------------------- Claim
 * @brief Claims zeroed arena storage for a registered placement or returns nonzero.
 */
int ba_claim(uint32_t type, size_t bytes, uint32_t flags, ba_chomp_t* out);
/** --------------------------------------------------------------------------------------------------------- Release
 * @brief Drops one descriptor's arena ownership and nulls it.
 */
void ba_release(ba_chomp_t* descriptor);
/** --------------------------------------------------------------------------------------------------------- Chomp Placement
 * @brief Resolves the owning placement identifier of a live descriptor.
 */
uint32_t ba_chomp_placement(const ba_chomp_t* descriptor);
/** --------------------------------------------------------------------------------------------------------- Chomp Novel
 * @brief Reports whether a live descriptor's backing is a dedicated novel buffer.
 */
int ba_chomp_is_novel(const ba_chomp_t* descriptor);
/** --------------------------------------------------------------------------------------------------------- Chomp Size
 * @brief Resolves a live descriptor's byte size; 0 when null.
 */
size_t ba_chomp_size(const ba_chomp_t* descriptor);
/** --------------------------------------------------------------------------------------------------------- Chomp Pointer
 * @brief Resolves a live descriptor's host pointer; NULL when null.
 */
void* ba_chomp_ptr(const ba_chomp_t* descriptor);
/** --------------------------------------------------------------------------------------------------------- Placement Name
 * @brief Returns a registered placement's process-lifetime name, or NULL when unknown.
 */
const char* ba_placement_name(uint32_t type);
