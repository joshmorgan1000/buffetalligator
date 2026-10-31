/** --------------------------------------------------------------------------------------------------------- Buffet Alligator
 * @file alligator.cpp
 * @brief The Alligator: every claim and release, the lanes of chains each placement keeps, and teardown.
 */
#include <logging.hpp>
#include <alligator.hpp>
#include <alligator/kitchen.hpp>
#include <alligator/easygpu.hpp>
#include <alligator/mmapbuffet.hpp>
#include <gpu/backend.hpp>
#include <gpu/easyvulkan.hpp>
#include <memory/tracker.hpp>
#include <atomic>
#include <cstring>
#include <thread>

namespace buffetalligator {
namespace {
inline static constexpr size_t MAX_SINGLE_UNITS = (1ull << 27) - 1;
inline static constexpr size_t MAX_VECTOR_UNITS = (1ull << 33) - 1;
inline static constexpr size_t MAX_MATRIX_UNITS = (1ull << 35) - 1;
static constexpr size_t UNIT_SIZES[4] = { 1, 64, 256, 4096 };
} // anonymous namespace
/** --------------------------------------------------------------------------------------------------------- Destructor
 * @brief Destructor for PackedSize, releases the claim if held.
 */
PackedSize::~PackedSize() { release(); }
/** --------------------------------------------------------------------------------------------------------- Unit
 * @brief Retrieves the unit size of the packed size.
 * @return The unit size in bytes.
 */
size_t PackedSize::unit() const { return (size_ & 1) == 1 ? UNIT_SIZES[(size_ >> 1) & 0x3] : 0; }
/** --------------------------------------------------------------------------------------------------------- Size
 * @brief Retrieves the size of the packed size.
 * @return The size in bytes.
 */
size_t PackedSize::size() const { return (size_ & 1) == 1 ? ((size_ >> 3) & 0x7FFFFFF) * unit() : 0; }
/** --------------------------------------------------------------------------------------------------------- Capacity
 * @brief Retrieves the capacity of the packed size.
 * @return The capacity in bytes.
 */
size_t PackedSize::capacity() const {
    if (!(size_ & 1)) return 0;  // Buffer is not allocated.
    if (unit() == 1) return (size() + 63) & ~size_t{63};
    if (unit() == 64) return (size() + 255) & ~size_t{255};
    if (unit() == 256) return (size() + 4095) & ~size_t{4095};
    return (size() + 65535) & ~size_t{65535};
}
/** --------------------------------------------------------------------------------------------------------- Try Claim
 * @brief Attempts to claim the packed size for exclusive use.
 * @return True if the claim was successful, false otherwise.
 */
bool PackedSize::try_claim() { return std::atomic_ref<uint32_t>(size_).fetch_or(1) == 0; }
/** --------------------------------------------------------------------------------------------------------- Release
 * @brief Releases the claim, making it available for others.
 */
void PackedSize::release() { std::atomic_ref<uint32_t>(size_).store(0, std::memory_order_release); }
/** --------------------------------------------------------------------------------------------------------- Constructor
 * @brief Constructs a PackedSize with the specified size.
 * @param size The size in bytes.
 */
PackedSize::PackedSize(size_t size) {
    if (size & ~MAX_SINGLE_UNITS) {
        if (size & ~MAX_VECTOR_UNITS) {
            if (size & ~MAX_MATRIX_UNITS) {
                size_ = (((size + 4095) & ~4095) >> 9) | 7u;
                return;
            }
            size_ = (((size + 255) & ~255) >> 5) | 5u;
            return;
        }
        size_ = (((size + 63) & ~63) >> 3) | 3u;
        return;
    }
    size_ = (size << 3) | 1u;
}
/** --------------------------------------------------------------------------------------------------------- Assignment Operator
 * @brief Assigns a new size to the PackedSize.
 * @param size The new size in bytes.
 * @return Reference to the PackedSize.
 */
PackedSize& PackedSize::operator=(size_t size) {
    *this = PackedSize(size);
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Assignment Operator
 * @brief Assigns the value of another PackedSize to this one.
 * @param other The other PackedSize.
 * @return Reference to this PackedSize.
 */
PackedSize& PackedSize::operator=(PackedSize&& other) {
    std::swap(size_, other.size_);
    return *this;
}
/** --------------------------------------------------------------------------------------------------------- Conversion Operator
 * @brief Converts the PackedSize to a size_t.
 * @return The size in bytes.
 */
PackedSize::operator size_t() const { return size(); }
/** --------------------------------------------------------------------------------------------------------- Constructor
 * @brief Constructs a PackedOffset with the specified offset.
 * @param offset The offset in bytes.
 */
PackedOffset::PackedOffset(size_t offset) {
    if (offset & ~((MAX_SINGLE_UNITS << 1) | 1)) {
        if (offset & ~((MAX_VECTOR_UNITS << 1) | 1)) {
            if (offset & ~((MAX_MATRIX_UNITS << 1) | 1)) {
                offset_ = (((offset + 4095) & ~4095) >> 10) | 3u;
                return;
            }
            offset_ = (((offset + 255) & ~255) >> 6) | 2u;
            return;
        }
        offset_ = (((offset + 63) & ~63) >> 4) | 1u;
        return;
    }
    offset_ = (offset << 2);
}
/** --------------------------------------------------------------------------------------------------------- Unit
 * @brief Returns the unit of the packed offset.
 * @return The unit value.
 */
size_t PackedOffset::unit() const { return offset_ & 3u; }
/** --------------------------------------------------------------------------------------------------------- Offset
 * @brief Returns the offset of the packed offset.
 * @return The offset value.
 */
size_t PackedOffset::offset() const {
    switch (offset_ & 3u) {
        case 3u: return ((offset_ & ~3u) << 10);
        case 2u: return ((offset_ & ~3u) << 6);
        case 1u: return ((offset_ & ~3u) << 4);
        default: return (offset_ >> 2);
    }
}
/** --------------------------------------------------------------------------------------------------------- Assignment Operator
 * @brief Assigns the value of another PackedOffset to this one.
 * @param other The other PackedOffset.
 * @return Reference to this PackedOffset.
 */
PackedOffset& PackedOffset::operator=(PackedOffset&& other) {
    std::swap(offset_, other.offset_);
    return *this;
}
/** ------------------------------------------------------------------------------------------- Allocator
 * @brief Returns the singleton instance of the allocator.
 * @return Reference to the allocator instance.
 */
IBuffet::Allocator& IBuffet::allocator() {
    static Allocator instance;
    return instance;
}
/** --------------------------------------------------------------------------------------- next_available
 * @brief Get the next available slice index for the given buffer.
 * @param buffer The buffer to allocate from.
 * @param offset The offset within the buffer.
 * @param size The size of the slice.
 * @return The index of the next available slice.
 */
uint32_t IBuffet::Allocator::next_available(
    std::shared_ptr<IBuffet> buffer,
    size_t offset,
    size_t size
) {
    if (offset == 0 && size == 0xFFFFFFFFu) {
        offset = 0;
        size = buffer->size();
    }
    while (true) {
        const size_t idx = next_idx.fetch_add(1) & 0xFFFFF;
        if (pool[idx].try_claim()) {
            pool[idx].buffer_ = buffer;
            pool[idx].size_ = size;
            return idx;
        }
    }
}
/** --------------------------------------------------------------------------------------- Next Available Slice
 * @brief Get the next available slice record from the pool.
 * @return Pointer to the next available SliceRec.
 */
SliceRec* IBuffet::Allocator::next_available() {
    while (true) {
        const size_t idx = next_idx.fetch_add(1) & 0xFFFFF;
        if (pool[idx].try_claim()) return &pool[idx];
    }
}
/** --------------------------------------------------------------------------------------- t_id
 * @brief Get the thread-local ID for the current thread.
 * @return The thread-local ID.
 */
size_t IBuffet::Allocator::t_id() {
    static thread_local size_t id = pick.fetch_add(1);
    return id;
}
} // namespace buffetalligator
