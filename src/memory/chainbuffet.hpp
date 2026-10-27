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
 * @brief An atomic linked-list of pre-allocated buffers for slices to claim from.
 */
class ChainBuffet {
private:
    /// @brief Indicates whether the system is in the process of shutting down.
    static std::atomic<bool>& shutting_down();
    /// @brief The handle to the allocated buffer.
    void* handle_;
    /// @brief The raw host pointer for this buffer.
    void* host_ptr_;
    /// @brief The buffet descriptor associated with this buffer.
    const BuffetDescriptor* desc_;
    /// @brief stores copy/move/destructor semantics.
    void* semantics_;
    /// @brief Type metadata (type_ in the Slice)
    uint32_t type_;
    /// @brief Device address, if applicable
    uint64_t device_;
    /// @brief Total size of the buffer
    uint32_t size_;
    /// @brief The refcount for this buffer.
    uint32_t count_{1};
    /// @brief The bump pointer for the current offset to slice.
    alignas(128) std::atomic<uint32_t> bump_ptr_{0};
    /// @brief The link ahead, always populated 3 buffers ahead.
    std::atomic<ChainBuffet*> next_{nullptr};
    /// @brief One previous buffer is held, rotated out when the
    /// chain advances before releasing.
    std::atomic<ChainBuffet*> prev_{nullptr};
    /// @brief Whether this buffer is a novel allocation, not part of the pre-allocated chain.
    bool novel_{false};
    /// @brief Pointer to the current reference in the chain.
    std::atomic<ChainBuffet*>* ref_current_{nullptr};
    /// @brief The number of buffers pre-allocated ahead of this one.
    std::atomic<int> buffers_ahead_{0};
    /// @brief Returns the next buffer in the chain, and makes sure at least 3 buffers
    /// ahead are pre-allocated to keep up with high demand when necessary.
    ChainBuffet* next(int allocate_next = false);
    /// @brief Claims a slice of the given size from this buffer.
    Slice claim(uint32_t size, bool novel_buffer = false);
    /// @brief Moves the specified buffer to the front of the chain (recycles it).
    void take_to_front(ChainBuffet* buf, int distance_from_current);
    /** ------------------------------------------------------------------------------------------- ChainBuffet Constructor
     * @brief Constructs a new ChainBuffet of the specified size and properties.
     * @param desc The buffet descriptor associated with this buffer.
     * @param size The total size of the buffer.
     * @param semantics The copy/move/destructor semantics.
     * @param type The type metadata for the buffer.
     * @param ref_current Pointer to the current reference in the chain.
     * @param alloc_ahead The number of buffers to pre-allocate ahead.
     * @param novel Whether this buffer is a novel allocation.
     */
    ChainBuffet(
        const BuffetDescriptor* desc,
        size_t size,
        void* semantics,
        uint32_t type,
        std::atomic<ChainBuffet*>* ref_current,
        int alloc_ahead,
        bool novel = false
    );
    friend class Alligator; friend class Slice; friend struct SliceStatics;
public:
    ChainBuffet(const ChainBuffet&) = delete;
    ChainBuffet& operator=(const ChainBuffet&) = delete;
    ChainBuffet(ChainBuffet&&) = delete;
    ChainBuffet& operator=(ChainBuffet&&) = delete;
    ~ChainBuffet() = default;
    static bool decrement_refcount(ChainBuffet* buf);
    static bool is_novel(const ChainBuffet* buf);
    bool is_novel() const;
};
/// @brief The sentinel a link slot holds while one thread makes the link for it.
inline ChainBuffet* const PENDING_LINK = reinterpret_cast<ChainBuffet*>(-1);
} // namespace buffetalligator
