#pragma once
/** --------------------------------------------------------------------------------------------------------- ChainBuffet
 * @file chainbuffet.hpp
 * @brief One link of a placement's chain: a slab, the bump cursor cutting records from it, and the count of
 * records on it.
 */
#include <alligator.hpp>
#include <array>
#include <atomic>
#include <cstdint>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- ChainBuffet
 * @class ChainBuffet
 * @brief One slab cut into records by a bump cursor and counted by the records on it plus the chain's own hold,
 * linked to the link kept ahead of it; a dedicated link serves one record and is counted by that record alone.
 */
class ChainBuffet {
private:
    void* buffer_;  ///< The placement's buffet object behind the slab, null once the slab is gone.
    const BuffetDescriptor* desc_;  ///< The placement.
    void* host_;  ///< The slab's host address.
    uint64_t device_;  ///< The slab's device address.
    size_t size_;  ///< The slab's bytes.
    bool novel_;  ///< A dedicated link, freed with its one record.
    alignas(128) std::atomic<uint32_t> count_;  ///< Records on the link plus the chain's hold, CLOSED once let go.
    alignas(128) std::atomic<size_t> bump_{0};  ///< Bytes cut so far.
    std::atomic<ChainBuffet*> next_{nullptr};  ///< The link ahead, the pending sentinel while a thread makes it.
    std::atomic<ChainBuffet*> prev_{nullptr};  ///< The link behind, let go of when this one fills.
    /// @brief One retired slab per placement, zeroed and parked for the next link to take; shells never return.
    static std::array<std::atomic<void*>, 128> spares_;
    ChainBuffet(const BuffetDescriptor* desc, size_t size, bool novel, void* slab);
    friend class Alligator; friend class Slice;
public:
    static constexpr uint32_t CLOSED = 0x80000000u;  ///< The chain has let go; the last count out frees the slab.
    static constexpr uint32_t RETIRED = CLOSED | 0x40000000u;  ///< The slab is gone from this shell for good.
    ChainBuffet(const ChainBuffet&) = delete;
    ChainBuffet& operator=(const ChainBuffet&) = delete;
    ChainBuffet(ChainBuffet&&) = delete;
    ChainBuffet& operator=(ChainBuffet&&) = delete;
    ~ChainBuffet() = default;
    /** ------------------------------------------------------------------------------------------- Open
     * @brief The next link for a placement: a fresh shell around its parked spare slab when there
     * is one, otherwise around a fresh slab; a chain link may bring the link ahead of it along.
     * @param desc The placement.
     * @param size The slab bytes.
     * @param novel Whether the link is dedicated to one record.
     * @param ahead Whether to make the link ahead as well.
     * @return The link.
     */
    static ChainBuffet* open(const BuffetDescriptor* desc, size_t size, bool novel, bool ahead);
    /** ------------------------------------------------------------------------------------------- Next
     * @brief The link ahead, made by whichever thread finds the slot empty.
     * @param allocate_next Whether the link ahead gets its own link ahead too.
     * @return The link ahead.
     */
    ChainBuffet* next(bool allocate_next = false);
    /** ------------------------------------------------------------------------------------------- Claim
     * @brief Cuts a record and its payload from this link, or from the lane's next link once full.
     * @param total The bytes to cut, the record granule included.
     * @return The record, with its GPU record, host pointer and link filled in.
     */
    SliceRecord* claim(size_t total);
    /** ------------------------------------------------------------------------------------------- Release
     * @brief One count leaves the link; the last one out of a closed link retires its slab.
     */
    void release();
    /** ------------------------------------------------------------------------------------------- Close
     * @brief The chain lets go of its hold and closes the link to new claims.
     */
    void close();
    /** ------------------------------------------------------------------------------------------- Retire
     * @brief The last count is gone: a dedicated link frees itself, a chain link parks or frees its slab.
     */
    void retire();
};
/// @brief The sentinel a link slot holds while one thread makes the link for it.
inline ChainBuffet* const PENDING_LINK = reinterpret_cast<ChainBuffet*>(-1);
} // namespace buffetalligator
