#pragma once
/** --------------------------------------------------------------------------------------------------------- Slot Arena
 * @file arena.hpp
 * @brief Private slot, buffer, and directory tables behind every 4-byte Slice id.
 *
 * - A Slice id is a 24-bit slot: the high 8 bits pick a 64K-slot segment, the low 16 the slot.
 * - Each slot has a GPU-visible 8-byte location word and a host-only 4-byte state.
 * - Each buffer has a host-only 32-byte record and a GPU-visible device base in the directory.
 * - The directory holds 256 segment addresses, one device base per buffer, then one 32-bit
 *   granule count per buffer for whole-buffer slots.
 */
#include <alligator.hpp>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace buffetalligator {
inline constexpr uint32_t SEGMENT_BITS = 16;  ///< Slots per segment as a power of two.
inline constexpr uint32_t SEGMENT_SLOTS = 1u << SEGMENT_BITS;
inline constexpr uint32_t SEGMENT_MASK = SEGMENT_SLOTS - 1;
inline constexpr uint32_t SEGMENT_WORDS = SEGMENT_SLOTS / 64;  ///< Occupancy words per segment.
inline constexpr uint32_t SEGMENT_WORDS_SHIFT = SEGMENT_BITS - 6;  ///< log2(SEGMENT_WORDS)
inline constexpr uint32_t SEGMENT_COUNT = 256;  ///< Segments a 24-bit slot id can name.
inline constexpr uint32_t BUFFER_COUNT = 1u << 16;  ///< Buffers a 16-bit location field can name.
inline constexpr uint32_t BUFFER_NONE = UINT32_MAX;  ///< Link with no successor yet.
inline constexpr uint32_t BUFFER_PENDING = UINT32_MAX - 1;  ///< Link while its buffer allocates.
inline constexpr uint64_t OFFSET_LIMIT = (uint64_t{1} << 26) - 1;  ///< Largest 64-byte offset.
inline constexpr uint64_t UNITS_LIMIT = (uint64_t{1} << 20) - 1;  ///< Largest length field value.
inline constexpr uint64_t LIVE_ONE = uint64_t{1} << 32;  ///< One live slot in a buffer cursor.
inline constexpr uint64_t BUMP_MASK = LIVE_ONE - 1;  ///< Bump granules in a buffer cursor.
inline constexpr size_t DIRECTORY_GRANULES = SEGMENT_COUNT + BUFFER_COUNT;  ///< Word where counts begin.
inline constexpr size_t DIRECTORY_WORDS = DIRECTORY_GRANULES + BUFFER_COUNT / 2;
inline constexpr uint32_t SPARE_LIMIT = 8;  ///< Released slab-sized buffers kept per placement.
inline constexpr uint32_t CHAIN_LANES = 2;  ///< Chains per placement, one picked per thread.
/** --------------------------------------------------------------------------------------------------------- Location
 * @struct Location
 * @brief Packs buffer 16 | scale 2 | offset 26 in 64-byte units | length 20 in scaled units.
 */
struct Location {
    static constexpr uint64_t pack(
        uint64_t buffer,
        uint64_t scale,
        uint64_t offset,
        uint64_t units
    ) {
        return buffer << 48 | scale << 46 | offset << 20 | units;
    }
    static constexpr uint32_t buffer(uint64_t word) { return static_cast<uint32_t>(word >> 48); }
    static constexpr uint64_t scale(uint64_t word) { return word >> 46 & 3; }
    static constexpr uint64_t offset(uint64_t word) { return word >> 20 & OFFSET_LIMIT; }
    static constexpr uint64_t units(uint64_t word) { return word & UNITS_LIMIT; }
};
/** --------------------------------------------------------------------------------------------------------- Extent
 * @struct Extent
 * @brief A view length in the smallest 64, 256, 1024, or 4096-byte unit whose count fits 20 bits.
 */
struct Extent {
    uint64_t scale;  ///< The unit is 64 << (2 * scale) bytes.
    uint64_t units;  ///< Length field value, zero when only a whole buffer can represent it.
    uint64_t granules;  ///< The represented length in 64-byte granules.
    /** ------------------------------------------------------------------------------------------- Of
     * @brief Rounds a 64-byte granule count up to its smallest representable unit.
     * @param granules The requested length in 64-byte granules.
     * @return The extent, with zero units past the largest scaled length.
     */
    static Extent of(uint64_t granules);
};
/** --------------------------------------------------------------------------------------------------------- Buffer Record
 * @struct BufferRecord
 * @brief Host-only facts for one backing buffer, with its bump offset and live slots in one word.
 */
struct alignas(32) BufferRecord {
    uint8_t* host_base;  ///< Host mapping of the buffer's first byte.
    void* native;  ///< Buffet handle the placement's hooks expect.
    uint64_t cursor;  ///< Live slots and chain hold high, bump granules low.
    uint32_t granules;  ///< Buffer size in 64-byte granules.
    uint8_t placement;  ///< Registered BuffetDescriptor index.
    uint8_t novel;  ///< Nonzero for a dedicated buffer.
};
static_assert(sizeof(BufferRecord) == 32, "BufferRecord must stay half a cache line");
/** --------------------------------------------------------------------------------------------------------- Slot State
 * @struct SlotState
 * @brief Host-only per-slot handle count and element type.
 */
struct SlotState {
    uint16_t references;  ///< Handle count, touched only through std::atomic_ref<uint16_t>.
    uint8_t type;  ///< SliceTypes index written once at claim, zero for raw bytes.
    uint8_t reserved;  ///< Unused.
};
static_assert(sizeof(SlotState) == 4, "SlotState must stay four bytes");
/** --------------------------------------------------------------------------------------------------------- Spares
 * @struct Spares
 * @brief Released slab-sized buffers of one placement, kept with their index for the next open.
 */
struct alignas(64) Spares {
    std::atomic_flag busy;  ///< Trivially destructible, since Slices release after teardown.
    uint32_t count = 0;  ///< Parked buffers.
    uint32_t buffers[SPARE_LIMIT] = {};  ///< Parked buffer indices, most recent last.
    uint32_t dirty[SPARE_LIMIT] = {};  ///< Granules each parked buffer handed out before release.
    void lock() {
        while (busy.test_and_set(std::memory_order_acquire))
            busy.wait(true, std::memory_order_relaxed);
    }
    void unlock() {
        busy.clear(std::memory_order_release);
        busy.notify_one();
    }
};
/** --------------------------------------------------------------------------------------------------------- Arena
 * @class Arena
 * @brief The process-wide tables; segments only grow and never move, so lookups need no locks.
 */
class Arena {
public:
    static inline uint64_t* locations[SEGMENT_COUNT] = {};  ///< GPU-visible location words.
    static inline SlotState* states[SEGMENT_COUNT] = {};  ///< Host-only slot states.
    static inline uint64_t* occupancy[SEGMENT_COUNT] = {};  ///< Host-only slot bitmaps.
    static inline void* segment_buffets[SEGMENT_COUNT] = {};  ///< Location word buffets.
    static inline std::atomic<uint32_t> segments{0};  ///< Published segment count.
    static inline std::mutex growth;  ///< Serializes the rare segment publication.
    static inline BufferRecord buffers[BUFFER_COUNT] = {};  ///< Host-only buffer records.
    static inline uint64_t buffer_bits[BUFFER_COUNT / 64] = {};  ///< Buffer index occupancy.
    static inline uint32_t successors[BUFFER_COUNT] = {};  ///< Chain link to the next buffer.
    static inline uint64_t* directory = nullptr;  ///< Host mapping of the GPU-visible directory.
    static inline void* directory_buffet = nullptr;  ///< Metadata buffet handle of the directory.
    static inline uint64_t directory_address = 0;  ///< Device address of the directory.
    static inline const BuffetDescriptor* metadata = nullptr;  ///< GPU-visible table placement.
    static inline Spares spares[8];  ///< Reusable slab-sized buffers per placement index.
    /** ------------------------------------------------------------------------------------------- Open
     * @brief Reuses a parked slab-sized buffer or allocates one, holding one live count.
     * @param placement The placement that allocates the buffer.
     * @param granules Buffer size in 64-byte granules.
     * @param novel Whether the buffer is dedicated to one Slice.
     * @return The buffer index.
     */
    static uint32_t open(const BuffetDescriptor* placement, uint64_t granules, bool novel);
    /** ------------------------------------------------------------------------------------------- Publish
     * @brief Claims a free slot, growing by one segment when every published slot is taken.
     * @param word The slot's location word.
     * @param type The slot's SliceTypes index.
     * @return The slot id, owned by one handle.
     */
    static uint32_t publish(uint64_t word, uint8_t type);
    /** ------------------------------------------------------------------------------------------- Retire
     * @brief Drops one live count and parks or frees the buffer after the last.
     * @param buffer The buffer index.
     */
    static void retire(uint32_t buffer);
    /** ------------------------------------------------------------------------------------------- Release Spares
     * @brief Frees every parked buffer and its index at arena shutdown.
     */
    static void release_spares();
};
} // namespace buffetalligator
