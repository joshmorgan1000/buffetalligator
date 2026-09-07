#pragma once
/** --------------------------------------------------------------------------------------------------------- Buffet
 * @file buffet.hpp
 * @brief Internal counted slab handle and preallocated chain link.
 */
#include <alligator.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace buffetalligator {
struct DeleteCold;
/** --------------------------------------------------------------------------------------------------------- Buffet
 * @class Buffet
 * @brief Owns one placement allocation and bump-allocates aligned units from it.
 */
class alignas(32) Buffet {
private:
    /** ------------------------------------------------------------------------------------------- Entree
     * @struct Entree
     * @brief Colder properties for the Buffet (kept in a struct for cache alignment)
     */
    struct Entree {
        /// @brief The published successor link, a swap sentinel while a claimant allocates it, or
        /// the novel sentinel for dedicated buffers.
        std::atomic<Buffet*> next{nullptr};
        /// @brief The placement factory that owns this buffer.
        const Placemat* placement = nullptr;
        /// @brief The handle to the placement allocation.
        Placemat::Handle* handle = nullptr;
        /// @brief Context pointer for arbitrary user data.
        void* context = nullptr;
        /// @brief The root of the chain of slices allocated from this slab.
        std::atomic<Slice*> root{nullptr};
    };
    /// @brief Marks the next_ link of a novel buffer, which has no chain successor.
    inline static Buffet* NOVEL_NEXT_SENTINEL = reinterpret_cast<Buffet*>(-2);
    /// @brief Marks the next_ link while one claimant is allocating the successor.
    inline static Buffet* SWAP_SENTINEL = reinterpret_cast<Buffet*>(-1);
    /// @brief Colder properties for the Buffet (kept in a struct for cache alignment)
    Entree* cold_ = nullptr;
    /// @brief Cached host base pointer for the allocation.
    void* host_ = nullptr;
    /// @brief The total size of the allocation in bytes, with the 17 LSB's being the alligator index.
    uint64_t size_ = 0;
    /// @brief The bump cursor within the allocation, counted in units set by
    /// the placement's claim granularity.
    std::atomic<uint32_t> bump_offset_{0};
    /// @brief The reference count for this slab.
    std::atomic<int32_t> ref_count_{1};
    /// @brief Frees the buffet if its reference count reaches zero.
    void free();
    /** ------------------------------------------------------------------------------------------- Slice
     * @brief Creates a new slice from the buffet at the specified offset and size. If the size is
     * not specified, it defaults to the remaining size from the offset.
     * @param offset The offset within the buffet where the slice should start.
     * @param size The size of the slice in bytes. Defaults to the remaining size from the offset.
     * @return A Slice object representing the requested portion of the buffet.
     */
    Slice slice(size_t offset = 0, size_t size = SIZE_MAX) {
        if (!cold_ || !cold_->placement || !cold_->handle) [[unlikely]] {
            ALLIGATOR_THROW("Buffet::new_slice: Buffet has encountered a corrupted state.");
        }
        if (offset == 0 && size == SIZE_MAX) {
            Slice sl;
            sl.cached_ = static_cast<uint8_t*>(cold_->placement->get_host_ptr_(cold_->handle));
            sl.meta_ = (static_cast<uint64_t>((size_ >> 17)) << 17) | (size_ & 0x1FFFF);
            return sl;
        }
        if (offset >= (size_ >> 17)) [[unlikely]] {
            ALLIGATOR_THROW("Buffet::new_slice: Offset is out of bounds.");
        }
        ref_count_.fetch_add(1, std::memory_order_relaxed);
        Slice slice;
        if (size == SIZE_MAX) {
            size = (size_ >> 17) - offset;
        }
        slice.cached_ = static_cast<uint8_t*>(cold_->placement->get_host_ptr_(cold_->handle)) + offset;
        slice.meta_ = (static_cast<uint64_t>(size) << 17) | (size_ & 0x1FFFF);
        return slice;
    }
    /** ------------------------------------------------------------------------------------------- Novel Slice
     * @brief Creates a new slice from a novel buffer with the specified size, placement, and context.
     * @param size The size of the slice in bytes.
     * @param placement The factory that owns the allocation.
     * @param context The context associated with the allocation.
     * @return A Slice object representing the requested portion of the novel buffer.
     */
    static Slice novel_slice(size_t size, const Placemat* placement, void* context);
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Creates the counted handle around one completed placement allocation.
     * @param placement The factory that owns the allocation.
     * @param handle The placement handle of the completed allocation.
     * @param size The granularity-rounded allocation size in bytes.
     * @param is_novel True for a dedicated buffer that has no chain successor.
     */
    Buffet(
        const Placemat* placement,
        void* context,
        size_t size,
        bool is_novel
    );
    friend class Alligator;
    friend class Placemat;
    friend class Slice;
    friend class SliceFriend;
    friend struct DeleteCold;
public:
    Buffet() = delete;
    Buffet(const Buffet&) = delete;
    Buffet& operator=(const Buffet&) = delete;
    Buffet(Buffet&&) = delete;
    Buffet& operator=(Buffet&&) = delete;
    /** ------------------------------------------------------------------------------------------- Destructor
     * @brief Runs the terminal teardown of the placement allocation; reached only through the
     * retirement protocol once the final reference count has been consumed.
     */
    ~Buffet();
    /** ------------------------------------------------------------------------------------------- Handle
     * @brief Returns the substrate handle associated with this allocation.
     * @return The substrate handle.
     */
    void* handle();
    /** ------------------------------------------------------------------------------------------- Handle (const)
     * @brief Returns the substrate handle associated with this allocation (const version).
     * @return The substrate handle.
     */
    const void* handle() const;
    /** ------------------------------------------------------------------------------------------- Deallocate
     * @brief Drops a used-up slab's self-reference; a slab no Slice holds is retired for the
     * worker to destroy.
     * @param buffer The slab to unpin.
     */
    static void deallocate(Buffet* buffer);
    /** ------------------------------------------------------------------------------------------- Raw
     * @brief Returns the allocation's stable host base pointer.
     * @return The host base pointer.
     */
    void* raw() { return host_; }
    /** ------------------------------------------------------------------------------------------- Raw (const)
     * @brief Returns the allocation's stable host base pointer (const version).
     * @return The host base pointer.
     */
    const void* raw() const { return host_; }
    /** ------------------------------------------------------------------------------------------- Size
     * @brief Returns the granularity-rounded slab size in bytes.
     * @return The slab size.
     */
    size_t size() const;
    /** ------------------------------------------------------------------------------------------- Placemat
     * @brief Returns the registered factory that owns this slab.
     * @return The placement factory.
     */
    Placemat* placement() const;
    /** ------------------------------------------------------------------------------------------- Full
     * @brief Reports whether the slab's bump cursor has reached its capacity.
     * @return True when no further claim begins within the slab.
     */
    bool full() const;
    /** ------------------------------------------------------------------------------------------- Next
     * @brief Returns the successor link, allocating and publishing a fresh slab first when the
     * worker has not prepared one yet.
     * @return The next Buffer in this placement chain.
     */
    Buffet* next();
    /** ------------------------------------------------------------------------------------------- Claim
     * @brief Bump-allocates a Slice from this chain.
     * @param size The exact requested byte size.
     * @return The claimed Slice.
     */
    Slice claim(size_t size);
    /** ------------------------------------------------------------------------------------------- Claim with Novel Buffer
     * @brief Bump-allocates a Slice from this chain, optionally creating a novel buffer.
     * @param size The exact requested byte size.
     * @param novel_buffer True for a dedicated buffer that has no chain successor.
     * @return The claimed Slice.
     */
    Slice claim(size_t size, bool novel_buffer);
    /** ------------------------------------------------------------------------------------------- Claim with Copy
     * @brief Bump-allocates a Slice from this chain and copies data into it.
     * @param copy_from The source data to copy.
     * @param size The exact requested byte size.
     * @param novel_buffer True for a dedicated buffer that has no chain successor.
     * @return The claimed Slice.
     */
    Slice claim(const void* copy_from, size_t size, bool novel_buffer = false);
};
static_assert(sizeof(Buffet) == 32, "Buffet must be 32 bytes in size");
} // namespace buffetalligator
