#pragma once
/** --------------------------------------------------------------------------------------------------------- Slice Layout
 * @file alligator_layout.h
 * @brief Shares the packed Slice layout between the C core and public C++ accessors.
 * NOTES: 2026-09-11 (Codex) WHY: Long-lived novel buffers exhausted the 17-bit backing registry.
 * CHANGE: Use 21 slot bits; rebuild the library and all consumers together when updating this layout.
 */
#define ALLIGATOR_SLOT_BITS 21u
