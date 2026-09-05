#pragma once
/** --------------------------------------------------------------------------------------------------------- Buffer
 * @file buffer.hpp
 * @brief Internal counted slab handle and preallocated chain link.
 */
#include <buffetalligator.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace buffetalligator {
/** --------------------------------------------------------------------------------------------------------- Buffer
 * @class Buffet
 * @brief Owns one placement allocation and bump-allocates 64-byte units from it.
 */
class Buffet {
private:
    struct Cold {
        /// @brief The worker publishes a fully allocated successor through this preallocated link.
        std::atomic<Buffet*> next{nullptr};
        /// @brief The placement factory that owns this buffer.
        const Placemat* placement = nullptr;
        /// @brief The handle to the placement allocation.
        Placemat::Handle* handle = nullptr;
    };
    /// @brief Colder properties for the Buffet (kept in a struct for cache alignment)
    Cold* cold_ = nullptr;
    /// @brief Cached host base pointer for the allocation.
    void* host_ = nullptr;
    /// @brief The current bump pointer within the allocation (in 64-byte units)
    std::atomic<uint32_t> ref_count_{0};
    /// @brief The current bump offset within the allocation (in 4096-byte units).
    std::atomic<uint32_t> bump_offset_{0};
    /** ------------------------------------------------------------------------------------------- Free
     * @brief Drops one slab reference and destroys the placement allocation at zero.
     */
    void free() {
        if (ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            BuffetOrder* order = new BuffetOrder(
                
            )

        }
    }
    /** ------------------------------------------------------------------------------------------- Next
     * @brief Waits for and returns the successor preallocated by the Arena worker.
     * @return The next Buffer in this placement chain.
     */
    Buffet* next();
    /** ------------------------------------------------------------------------------------------- Constructor
     * @brief Creates the counted handle around one completed placement allocation.
     * @param placement The factory that owns the allocation.
     * @param size The page-rounded allocation size.
     * @param is_novel True for a dedicated buffer that has no chain successor.
     */
    Buffet(
        const Placemat* placement,
        size_t size,
        bool is_novel
    );
    friend class Alligator;
    friend class Placemat;
    friend class Slice;
public:
    Buffet() = delete;
    Buffet(const Buffet&) = delete;
    Buffet& operator=(const Buffet&) = delete;
    Buffet(Buffet&&) = delete;
    Buffet& operator=(Buffet&&) = delete;
    ~Buffet() { free(); }
    /** ------------------------------------------------------------------------------------------- Deallocate
     * @brief Drops the Arena's chain pin on a demoted slab.
     * @param buffer The slab to unpin.
     */
    static void deallocate(Buffet* buffer);
    /** ------------------------------------------------------------------------------------------- Raw
     * @brief Returns the allocation's stable host base pointer.
     * @return The host base pointer.
     */
    void* raw() { return host_; }
    const void* raw() const { return host_; }
    /** ------------------------------------------------------------------------------------------- Size
     * @brief Returns the page-rounded slab size in bytes.
     * @return The slab size.
     */
    size_t size() const { return size_ == UINT32_MAX ? 0 : static_cast<size_t>(size_) << 12; }
    /** ------------------------------------------------------------------------------------------- Placemat
     * @brief Returns the registered factory that owns this slab.
     * @return The placement factory.
     */
    Placemat& placement() const { return *placement_; }
    /** ------------------------------------------------------------------------------------------- Full
     * @brief Reports whether the slab's bump cursor has reached its capacity.
     * @return True when no further claim begins within the slab.
     */
    bool full() const {
        if (!bump_ptr_) return true;
        return static_cast<size_t>(bump_ptr_->load(std::memory_order_acquire)) * 64 >= size();
    }
    /** ------------------------------------------------------------------------------------------- Claim
     * @brief Bump-allocates a Slice from this chain.
     * @param size The exact requested byte size.
     * @return The claimed Slice.
     */
    Slice claim(size_t size);
    Slice claim(size_t size, bool novel_buffer);
    Slice claim(const void* copy_from, size_t size, bool novel_buffer = false);
};
} // namespace buffetalligator
